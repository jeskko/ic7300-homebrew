#!/usr/bin/env python3
"""Ground-truth check, 2026-09-10 -- the sdcard_file_rpc_dispatch_task attribution for the ring-
overflow burst was WRONG (a second retraction this session, after the SVC-dispatch one): direct
breakpoints on file_rpc_post_command, the generic FUN_20186e68 signal primitive, AND the exact
call site inside sdcard_file_rpc_dispatch_task that posts to this ring (0x200b9ddc) all showed
ZERO hits before the overflow, even though the overflow trap's own r0/lr consistently confirm
FUN_20187bb4 (this ring's real, single-producer-function per the 2026-09-09 static analysis) is
genuinely being called 16 times by something.

FUN_20186fb4 (the only real path into FUN_20186c4c -> FUN_20187bb4) has exactly 3 static
references: two direct UNCONDITIONAL_CALLs (0x2007e480, already ruled out as 0x200b9ddc was --
NOT yet checked directly, this script's job) and one COMPUTED_CALL (0x20186fb0), which is the
much more likely real path if the caller genuinely goes through the SWI(0)-mediated user-mode
trap (matching notes/kernel-rtos.md's documented "check mode, SWI(0) if user" convention) --
that path passes FUN_20186fb4's address as a runtime function pointer, not a fixed call site,
so it can't be breakpointed directly the same way; this script checks the one remaining STATIC
direct call site (0x2007e480) first, since it's cheap and rules something out either way.

Breakpoints FUN_20186fb4's own entry directly (not a specific call site) and logs (lr, r0=msg
ptr, r1=param_2) for every hit -- this catches ALL real invocations regardless of which of the
3 paths they come through, unlike the narrower single-call-site checks already done.

Usage: trace_20186fb4_callers.py [max_hits] [max_seconds]

RESULT, 2026-09-10: found the real producer cleanly -- a perfectly regular ~82ms cadence from
t=0, lr=0x20005bb4 every single time (FUN_20005b98, since renamed
`mtu2_ch3_periodic_housekeeping_tick` in Ghidra -- registered as MTU2 channel 3's TGI3A handler,
GIC ID 154, this project's own already-confirmed real timer). Not a burst at all -- a steady
tick that runs the whole boot; the ring only overflows once its consumer
(irq_context_switch_id0) stops keeping pace with it, around ~27s in. See
qemu-machine/README-history.md's newest section for the full derivation, including the two
wrong leads (SVC dispatch, sdcard_file_rpc_dispatch_task) ruled out first.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

FUN_20186FB4 = 0x20186fb4
OVERFLOW_TRAP = 0x200b93fc


def main():
    max_hits = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    max_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0

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

        g.set_breakpoint(FUN_20186FB4)
        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        hits = 0
        while hits < max_hits and time.time() - start < max_seconds:
            try:
                g.wait_stop(timeout=max_seconds - (time.time() - start))
            except (TimeoutError, OSError):
                print(f"t={time.time()-start:.1f}s  TIMEOUT, {hits} FUN_20186fb4 hits so far")
                break
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            if pc == OVERFLOW_TRAP:
                print(f"t={elapsed:7.3f}s  *** OVERFLOW TRAP *** after {hits} FUN_20186fb4 hits")
                break
            lr = regs["r14"]
            r0 = regs["r0"]
            r1 = regs["r1"]
            print(f"t={elapsed:7.3f}s  hit#{hits}  FUN_20186fb4(msg={r0:#010x}, param2={r1:#010x})"
                  f"  lr={lr:#010x}")
            hits += 1
            g.cont()
        else:
            print(f"reached max_hits={max_hits} without an overflow")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
