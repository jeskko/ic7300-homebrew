#!/usr/bin/env python3
"""Finds what in body.bin triggers the RIIC2 (I2C/diode-matrix-EEPROM) transaction burst that
dominates GIC activity (ID 205, TEI) during the outer-ring overflow window (see README.md's
Status section -- the `trace_irq_frequency.py` finding).

Runs with RZA1H_DEBUG=riic (this project's own established host-side, GDB-free logging, proven
non-perturbing -- see rza1h_debug.h) captured to a log file, while separately polling the outer
ring header via QMP on the *same* host machine. Both `rza1h_debug()`'s own timestamp
(`g_get_monotonic_time()`) and this script's own (`time.monotonic()`) read the same underlying
CLOCK_MONOTONIC source on Linux, so the two timelines are directly comparable without any
alignment step -- print both merged, sorted by that one shared clock, to see exactly what RIIC2
is doing relative to the ring backing up.

Usage: trace_riic2_burst_source.py [seconds]
"""

from __future__ import annotations

import json
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"
LOG = Path("/tmp/riic2_burst.log")

RING_BASE = 0x20420120

LOG_RE = re.compile(r"\[rza1h:(\w+) t=([\d.]+)\] (.*)")


def qmp_open(sock_path: str):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.recv(65536)
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    return s


def hmp(s: socket.socket, cmd: str) -> str:
    s.send(json.dumps({"execute": "human-monitor-command",
                        "arguments": {"command-line": cmd}}).encode())
    return json.loads(s.recv(65536).decode())["return"]


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0

    sock_path = "/tmp/qemu_riic2burst.sock"
    Path(sock_path).unlink(missing_ok=True)
    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=auto",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    env = {"RZA1H_DEBUG": "riic"}
    events = []  # (mono_time, source, text)

    with open(LOG, "w") as logf:
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=logf, env=env)
        try:
            time.sleep(1.0)
            s = qmp_open(sock_path)
            start_wall = time.monotonic()
            last_pending = None
            while time.monotonic() - start_wall < seconds:
                time.sleep(0.1)
                now = time.monotonic()
                try:
                    hdr_raw = hmp(s, f"xp /4xb 0x{RING_BASE:x}")
                    hdr = [int(x, 16) for x in hdr_raw.strip().split()[1:]]
                except (socket.timeout, ConnectionResetError, BrokenPipeError):
                    continue
                pending = hdr[2]
                if pending != last_pending:
                    events.append((now, "ring", f"write={hdr[0]} read={hdr[1]} pending={pending}"))
                last_pending = pending
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            Path(sock_path).unlink(missing_ok=True)

    for line in LOG.read_text(errors="replace").splitlines():
        m = LOG_RE.match(line)
        if m:
            events.append((float(m.group(2)), m.group(1), m.group(3)))

    events.sort(key=lambda e: e[0])
    print(f"{len(events)} merged events (ring + riic debug log), sorted by shared host clock:\n")
    for t, src, text in events:
        print(f"t={t:10.3f}  [{src:5s}]  {text}")


if __name__ == "__main__":
    main()
