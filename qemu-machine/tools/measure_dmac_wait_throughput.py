#!/usr/bin/env python3
"""Tests a new hypothesis for the DMAC/icount stall (README.md's Status section, 2026-09-09):
not a deadlock, but MMIO-dispatch overhead making this *specific* busy-wait's real per-
instruction cost disproportionately high relative to ordinary code -- meaning `-icount`'s
adaptive tuner (capped at MAX_ICOUNT_SHIFT=10, confirmed from qemu-src/accel/tcg/icount-
common.c) may not be able to credit enough virtual-ns/instruction to keep this region's
guest-perceived time honestly tracking real elapsed time, so it just needs far more real
wall-clock time than any trial so far has run long enough to see through -- not a bug.

Method: single-step timing (gdbrsp's `step()` blocks until the instruction completes, so
wall-clock around N consecutive steps directly captures real per-instruction cost including
any device-model dispatch). Measured at two points close together in boot time (controlling
for the icount shift's own state at that point in boot, so only the *code region* differs):
  (a) right after DMAC_ISR (0x200b5b90) is entered -- ordinary ISR code, not a tight poll.
  (b) right after DMAC_WAIT_LOOP (0x200b5f28) is first hit -- the tight MMIO-polling loop.

Usage: measure_dmac_wait_throughput.py [n_steps]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

DMAC_ISR = 0x200b5b90
DMAC_WAIT_LOOP = 0x200b5f28


def time_n_steps(g: GdbRsp, n: int) -> tuple[float, list[int]]:
    """Single-step n times, returning (total real seconds, list of PCs visited)."""
    pcs = []
    start = time.time()
    for _ in range(n):
        g.step()
        pcs.append(g.read_registers()["r15"])
    return time.time() - start, pcs


def main():
    n_steps = int(sys.argv[1]) if len(sys.argv) > 1 else 30

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

        g.set_breakpoint(DMAC_ISR)
        g.set_breakpoint(DMAC_WAIT_LOOP)
        g.cont()

        # --- Catch DMAC_ISR, measure N steps of ordinary ISR code right there ---
        g.wait_stop(timeout=30.0)
        regs = g.read_registers()
        assert regs["r15"] == DMAC_ISR, f"expected DMAC_ISR, got pc={regs['r15']:#x}"
        print(f"DMAC_ISR hit -- measuring {n_steps} single-steps of ordinary ISR code...")
        g.remove_breakpoint(DMAC_ISR)
        isr_elapsed, isr_pcs = time_n_steps(g, n_steps)
        isr_span = f"{min(isr_pcs):#x}-{max(isr_pcs):#x}"
        print(f"  ISR region: {n_steps} steps in {isr_elapsed:.4f}s real "
              f"({isr_elapsed / n_steps * 1e6:.1f} us/step)  PC span={isr_span}")
        g.set_breakpoint(DMAC_ISR)  # restore, harmless if never hit again
        g.cont()

        # --- Catch DMAC_WAIT_LOOP, measure N steps of the tight MMIO-poll loop ---
        g.wait_stop(timeout=30.0)
        regs = g.read_registers()
        assert regs["r15"] == DMAC_WAIT_LOOP, f"expected DMAC_WAIT_LOOP, got pc={regs['r15']:#x}"
        print(f"DMAC_WAIT_LOOP hit -- measuring {n_steps} single-steps of the poll loop...")
        g.remove_breakpoint(DMAC_WAIT_LOOP)
        wait_elapsed, wait_pcs = time_n_steps(g, n_steps)
        wait_span = f"{min(wait_pcs):#x}-{max(wait_pcs):#x}"
        print(f"  WAIT region: {n_steps} steps in {wait_elapsed:.4f}s real "
              f"({wait_elapsed / n_steps * 1e6:.1f} us/step)  PC span={wait_span}")

        ratio = (wait_elapsed / n_steps) / (isr_elapsed / n_steps) if isr_elapsed else float("inf")
        print(f"\nratio (wait-loop us/step) / (ISR us/step) = {ratio:.2f}x")
        if ratio > 3.0:
            print("-> DMAC_WAIT_LOOP's per-instruction real cost is dramatically higher "
                  "than ordinary ISR code under the identical measurement harness -- "
                  "supports the MMIO-dispatch-overhead-saturation hypothesis (this specific "
                  "busy-wait may just need far more real wall-clock time, not be stuck).")
        else:
            print("-> No dramatic per-instruction cost difference found -- the disproportionate-"
                  "throttling hypothesis is NOT supported by this measurement; something else "
                  "(a genuine deadlock/logic bug) remains the more likely explanation.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
