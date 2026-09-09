#!/usr/bin/env python3
"""Decisive, near-zero-perturbation test for the DMAC/icount stall (README.md's Status
section, 2026-09-09): does FUN_200b5ea4's busy-wait (DMAC_WAIT_LOOP, 0x200b5f28) ever
actually complete, given enough real wall-clock time?

Earlier single-step timing (measure_dmac_wait_throughput.py) turned out invalid -- GDB's
single-step round-trip cost (~82ms/step under this icount config, confirmed identical
regardless of what instruction executes) completely swamped any real per-instruction MMIO-
dispatch difference. This script avoids that failure mode entirely: exactly ONE breakpoint,
at 0x200b5f34 (`ldmia sp!,{r4,r5,r6,pc}` -- FUN_200b5ea4's own real return, confirmed via
the raw listing: the loop at 0x200b5f28 falls straight through here once r4+2 clears), which
is only ever reached once the wait genuinely finishes. Nothing is set at or near the loop
itself, so it runs at full native TCG speed the whole time -- the cleanest possible "does
this ever finish" test, no observation-method confound at all.

Usage: trace_dmac_wait_completion.py [max_seconds]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

DMAC_WAIT_RETURN = 0x200b5f34   # FUN_200b5ea4's real return -- only reached post-completion
OVERFLOW_TRAP = 0x200b93fc       # safety-net canary, already fixed but cheap to watch for


def main():
    max_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 900.0

    if not RIIC_IMAGE.exists():
        print(f"missing {RIIC_IMAGE} -- run tools/build_riic_eeprom_image.py first",
              file=sys.stderr)
        sys.exit(1)

    proc = launch_qemu(["-global", f"rza1h-riic.image={RIIC_IMAGE}"])
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, f"not a fresh boot, PC={regs['r15']:#x}"

        g.set_breakpoint(DMAC_WAIT_RETURN)
        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        print(f"free-running, watching only for DMAC_WAIT_RETURN ({DMAC_WAIT_RETURN:#x}) "
              f"or OVERFLOW_TRAP -- up to {max_seconds:.0f}s, zero interference in between")
        try:
            g.wait_stop(timeout=max_seconds)
        except (TimeoutError, OSError):
            elapsed = time.time() - start
            print(f"\nt={elapsed:.1f}s  TIMEOUT -- neither breakpoint fired within "
                  f"{max_seconds:.0f}s real time. Genuinely inconclusive on its own (no "
                  "principled upper bound on 'how slow is too slow'), but this is now the "
                  "longest completely un-instrumented free run tried on this specific wait.")
            return

        regs = g.read_registers()
        pc = regs["r15"]
        elapsed = time.time() - start
        if pc == DMAC_WAIT_RETURN:
            print(f"\nt={elapsed:.1f}s  *** DMAC_WAIT_RETURN hit *** -- the wait genuinely "
                  f"completed. r0={regs['r0']:#x} (post-loop value)  lr={regs['r14']:#x}")
            print("-> NOT a permanent stall -- it just needed real wall-clock time. Confirms "
                  "the disproportionate-real-time-cost picture from the 'why minutes not "
                  "seconds' discussion, not a logic bug in dmac.c or the guest code.")
        elif pc == OVERFLOW_TRAP:
            print(f"\nt={elapsed:.1f}s  *** OVERFLOW TRAP hit instead *** -- boot got past "
                  "the DMAC wait but hit the (previously-fixed) ring overflow again; r0="
                  f"{regs['r0']:#x} lr={regs['r14']:#x}. Worth a closer look on its own.")
        else:
            print(f"\nt={elapsed:.1f}s  unexpected stop at pc={pc:#010x}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
