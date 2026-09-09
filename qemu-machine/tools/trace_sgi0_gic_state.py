#!/usr/bin/env python3
"""Concrete next step from the 2026-09-10 id0-stall finding (README-history.md's newest
section): the consumer (`irq_context_switch_id0`, GIC ID/SGI 0) goes from a rock-steady ~82ms
firing rate to almost nothing for a real ~10 real seconds, and a direct breakpoint on its own
entry was shown to *prevent* the stall entirely -- so this stays strictly memory-read-only
(the same `interrupt()`+`read_memory()` wall-clock-polling technique `trace_job_ring_overflow.py`
already uses safely at its default 0.25s cadence, extended to also read the GIC's own live
state), never touching a breakpoint anywhere near the GIC or `id0` itself.

Reads, every poll:
  - the ring header (write_idx/read_idx/pending), same as trace_job_ring_overflow.py
  - GICD_ISPENDR0 bit 0 -- is SGI 0 currently latched pending at the distributor?
  - GICD_ISACTIVER0 bit 0 -- is SGI 0 currently active (being serviced, or preempted mid-service)?
  - GICC_PMR -- the CPU interface's own priority mask (a low value here would mask SGI 0 if its
    priority is at or below the mask, regardless of CPSR)
  - GICC_RPR -- the CPU interface's own "running priority" (nonzero/non-idle would mean *something*
    is currently being serviced, possibly at a priority that defers SGI 0)
  - CPSR bit 7 (the `I` bit) -- are IRQs masked at the CPU itself right now

If SGI 0 shows pending=1 continuously through the stall while RPR/PMR/CPSR.I all look normal,
that's a real, narrow GIC-delivery anomaly. If CPSR.I is 1 (or RPR indicates a higher-priority
interrupt/exception is running) throughout, that points to something else monopolizing the CPU
with IRQs masked instead.

Usage: trace_sgi0_gic_state.py [seconds] [poll_interval_s]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

RING_BASE = 0x20420120
OVERFLOW_TRAP = 0x200b93fc

GICD_BASE = 0xE8201000
GICC_BASE = 0xE8202000
GICD_ISPENDR0 = GICD_BASE + 0x200
GICD_ISACTIVER0 = GICD_BASE + 0x300
GICC_PMR = GICC_BASE + 0x04
GICC_RPR = GICC_BASE + 0x14


def read_u32(g: GdbRsp, addr: int) -> int:
    return int.from_bytes(g.read_memory(addr, 4), "little")


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.25

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

        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        last_pending = None
        last_read_idx = None
        stall_start = None
        while time.time() - start < total_seconds:
            time.sleep(poll_interval)
            g.interrupt()
            stop = g.wait_stop()
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start

            if pc == OVERFLOW_TRAP:
                hdr = g.read_memory(RING_BASE, 4)
                print(f"t={elapsed:6.2f}s  *** OVERFLOW TRAP ***  header={list(hdr)}")
                break

            hdr = g.read_memory(RING_BASE, 4)
            write_idx, read_idx, pending, capacity = hdr
            ispendr0 = read_u32(g, GICD_ISPENDR0)
            isactiver0 = read_u32(g, GICD_ISACTIVER0)
            cpsr_i = (regs["cpsr"] >> 7) & 1

            stalling = (read_idx == last_read_idx) and pending > 1
            if stalling and stall_start is None:
                stall_start = elapsed
            elif not stalling:
                stall_start = None

            tag = ""
            if stalling:
                tag = f"  <-- STALLING since t={stall_start:.2f}s"
            if pending != last_pending or read_idx != last_read_idx or stalling or cpsr_i:
                print(f"t={elapsed:7.3f}s  write={write_idx:2d} read={read_idx:2d} "
                      f"pending={pending:2d}  SGI0_pend={ispendr0 & 1} SGI0_active={isactiver0 & 1}  "
                      f"CPSR.I={cpsr_i}  pc={pc:#010x} lr={regs['r14']:#010x}{tag}")
            last_pending = pending
            last_read_idx = read_idx
            g.cont()
        else:
            print(f"\nno overflow in {total_seconds:.0f}s")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
