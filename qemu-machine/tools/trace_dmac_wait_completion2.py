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

LOOP1_EXIT = 0x200b5f18          # loop-1 clears (shared slot was idle) -- about to arm the transfer
DMAC_WAIT_RETURN = 0x200b5f34   # FUN_200b5ea4's real return -- only reached post-completion
OVERFLOW_TRAP = 0x200b93fc       # safety-net canary, already fixed but cheap to watch for

TARGETS = {
    LOOP1_EXIT: "LOOP1_EXIT (shared slot was free, about to arm)",
    DMAC_WAIT_RETURN: "DMAC_WAIT_RETURN (own transfer completed)",
    OVERFLOW_TRAP: "OVERFLOW_TRAP",
}


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

        for addr in TARGETS:
            g.set_breakpoint(addr)
        g.cont()

        start = time.time()
        print(f"free-running, watching LOOP1_EXIT/DMAC_WAIT_RETURN/OVERFLOW_TRAP -- up to "
              f"{max_seconds:.0f}s, zero interference in between (LOOP1_EXIT disarms after "
              "its first hit, nothing else touched)")

        loop1_seen = None
        while True:
            remaining = max_seconds - (time.time() - start)
            if remaining <= 0:
                elapsed = time.time() - start
                print(f"\nt={elapsed:.1f}s  TIMEOUT -- no further breakpoint fired within "
                      f"{max_seconds:.0f}s real time (loop1_seen={loop1_seen!r}). Genuinely "
                      "inconclusive on its own (no principled upper bound on 'how slow is too "
                      "slow'), but this is now the longest completely un-instrumented free run "
                      "tried on this specific wait.")
                return
            try:
                g.wait_stop(timeout=remaining)
            except (TimeoutError, OSError):
                continue

            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start

            if pc == LOOP1_EXIT:
                loop1_seen = elapsed
                print(f"t={elapsed:.1f}s  LOOP1_EXIT hit -- shared slot was free, transfer "
                      "about to be armed (own N0SA_0/N0DA_0/N0TB_0 write coming up) -- "
                      "disarming, won't log again")
                g.remove_breakpoint(LOOP1_EXIT)
                g.cont()
                continue

            if pc == DMAC_WAIT_RETURN:
                print(f"\nt={elapsed:.1f}s  *** DMAC_WAIT_RETURN hit *** (loop1_seen="
                      f"{loop1_seen!r}) -- the wait genuinely completed. r0={regs['r0']:#x} "
                      f"(post-loop value)  lr={regs['r14']:#x}")
                print("-> NOT a permanent stall -- it just needed real wall-clock time. Confirms "
                      "the disproportionate-real-time-cost picture from the 'why minutes not "
                      "seconds' discussion, not a logic bug in dmac.c or the guest code.")
            elif pc == OVERFLOW_TRAP:
                print(f"\nt={elapsed:.1f}s  *** OVERFLOW TRAP hit instead *** (loop1_seen="
                      f"{loop1_seen!r}) -- boot got past the DMAC wait but hit the "
                      f"(previously-fixed) ring overflow again; r0={regs['r0']:#x} "
                      f"lr={regs['r14']:#x}. Worth a closer look on its own.")
            else:
                print(f"\nt={elapsed:.1f}s  unexpected stop at pc={pc:#010x} "
                      f"(loop1_seen={loop1_seen!r})")
            return
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
