#!/usr/bin/env python3
"""Finds which of FUN_2001e484's (the generic "read N EEPROM bytes" chunking wrapper) 24 callers
drives the 557-transaction burst (see README.md's Status section). A one-shot GDB breakpoint on
its entry, collecting LR across many hits then releasing it -- same bounded-probe technique
already used successfully for the lower-level DRT watchpoint.

Usage: trace_eeprom_scan_caller.py [n_hits]
"""

from __future__ import annotations

import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"
FUN_ENTRY = 0x2001E484


def main():
    n_hits = int(sys.argv[1]) if len(sys.argv) > 1 else 50

    proc = launch_qemu(["-global", f"rza1h-riic.image={RIIC_IMAGE}"])
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, f"not a fresh boot, PC={regs['r15']:#x}"

        g.set_breakpoint(FUN_ENTRY)
        g.cont()

        lr_counts = Counter()
        hits = []
        start = time.time()
        for i in range(n_hits):
            g.wait_stop(timeout=30)
            regs = g.read_registers()
            lr = regs["r14"]
            r0, r1, r2 = regs.get("r0"), regs.get("r1"), regs.get("r2")
            lr_counts[lr] += 1
            hits.append((time.time() - start, i, lr, r0, r1, r2))
            g.cont()

        try:
            g.remove_breakpoint(FUN_ENTRY)
        except Exception as e:
            print(f"(remove_breakpoint failed, ignoring: {e})")

        print(f"{len(hits)} breakpoint hits collected\n")
        print("Last 15 hits (t, #, lr=caller return addr, r0=addr, r1=buf, r2=count):")
        for t, i, lr, r0, r1, r2 in hits[-15:]:
            print(f"  t={t:6.3f}s  #{i:3d}  lr={lr:#010x}  addr={r0:#x}  buf={r1:#x}  count={r2}")
        print("\nLR histogram (which caller dominates):")
        for lr, n in lr_counts.most_common(10):
            print(f"  lr={lr:#010x}  {n:3d} hits")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
