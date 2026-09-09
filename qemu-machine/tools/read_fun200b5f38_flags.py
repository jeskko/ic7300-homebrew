#!/usr/bin/env python3
"""Direct-memory-read follow-up to trace_fun200b5f38_wait.py's TIMEOUT result (confirmed
2026-09-10: neither LOOP1_EXIT nor OWN_RETURN ever fires in a 300s free-running trial --
FUN_200b5f38 is genuinely stuck in its own loop 1, never arms its own transfer).

Per that tool's own module comment, the next step is a plain memory read of the two flag
bytes loop 1 checks -- NOT another breakpoint, deliberately, since a breakpoint sitting at a
tight loop's own natural exit is exactly the pattern already proven (DMAC/icount stall,
README-history.md) to be a real QEMU gdbstub reliability artifact under `-icount shift=auto`.
A generic `interrupt()` + `read_memory()` is a different, already-cleared code path (the DMAC
finding explicitly tested "a real hit-and-resume cycle" and "forced interrupt reporting a
stale PC" as innocent variables) -- this stays GDB-free-equivalent in spirit.

  FLAG_MAIN   = 0x203906EE  -- loop 1's first check (shared with FUN_200b5ea4)
  FLAG_SECOND = 0x203906ED  -- loop 1's second check (not separately traced before)

Also reads r15 (PC) each sample, to confirm the core is genuinely parked inside loop 1
(0x200b5fa4-0x200b5fac) rather than stuck somewhere else entirely (which would retract this
whole line of diagnosis).

Usage: read_fun200b5f38_flags.py [settle_seconds] [num_samples] [sample_interval]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

FLAG_MAIN = 0x203906EE
FLAG_SECOND = 0x203906ED
LOOP1_LO = 0x200b5fa4
LOOP1_HI = 0x200b5fac


def main():
    settle = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    samples = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    interval = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

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

        print(f"free-running {settle:.0f}s (no breakpoints at all) before first sample...")
        g.cont()
        time.sleep(settle)

        for i in range(samples):
            g.interrupt()
            g.wait_stop(timeout=5.0)
            regs = g.read_registers()
            pc = regs["r15"]
            main_val = g.read_memory(FLAG_MAIN, 1)[0]
            second_val = g.read_memory(FLAG_SECOND, 1)[0]
            in_loop1 = LOOP1_LO <= pc <= LOOP1_HI
            print(f"sample {i}: t~={settle + i*interval:.0f}s  pc={pc:#010x} "
                  f"in_loop1={in_loop1}  FLAG_MAIN[{FLAG_MAIN:#x}]={main_val:#04x} "
                  f"FLAG_SECOND[{FLAG_SECOND:#x}]={second_val:#04x}")
            if i < samples - 1:
                g.cont()
                time.sleep(interval)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
