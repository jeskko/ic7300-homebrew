#!/usr/bin/env python3
"""Sweep radio settings over CI-V and record which DSP commands each one causes.

Built 2026-09-24. Boots QEMU (sleep=off, fast) with SCIF0 on a CI-V socket and
RZA1H_DEBUG=dsp, waits until CI-V answers, then for each step sends one CI-V command and
collects the fake DSP's "cmd ..." log lines (they only log a command word that changed)
stamped inside that step's window. Log timestamps are g_get_monotonic_time(), the same
clock as Python's time.monotonic().

Usage: civ_dsp_sweep.py [--addr 0x00] [--settle S] [--out FILE]
Steps are the STEPS table below: (label, CI-V bytes).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from civ import Civ  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE  # noqa: E402

# IC-7300 CI-V (0x27 = scope). Values per the IC-7300 CI-V reference; each read-back after a
# set confirms the radio accepted it. Span = +-half-width in Hz, 5 bytes little-endian BCD
# after the main/sub selector byte (FB = accepted, FA = rejected).
STEPS: list[tuple[str, tuple[int, ...]]] = [
    ("read scope on/off", (0x27, 0x10)),
    ("scope ON", (0x27, 0x10, 0x01)),
    ("read scope on/off", (0x27, 0x10)),
    ("scope data out ON", (0x27, 0x11, 0x01)),
    ("mode: center", (0x27, 0x14, 0x00, 0x00)),
    ("mode: fixed", (0x27, 0x14, 0x00, 0x01)),
    ("mode: scroll-C", (0x27, 0x14, 0x00, 0x02)),
    ("mode: scroll-F", (0x27, 0x14, 0x00, 0x03)),
    ("mode: center", (0x27, 0x14, 0x00, 0x00)),
    ("span 2.5k", (0x27, 0x15, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("span 5k", (0x27, 0x15, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("span 10k", (0x27, 0x15, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("span 25k", (0x27, 0x15, 0x00, 0x00, 0x50, 0x02, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("span 50k", (0x27, 0x15, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("span 100k", (0x27, 0x15, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("span 250k", (0x27, 0x15, 0x00, 0x00, 0x50, 0x25, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("span 500k", (0x27, 0x15, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("read span", (0x27, 0x15, 0x00)),
    ("speed fast", (0x27, 0x1A, 0x00, 0x00)),
    ("speed mid", (0x27, 0x1A, 0x00, 0x01)),
    ("speed slow", (0x27, 0x1A, 0x00, 0x02)),
    ("ref level +10dB", (0x27, 0x19, 0x00, 0x01, 0x00, 0x00)),
    ("ref level 0", (0x27, 0x19, 0x00, 0x00, 0x00, 0x00)),
    ("hold ON", (0x27, 0x17, 0x00, 0x01)),
    ("hold OFF", (0x27, 0x17, 0x00, 0x00)),
    ("edge 2", (0x27, 0x16, 0x00, 0x02)),
    ("edge 1", (0x27, 0x16, 0x00, 0x01)),
    ("scope OFF", (0x27, 0x10, 0x00)),
    ("scope ON", (0x27, 0x10, 0x01)),
    ("op mode LSB", (0x06, 0x00, 0x01)),
    ("op mode CW", (0x06, 0x03, 0x01)),
    ("op mode AM", (0x06, 0x02, 0x01)),
    ("op mode FM", (0x06, 0x05, 0x01)),
    ("op mode RTTY", (0x06, 0x04, 0x01)),
    ("op mode USB FIL2", (0x06, 0x01, 0x02)),
    ("freq 7.100.000", (0x05, 0x00, 0x00, 0x10, 0x07, 0x00)),
    ("freq 14.100.000", (0x05, 0x00, 0x00, 0x10, 0x14, 0x00)),
]

LOG_RE = re.compile(r"\[rza1h:(\w+) t=([0-9.]+)\] (.*)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=0x00,
                    help="radio CI-V address (the synthetic EEPROM leaves it 0x00)")
    ap.add_argument("--settle", type=float, default=1.5)
    ap.add_argument("--out")
    ap.add_argument("--debug", default="dsp", help="RZA1H_DEBUG devices to log and collect")
    ap.add_argument("--only", help="regex: run only steps whose label matches")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="civsweep"))
    sock, log = tmp / "civ.sock", tmp / "qemu.log"
    env = dict(**__import__("os").environ, RZA1H_DEBUG=args.debug)
    proc = subprocess.Popen(
        [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH), "-monitor", "none",
         "-global", f"rza1h-riic.image={HERE / 'riic2_eeprom.img'}",
         "-icount", "shift=1,sleep=off",
         "-chardev", f"socket,id=civ,path={sock},server=on,wait=off", "-serial", "chardev:civ"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=open(log, "w"), env=env)
    results = []
    try:
        time.sleep(0.5)
        c = Civ(str(sock), radio=args.addr)
        t0 = time.time()
        while time.time() - t0 < 60 and not c.cmd(0x03, timeout=0.5):
            time.sleep(0.5)
        time.sleep(3)  # let boot-time traffic finish
        for label, cmd in STEPS:
            if args.only and not re.search(args.only, label):
                continue
            a = time.monotonic()
            reply = c.cmd(*cmd, timeout=1.0)
            time.sleep(args.settle)
            results.append((label, cmd, reply, a, time.monotonic()))
    finally:
        proc.terminate()
        proc.wait(timeout=10)

    lines = []
    for ln in log.read_text(errors="replace").splitlines():
        m = LOG_RE.match(ln)
        if m:
            msg = m.group(3) if m.group(1) == "dsp" else f"{m.group(1)}: {m.group(3)}"
            if m.group(1) != "dsp" or msg.startswith("cmd "):
                lines.append((float(m.group(2)), msg))
    out = []
    for label, cmd, reply, a, b in results:
        rep = " | ".join(f.hex(" ") for f in reply) or "(no reply)"
        out.append(f"## {label}: CI-V {' '.join(f'{x:02x}' for x in cmd)} -> {rep}")
        for t, msg in lines:
            if a <= t < b:
                out.append(f"    {msg}")
    text = "\n".join(out)
    print(text)
    if args.out:
        Path(args.out).write_text(text + "\n")


if __name__ == "__main__":
    main()
