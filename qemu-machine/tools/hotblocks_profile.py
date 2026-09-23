#!/usr/bin/env python3
"""CPU "heat map" profiler for body.bin, using QEMU's own built-in TCG plugin
(contrib/plugins/hotblocks.c) -- no new C code needed, see README.md's Status section for the
feasibility check and why this is a genuinely clean signal, not just another sampling profiler:

  - The plugin counts real TCG-block executions and each block's own real instruction count at
    the TCG level -- i.e. actual ARM instructions retired, not wall-clock samples. Under -icount
    (already this machine's own default), that's a ground-truth measure, completely orthogonal
    to the round-robin-main-loop-overhead/icount-shift confounds this project's own earlier
    session spent real effort untangling (see README-history.md) -- a plugin-based heat map
    can't be distorted by that class of QEMU-scheduling artifact the way a real-time sampling
    profiler could be.
  - It runs inline with normal TCG execution -- no vm_stop(), no GDB, none of the perturbation
    this project has repeatedly found from GDB-based techniques. It does slow down *host* wall
    time (per-block bookkeeping overhead), but since -icount ties virtual time to instructions
    retired (not wall clock), this should not change any ptimer/device-visible *behavior* --
    only how long the run takes on this machine, in real terms.
  - Confirmed live (2026-09-10): flushes its report cleanly even on SIGTERM (matches this
    project's own established proc.terminate()-based shutdown, used everywhere else already) --
    no need for a clean QMP `quit`.

Prerequisite (not automatic -- build once per session, matches this project's own convention for
diagnostics that aren't part of the default build):
    ninja -C qemu-machine/qemu-src/build contrib/plugins/libhotblocks.so

Usage: hotblocks_profile.py [seconds] [top_n]

Output: a CSV of every distinct translation block hit (pc, tcount, icount, ecount) at
/tmp/hotblocks_raw.csv, plus a printed, sorted-by-total-retired-instructions (icount * ecount)
top-N summary with each block's share of the run's total retired instructions -- this is the
list of addresses to hand to Ghidra (one inspect.decompile-by-address call per address, NOT a
full function-table export -- see README.md's Status section for why the full table wasn't
pre-exported: it's thousands of entries, expensive for little benefit when only a small number
of addresses ever turn out to matter).
"""
from __future__ import annotations

import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
PLUGIN = HERE / "qemu-src" / "build" / "contrib" / "plugins" / "libhotblocks.so"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"
RAW_CSV = Path("/tmp/hotblocks_raw.csv")

LINE_RE = re.compile(r"^0x([0-9a-fA-F]+), (\d+), (\d+), (\d+)$")


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    top_n = int(sys.argv[2]) if len(sys.argv) > 2 else 50

    if not QEMU.exists():
        sys.exit(f"{QEMU} not found -- run setup.sh first")
    if not PLUGIN.exists():
        sys.exit(f"{PLUGIN} not found -- run: "
                 f"ninja -C {HERE}/qemu-src/build contrib/plugins/libhotblocks.so")

    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-plugin", f"file={PLUGIN},limit=0",  # limit=0: dump every block, we sort ourselves --
                                                # the plugin's own default top-20-by-ecount cutoff
                                                # would silently drop a low-ecount/high-icount
                                                # block that actually ranks higher by total
                                                # retired instructions (icount * ecount).
        "-d", "plugin",  # REQUIRED for the report to actually appear, confirmed live (2026-09-10,
                          # 3/3 trials each way): without any "-d <category>" flag, the plugin's
                          # own qemu_plugin_outs() report is silently lost on shutdown (0/3
                          # trials produced it); with "-d plugin" present, it's reliably flushed
                          # (3/3). Not root-caused (glibc stdio buffering vs. QEMU's own -d
                          # logging setup forcing a stream into a different buffering mode is the
                          # leading guess, not confirmed) -- treat as a required flag, not cosmetic.
    ]
    print(f"Running {seconds}s with the hotblocks TCG plugin attached "
          f"(this will take noticeably longer in real wall time than an unplugged run -- "
          f"expected, see this file's own header comment)...", flush=True)

    log_path = Path("/tmp/hotblocks_qemu_stderr.log")
    t0 = time.monotonic()
    with open(log_path, "w") as logf:
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=logf)
        try:
            time.sleep(seconds)
        finally:
            proc.terminate()  # the plugin's report (qemu_plugin_outs()) flushes to stderr on
                               # SIGTERM -- confirmed live, same convention as every other trace
                               # tool in this project (trace_rr_loop_overhead.py etc.) that reads
                               # a log file rather than a subprocess pipe (a PIPE-based capture
                               # was tried first here and silently lost the final flush -- writing
                               # straight to a file, like every other tool already does, doesn't).
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    out = log_path.read_text(errors="replace")
    wall = time.monotonic() - t0
    print(f"Ran for {wall:.1f}s wall-clock (requested {seconds}s virtual-boot time -- these "
          f"diverge under the plugin's own overhead, that's expected and not itself a finding).")

    blocks = []  # (pc, tcount, icount, ecount)
    for line in out.splitlines():
        m = LINE_RE.match(line.strip())
        if m:
            pc, tcount, icount, ecount = m.groups()
            blocks.append((int(pc, 16), int(tcount), int(icount), int(ecount)))

    if not blocks:
        print("No hotblocks output found -- check the plugin actually loaded "
              "(rerun without redirecting stderr to see QEMU's own startup messages).")
        print(out[-2000:])
        return 1

    total_instrs = sum(icount * ecount for _, _, icount, ecount in blocks)
    blocks.sort(key=lambda b: b[2] * b[3], reverse=True)

    with open(RAW_CSV, "w") as f:
        f.write("pc,tcount,icount,ecount,total_instrs\n")
        for pc, tcount, icount, ecount in blocks:
            f.write(f"0x{pc:x},{tcount},{icount},{ecount},{icount * ecount}\n")
    print(f"\n{len(blocks)} distinct blocks, {total_instrs:,} total retired instructions. "
          f"Full data: {RAW_CSV}\n")

    print(f"Top {top_n} blocks by total retired instructions (pc, ecount, icount/block, "
          f"total_instrs, % of run):")
    print(f"{'pc':<20} {'ecount':>12} {'icount/blk':>10} {'total_instrs':>14} {'%':>7}")
    for pc, tcount, icount, ecount in blocks[:top_n]:
        ti = icount * ecount
        pct = 100.0 * ti / total_instrs if total_instrs else 0.0
        print(f"0x{pc:016x} {ecount:>12,} {icount:>10} {ti:>14,} {pct:6.2f}%")

    print(f"\nNext step (fresh session): resolve each of these top {top_n} addresses to its "
          f"containing function via Ghidra (mcp__ghidra__inspect, action=decompile, one call per "
          f"address -- NOT a full function-table export, see this file's own header comment) and "
          f"aggregate by function to see which named function actually dominates. Flag anything "
          f"surprising -- a small/trivial-seeming function eating a large % share, or a hot spot "
          f"that isn't one of the already-known busy-waits documented in README.md/README-history.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
