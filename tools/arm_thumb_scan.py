#!/usr/bin/env python3
"""
Scan a raw ARM firmware image and classify each region as ARM or Thumb code
(or ambiguous/data) using objdump's own "UNDEFINED"/"(bad)" instruction counts
as a decode-quality heuristic, cross-checked in both modes.

Why this exists
----------------
Built for the IC-7300 body.bin RE project after discovering (2026-08-29) that
notes/kernel-rtos.md's prior "zero genuine Thumb code anywhere in this
firmware" conclusion was WRONG: a large statically-linked FreeType Thumb-2
codebase lives around 0x2014b000-0x2017d000. That finding came from checking
one address by hand because the user noticed Ghidra decoding it differently
in ARM vs Thumb mode -- the project's earlier "Bad Instruction" bookmark
sweep never surfaced it, because Ghidra's auto-analysis had *already* guessed
that region correctly, so no conflict bookmark was ever created there.

This script is a **ground-truth locator, not an auto-fixer**. It never
touches the live Ghidra project. Point it at a range, get back a region map
of what the bytes actually decode as in each mode; use that map to decide
where Ghidra's current listing might be wrong, then apply any real fix by
hand in the GUI (Clear Code Bytes + Set Register TMode + Disassemble) the
same way every other fix in this project has been verified. Do NOT wire this
into a script that blindly rewrites the Ghidra database -- a region that
looks "bad" in one mode may simply be data (literal pools, glyph tables,
padding), and a region that already analyzes correctly should never be
touched just because this heuristic also thinks it looks fine the other way.

Usage
-----
    python3 arm_thumb_scan.py <raw.bin> --start 0x20140000 --end 0x20200000
    python3 arm_thumb_scan.py <raw.bin> --start 0x20005000 --end 0x20395b18 \
        --window 0x4000 --out /tmp/full_scan.json

Requires arm-none-eabi-objdump on PATH. Addresses are absolute (already
including --base); pass --base to match how the image is based in Ghidra
(IC-7300 body.bin: 0x20005000, RAM-based, not flash).
"""
import argparse
import json
import subprocess
import sys


def objdump_bad_count(path, base, lo, hi, thumb):
    args = [
        "arm-none-eabi-objdump", "-D", "-b", "binary", "-m", "arm",
        "--adjust-vma", hex(base),
        "--start-address", hex(lo), "--stop-address", hex(hi),
    ]
    if thumb:
        args += ["-M", "force-thumb"]
    args.append(path)
    out = subprocess.run(args, capture_output=True, text=True).stdout
    return out.count("UNDEFINED") + out.count("(bad)")


def classify(path, base, lo, hi):
    a = objdump_bad_count(path, base, lo, hi, False)
    t = objdump_bad_count(path, base, lo, hi, True)
    if a == 0 and t == 0:
        return "either", a, t
    if t == 0 and a > 0:
        return "thumb", a, t
    if a == 0 and t > 0:
        return "arm", a, t
    if t < a * 0.25:
        return "thumb", a, t
    if a < t * 0.25:
        return "arm", a, t
    return "ambiguous", a, t  # both bad -- probably data, not instructions


def scan(path, base, start, end, window):
    lo = start
    rows = []
    while lo < end:
        hi = min(lo + window, end)
        cls, a, t = classify(path, base, lo, hi)
        rows.append({"start": lo, "end": hi, "mode": cls, "arm_bad": a, "thumb_bad": t})
        lo = hi
    return rows


def merge(rows):
    regions = []
    for r in rows:
        if regions and regions[-1]["mode"] == r["mode"] and regions[-1]["end"] == r["start"]:
            regions[-1]["end"] = r["end"]
        else:
            regions.append({"start": r["start"], "end": r["end"], "mode": r["mode"]})
    return regions


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binfile")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x20005000)
    ap.add_argument("--start", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--end", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--window", type=lambda s: int(s, 0), default=0x1000)
    ap.add_argument("--out", default=None, help="write region JSON here")
    ap.add_argument("--min-len", type=lambda s: int(s, 0), default=0,
                     help="omit merged regions shorter than this from the printed summary")
    args = ap.parse_args()

    rows = scan(args.binfile, args.base, args.start, args.end, args.window)
    regions = merge(rows)

    if args.out:
        with open(args.out, "w") as f:
            json.dump({"base": args.base, "window": args.window, "regions": regions}, f, indent=2)

    for r in regions:
        length = r["end"] - r["start"]
        if length < args.min_len:
            continue
        print(f"0x{r['start']:08x}-0x{r['end']:08x}  {r['mode']:9s}  len=0x{length:x}")


if __name__ == "__main__":
    sys.exit(main())
