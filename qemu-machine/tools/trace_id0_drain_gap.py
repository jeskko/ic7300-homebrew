#!/usr/bin/env python3
"""Concrete next step from the 2026-09-10 ring-producer finding (README-history.md's newest
section): `mtu2_ch3_periodic_housekeeping_tick` posts a perfectly steady ~82ms doorbell the
whole boot, so the ring (0x20420120) only overflows once its consumer
(`irq_context_switch_id0`) stops keeping pace with it -- somewhere around the ~27s mark in this
session's trials. This directly tests that, rather than assuming it: breakpoints
`irq_context_switch_id0`'s own entry (0x20005960) and the overflow trap (0x200b93fc) together,
recording every id0 hit's timestamp, then reports the interval *distribution* -- in particular
whether the last few intervals before the overflow are visibly larger than the steady-state
median, which would directly confirm a real drain-side gap (as opposed to, say, a sudden change
in the *producer* rate instead).

Same low-perturbation category this project has already used safely for this exact question
(README-history.md's "id0-hit-counting technique", used previously to count 142 real hits over
30s without masking the bug it was checking) -- id0 fires at a real, moderate, boot-scheduling-
driven rate (not a tight, no-progress loop), so a plain low-overhead breakpoint here is expected
to behave like the safe "rare/moderate event" category this project's own methodology
distinguishes from a hot, no-forward-progress loop -- but the interval log itself is the check:
a suspiciously exact, unvarying cadence (this project's own known gdbstub-artifact signature,
see trace_dmac_race.py's retraction) would show up directly in the numbers below rather than
being silently trusted.

Usage: trace_id0_drain_gap.py [max_seconds]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

IRQ_CONTEXT_SWITCH_ID0 = 0x20005960
OVERFLOW_TRAP = 0x200b93fc
RING_BASE = 0x20420120


def main():
    max_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

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

        g.set_breakpoint(IRQ_CONTEXT_SWITCH_ID0)
        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        id0_hits = []
        overflowed = False
        while time.time() - start < max_seconds:
            try:
                g.wait_stop(timeout=max_seconds - (time.time() - start))
            except (TimeoutError, OSError):
                print(f"t={time.time()-start:.1f}s  TIMEOUT, {len(id0_hits)} id0 hits so far, "
                      "no overflow")
                break
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            if pc == OVERFLOW_TRAP:
                pending = g.read_memory(RING_BASE + 2, 1)[0]
                print(f"\nt={elapsed:7.3f}s  *** OVERFLOW TRAP *** after {len(id0_hits)} id0 "
                      f"hits (ring pending={pending})")
                overflowed = True
                break
            id0_hits.append(elapsed)
            g.cont()

        if len(id0_hits) >= 2:
            intervals = [b - a for a, b in zip(id0_hits, id0_hits[1:])]
            n = len(intervals)
            median = sorted(intervals)[n // 2]
            print(f"\n{len(id0_hits)} id0 hits, {n} intervals, median={median*1000:.1f}ms, "
                  f"min={min(intervals)*1000:.1f}ms, max={max(intervals)*1000:.1f}ms")
            tail = intervals[-15:]
            print("last 15 intervals before " +
                  ("the overflow" if overflowed else "timeout") +
                  " (ms): " + ", ".join(f"{x*1000:.1f}" for x in tail))
            big = [(i, x) for i, x in enumerate(intervals) if x > median * 3]
            if big:
                print(f"\n{len(big)} interval(s) > 3x median (real drain gaps, not just jitter):")
                for i, x in big[-10:]:
                    print(f"  gap at id0-hit-index {i}->{i+1}, t~={id0_hits[i]:.2f}s, "
                          f"interval={x*1000:.1f}ms")
            else:
                print("\nno interval > 3x median -- id0 kept a steady rhythm right up to the "
                      "overflow, no visible drain-side gap; look elsewhere (producer rate "
                      "change, or the consumer's own dispatch taking longer per hit rather "
                      "than firing less often)")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
