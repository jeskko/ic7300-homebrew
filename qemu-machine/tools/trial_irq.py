#!/usr/bin/env python3
"""Repeated-trial IRQ-delivery escape-rate measurement.

Built 2026-09-08 to settle whether a run of "still stuck in idle after
arming OSTM0" results (seen right after adding mmc.c) is a real
regression or just sampling noise -- a single real-time snapshot can't
tell the difference (see qemu-machine/README.md's own "one real scare"
section from the SCIF work earlier this session). Manages the QEMU
process directly via subprocess.Popen rather than shell backgrounding,
to sidestep this session's own repeated pkill/pgrep self-match footguns.

Usage: trial_irq.py <n_trials> [extra qemu args...]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu

WFE_LOOP_LO = 0x200b939c
WFE_LOOP_HI = 0x200b93ac
GIC_DIST = 0xE8201000
GIC_CPU = 0xE8202000
OSTM0_BASE = 0xFCFEC000
ARM_CMP = 5_000_000


def run_one_trial(extra_args: list[str]) -> bool:
    """Returns True if the CPU left the idle loop after arming OSTM0."""
    proc = launch_qemu(extra_args)
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, "not a fresh boot"

        g.cont()
        time.sleep(1.5)
        g.interrupt()
        g.wait_stop()

        g.write_u32(GIC_DIST + 0x000, 1)
        g.write_u32(GIC_DIST + 0x110, 1 << 6)
        g.write_u32(GIC_CPU + 0x000, 1)
        g.write_u32(GIC_CPU + 0x004, 0xFF)
        g.write_memory(GIC_DIST + 0x800 + 134, bytes([0x01]))
        g.write_u32(GIC_DIST + 0xc20, 0xFFFFFFFF)
        g.write_u32(OSTM0_BASE + 0x00, ARM_CMP)
        g.write_u32(OSTM0_BASE + 0x14, 1)

        g.cont()
        time.sleep(0.3)
        g.interrupt()
        g.wait_stop()
        regs = g.read_registers()
        g.close()
        return not (WFE_LOOP_LO <= regs["r15"] <= WFE_LOOP_HI)
    finally:
        proc.kill()
        proc.wait()


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    extra = sys.argv[2:]
    escaped = 0
    for i in range(n):
        ok = run_one_trial(extra)
        print(f"trial {i}: {'escaped' if ok else 'STILL IDLE'}")
        escaped += ok
    print(f"\n{escaped}/{n} trials escaped the idle loop")


if __name__ == "__main__":
    main()
