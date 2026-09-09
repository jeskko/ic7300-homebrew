#!/usr/bin/env python3
"""Follow-up to dump_job_ring_at_overflow.py's finding (2026-09-10): the 16 identical-looking
overflow entries are 16 REAL, distinct file-RPC completions from sdcard_file_rpc_dispatch_task
(alive for the first time ever, post-DMAC-fix), each dutifully posting the same generic
"results ready" doorbell -- not 16 copies of one event. This traces WHICH command(s) actually
flood in, by breakpointing `file_rpc_post_command` (0x200bc048) itself: its 26 real callers are
tiny per-command-ID wrapper functions (0x200bc0fc-0x200bca08, one per file-RPC command,
0200bc148/0x200bc1a0/... in the references_to list, 0x58 bytes apart) -- so the LR at each hit
directly identifies which command got posted, without needing to trace further up to whatever
task originally requested it.

Low-perturbation by this project's own established standard: unlike a breakpoint on a tight,
continuously-executing loop (already shown to mask bugs), this fires only ~16 times total
across a ~45s boot, each a genuinely distinct real event -- much closer to the "rare event"
category (like the overflow trap itself) than the "hot path" category. Confirmed same session
NOT to reintroduce the earlier polling-perturbation problem: still stop, record, and continue
immediately each time, no extra delay.

Usage: trace_file_rpc_burst_source.py [max_hits] [max_seconds]

RESULT, 2026-09-10: zero hits before the overflow -- file_rpc_post_command is NOT the producer.
Ruled out cleanly (not a tooling bug -- see trace_20186fb4_callers.py, which confirmed the real
producer via the same breakpoint-based technique and got a clean, high-confidence positive
result). The sdcard_file_rpc_dispatch_task connection this tool assumed was itself wrong; kept
for the honest derivation trail, see README-history.md's newest section.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

FILE_RPC_POST_COMMAND = 0x200bc048
OVERFLOW_TRAP = 0x200b93fc

# 26 per-command wrapper functions, 0x200bc0fc + 0x58*n, n=0..25 (cmd id = n+1, matches each
# wrapper's own `mov r5,#n+1` at offset 0x8) -- confirmed via raw listing: each wrapper is
# exactly 0x58 bytes, its own `bl file_rpc_post_command` sits at offset 0x4c.
WRAPPER_BASE = 0x200bc0fc
WRAPPER_STRIDE = 0x58
CALL_OFFSET = 0x4c


def wrapper_id(lr: int) -> str:
    call_addr = lr - 4  # LR = return address = instruction right after the `bl`
    wrapper_start = call_addr - CALL_OFFSET
    idx, rem = divmod(wrapper_start - WRAPPER_BASE, WRAPPER_STRIDE)
    if rem == 0 and 0 <= idx < 26:
        return f"wrapper #{idx + 1} (cmd id {idx + 1}, entry {wrapper_start:#010x})"
    return f"unrecognized LR={lr:#010x} (not one of the 26 known wrappers)"


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

        g.set_breakpoint(FILE_RPC_POST_COMMAND)
        g.set_breakpoint(OVERFLOW_TRAP)
        g.cont()

        start = time.time()
        hits = 0
        while hits < max_hits and time.time() - start < max_seconds:
            try:
                g.wait_stop(timeout=max_seconds - (time.time() - start))
            except (TimeoutError, OSError):
                print(f"t={time.time()-start:.1f}s  TIMEOUT, {hits} file_rpc_post_command "
                      "hits so far")
                break
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            if pc == OVERFLOW_TRAP:
                print(f"t={elapsed:7.3f}s  *** OVERFLOW TRAP *** after {hits} "
                      "file_rpc_post_command hits")
                break
            lr = regs["r14"]
            print(f"t={elapsed:7.3f}s  hit#{hits}  file_rpc_post_command called from "
                  f"lr={lr:#010x} -- {wrapper_id(lr)}")
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
