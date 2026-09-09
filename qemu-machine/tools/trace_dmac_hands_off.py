#!/usr/bin/env python3
"""Sanity-check re-confirmation for the DMAC/icount stall (README.md's Status section,
2026-09-09): before chasing the adaptive-shift hypothesis further, re-confirm the stall
still reproduces at all under the *original* diagnostic conditions -- a genuinely hands-off
free run, zero GDB interaction after the initial continue (not even periodic interrupt()/
cont() polling), checked only externally via host `ps` CPU%. This is the cleanest possible
baseline: no breakpoints ever armed, no periodic pausing, nothing that could itself perturb
icount/TCG translation-block behavior.

Usage: trace_dmac_hands_off.py [seconds]
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"


def cpu_percent(pid: int) -> float | None:
    try:
        out = subprocess.check_output(["ps", "-o", "%cpu=", "-p", str(pid)], text=True)
        return float(out.strip())
    except Exception:
        return None


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0

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
        g.cont()
        # From here on: zero GDB interaction. gdbrsp's own socket is left open but idle
        # (not polled, not read) -- the guest runs completely free. Only host-side `ps`
        # sampling, well-separated in time, checks in.
        del g

        start = time.time()
        samples = []
        while time.time() - start < total_seconds:
            time.sleep(10.0)
            elapsed = time.time() - start
            pct = cpu_percent(proc.pid)
            samples.append((elapsed, pct))
            print(f"t={elapsed:7.1f}s  qemu CPU={pct}%")

        print("\nfinal samples:", samples)
        # A genuine spin-stall shows as CPU pegged near 100% (single-threaded TCG) for
        # multiple consecutive samples in a row, especially toward the end of the window.
        tail = [p for _, p in samples[-6:] if p is not None]
        if tail and min(tail) > 85.0:
            print("-> CPU pegged high (>85%) for the last several samples -- consistent "
                  "with a genuine spin-stall (matches the original ps-confirmed finding).")
        else:
            print("-> CPU did not stay pegged high -- no stall reproduced in this "
                  "hands-off run (a real result either way: either the stall needs "
                  "longer than this window, or it no longer reproduces under current "
                  "conditions).")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
