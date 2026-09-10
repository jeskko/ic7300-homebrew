#!/usr/bin/env python3
"""Free-runs a real boot with the new host-side IRQ-mask-transition hook enabled
(RZA1H_IRQ_TRACE=1, patches/irq-mask-trace.patch -- see tools/apply_irq_mask_trace.sh
and README.md's Status section) and reports the widest CLR->SET gap in the log --
directly answering the open resume point ("what holds CPSR.I=1 for ~10s") instead of
grepping the firmware for cpsid/cpsie call sites and guessing.

Deliberately GDB-free (no -S/-gdb, no gdbrsp.py): this project has repeatedly found
GDB breakpoints and even extra memory-read polling suppress this exact stall, so this
tool free-runs qemu-system-arm exactly the way README.md's "Running it" section
recommends and only reads its stderr log after the fact.

Usage: trace_irq_mask.py [seconds]
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"
LOG = HERE / "scratch" / "irqtrace.log" if (HERE / "scratch").is_dir() else Path("/tmp/irqtrace.log")

LINE_RE = re.compile(
    r"\[irqtrace\] t=(?P<t>\d+)us I (?P<edge>SET|CLR) pc=0x(?P<pc>[0-9a-f]+) "
    r"mode=0x(?P<mode>[0-9a-f]+) via=(?P<via>\S+)"
)


def main() -> None:
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    LOG.parent.mkdir(parents=True, exist_ok=True)

    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=auto",
    ]
    env = {"RZA1H_IRQ_TRACE": "1"}
    print(f"Launching qemu-system-arm for {seconds}s, RZA1H_IRQ_TRACE=1 -> {LOG}")
    with open(LOG, "wb") as logf:
        proc = subprocess.Popen(
            args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=logf, env=env,
        )
        try:
            time.sleep(seconds)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    events = []
    for line in LOG.read_text(errors="replace").splitlines():
        m = LINE_RE.search(line)
        if m:
            events.append(
                (int(m["t"]), m["edge"], int(m["pc"], 16), int(m["mode"], 16), m["via"])
            )

    print(f"{len(events)} I-bit transitions captured")
    if not events:
        print("No transitions logged -- check that the patch is applied and "
              "the build used has the hook (tools/apply_irq_mask_trace.sh).")
        return

    # Widest gap between a CLR (I=0, unmasked) and the next SET (I=1, masked) --
    # the "held masked" windows are SET..CLR, so look at CLR..next-SET gaps too;
    # report both directions, largest first.
    gaps = []
    for i in range(1, len(events)):
        t0, edge0, pc0, mode0, via0 = events[i - 1]
        t1, edge1, pc1, mode1, via1 = events[i]
        gaps.append((t1 - t0, events[i - 1], events[i]))
    gaps.sort(key=lambda g: -g[0])

    print("\nTop 5 widest gaps between consecutive I-bit transitions:")
    for dur_us, before, after in gaps[:5]:
        t0, edge0, pc0, mode0, via0 = before
        t1, edge1, pc1, mode1, via1 = after
        print(
            f"  {dur_us/1e6:.3f}s gap: [t={t0}us] I->{edge0} pc=0x{pc0:08x} via={via0}  "
            f"...then...  [t={t1}us] I->{edge1} pc=0x{pc1:08x} via={via1}"
        )


if __name__ == "__main__":
    main()
