#!/usr/bin/env python3
"""Sweep radio settings over CI-V and record which DSP commands each one causes.

Built 2026-09-24. Boots QEMU (sleep=off, fast) with SCIF0 on a CI-V socket and
RZA1H_DEBUG=dsp, waits until CI-V answers, then for each step sends one CI-V command and
collects the fake DSP's "cmd ..." log lines (they only log a command word that changed)
stamped inside that step's window. Log timestamps are g_get_monotonic_time(), the same
clock as Python's time.monotonic().

Usage: civ_dsp_sweep.py [--addr 0x94] [--settle S] [--out FILE]
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
    ('scope ON', (0x27, 0x10, 0x01,)),
    ('scope mode center', (0x27, 0x14, 0x00, 0x00,)),
    ('scope mode fixed', (0x27, 0x14, 0x00, 0x01,)),
    ('scope mode scroll-C', (0x27, 0x14, 0x00, 0x02,)),
    ('scope mode scroll-F', (0x27, 0x14, 0x00, 0x03,)),
    ('scope mode center', (0x27, 0x14, 0x00, 0x00,)),
    ('scope span 5k', (0x27, 0x15, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00,)),
    ('scope span 25k', (0x27, 0x15, 0x00, 0x00, 0x50, 0x02, 0x00, 0x00,)),
    ('scope span 250k', (0x27, 0x15, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00,)),
    ('scope span 2.5k', (0x27, 0x15, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00,)),
    ('scope ref +10dB', (0x27, 0x19, 0x00, 0x10, 0x00, 0x00,)),
    ('scope ref -10dB', (0x27, 0x19, 0x00, 0x10, 0x00, 0x01,)),
    ('scope ref 0dB', (0x27, 0x19, 0x00, 0x00, 0x00, 0x00,)),
    ('scope speed 0', (0x27, 0x1a, 0x00, 0x00,)),
    ('scope speed 1', (0x27, 0x1a, 0x00, 0x01,)),
    ('scope speed 2', (0x27, 0x1a, 0x00, 0x02,)),
    ('scope center type 0', (0x27, 0x1c, 0x00, 0x00,)),
    ('scope center type 1', (0x27, 0x1c, 0x00, 0x01,)),
    ('scope center type 2', (0x27, 0x1c, 0x00, 0x02,)),
    ('scope VBW wide', (0x27, 0x1d, 0x00, 0x01,)),
    ('scope VBW narrow', (0x27, 0x1d, 0x00, 0x00,)),
    ('scope during TX on', (0x27, 0x1b, 0x01,)),
    ('scope during TX off', (0x27, 0x1b, 0x00,)),
    ('scope hold on', (0x27, 0x17, 0x00, 0x01,)),
    ('scope hold off', (0x27, 0x17, 0x00, 0x00,)),
    ('scope data out ON', (0x27, 0x11, 0x01,)),
    ('scope data out OFF', (0x27, 0x11, 0x00,)),
    ('preamp 0', (0x16, 0x02, 0x00,)),
    ('preamp 1', (0x16, 0x02, 0x01,)),
    ('preamp 2', (0x16, 0x02, 0x02,)),
    ('att 20dB', (0x11, 0x20,)),
    ('att off', (0x11, 0x00,)),
    ('AGC fast', (0x16, 0x12, 0x01,)),
    ('AGC slow', (0x16, 0x12, 0x03,)),
    ('AGC mid', (0x16, 0x12, 0x02,)),
    ('NB on', (0x16, 0x22, 0x01,)),
    ('NB level 200', (0x14, 0x12, 0x02, 0x00,)),
    ('NB off', (0x16, 0x22, 0x00,)),
    ('NR on', (0x16, 0x40, 0x01,)),
    ('NR level 200', (0x14, 0x06, 0x02, 0x00,)),
    ('NR off', (0x16, 0x40, 0x00,)),
    ('auto notch on', (0x16, 0x41, 0x01,)),
    ('auto notch off', (0x16, 0x41, 0x00,)),
    ('manual notch on', (0x16, 0x48, 0x01,)),
    ('notch pos 200', (0x14, 0x0d, 0x02, 0x00,)),
    ('manual notch off', (0x16, 0x48, 0x00,)),
    ('filter shape soft', (0x16, 0x56, 0x01,)),
    ('filter shape sharp', (0x16, 0x56, 0x00,)),
    ('twin peak on (RTTY only?)', (0x16, 0x4f, 0x01,)),
    ('AF gain 50', (0x14, 0x01, 0x00, 0x50,)),
    ('AF gain 200', (0x14, 0x01, 0x02, 0x00,)),
    ('RF gain 100', (0x14, 0x02, 0x01, 0x00,)),
    ('RF gain 255', (0x14, 0x02, 0x02, 0x55,)),
    ('squelch 100', (0x14, 0x03, 0x01, 0x00,)),
    ('squelch 0', (0x14, 0x03, 0x00, 0x00,)),
    ('PBT in 50', (0x14, 0x07, 0x00, 0x50,)),
    ('PBT in 128', (0x14, 0x07, 0x01, 0x28,)),
    ('PBT out 200', (0x14, 0x08, 0x02, 0x00,)),
    ('PBT out 128', (0x14, 0x08, 0x01, 0x28,)),
    ('RF power 50', (0x14, 0x0a, 0x00, 0x50,)),
    ('mic gain 200', (0x14, 0x0b, 0x02, 0x00,)),
    ('comp level 100', (0x14, 0x0e, 0x01, 0x00,)),
    ('comp on', (0x16, 0x44, 0x01,)),
    ('monitor on', (0x16, 0x45, 0x01,)),
    ('monitor level 200', (0x14, 0x15, 0x02, 0x00,)),
    ('monitor off', (0x16, 0x45, 0x00,)),
    ('VOX on', (0x16, 0x46, 0x01,)),
    ('VOX off', (0x16, 0x46, 0x00,)),
    ('filter FIL1', (0x06, 0x01, 0x01,)),
    ('filter FIL3', (0x06, 0x01, 0x03,)),
    ('filter FIL2', (0x06, 0x01, 0x02,)),
    ('data mode on', (0x1a, 0x06, 0x01, 0x01,)),
    ('data mode off', (0x1a, 0x06, 0x00, 0x00,)),
    ('op mode CW', (0x06, 0x03, 0x01,)),
    ('CW pitch 200', (0x14, 0x09, 0x02, 0x00,)),
    ('key speed 200', (0x14, 0x0c, 0x02, 0x00,)),
    ('break-in semi', (0x16, 0x47, 0x01,)),
    ('break-in off', (0x16, 0x47, 0x00,)),
    ('op mode LSB', (0x06, 0x00, 0x01,)),
    ('op mode AM', (0x06, 0x02, 0x01,)),
    ('op mode FM', (0x06, 0x05, 0x01,)),
    ('op mode RTTY', (0x06, 0x04, 0x01,)),
    ('op mode USB', (0x06, 0x01, 0x02,)),
    ('freq 7.100.000', (0x05, 0x00, 0x00, 0x10, 0x07, 0x00,)),
    ('freq 50.100.000', (0x05, 0x00, 0x00, 0x10, 0x50, 0x00,)),
    ('freq 14.100.000', (0x05, 0x00, 0x00, 0x10, 0x14, 0x00,)),
    ('PTT TX on', (0x1c, 0x00, 0x01,)),
    ('PTT TX off', (0x1c, 0x00, 0x00,)),
    ('tuner on', (0x1c, 0x01, 0x01,)),
    ('tuner off', (0x1c, 0x01, 0x00,)),
]

LOG_RE = re.compile(r"\[rza1h:(\w+) t=([0-9.]+)\] (.*)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=0x94,
                    help="radio CI-V address (factory default 0x94)")
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
            if (m.group(1) == "dsp" and msg.startswith("cmd ")) or \
               (m.group(1) == "rspi2" and "frame" in msg) or m.group(1) not in ("dsp", "rspi2"):
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
