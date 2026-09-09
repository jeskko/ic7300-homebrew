#!/usr/bin/env python3
"""Direct ground-truth check, 2026-09-10: file_rpc_post_command and the generic FUN_20186e68
signal primitive BOTH showed zero hits before the ring overflow (trace_file_rpc_burst_source.py,
trace_signal_calls.py) -- meaning no NEW file-RPC command arrived from outside during the whole
run-up. Yet the doorbell (0x20420120) still floods with 16 identical posts. This breakpoints the
exact call site inside sdcard_file_rpc_dispatch_task that posts its own "results ready" doorbell
(0x200b9ddc, `blx 0x20186fb4` with r1=1) directly -- bypassing every assumption about how a
command gets *in*, to see directly whether the task's own loop is re-posting 16 times without
ever receiving a genuinely new external command (a self-contained loop/logic issue) or something
else entirely.

Usage: trace_task_own_post.py [max_hits] [max_seconds]

RESULT, 2026-09-10: zero hits before the overflow -- this exact call site never fires during
the run-up, confirming sdcard_file_rpc_dispatch_task is not the source (see
trace_signal_calls.py and trace_file_rpc_burst_source.py for the other two rule-outs on the
same wrong lead). The real producer is a periodic MTU2 ch3 tick, found via
trace_20186fb4_callers.py. Kept for the honest derivation trail.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

POST_CALL_SITE = 0x200b9ddc  # `blx 0x20186fb4` inside sdcard_file_rpc_dispatch_task
OVERFLOW_TRAP = 0x200b93fc


def main():
    max_hits = int(sys.argv[1]) if len(sys.argv) > 1 else 30
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

        g.set_breakpoint(POST_CALL_SITE)
        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        hits = 0
        while hits < max_hits and time.time() - start < max_seconds:
            try:
                g.wait_stop(timeout=max_seconds - (time.time() - start))
            except (TimeoutError, OSError):
                print(f"t={time.time()-start:.1f}s  TIMEOUT, {hits} own-post hits so far")
                break
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            if pc == OVERFLOW_TRAP:
                print(f"t={elapsed:7.3f}s  *** OVERFLOW TRAP *** after {hits} own-post hits")
                break
            r0 = regs["r0"]  # the msg record pointer being posted to
            print(f"t={elapsed:7.3f}s  hit#{hits}  post(msg={r0:#010x})")
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
