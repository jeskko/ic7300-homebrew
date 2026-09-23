#!/usr/bin/env python3
"""Answers the user's direct question: which GIC ID is actually firing/running during the
overflow window, and how does its rate compare to the rest of the boot? Fully GDB-free (QMP
`xp` only, no breakpoints, no polling-perturbation risk beyond what trace_dsp_param_burst.py and
trace_sgi0_gic_state_qmp.py already established as safe).

Polls, every `poll_interval` seconds:
  - GICC_HPPIR (Highest Priority Pending Interrupt Register, GICC_BASE+0x18) -- the ID of
    whatever's highest-priority pending/active right now, read-only, no side effect (unlike
    GICC_IAR, which would acknowledge/consume the interrupt if read).
  - the outer ring header, to correlate against `pending`.

Tallies a histogram of HPPIR IDs seen overall, and separately during a "backlog window" (any
sample where the outer ring's `pending` > 0) vs. the rest of the boot -- directly answers
"is some ID particularly active when the overflow happens" without guessing.

Usage: trace_irq_frequency.py [seconds] [poll_interval_s]
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"

RING_BASE = 0x20420120
OVERFLOW_TRAP = 0x200B93FC
GICD_BASE = 0xE8201000
GICC_BASE = 0xE8202000
GICC_HPPIR = GICC_BASE + 0x18
GICD_ISENABLER = GICD_BASE + 0x100  # ISENABLERn at +0x100+4n, covers IDs [32n, 32n+31]

# Registers covering every ID this project has ever named real (see README.md's peripheral
# table): 0=SGI0(0-31), 1=DMAC0(32-63), 4=OSTM0/MTU2ch3/ch4A(128-159), 5=MTU2ch4C(160-191),
# 7=SCIF5 BRI/ERI/RXI/TXI(224-255). Answers the user's follow-up directly: does any of these
# transition from disabled to enabled right around the overflow, not just fire more often.
ISENABLER_REGS = [0, 1, 4, 5, 7]

# Known real GIC IDs from README.md's peripheral table, for readable output.
ID_NAMES = {
    0: "SGI0(ring-drain)", 41: "DMAC0", 134: "OSTM0",
    154: "MTU2ch3-TGI3A", 159: "MTU2ch4-TGI4A", 161: "MTU2ch4-TGI4C",
    241: "SCIF5-BRI", 242: "SCIF5-ERI", 243: "SCIF5-RXI", 244: "SCIF5-TXI",
    1023: "(spurious/idle)",
}


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


def name(gic_id: int) -> str:
    return ID_NAMES.get(gic_id, f"ID{gic_id}")


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.05

    sock_path = "/tmp/qemu_irqfreq.sock"
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
    overall = Counter()
    backlog = Counter()
    idle = Counter()
    samples = []
    last_enabler = {}
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)
        start = time.time()
        overflowed = False
        while time.time() - start < seconds:
            time.sleep(poll_interval)
            elapsed = time.time() - start
            try:
                hppir = xp32(s, GICC_HPPIR) & 0x3ff
                hdr_raw = hmp(s, f"xp /4xb 0x{RING_BASE:x}")
                hdr = [int(x, 16) for x in hdr_raw.strip().split()[1:]]
                pending = hdr[2]
                enabler = {n: xp32(s, GICD_ISENABLER + 4 * n) for n in ISENABLER_REGS}
            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                continue
            overall[hppir] += 1
            if pending > 0:
                backlog[hppir] += 1
            else:
                idle[hppir] += 1
            samples.append((elapsed, hppir, pending))
            if pending == 16 and not overflowed:
                print(f"t={elapsed:7.3f}s  *** pending=16 (overflow) ***  HPPIR={name(hppir)}")
                overflowed = True
            for n, val in enabler.items():
                prev = last_enabler.get(n)
                if prev is not None and prev != val:
                    newly_on = val & ~prev
                    newly_off = prev & ~val
                    for bit in range(32):
                        gid = n * 32 + bit
                        if newly_on & (1 << bit):
                            print(f"t={elapsed:7.3f}s  ISENABLER: {name(gid)} (ID {gid}) "
                                  f"newly ENABLED")
                        if newly_off & (1 << bit):
                            print(f"t={elapsed:7.3f}s  ISENABLER: {name(gid)} (ID {gid}) "
                                  f"newly disabled")
            last_enabler = enabler
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        Path(sock_path).unlink(missing_ok=True)

    print(f"\n{len(samples)} samples over {seconds:.0f}s")
    print("\nHPPIR histogram, samples with ring pending==0 (normal/idle):")
    for gid, n in idle.most_common(10):
        print(f"  {name(gid):20s} {n:5d}  ({100*n/max(1,sum(idle.values())):.1f}%)")
    print("\nHPPIR histogram, samples with ring pending>0 (backlog/overflow window):")
    for gid, n in backlog.most_common(10):
        print(f"  {name(gid):20s} {n:5d}  ({100*n/max(1,sum(backlog.values())):.1f}%)")


if __name__ == "__main__":
    main()
