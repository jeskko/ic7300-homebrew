#!/usr/bin/env python3
"""Decisive, cheap test for the DMAC/icount stall's refined hypothesis (README.md's Status
section, README-history.md's newest section, 2026-09-09): does the *periodic wall-clock
interrupt()/cont() cycling* itself (not GDB per se, not which breakpoints are set) reproduce
the stall?

`trace_dmac_isr.py` (light: 3 static breakpoints, no periodic stop/resume) sees the DMAC
completion wait clear in well under a second. `trace_job_ring_overflow.py`-style scripts (the
ones that originally surfaced the "wall-clock-polling trial" stalls) periodically call
`g.interrupt()` (GDB break, triggers QEMU's `vm_stop()`) then `g.cont()` (`vm_start()`) on a
fixed wall-clock cadence, regardless of guest PC.

Mechanism this is testing: `-icount shift=auto`'s adaptive tuner (`icount-common.c`'s
`icount_adjust()`, fires every real 1000ms via `icount_rt_timer` on QEMU_CLOCK_VIRTUAL_RT --
a clock that keeps advancing in real time even while the vCPU is `vm_stop()`ped) compares
executed-icount-as-ns against elapsed real time. Every `interrupt()`-then-`cont()` pause burns
real wall-clock time with zero instructions executed, pushing `delta = cur_icount - cur_time`
more negative each cycle -- which the algorithm reads as "guest is falling behind" and answers
by incrementing `icount_time_shift` (more virtual ns credited per executed instruction), up to
`MAX_ICOUNT_SHIFT` (10). Enough polling cycles could ratchet shift to its ceiling well before
the DMAC wait is ever reached, changing how the ptimer's real-Hz-calibrated deadline maps onto
executed guest instructions at that point in boot -- a concrete, checkable link between
"observation method" and "does this specific busy-wait ever clear".

This script isolates the variable: same 3 breakpoints as trace_dmac_isr.py (so if a stall
happens, it's not because of a *different* breakpoint set), but adds a periodic interrupt()/
cont() cycle on a plain timer, independent of any guest PC, running the whole time. If the
stall reproduces here, periodic stop/resume cycling alone is implicated (regardless of what
address is being polled) -- if it doesn't, the polling scripts' specific MMIO reads/breakpoint
choices matter more than the cycling itself.

Usage: trace_dmac_icount_shift.py [seconds] [poll_interval_s]
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
OVERFLOW_TRAP = 0x200b93fc

BREAKPOINTS = {
    DMAC_ISR: "DMAC ISR entry",
    DMAC_WAIT_LOOP: "DMAC wait-loop check",
    OVERFLOW_TRAP: "OVERFLOW TRAP",
}


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

        for addr in BREAKPOINTS:
            g.set_breakpoint(addr)
        g.cont()

        start = time.time()
        last_poll = start
        isr_hits = 0
        wait_hits = 0
        wait_first_seen = None
        poll_count = 0
        stopped_for_wait_loop = False

        while time.time() - start < total_seconds:
            now = time.time()

            # Periodic wall-clock interrupt()/cont() cycle, independent of guest PC --
            # the one variable this script adds on top of trace_dmac_isr.py.
            if not stopped_for_wait_loop and now - last_poll >= poll_interval:
                g.interrupt()
                try:
                    stop = g.wait_stop(timeout=2.0)
                except (TimeoutError, OSError):
                    stop = "?"
                poll_count += 1
                regs = g.read_registers()
                pc = regs["r15"]
                elapsed = now - start
                label = BREAKPOINTS.get(pc)
                if label is not None:
                    # We happened to land exactly on one of our breakpoints via the
                    # interrupt -- handle it the same way the main loop below would.
                    if pc == DMAC_WAIT_LOOP:
                        wait_hits += 1
                        if wait_first_seen is None:
                            wait_first_seen = elapsed
                            print(f"t={elapsed:7.3f}s  {label} (via poll)  "
                                  f"(isr_hits={isr_hits}, poll#{poll_count}) -- disarming")
                            g.remove_breakpoint(pc)
                    elif pc == DMAC_ISR:
                        isr_hits += 1
                        print(f"t={elapsed:7.3f}s  {label} (via poll)  poll#{poll_count}")
                    elif pc == OVERFLOW_TRAP:
                        print(f"t={elapsed:7.3f}s  *** OVERFLOW TRAP *** (via poll)  poll#{poll_count}")
                        break
                elif wait_first_seen is not None:
                    # Post-disarm: log PC on every poll so we can tell "genuinely moved
                    # on" (varying/advancing PC) apart from "silently still stuck in the
                    # tight spin loop" (PC pinned in/near FUN_200b5ea4 every time).
                    in_wait_fn = 0x200b5ea4 <= pc < 0x200b5f40
                    print(f"t={elapsed:7.3f}s  post-disarm poll#{poll_count}  pc={pc:#010x}"
                          f"{'  <-- STILL IN WAIT FN' if in_wait_fn else ''}")
                last_poll = now
                g.cont()
                continue

            try:
                g.wait_stop(timeout=min(0.5, total_seconds - (time.time() - start)))
            except (TimeoutError, OSError):
                continue

            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            label = BREAKPOINTS.get(pc)

            if label is None:
                print(f"t={elapsed:7.3f}s  unexpected stop pc={pc:#010x}")
                g.cont()
                continue

            if pc == DMAC_WAIT_LOOP:
                wait_hits += 1
                if wait_first_seen is None:
                    wait_first_seen = elapsed
                    print(f"t={elapsed:7.3f}s  {label}  pc={pc:#010x}  "
                          f"(isr_hits={isr_hits}) -- disarming, won't log again")
                    g.remove_breakpoint(pc)
                g.cont()
                continue

            if pc == DMAC_ISR:
                isr_hits += 1

            print(f"t={elapsed:7.3f}s  {label}  pc={pc:#010x}  "
                  f"(isr_hits={isr_hits}, wait_hits={wait_hits})")

            if pc == OVERFLOW_TRAP:
                break

            g.remove_breakpoint(pc)
            g.step()
            g.set_breakpoint(pc)
            g.cont()

        print(f"\nfinal: DMAC ISR entered {isr_hits} time(s); wait-loop first seen at "
              f"{wait_first_seen!r}; {poll_count} periodic poll cycles completed "
              f"(interval={poll_interval}s)")
        if wait_first_seen is not None and wait_first_seen < 2.0:
            print("-> wait-loop cleared quickly EVEN WITH periodic interrupt()/cont() "
                  "cycling running -- periodic stop/resume alone does NOT reproduce the "
                  "stall; the polling scripts' specific behavior must matter more than "
                  "just cycling vm_stop()/vm_start().")
        elif wait_first_seen is None:
            print("-> wait-loop was never even reached in this window (or reached and "
                  "genuinely stalled past the end) -- consistent with periodic "
                  "interrupt()/cont() cycling alone reproducing (or contributing to) the "
                  "stall.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
