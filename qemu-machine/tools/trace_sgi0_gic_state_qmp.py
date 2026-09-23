#!/usr/bin/env python3
"""GDB-free redo of `trace_sgi0_gic_state.py`. That tool (GDB `interrupt()`+`read_memory()`
polling, the exact technique that caught the original CPSR.I=1/SGI0-pending correlation
pre-fix) was tried again post-HSK1-fix and, this time, suppressed the now-much-faster overflow
outright -- 15s with GDB polling, zero overflows, vs. every QMP-only trial this session
overflowing by t=3-4s. So this is the same GIC-state check (GICD_ISPENDR0/ISACTIVER0,
GICC_PMR/RPR, CPSR.I, the outer ring header), but via QMP `xp`/`info registers` only -- zero
GDB, zero breakpoints, zero `interrupt()` calls -- matching the technique that already worked
for the SCIF5-burst/outer-ring correlation (`trace_dsp_param_burst.py`).

Usage: trace_sgi0_gic_state_qmp.py [seconds] [poll_interval_s]
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

GICD_BASE = 0xE8201000
GICC_BASE = 0xE8202000
GICD_ISPENDR0 = GICD_BASE + 0x200
GICD_ISACTIVER0 = GICD_BASE + 0x300
GICC_PMR = GICC_BASE + 0x04
GICC_RPR = GICC_BASE + 0x14


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


def xp32(s, addr: int) -> int:
    reply = hmp(s, f"xp /1xw 0x{addr:x}")
    return int(reply.strip().split()[-1], 16)


def regs(s) -> tuple[int, int, int]:
    text = hmp(s, "info registers")
    r0 = pc = psr = None
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R00="):
                r0 = int(tok[4:], 16)
            elif tok.startswith("R15="):
                pc = int(tok[4:], 16)
            elif tok.startswith("PSR="):
                psr = int(tok[4:], 16)
    return r0, pc, psr


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.1

    sock_path = "/tmp/qemu_sgi0gic.sock"
    Path(sock_path).unlink(missing_ok=True)
    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)
        start = time.time()
        last_pending = None
        while time.time() - start < seconds:
            time.sleep(poll_interval)
            elapsed = time.time() - start
            try:
                hdr_raw = hmp(s, f"xp /4xb 0x{RING_BASE:x}")
                hdr = [int(x, 16) for x in hdr_raw.strip().split()[1:]]
                write_idx, read_idx, pending, capacity = hdr
                ispendr0 = xp32(s, GICD_ISPENDR0)
                isactiver0 = xp32(s, GICD_ISACTIVER0)
                pmr = xp32(s, GICC_PMR)
                rpr = xp32(s, GICC_RPR)
                r0, pc, psr = regs(s)
            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                print(f"t={elapsed:6.2f}s  QMP read failed, retrying")
                continue
            cpsr_i = (psr >> 7) & 1 if psr is not None else -1
            if pc == OVERFLOW_TRAP:
                print(f"t={elapsed:7.3f}s  *** OVERFLOW TRAP ***  r0={r0} "
                      f"SGI0_pend={ispendr0 & 1} SGI0_active={isactiver0 & 1} "
                      f"PMR=0x{pmr:02x} RPR=0x{rpr:02x} CPSR.I={cpsr_i}")
                break
            if pending != last_pending:
                print(f"t={elapsed:7.3f}s  write={write_idx:2d} read={read_idx:2d} "
                      f"pending={pending:2d}  SGI0_pend={ispendr0 & 1} SGI0_active={isactiver0 & 1} "
                      f"PMR=0x{pmr:02x} RPR=0x{rpr:02x} CPSR.I={cpsr_i}  pc=0x{pc:08x}")
            last_pending = pending
        else:
            print(f"no overflow in {seconds:.0f}s")
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
