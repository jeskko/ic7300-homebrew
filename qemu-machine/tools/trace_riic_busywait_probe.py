#!/usr/bin/env python3
"""Checks, live, whether the busy-wait loop in FUN_2001dcc4/FUN_2001dd58 (`while (*pcVar1 != 0)
FUN_20062c1c();`, called once per 32-byte RIIC2 chunk transaction -- see README.md's Status
section) genuinely blocks the calling task via a real SVC/kernel trap, or always takes the
"already in kernel context" fast-fail path (r0=0x82, no wait at all -- see FUN_20187010's own
decompile) -- i.e. whether this is a real busy-spin or a real (if repeated) RTOS block/wake.

Two one-shot, bounded breakpoints (same discipline as this project's other bounded-probe tools):
  - 0x2018701e -- the fast-fail path (`movs r0,#0x82`), taken when in_kernel_context() != 0.
  - 0x20187034 -- the real `svc 0x0` instruction, only reached when in_kernel_context() == 0.
Whichever one dominates (or whether both fire) directly answers the question -- this is a
structural/control-flow check, not a timing measurement, so the well-established caution in this
project about GDB perturbing *timing* results applies much less here.

Usage: trace_riic_busywait_probe.py [n_hits]
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
FASTFAIL_ADDR = 0x2018701E
SVC_ADDR = 0x20187034


def main():
    n_hits = int(sys.argv[1]) if len(sys.argv) > 1 else 100

    proc = launch_qemu(["-global", f"rza1h-riic.image={RIIC_IMAGE}"])
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, f"not a fresh boot, PC={regs['r15']:#x}"

        # FUN_20187010 is THUMB code (confirmed via raw bytes -- 2-byte opcodes like
        # `30 b5` push{r4,r5,lr}), not this project's usual ARM-mode default -- kind=2, not 4.
        g.set_breakpoint(FASTFAIL_ADDR, kind=2)
        g.set_breakpoint(SVC_ADDR, kind=2)
        g.cont()

        hit_counts = Counter()
        hits = []
        start = time.time()
        for i in range(n_hits):
            g.wait_stop(timeout=30)
            regs = g.read_registers()
            pc = regs["r15"]
            lr = regs["r14"]
            which = "FASTFAIL(0x82)" if pc == FASTFAIL_ADDR else (
                "SVC" if pc == SVC_ADDR else f"unknown pc={pc:#x}")
            hit_counts[which] += 1
            hits.append((time.time() - start, i, which, lr))
            g.cont()

        try:
            g.remove_breakpoint(FASTFAIL_ADDR, kind=2)
            g.remove_breakpoint(SVC_ADDR, kind=2)
        except Exception as e:
            print(f"(remove_breakpoint failed, ignoring: {e})")

        print(f"{len(hits)} breakpoint hits collected\n")
        print("Last 20 hits (t, #, which, lr=caller return addr):")
        for t, i, which, lr in hits[-20:]:
            print(f"  t={t:6.3f}s  #{i:3d}  {which:16s}  lr={lr:#010x}")
        print("\nHit-type histogram:")
        for which, n in hit_counts.most_common():
            print(f"  {which:16s}  {n:4d} hits ({100*n/len(hits):.1f}%)")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
