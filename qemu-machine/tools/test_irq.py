#!/usr/bin/env python3
"""Force-arm OSTM0 and check whether real firmware's own idle/WFE loop
actually gets woken by a real GIC-delivered interrupt and diverted
somewhere else -- qemu-machine/README.md's Extension-roadmap item 1's
actual motivating question, now testable via the raw-RSP driver in
gdbrsp.py instead of gdb's own (unreliable, in this project's earlier
sessions) Python API.

Context, confirmed 2026-09-08 by disassembling the real stop point
(qemu-machine/README.md's WFE address, 0x200b93ac) directly out of a live
GDB memory dump:

    200b939c: dsb sy
    200b93a0: sev
    200b93a4: wfe
    200b93a8: wfe
    200b93ac: b 0x200b939c

This is an *unconditional* loop -- no flag check anywhere in it. That
means it isn't "wait until some condition becomes true"; it's a generic
power-saving idle spin (almost certainly FreeRTOS's own idle task). The
only way execution ever leaves this 5-instruction span is if an interrupt
fires *and* its handler diverts the return elsewhere -- e.g. a scheduler
tick ISR performing a context switch by rewriting the saved return context
on the stack before returning. So "does PC end up somewhere outside
0x200b939c-0x200b93ac after we arm a timer IRQ" is a clean, strong test of
this project's actual open question (real GIC IRQ delivery -> real ISR ->
real return-to-different-context all working), independent of whether
OSTM0 is literally the tick source `body.bin` itself arms (a separate RE
question, qemu-machine/README.md's Extension-roadmap item 2).

Usage: run against an already-running `rz-a1h` QEMU instance stopped (or
running -- this interrupts it itself) with `-gdb tcp::1234`.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdbrsp import GdbRsp

WFE_LOOP_LO = 0x200b939c
WFE_LOOP_HI = 0x200b93ac

OSTM0_BASE = 0xFCFEC000
OSTM_CMP = OSTM0_BASE + 0x00
OSTM_TS = OSTM0_BASE + 0x14

# GICv2 distributor/CPU-interface registers (rz_a1h.h's RZA1H_GIC_DIST_BASE/
# RZA1H_GIC_CPU_BASE). body.bin's own boot code is known to touch the GIC
# (gic_distributor_disable/FUN_200b848c, from the Unicorn-side tracing) but
# whether it ever *enables* ID 134 (OSTM0's line, since OSTM0 isn't the
# tick source body.bin actually arms -- roadmap item 2, still open) is a
# separate question from "can this QEMU model deliver an IRQ at all", so
# this test arms the GIC itself rather than assuming firmware already did.
GIC_DIST_BASE = 0xE8201000
GIC_CPU_BASE = 0xE8202000
GICD_CTLR = GIC_DIST_BASE + 0x000
GICD_ISENABLER4 = GIC_DIST_BASE + 0x100 + 4 * 4  # covers IDs 128-159
GICC_CTLR = GIC_CPU_BASE + 0x000
GICC_PMR = GIC_CPU_BASE + 0x004
GICD_ITARGETSR_134 = GIC_DIST_BASE + 0x800 + 134  # byte-per-SPI target-CPU mask
GICD_ISPENDR4 = GIC_DIST_BASE + 0x200 + 4 * 4  # covers IDs 128-159
# ICFGR: 2 bits/interrupt, 16 interrupts/register -- offset for ID134's
# register is 0xc00 + (134/16)*4 = 0xc20 (covers IDs 128-143). QEMU's
# arm_gic (hw/intc/arm_gic.c gic_set_irq_generic) only marks a level-
# sensitive SPI *pending* on an edge write (GIC_DIST_TEST_EDGE_TRIGGER) --
# a level line that's pulsed high-then-low within one call (our OSTM
# device's qemu_irq_pulse) never latches at all otherwise, since nothing
# samples "level" again after the model drops it back to 0.
GICD_ICFGR_134_WORD = GIC_DIST_BASE + 0xc20
OSTM0_IRQ_ID = 134  # ostm.c's file comment; bit (134 % 32) = 6 in ISENABLER4

# 5,000,000 counts at ostm.c's OSTM_FREQ_HZ (500 MHz) = 10ms real time.
ARM_CMP = 5_000_000


def main():
    g = GdbRsp(port=1234)

    # Make sure we're actually stopped and see where we are. Query status
    # first (always answered) rather than interrupt() (only answered if
    # running). If not yet in the idle loop, run forward until it is --
    # earlier sessions found ~1.5s of real time reliably gets a fresh boot
    # there.
    print("stop:", g.status())
    regs = g.read_registers()
    pc = regs["r15"]
    print(f"PC={pc:08x} CPSR={regs['cpsr']:08x}")

    if not (WFE_LOOP_LO <= pc <= WFE_LOOP_HI):
        print("not yet in the idle loop -- running forward ~1.5s...")
        g.cont()
        time.sleep(1.5)
        g.interrupt()
        print("stop:", g.wait_stop())
        regs = g.read_registers()
        pc = regs["r15"]
        print(f"PC={pc:08x} CPSR={regs['cpsr']:08x}")
        if not (WFE_LOOP_LO <= pc <= WFE_LOOP_HI):
            print("NOTE: still not in the idle loop -- continuing anyway, "
                  "but this run's baseline is weaker.")

    # Manually configure the GIC to actually forward OSTM0's line -- this
    # is a genuinely separate question from whether body.bin itself ever
    # arms it (roadmap item 2, still open): without GICD_ISENABLERn's bit
    # set for ID 134, the distributor never forwards the line to the CPU
    # at all regardless of the CPU's own I-bit, so a real-firmware-arms-it
    # test and a "can this QEMU model deliver an IRQ at all" test need to
    # be kept separate. This is the latter.
    print("configuring GIC: enabling distributor, CPU interface, ID 134, "
          "priority mask 0xff, SPI target-CPU mask")
    g.write_u32(GICD_CTLR, 1)
    g.write_u32(GICD_ISENABLER4, 1 << (OSTM0_IRQ_ID % 32))
    g.write_u32(GICC_CTLR, 1)
    g.write_u32(GICC_PMR, 0xFF)
    # SPIs (ID >= 32) additionally need a target-CPU mask in GICD_ITARGETSRn
    # (byte-per-interrupt) or the distributor latches them as pending but
    # never forwards them to any CPU interface -- defaults to 0 (no
    # targets) at reset on real GICv2, unlikely body.bin's own tick source
    # (whatever it turns out to be, roadmap item 2) skips this either.
    g.write_memory(GICD_ITARGETSR_134, bytes([0x01]))
    # Mark every interrupt in ID134's ICFGR word as edge-triggered (blasting
    # the whole word rather than hand-deriving the sub-field bit offset --
    # a scratch test, not production config; harmless to also mark 128-143
    # as edge).
    g.write_u32(GICD_ICFGR_134_WORD, 0xFFFFFFFF)

    print(f"arming OSTM0: CMP={ARM_CMP} (~{ARM_CMP / 500_000_000 * 1000:.1f}ms), "
          f"writing CMP@{OSTM_CMP:08x} then TS@{OSTM_TS:08x}")
    g.write_u32(OSTM_CMP, ARM_CMP)
    g.write_u32(OSTM_TS, 1)  # any value starts the timer, see ostm.c

    print("continuing...")
    g.cont()
    time.sleep(0.3)  # generous margin over the ~10ms period
    g.interrupt()
    print("stop:", g.wait_stop())
    regs2 = g.read_registers()
    pc2 = regs2["r15"]
    print(f"PC={pc2:08x} CPSR={regs2['cpsr']:08x}")
    cnt = g.read_u32(OSTM0_BASE + 0x04)
    pend = g.read_u32(GICD_ISPENDR4)
    print(f"OSTM0.CNT={cnt} (CMP was {ARM_CMP}) GICD_ISPENDR4={pend:08x} "
          f"(bit {OSTM0_IRQ_ID % 32} {'SET' if pend & (1 << (OSTM0_IRQ_ID % 32)) else 'clear'})")

    if WFE_LOOP_LO <= pc2 <= WFE_LOOP_HI:
        print("RESULT: still inside the idle loop -- IRQ either wasn't "
              "delivered, wasn't taken, or its handler returned to the "
              "same context without a switch.")
    else:
        print("RESULT: PC left the idle loop -- a real GIC IRQ was "
              "delivered, taken, and its handler diverted execution "
              "elsewhere (consistent with a real scheduler-tick context "
              "switch). This is the migration's motivating question, "
              "answered.")

    g.close()


if __name__ == "__main__":
    main()
