#!/usr/bin/env python3
"""Follow-up to trace_job_ring_overflow.py -- instead of tracing who calls the producer, just
read what the ring is actually full OF at the moment it overflows: each of the 16 entries is
8 bytes (a 4-byte job-object pointer + a 4-byte payload word, per the struct layout
trace_job_ring_overflow.py's own module comment already derived), and the consumer
(FUN_20187ae4) dispatches on the job object's own first byte (0/1/other -> FUN_201874a8/
FUN_201877e4/FUN_20187e34). Reading the actual queued job objects' type bytes (and a few
payload bytes each) directly answers "what kind of messages are these" without needing to
trace the producer call chain at all.

Same low-perturbation technique as trace_job_ring_overflow.py: free-run with exactly one
breakpoint (the overflow trap itself, which only ever fires once and stops everything anyway)
-- no polling/interrupting during the run-up, so this doesn't risk masking the burst the way
even light periodic polling was shown to (2026-09-10 session).

Usage: dump_job_ring_at_overflow.py [max_seconds]
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
RING_ENTRIES = RING_BASE + 4
ENTRY_SIZE = 8
CAPACITY = 16

OVERFLOW_TRAP = 0x200b93fc


def main():
    max_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0

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
        try:
            g.wait_stop(timeout=max_seconds)
        except (TimeoutError, OSError):
            print(f"no overflow in {max_seconds:.0f}s")
            return

        regs = g.read_registers()
        elapsed = time.time() - start
        print(f"t={elapsed:.2f}s  overflow trap hit, r0={regs['r0']:#x} lr={regs['r14']:#x}\n")

        hdr = g.read_memory(RING_BASE, 4)
        write_idx, read_idx, pending, capacity = hdr
        print(f"header: write_idx={write_idx} read_idx={read_idx} pending={pending} "
              f"capacity={capacity}\n")

        # sdcard_file_rpc_dispatch_task's own small result ring (FUN_200b97c4, capacity 10,
        # word0=count then (cmd_id, result) pairs) -- reveals which actual file-RPC commands
        # were processed to produce the identical doorbell posts below. 0x200ba170 is itself
        # a literal-pool CELL holding the real RAM base (confirmed via FUN_200b97c4's own raw
        # listing: `ldr r3,[0x200ba170]` then `ldr r1,[r3,#0]` for the count) -- not the array
        # itself. First read through that one level of indirection.
        RESULT_RING = int.from_bytes(g.read_memory(0x200ba170, 4), "little")
        print(f"result ring base (via literal pool @0x200ba170): {RESULT_RING:#x}")
        count = int.from_bytes(g.read_memory(RESULT_RING, 4), "little")
        print(f"sdcard_file_rpc result ring: count={count}")
        for i in range(min(count, 10)):
            cmd_id = int.from_bytes(g.read_memory(RESULT_RING + 4 + i * 8, 4), "little")
            result = int.from_bytes(g.read_memory(RESULT_RING + 8 + i * 8, 4), "little")
            print(f"  [{i}] cmd_id={cmd_id:#x} ({cmd_id})  result={result:#x}")
        print()

        print(f"{'slot':>4}  {'job_ptr':>10}  {'payload':>10}  {'type_byte':>9}  "
              f"first_16_bytes_of_job_object")
        for slot in range(CAPACITY):
            entry = g.read_memory(RING_ENTRIES + slot * ENTRY_SIZE, ENTRY_SIZE)
            job_ptr = int.from_bytes(entry[0:4], "little")
            payload = int.from_bytes(entry[4:8], "little")
            if job_ptr == 0:
                print(f"{slot:>4}  {job_ptr:#010x}  {payload:#010x}  {'(null)':>9}")
                continue
            try:
                job_bytes = g.read_memory(job_ptr, 16)
                type_byte = job_bytes[0]
                hex_bytes = job_bytes.hex()
            except Exception as e:
                type_byte = None
                hex_bytes = f"(unreadable: {e})"
            print(f"{slot:>4}  {job_ptr:#010x}  {payload:#010x}  "
                  f"{type_byte if type_byte is not None else '?':>9}  {hex_bytes}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
