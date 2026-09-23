#!/usr/bin/env python3
"""GDB-free re-confirmation of the `trace_eeprom_scan_caller.py` finding (see README.md's Status
section): a one-shot GDB breakpoint on `FUN_2001e484`'s entry (the generic EEPROM-read chunking
wrapper) found every hit in a 60-hit/~9.7s capture traced back to `cold_boot_hw_init` itself,
reading a fixed 16 bytes from EEPROM offset `0x3df0`, repeating roughly every ~83ms -- surprising
because `cold_boot_hw_init`'s own decompiled body has no loop around that call site. Not yet
confirmed whether this is real re-entry or an artifact of the live-probing technique itself.

This session directly read QEMU's own gdbstub/icount internals (README-history.md's newest
section) and confirmed a GDB pause adds *zero* virtual-time distortion of its own -- so a live-
probing artifact, if real, would have to come from something else (a `gdbrsp.py` breakpoint-
handling bug, most plausibly) rather than the icount/vm_stop mechanism itself. This tool gets an
independent, fully GDB-free confirmation of the ~83ms/`0x3df0` pattern regardless: it relies on
riic.c's own new "EEPROM addr=... resolved" debug line (2026-09-10, same session) -- host-side,
GDB-free, no perturbation -- to see every individual RIIC2 read's *target address* directly,
without needing to breakpoint the caller at all. If a `0x3df0` read recurs at ~83ms intervals in
this GDB-free log, `cold_boot_hw_init`'s own re-entry is real, not a probing artifact -- a live-
probing artifact could not fabricate entries in a log that never involves GDB in the first place.

Usage: trace_eeprom_addr_gdbfree.py [seconds]
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"
LOG = Path("/tmp/eeprom_addr_gdbfree.log")

ADDR_RE = re.compile(r"\[rza1h:riic t=([\d.]+)\] riic(\d+): EEPROM addr=(0x[0-9a-fA-F]+) resolved")

COLD_BOOT_ADDR = 0x3DF0


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
    ]
    env = {"RZA1H_DEBUG": "riic"}

    with open(LOG, "w") as logf:
        launch_mono = time.monotonic()
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=logf, env=env)
        try:
            time.sleep(seconds)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    hits = []  # (t_rel_to_launch, channel, addr)
    for line in LOG.read_text(errors="replace").splitlines():
        m = ADDR_RE.match(line)
        if m:
            hits.append((float(m.group(1)) - launch_mono, int(m.group(2)), int(m.group(3), 16)))

    print(f"{len(hits)} total EEPROM-address-resolved events over {seconds:.1f}s, GDB-free "
          f"(t below is seconds since QEMU launch)\n")

    addr_counts = Counter(addr for _, _, addr in hits)
    print("Address histogram (top 15):")
    for addr, n in addr_counts.most_common(15):
        print(f"  addr={addr:#06x}  {n:4d} hits")

    print("\nFull timeline (t, addr):")
    for t, ch, a in hits:
        print(f"  t={t:8.3f}s  riic{ch}  addr={a:#06x}")

    cold_boot_hits = [(t, ch, a) for t, ch, a in hits if a == COLD_BOOT_ADDR]
    print(f"\n{len(cold_boot_hits)} hits at cold_boot_hw_init's own address ({COLD_BOOT_ADDR:#06x}):")
    prev_t = None
    for t, ch, a in cold_boot_hits:
        gap = f"  (+{t - prev_t:.3f}s)" if prev_t is not None else ""
        print(f"  t={t:8.3f}s  riic{ch}{gap}")
        prev_t = t

    if len(cold_boot_hits) >= 2:
        gaps = [cold_boot_hits[i][0] - cold_boot_hits[i - 1][0] for i in range(1, len(cold_boot_hits))]
        print(f"\nGap stats: min={min(gaps):.3f}s max={max(gaps):.3f}s "
              f"mean={sum(gaps) / len(gaps):.3f}s (n={len(gaps)} gaps)")


if __name__ == "__main__":
    main()
