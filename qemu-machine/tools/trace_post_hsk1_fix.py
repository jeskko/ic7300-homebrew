#!/usr/bin/env python3
"""Free-runs a boot with the new HSK1/P8_9 gpio.c fix in place (see README.md's Status section)
and reports PC + the outer job-ring header every few seconds, fully GDB-free (QMP only) -- checks
how much further `cold_boot_hw_init` gets now that `scif5_wait_hsk1_ready` should return almost
immediately instead of taking its full ~16-minute software timeout.

Usage: trace_post_hsk1_fix.py [seconds] [poll_interval_s]
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"
RING_BASE = 0x20420120
OVERFLOW_TRAP = 0x200B93FC
HSK1_WAIT = 0x200B48E4
HSK1_WAIT_END = 0x200B490F


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


def regs(s) -> tuple[int, int]:
    text = hmp(s, "info registers")
    r0 = pc = None
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R00="):
                r0 = int(tok[4:], 16)
            elif tok.startswith("R15="):
                pc = int(tok[4:], 16)
    return r0, pc


def read_ring(s) -> str:
    reply = hmp(s, f"xp /4xb 0x{RING_BASE:x}")
    vals = [int(x, 16) for x in reply.strip().split()[1:]]
    return f"write={vals[0]} read={vals[1]} pending={vals[2]} capacity={vals[3]}"


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

    sock_path = "/tmp/qemu_hsk1fix.sock"
    Path(sock_path).unlink(missing_ok=True)
    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=auto",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)
        start = time.time()
        while time.time() - start < seconds:
            time.sleep(poll_interval)
            elapsed = time.time() - start
            try:
                r0, pc = regs(s)
                ring = read_ring(s)
            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                print(f"t={elapsed:6.1f}s  QMP read failed, retrying next poll")
                continue
            in_hsk1_wait = HSK1_WAIT <= pc <= HSK1_WAIT_END
            note = ""
            if pc == OVERFLOW_TRAP:
                note = f"  *** OVERFLOW TRAP, r0={r0} ***"
            elif in_hsk1_wait:
                note = "  (still in scif5_wait_hsk1_ready)"
            print(f"t={elapsed:6.1f}s  pc=0x{pc:08x}  ring[{ring}]{note}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
