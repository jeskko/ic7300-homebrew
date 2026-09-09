#!/usr/bin/env python3
"""The missing piece after trace_irq_mask_window.py's own correction (README-history.md's
newest section, 2026-09-09): the drain trigger (`irq_context_switch_id0`) fires reliably
(~every 200ms) right up to an overflow, so starvation isn't the cause -- the open question is
which call site(s) actually push the ~16 entries that land inside one such gap.

Breakpointing the producer (`FUN_20187bb4`, 0x20187bb4) for the *entire* boot reproduces the
exact masking problem already hit twice this thread (a per-push breakpoint desynchronizes
producer and consumer enough to prevent the overflow from happening at all). This script avoids
that by combining the two techniques already proven safe this session:
  - Cheap, non-invasive wall-clock polling of the ring's own `pending` byte (proven not to
    perturb the outcome, unlike per-call breakpoints) to *detect* the run-up.
  - Reactively arming the producer breakpoint only once `pending` crosses a low threshold
    (default 4) -- so the breakpoint is live for at most a couple hundred milliseconds around
    the actual burst, not the whole 60+ second boot, the same trick that let
    trace_irq_mask_window.py count id0 hits without masking anything.

Each hit while armed logs elapsed time, `pending`, LR (breakpointing `FUN_20186c4c`, the
broadcast dispatcher, one level above the actual ring-push helper `FUN_20187bb4` -- the latter
has exactly one static caller so its own LR is always identical and useless for telling sources
apart; `FUN_20186c4c`'s LR is its *true* caller's return address), plus r0/r1 (payload and
channel number, `FUN_20186c4c`'s own two parameters) -- together enough to tell "one call site
looping" from "many different call sites contributing" apart, and to see which channel(s) are
involved.

Usage: trace_job_ring_producer.py [seconds] [arm_threshold] [poll_interval]
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
RING_PENDING = RING_BASE + 2
# Breakpointing FUN_20186c4c (the broadcast dispatcher), not FUN_20187bb4 (the actual ring
# push one level down): FUN_20187bb4 has exactly one static caller, so its own LR is always
# the same fixed address and useless for telling different sources apart. FUN_20186c4c's own
# LR is its *true* caller's return address -- r0=payload, r1=channel number here too.
PRODUCER_ADDR = 0x20186c4c
OVERFLOW_TRAP = 0x200b93fc


def launch_qemu() -> subprocess.Popen:
    return subprocess.Popen(
        [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
         "-serial", "none", "-monitor", "none", "-S", "-gdb", "tcp::1234",
         "-global", f"rza1h-riic.image={RIIC_IMAGE}"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
    arm_threshold = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    poll_interval = float(sys.argv[3]) if len(sys.argv) > 3 else 0.1

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
        producer_armed = False
        producer_hits = []
        overflowed = False
        last_pending = None

        while time.time() - start < total_seconds:
            time.sleep(poll_interval)
            g.interrupt()
            stop = g.wait_stop()
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start

            if pc == OVERFLOW_TRAP:
                print(f"t={elapsed:7.3f}s  *** OVERFLOW *** r0(err)={regs['r0']:#x} "
                      f"lr={regs['r14']:#x}")
                overflowed = True
                break

            if pc == PRODUCER_ADDR and producer_armed:
                pending = g.read_memory(RING_PENDING, 1)[0]
                lr = regs["r14"]
                r0 = regs["r0"]
                r1 = regs["r1"]
                producer_hits.append((elapsed, pending, lr, r0, r1))
                print(f"t={elapsed:7.3f}s  PUSH  pending={pending:3d}  caller_lr={lr:#010x}  "
                      f"r0(payload)={r0:#x}  r1(channel)={r1:#x}")
                g.remove_breakpoint(pc)
                g.step()
                g.set_breakpoint(pc)
                g.cont()
                continue

            # A plain wall-clock poll stop -- inspect pending, arm/disarm the producer
            # breakpoint reactively.
            pending = g.read_memory(RING_PENDING, 1)[0]
            if pending != last_pending:
                print(f"t={elapsed:7.3f}s  poll  pending={pending}  pc={pc:#010x}")
                last_pending = pending

            if not producer_armed and pending >= arm_threshold:
                g.set_breakpoint(PRODUCER_ADDR)
                producer_armed = True
                print(f"t={elapsed:7.3f}s  *** armed producer watch (pending={pending}) ***")
            elif producer_armed and pending == 0:
                g.remove_breakpoint(PRODUCER_ADDR)
                producer_armed = False
                print(f"t={elapsed:7.3f}s  (disarmed producer watch, ring drained, "
                      f"{len(producer_hits)} pushes seen)")

            g.cont()

        if not overflowed:
            print(f"\nno overflow in {total_seconds:.0f}s -- {len(producer_hits)} producer "
                  "pushes captured while armed, if any are printed above they're still useful")
        else:
            print(f"\n{len(producer_hits)} producer pushes captured during the run-up.")
            print("Distinct caller LRs:", sorted({h[2] for h in producer_hits}))
            print("Distinct channel numbers (r1):", sorted({h[4] for h in producer_hits}))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
