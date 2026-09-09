#!/usr/bin/env python3
"""Targeted trace for the qemu-machine/ README.md active resume point (2026-09-09):
root-cause the 0x20420120 shared job-ring's own overflow.

Static re-derivation this session (Ghidra, before writing this script) pinned down the
mechanism fully:
  - 0x20420120 is a single global work-queue struct: byte 0 = write index (producer-owned),
    byte 1 = read index (consumer-owned), byte 2 = pending count (LDREX/STREX-atomic,
    incremented by the producer / decremented by the consumer), byte 3 = capacity (16),
    then 16 * 8-byte entries (4-byte job-object pointer + 4-byte payload word) starting at
    offset 4 -- matches the prior session's own live-polled "capacity 16" observation exactly.
  - Producer: FUN_20187bb4, called from FUN_20186c4c (the "broadcast to N subscriber
    channels" dispatcher) -- confirmed via its own call site at 0x20186c62 returning to
    0x20186c66/LR=0x20186c67, the *exact* LR the prior session's push-helper breakpoint
    already caught (their belief it was feeding a "different ring", 0x20415c60, was a
    misread: that value is r0/param_1, an opaque per-channel job-object pointer stored as
    *data* in the queue entry, not a destination ring -- every call through this path
    always targets this one queue).
  - Consumer: FUN_20187ae4, drains the queue while pending != 0, called from
    irq_context_switch_id0 (0x20005960) -- an existing, already-named, already-resolved
    (2026-08-30, notes/kernel-rtos.md) real hardware/software IRQ-triggered FreeRTOS
    context-switch handler, GIC ID 0. So the queue only ever drains on a context-switch
    IRQ, not on a dedicated timer/doorbell of its own.
  - Overflow trap: FUN_200b93fc (unconditional infinite loop, r0 = caller's error code --
    2 from FUN_20187bb4, 3 from FUN_201877e4's own *different*, unrelated per-object ring
    shape found while tracing this, not otherwise relevant here).

This points at a genuine *scheduling-gap* hypothesis (IRQ ID 0 not firing often enough
right before the overflow), not a data race in the queue's own LDREX/STREX bookkeeping --
but per this project's own hard-won methodology, that must be confirmed live, not asserted
from decompiled code alone. The prior session's own targeted-producer breakpoint already
demonstrated that a per-push-call breakpoint changes timing enough to hide the bug
entirely (masks it into "zero overflows, 1092 hits, 90s"), and a per-header-write
watchpoint would very likely do the same (same call frequency). So this script
deliberately avoids breakpointing/watchpointing anything on the hot push/drain path --
it free-runs the whole trial and only pauses briefly, on a fixed wall-clock cadence
(not tied to any specific guest code path), to sample the queue header -- the same
"poll, don't break" technique this project has repeatedly found is the low-perturbation
option. A single breakpoint on the overflow trap itself (FUN_200b93fc) is included since
it only ever fires once, at the moment of genuine interest, and stops everything anyway.

Usage: trace_job_ring_overflow.py [seconds] [poll_interval_s]
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"

RING_BASE = 0x20420120
RING_WRITE_IDX = RING_BASE + 0   # producer-owned
RING_READ_IDX = RING_BASE + 1    # consumer-owned
RING_PENDING = RING_BASE + 2     # atomic, both sides
RING_CAPACITY = RING_BASE + 3    # constant (16)

OVERFLOW_TRAP = 0x200b93fc


import os

DMAC_DEBUG_LOG = os.environ.get("DMAC_DEBUG_LOG")
ICOUNT = os.environ.get("ICOUNT")  # e.g. "shift=auto" -- see README-history.md's clock-realism
                                    # thread, 2026-09-09, for why this is being tried


def launch_qemu() -> subprocess.Popen:
    stderr_target = open(DMAC_DEBUG_LOG, "w") if DMAC_DEBUG_LOG else subprocess.DEVNULL
    args = [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
            "-serial", "none", "-monitor", "none", "-S", "-gdb", "tcp::1234",
            "-global", f"rza1h-riic.image={RIIC_IMAGE}"]
    if ICOUNT:
        args += ["-icount", ICOUNT]
    return subprocess.Popen(
        args,
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=stderr_target,
    )


def read_ring_header(g: GdbRsp) -> dict:
    raw = g.read_memory(RING_BASE, 4)
    return {
        "write_idx": raw[0],
        "read_idx": raw[1],
        "pending": raw[2],
        "capacity": raw[3],
    }


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.25
    fine_start = float(sys.argv[3]) if len(sys.argv) > 3 else None
    fine_interval = float(sys.argv[4]) if len(sys.argv) > 4 else 0.02

    if not RIIC_IMAGE.exists():
        print(f"missing {RIIC_IMAGE} -- run tools/build_riic_eeprom_image.py first",
              file=sys.stderr)
        sys.exit(1)

    proc = launch_qemu()
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, f"not a fresh boot, PC={regs['r15']:#x}"

        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        last_pending = None
        last_heartbeat = -1
        overflowed = False
        samples = 0
        while time.time() - start < total_seconds:
            elapsed_before = time.time() - start
            cur_interval = (fine_interval if fine_start is not None and elapsed_before >= fine_start
                             else poll_interval)
            time.sleep(cur_interval)
            g.interrupt()
            stop = g.wait_stop()
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start

            if pc == OVERFLOW_TRAP:
                hdr = read_ring_header(g)
                print(f"t={elapsed:6.2f}s  *** OVERFLOW TRAP HIT ***  "
                      f"r0(err)={regs['r0']:#x}  lr={regs['r14']:#x}  "
                      f"header={hdr}")
                overflowed = True
                break

            hdr = read_ring_header(g)
            samples += 1
            in_fine = fine_start is not None and elapsed_before >= fine_start
            heartbeat = int(elapsed) % 15 == 0 and int(elapsed) != int(last_heartbeat)
            if hdr["pending"] != last_pending or in_fine or heartbeat:
                tag = "  (heartbeat, no pending change)" if heartbeat and hdr["pending"] == last_pending else ""
                print(f"t={elapsed:6.3f}s  pc={pc:#010x} lr={regs['r14']:#x}  "
                      f"header={hdr}  (stop={stop!r}){tag}")
                last_pending = hdr["pending"]
                if heartbeat:
                    last_heartbeat = elapsed

            g.cont()

        if not overflowed:
            print(f"\nno overflow in {total_seconds:.0f}s ({samples} samples, "
                  f"poll_interval={poll_interval}s) -- either it needs longer, or "
                  "the polling cadence itself is still perturbing timing enough to "
                  "avoid it; try a longer run or a coarser poll_interval before "
                  "concluding anything")
        else:
            print("\nconfirmed: overflow reproduced under wall-clock polling "
                  "(not a per-push/per-header-write breakpoint) -- see the last "
                  "few header samples above for the run-up shape")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
