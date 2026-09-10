#!/usr/bin/env python3
"""Decodes a live RIIC2 capture (tools/live_riic2_capture.sh's own .sr output) via sigrok's
built-in I2C protocol decoder and extracts real per-byte timing, for direct comparison against
this project's own decompiled-register-derived bit-rate formula (~340kHz, ~26.4us/byte -- see
riic.c's own riic_scl_periods_ns() comment for the full derivation). 2026-09-10.

This is the actual ground-truth check the whole `qemu-machine/` RIIC2 timing model has never had
until now -- every number so far was derived from decompiled firmware register values
(FRQCR/BRL/BRH/MR1/MR3/FER), confirmed internally consistent, but never checked against a real,
independently-measured bus signal.

Output line format verified directly against the installed i2c decoder (not assumed) via a
synthetic waveform before trusting this on the one-shot real capture -- e.g.:
  "40-124 i2c-1: Address write: 50"    (sample_start-sample_end, includes the decoded value)
  "148-244 i2c-1: Data write: 42"
  "136-148 i2c-1: ACK"
  "20-20 i2c-1: Start" / "261-261 i2c-1: Stop"
Bare "0"/"1" lines are individual raw bits, not the byte-level events we want -- filtered out.

Usage: analyze_riic2_capture.py <capture.sr> [sample_rate_hz]

`sample_rate_hz` only needed if it can't be read from the .sr file's own metadata (it normally
can -- sigrok sessions embed their own sample rate).
"""

from __future__ import annotations

import re
import statistics
import subprocess
import sys
import zipfile
from pathlib import Path

# Byte-level events only -- excludes bare bit lines ("0"/"1") and Start/Stop/ACK/NACK, which
# don't by themselves represent a full byte's own clocking span.
EVENT_RE = re.compile(
    r"^(\d+)-(\d+)\s+i2c-\d+:\s+"
    r"(Start(?:\s+repeat)?|Stop|ACK|NACK|"
    r"(Address write|Address read|Data write|Data read):\s*([0-9A-Fa-f]+))\s*$"
)


def get_samplerate(sr_path: Path) -> float | None:
    """.sr files are zip archives with a metadata text file naming samplerate=... """
    try:
        with zipfile.ZipFile(sr_path) as z:
            meta = z.read("metadata").decode()
        m = re.search(r"samplerate\s*=\s*([\d.]+)\s*([kMG]?)", meta)
        if not m:
            return None
        val = float(m.group(1))
        mult = {"": 1, "k": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]
        return val * mult
    except Exception:
        return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sr_path = Path(sys.argv[1])
    if not sr_path.exists():
        sys.exit(f"not found: {sr_path}")

    samplerate = get_samplerate(sr_path)
    if len(sys.argv) > 2:
        samplerate = float(sys.argv[2])
    if not samplerate:
        sys.exit("could not determine sample rate from .sr metadata -- pass it explicitly")

    cmd = [
        "sigrok-cli", "-i", str(sr_path),
        "-P", "i2c:scl=0:sda=1",
        "--protocol-decoder-samplenum",
    ]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"sigrok-cli decode failed:\n{out.stderr}")

    events = []  # (start_sample, end_sample, label)
    for line in out.stdout.splitlines():
        m = EVENT_RE.match(line.strip())
        if m:
            events.append((int(m.group(1)), int(m.group(2)), m.group(3)))

    if not events:
        print("No I2C activity decoded in this capture -- likely missed the scan window "
              "(or nothing is connected to the probes yet).")
        print("Try again with a different --delay in live_riic2_capture.sh.")
        return

    print(f"Sample rate: {samplerate/1e6:.3f} MHz")
    print(f"{len(events)} decoded I2C events, spanning "
          f"{(events[-1][1]-events[0][0])/samplerate*1e3:.2f}ms of real capture\n")

    byte_events = [e for e in events if e[2].startswith(
        ("Address write:", "Address read:", "Data write:", "Data read:"))]
    print(f"{len(byte_events)} byte-level events (address+data)")

    if len(byte_events) >= 2:
        gaps_ns = []
        for (s0, _, _), (s1, _, _) in zip(byte_events, byte_events[1:]):
            gap_s = (s1 - s0) / samplerate
            if gap_s < 0.001:  # exclude gaps spanning a STOP/START boundary (new transaction)
                gaps_ns.append(gap_s * 1e9)
        if gaps_ns:
            median_ns = statistics.median(gaps_ns)
            predicted_ns = 26433  # riic_byte_time_ns()'s own real, decompiled-register-derived value
            print(f"\nReal measured per-byte gap (consecutive same-transaction byte-starts), ns:")
            print(f"  n={len(gaps_ns)} mean={statistics.mean(gaps_ns):.0f} "
                  f"median={median_ns:.0f} min={min(gaps_ns):.0f} max={max(gaps_ns):.0f}")
            print(f"\nThis project's decompiled-register-derived prediction: {predicted_ns}ns/byte "
                  f"(~340kHz)")
            print(f"Real measured: {median_ns:.0f}ns/byte "
                  f"(~{1/(median_ns*1e-9)/1000:.0f}kHz effective byte rate)")
            ratio = median_ns / predicted_ns
            print(f"Ratio (measured/predicted): {ratio:.3f}x")
            if 0.8 < ratio < 1.25:
                print("-> Formula confirmed within ~20%: real hardware matches the decompiled-"
                      "register derivation.")
            else:
                print("-> Real, meaningful discrepancy -- worth re-checking the bit-rate formula "
                      "or the register values it was derived from.")
        else:
            print("\nAll byte events were isolated (single-byte transactions) -- no "
                  "within-transaction gap to measure. Still useful: check the Address write: "
                  "value below against 0x50 (this project's own decompiled RIIC2_EEPROM_I2C_ADDR).")

    print("\nFirst 30 decoded events:")
    for s0, s1, label in events[:30]:
        print(f"  t={s0/samplerate*1e3:9.4f}ms  {label}")


if __name__ == "__main__":
    main()
