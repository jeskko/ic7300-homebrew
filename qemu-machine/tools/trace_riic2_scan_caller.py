#!/usr/bin/env python3
"""Finds the body.bin caller driving the RIIC2 EEPROM scan burst (see README.md's Status
section -- 557 I2C transactions in ~6s, correlated with the outer ring's overflow). A write
watchpoint on RIIC2's real DRT register (0xFCFEE83C) catches every low-level I2C byte send;
this collects LR across many hits to find which return address (the actual scanning loop, not a
one-off cold-boot signature check) dominates once the real bulk-scan burst is underway.

Usage: trace_riic2_scan_caller.py [n_hits]
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
RIIC2_DRT = 0xFCFEE83C


def main():
    n_hits = int(sys.argv[1]) if len(sys.argv) > 1 else 40

    proc = launch_qemu(["-global", f"rza1h-riic.image={RIIC_IMAGE}"])
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, f"not a fresh boot, PC={regs['r15']:#x}"

        g.set_watchpoint(RIIC2_DRT, 1, kind=2)  # Z2, write
        g.cont()

        lr_counts = Counter()
        hits = []
        start = time.time()
        for i in range(n_hits):
            g.wait_stop(timeout=30)
            regs = g.read_registers()
            lr = regs["r14"]
            pc = regs["r15"]
            lr_counts[lr] += 1
            hits.append((time.time() - start, i, pc, lr, regs.get("r1")))
            g.cont()

        try:
            g.remove_watchpoint(RIIC2_DRT, 1, kind=2)
        except Exception as e:
            print(f"(remove_watchpoint failed, ignoring: {e})")

        print(f"{len(hits)} watchpoint hits collected\n")
        print("Last 10 hits (t, #, pc, lr, r1=written value):")
        for t, i, pc, lr, r1 in hits[-10:]:
            print(f"  t={t:6.3f}s  #{i:3d}  pc={pc:#010x}  lr={lr:#010x}  r1={r1:#x}")
        print("\nLR histogram (which return address dominates):")
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
