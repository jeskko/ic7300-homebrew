#!/usr/bin/env python3
"""Follow-up to trace_file_rpc_burst_source.py's negative result (2026-09-10): breakpointing
`file_rpc_post_command` (the 26-wrapper-fed path) caught ZERO calls before the ring overflow --
meaning the 16 "results ready" doorbell posts don't come from that path at all. `FUN_20186e68`
(the generic "signal an object" kernel primitive `file_rpc_post_command` itself calls, one
level down) has SIX real static callers, not one -- so this traces all of them directly, to
find which one actually targets sdcard_file_rpc_dispatch_task's own wait object
(`*(0x203907f0+0x28)`, read live each hit since it's a runtime-populated pointer, not a fixed
constant).

Logs (lr, r0=signaled object, r1=value) for every hit, unfiltered -- deliberately not
pre-filtering by object address, so a suspiciously-regular hit pattern (this project's own
known gdbstub-artifact signature) would be visible directly in the raw log rather than
silently filtered away.

Usage: trace_signal_calls.py [max_hits] [max_seconds]

RESULT, 2026-09-10: zero hits before the overflow, and sdcard_file_rpc_dispatch_task's own wait
target read back 0x0 (never populated during the run) -- confirms this whole task was the wrong
lead. The real producer, found right after via trace_20186fb4_callers.py, is a periodic MTU2
ch3 (TGI3A) housekeeping tick, unrelated to sdcard_file_rpc_dispatch_task entirely. Kept for the
honest derivation trail, see README-history.md's newest section.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

SIGNAL_FN = 0x20186e68
OVERFLOW_TRAP = 0x200b93fc
TASK_STRUCT = 0x203907f0
TASK_WAIT_FIELD = TASK_STRUCT + 0x28


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

        g.set_breakpoint(SIGNAL_FN)
        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        hits = 0
        while hits < max_hits and time.time() - start < max_seconds:
            try:
                g.wait_stop(timeout=max_seconds - (time.time() - start))
            except (TimeoutError, OSError):
                print(f"t={time.time()-start:.1f}s  TIMEOUT, {hits} signal() hits so far")
                break
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            if pc == OVERFLOW_TRAP:
                task_target = int.from_bytes(g.read_memory(TASK_WAIT_FIELD, 4), "little")
                print(f"\nt={elapsed:7.3f}s  *** OVERFLOW TRAP *** after {hits} signal() hits "
                      f"-- sdcard_file_rpc_dispatch_task's own wait target is now {task_target:#x}")
                break
            lr = regs["r14"]
            r0 = regs["r0"]
            r1 = regs["r1"]
            print(f"t={elapsed:7.3f}s  hit#{hits}  signal(obj={r0:#010x}, val={r1:#010x})  "
                  f"lr={lr:#010x}")
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
