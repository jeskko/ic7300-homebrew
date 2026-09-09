#!/usr/bin/env python3
"""Follow-up to trace_job_ring_overflow.py's own resume point (README-history.md's newest
section, 2026-09-09): a much sharper hypothesis than "some interrupt is dispatching a lot" --
`FUN_200605fc`/`FUN_200605e4` (the merged tail-chain Ghidra decompiles under symbol
FUN_200605fc, reached from `FUN_2005ff1c`'s two call sites) brackets ~20 MTU2-rate-limiter-gated
busy-wait retry loops between a real `cpsid i` (0x20060040) and `cpsie i` (0x200604bc) -- i.e.
IRQs, including GIC ID 0 (the job-ring's *only* drain trigger, per README-history.md's fuller
derivation), are architecturally incapable of firing for the whole span between those two
addresses. If real message-post activity happens (via direct calls, not interrupts) during that
masked window, it would accumulate with zero drain opportunity -- a clean, deterministic
explanation for the burst shape already confirmed live (near-empty to full within well under
half a second).

This script breakpoints exactly three addresses, all rare/one-shot by construction (unlike the
earlier per-push breakpoint that fired ~12/sec and masked the bug by desynchronizing producer
and consumer): the disable point, the enable point, and the ring-overflow trap
(0x200b93fc). Each hit only pauses long enough to log a timestamp and single-step past it (the
standard remove/step/re-add breakpoint dance) -- total overhead across a whole boot should be a
handful of pauses, not thousands, so it shouldn't reproduce the masking problem.

Usage: trace_irq_mask_window.py [seconds]
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

CPSID_ADDR = 0x20060040   # FUN_200605e4/fc's shared tail: cpsid i
CPSIE_ADDR = 0x200604bc   # ...cpsie i, right before return
OVERFLOW_TRAP = 0x200b93fc
IRQ0_ENTRY = 0x20005960   # irq_context_switch_id0 -- the ring's *only* drain trigger

BREAKPOINTS = {
    CPSID_ADDR: "CPSID (IRQ mask ON)",
    CPSIE_ADDR: "CPSIE (IRQ mask OFF)",
    OVERFLOW_TRAP: "OVERFLOW TRAP",
}

RING_BASE = 0x20420120


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0

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

        for addr in BREAKPOINTS:
            g.set_breakpoint(addr)
        g.cont()

        start = time.time()
        mask_start = None
        overflowed = False
        id0_armed = False
        id0_armed_at = None
        id0_hits = 0
        while time.time() - start < total_seconds:
            try:
                g.wait_stop(timeout=min(2.0, total_seconds - (time.time() - start)))
            except (TimeoutError, OSError):
                # Nothing hit in this slice -- if we've been watching id0 for a
                # while with no overflow, disarm it again (keep long-term overhead low).
                if id0_armed and time.time() - id0_armed_at > 2.0:
                    g.remove_breakpoint(IRQ0_ENTRY)
                    id0_armed = False
                    print(f"           (disarmed id0 watch, {id0_hits} hits, no overflow followed)")
                continue

            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start

            if pc == IRQ0_ENTRY:
                id0_hits += 1
                g.remove_breakpoint(pc)
                g.step()
                g.set_breakpoint(pc)
                g.cont()
                continue

            label = BREAKPOINTS.get(pc)
            if label is None:
                print(f"t={elapsed:7.3f}s  unexpected stop pc={pc:#010x}")
                g.cont()
                continue

            pending = g.read_memory(RING_BASE + 2, 1)[0]
            print(f"t={elapsed:7.3f}s  {label:<24s} pc={pc:#010x}  ring_pending={pending}")

            if pc == CPSID_ADDR:
                mask_start = elapsed
            elif pc == CPSIE_ADDR:
                if mask_start is not None:
                    print(f"           -> IRQ-masked window lasted {elapsed - mask_start:.4f}s")
                    mask_start = None
                if not id0_armed:
                    g.set_breakpoint(IRQ0_ENTRY)
                    id0_armed = True
                    id0_armed_at = time.time()
                    id0_hits = 0
                    print("           (armed id0-entry watch)")
            elif pc == OVERFLOW_TRAP:
                print(f"           *** OVERFLOW *** r0={regs['r0']:#x} lr={regs['r14']:#x} "
                      f"id0_hits_since_cpsie={id0_hits}")
                overflowed = True
                break

            g.remove_breakpoint(pc)
            g.step()
            g.set_breakpoint(pc)
            g.cont()

        if not overflowed:
            print(f"\nno overflow in {total_seconds:.0f}s -- either it needs longer, or this "
                  "trial just didn't hit it (the burst has looked probabilistic across trials "
                  "so far); the CPSID/CPSIE timings above are still useful on their own")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
