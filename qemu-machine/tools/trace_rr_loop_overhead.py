#!/usr/bin/env python3
"""Correlates the round-robin main-loop trace (patches/rr-loop-trace.patch, RZA1H_RR_TRACE=1)
against riic.c's own permanent per-event instrumentation (RZA1H_DEBUG=riic, the icount_get_raw()/
host-microsecond-timestamp log added 2026-09-10 in riic_schedule_irq_delay()) to localize the
real, measured ~65-90us-per-scheduled-event host wall-clock cost to a specific piece of QEMU's
round-robin loop -- see README.md's Status section for the full background and why this matters
(the same overhead will very likely resurface once mmc.c gets real SD-bus-speed pacing, since it
currently has none).

Prerequisites (not automatic -- run once per session):
    qemu-machine/tools/apply_rr_loop_trace.sh    # applies patches/rr-loop-trace.patch, rebuilds

Usage: trace_rr_loop_overhead.py [seconds] [icount_value]

`icount_value` (2026-09-10 addition, for the "does shift=auto's own adaptive retuning add
per-pass overhead" question -- see README.md's Status section) overrides the `-icount` argument,
default "shift=auto". Pass e.g. "shift=7" to compare against a fixed shift. Per
accel/tcg/icount-common.c (read directly, not guessed): auto-mode's own re-adjustment
(icount_adjust()) only runs from two timers firing every 100ms/1000ms -- far too infrequent to
plausibly explain a per-pass (~us-scale, thousands/sec) cost -- so this is expected to show no
difference, but checked rather than assumed, matching this project's own methodology.

Reads both RZA1H_RR_TRACE and RZA1H_DEBUG=riic output from the same free-running boot (both are
host-side, GDB-free, zero perturbation -- safe to combine), merges by the shared host monotonic
clock, and reports, per outer-loop iteration, how long each checkpoint-to-checkpoint span took:
    loop_top -> after_wait_io           (rr_wait_io_event() + rr_deal_with_unplugged_cpus())
    after_wait_io -> after_relock       (bql_unlock/replay_mutex_lock/bql_lock -- the "lock shuffle")
    after_relock -> after_icount_bookkeeping   (icount_account_warp_timer/icount_handle_deadline)
    after_icount_bookkeeping -> before_tcg_cpu_exec  (inner-loop setup, icount_prepare_for_run)
    before_tcg_cpu_exec -> after_tcg_cpu_exec  (the actual guest-execution slice)
Correlates each such iteration against the nearest riic.c "schedule irq=..." log line (by host
timestamp) to see which RIIC2 phase transition it corresponds to, if any.
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"
LOG = Path("/tmp/rr_loop_overhead.log")

RR_RE = re.compile(r"\[rrtrace\] t=(\d+)us (\S+)")
RIIC_RE = re.compile(
    r"riic\d: schedule irq=(-?\d+) delay_ns=(\d+) icount_raw=(\d+) host_us=(\d+)")

CHECKPOINTS = ["loop_top", "after_wait_io", "after_relock", "after_icount_bookkeeping",
               "before_tcg_cpu_exec", "after_tcg_cpu_exec"]


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
    icount_value = sys.argv[2] if len(sys.argv) > 2 else "shift=auto"

    if not QEMU.exists():
        sys.exit(f"{QEMU} not found -- run setup.sh first")

    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", icount_value,
    ]
    env = {"RZA1H_DEBUG": "riic", "RZA1H_RR_TRACE": "1"}

    with open(LOG, "w") as logf:
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=logf, env=env)
        try:
            time.sleep(seconds)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    rr_events = []   # (host_us, checkpoint)
    riic_events = []  # (host_us, irq_idx, delay_ns, icount_raw)
    n_rr_lines = 0
    for line in LOG.read_text(errors="replace").splitlines():
        m = RR_RE.search(line)
        if m:
            n_rr_lines += 1
            rr_events.append((int(m.group(1)), m.group(2)))
            continue
        m = RIIC_RE.search(line)
        if m:
            riic_events.append((int(m.group(4)), int(m.group(1)), int(m.group(2)), int(m.group(3))))

    print(f"-icount {icount_value}")
    print(f"{n_rr_lines} rrtrace lines, {len(riic_events)} riic schedule-irq lines\n")
    if not rr_events:
        print("No [rrtrace] lines found -- did you run tools/apply_rr_loop_trace.sh first, "
              "and is RZA1H_RR_TRACE=1 set? (this script sets it automatically, so check the "
              "patch is actually applied: qemu-src/accel/tcg/tcg-accel-ops-rr.c should contain "
              "rza1h_rr_trace calls.)")
        return

    # Group consecutive rr_events into "loop iterations" -- one iteration is a run through
    # CHECKPOINTS in order; a shorter run (e.g. no tcg_cpu_exec this time round, or WFI idle)
    # is also valid, just fewer checkpoints hit.
    span_totals = defaultdict(list)  # (from_cp, to_cp) -> [duration_us, ...]
    i = 0
    n = len(rr_events)
    while i < n - 1:
        t0, cp0 = rr_events[i]
        t1, cp1 = rr_events[i + 1]
        span_totals[(cp0, cp1)].append(t1 - t0)
        i += 1

    print("Checkpoint-to-checkpoint span statistics (all consecutive pairs seen, us):")
    print(f"{'span':45s} {'n':>7s} {'mean':>9s} {'median':>9s} {'min':>7s} {'max':>9s}")
    import statistics
    for (cp0, cp1), durs in span_totals.items():
        label = f"{cp0} -> {cp1}"
        print(f"{label:45s} {len(durs):7d} {statistics.mean(durs):9.2f} "
              f"{statistics.median(durs):9.1f} {min(durs):7d} {max(durs):9d}")

    # Full-iteration total: loop_top -> next loop_top
    loop_tops = [(t, cp) for t, cp in rr_events if cp == "loop_top"]
    if len(loop_tops) > 1:
        iter_totals = [loop_tops[i + 1][0] - loop_tops[i][0] for i in range(len(loop_tops) - 1)]
        print(f"\nFull outer-loop iteration (loop_top -> next loop_top), us: "
              f"n={len(iter_totals)} mean={statistics.mean(iter_totals):.2f} "
              f"median={statistics.median(iter_totals)} min={min(iter_totals)} "
              f"max={max(iter_totals)}")

    print(f"\nFirst 40 rrtrace events (t_us, checkpoint):")
    for t, cp in rr_events[:40]:
        print(f"  t={t:14d}us  {cp}")


if __name__ == "__main__":
    main()
