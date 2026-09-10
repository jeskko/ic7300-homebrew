#!/usr/bin/env python3
"""Runs N independent trials, each free-running (no GDB, no breakpoints) until the known
job-ring-overflow trap (PC=0x200b93fc) is hit, then reports r0 (the trap's own error-code
argument) via a single QMP `info registers` read-only spot-check -- the same technique already
used once this session. r0=2 means the *producer* (FUN_20187bb4) found the outer 16-slot ring
already full on its own periodic push; r0=3 means the *consumer* (FUN_201877e4's type==1 branch)
found a downstream per-job-object secondary buffer full while forwarding a dequeued message --
see README.md's Status section for what this distinguishes.

Coarse polling only (every few seconds) -- this project has repeatedly found tight/frequent
polling suppresses the very stall being chased; a few-second cadence has not shown that problem.

Usage: check_overflow_r0.py [n_trials] [max_seconds_per_trial]
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
OVERFLOW_TRAP = 0x200B93FC


def qmp_regs(sock_path: str) -> str:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.recv(65536)  # greeting
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    s.send(b'{"execute":"human-monitor-command","arguments":{"command-line":"info registers"}}')
    reply = json.loads(s.recv(65536).decode())
    s.close()
    return reply["return"]


def parse_r0_pc(regs_text: str) -> tuple[int, int]:
    r0 = pc = None
    for line in regs_text.splitlines():
        for tok in line.split():
            if tok.startswith("R00="):
                r0 = int(tok[4:], 16)
            elif tok.startswith("R15="):
                pc = int(tok[4:], 16)
    return r0, pc


def run_trial(n: int, max_seconds: float, poll_interval: float = 3.0) -> None:
    sock_path = f"/tmp/qemu_r0check_{n}.sock"
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
        time.sleep(1.0)  # let QMP socket come up
        start = time.time()
        hit = False
        while time.time() - start < max_seconds:
            time.sleep(poll_interval)
            try:
                regs = qmp_regs(sock_path)
            except (ConnectionRefusedError, FileNotFoundError, socket.timeout):
                continue
            r0, pc = parse_r0_pc(regs)
            elapsed = time.time() - start
            if pc == OVERFLOW_TRAP:
                print(f"trial {n}: t={elapsed:5.1f}s  *** TRAP HIT ***  r0={r0}  pc=0x{pc:08x}")
                hit = True
                break
            else:
                print(f"trial {n}: t={elapsed:5.1f}s  pc=0x{pc:08x} (not yet at trap)")
        if not hit:
            print(f"trial {n}: did not hit the trap within {max_seconds:.0f}s")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        Path(sock_path).unlink(missing_ok=True)


def main() -> None:
    n_trials = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    max_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    for n in range(1, n_trials + 1):
        run_trial(n, max_seconds)


if __name__ == "__main__":
    main()
