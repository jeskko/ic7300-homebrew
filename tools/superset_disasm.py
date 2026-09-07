#!/usr/bin/env python3
"""
Superset (shingled) disassembly of a raw ARM firmware image, in both ARM and
Thumb mode, persisted to SQLite for cheap offline search.

Why this exists
----------------
arm_thumb_scan.py (same directory) already classifies windows as ARM/Thumb/
ambiguous using objdump's bad-instruction-count heuristic, but only keeps the
verdict, not the instructions themselves -- you can't grep it, and it can't
resolve a PC-relative literal load to the string/data address it targets.

This script instead tries to decode EVERY address independently in EACH mode
(every 4-byte-aligned address in ARM mode, every 2-byte-aligned address in
Thumb mode), regardless of what Ghidra currently believes is code/data/mode
there, and stores every decode attempt (valid or not) in a queryable table.
This is the standard "superset/shingled disassembly" technique for resolving
ARM/Thumb ambiguity in a blob with no embedded mode info -- decoding each
candidate address on its own (rather than one continuous linear sweep) means
a bad decode never desyncs a later, unrelated region the way objdump's own
continuous -D sweep can.

This is a ground-truth locator, same spirit as arm_thumb_scan.py: it never
touches the live Ghidra project. Use it to grep/query for real fix
candidates and literal-pool xrefs, then apply any fix by hand in the GUI.

Usage
-----
    python3 superset_disasm.py scratch/unpacked/142/body.bin \
        --base 0x20005000 --out scratch/superset_142.sqlite

    # cheap searches afterwards, e.g.:
    sqlite3 scratch/superset_142.sqlite \
        "select hex(addr), mnemonic, op_str from thumb_insns where target=X'2035a010'"
    sqlite3 scratch/superset_142.sqlite \
        "select addr, mode, mnemonic, op_str from insns
         where mnemonic like 'ldr%' and op_str like '%pc%' limit 20"

Requires python-capstone (`pacman -S python-capstone` on Arch/CachyOS).
"""
import argparse
import sqlite3
import sys

import capstone

SCHEMA = """
CREATE TABLE insns (
    addr    INTEGER NOT NULL,
    mode    TEXT NOT NULL CHECK(mode IN ('arm','thumb')),
    valid   INTEGER NOT NULL,
    size    INTEGER,
    mnemonic TEXT,
    op_str  TEXT,
    bytes_hex TEXT,
    target  INTEGER,      -- resolved PC-relative literal-load target, if any
    PRIMARY KEY (addr, mode)
);
CREATE INDEX idx_insns_target ON insns(target);
CREATE INDEX idx_insns_mnemonic ON insns(mnemonic);
CREATE INDEX idx_insns_mode_valid ON insns(mode, valid);
"""


def resolve_pc_literal(addr, mode, mnemonic, op_str, size):
    """Resolve `ldr Rd, [pc, #imm]`-style literal loads to their target
    address. Returns None if this instruction isn't a PC-relative literal
    load we know how to resolve (covers the dominant string/data-xref case;
    movw/movt absolute-load idioms are a separate pattern, not handled here).
    """
    if not mnemonic.startswith("ldr"):
        return None
    if "[pc" not in op_str and ", pc," not in op_str and "pc]" not in op_str:
        return None
    # op_str looks like "r0, [pc, #0x123]" or "r0, [pc, #-0x123]"
    import re
    m = re.search(r"\[pc,\s*#(-?0x[0-9a-fA-F]+|-?\d+)\]", op_str)
    if not m:
        return None
    imm = int(m.group(1), 0)
    # ARM: PC reads as addr+8, word-aligned (always is, ARM is 4-aligned).
    # Thumb: PC reads as addr+4, then aligned down to a 4-byte boundary
    # before adding the immediate (per the ARM ARM's Thumb LDR-literal spec).
    if mode == "arm":
        base = addr + 8
    else:
        base = (addr + 4) & ~3
    return base + imm


def scan_mode(data, base, mode_flag, mode_name, step, conn):
    md = capstone.Cs(capstone.CS_ARCH_ARM, mode_flag)
    md.detail = False
    rows = []
    n = len(data)
    for off in range(0, n, step):
        addr = base + off
        chunk = data[off:off + 4]
        if not chunk:
            continue
        decoded = list(md.disasm_lite(chunk, addr))
        if decoded:
            a, size, mnemonic, op_str = decoded[0]
            target = resolve_pc_literal(a, mode_name, mnemonic, op_str, size)
            rows.append((a, mode_name, 1, size, mnemonic, op_str,
                         chunk[:size].hex(), target))
        else:
            rows.append((addr, mode_name, 0, None, None, None,
                         chunk.hex(), None))
        if len(rows) >= 20000:
            conn.executemany(
                "INSERT INTO insns VALUES (?,?,?,?,?,?,?,?)", rows)
            rows = []
    if rows:
        conn.executemany("INSERT INTO insns VALUES (?,?,?,?,?,?,?,?)", rows)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binfile")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x20005000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--start", type=lambda s: int(s, 0), default=None,
                     help="restrict scan to [start,end) absolute addresses (default: whole file)")
    ap.add_argument("--end", type=lambda s: int(s, 0), default=None)
    args = ap.parse_args()

    with open(args.binfile, "rb") as f:
        data = f.read()

    lo = 0 if args.start is None else args.start - args.base
    hi = len(data) if args.end is None else args.end - args.base
    lo = max(0, lo)
    hi = min(len(data), hi)
    data = data[lo:hi]
    base = args.base + lo

    conn = sqlite3.connect(args.out)
    conn.executescript(SCHEMA)

    print(f"scanning {len(data)} bytes @ base 0x{base:08x} ...", file=sys.stderr)
    print("  ARM pass (4-byte aligned)...", file=sys.stderr)
    scan_mode(data, base, capstone.CS_MODE_ARM, "arm", 4, conn)
    conn.commit()
    print("  Thumb pass (2-byte aligned)...", file=sys.stderr)
    scan_mode(data, base, capstone.CS_MODE_THUMB, "thumb", 2, conn)
    conn.commit()

    n_arm = conn.execute("SELECT COUNT(*) FROM insns WHERE mode='arm'").fetchone()[0]
    n_thumb = conn.execute("SELECT COUNT(*) FROM insns WHERE mode='thumb'").fetchone()[0]
    n_targets = conn.execute("SELECT COUNT(*) FROM insns WHERE target IS NOT NULL").fetchone()[0]
    print(f"done: {n_arm} ARM candidates, {n_thumb} Thumb candidates, "
          f"{n_targets} resolved PC-relative literal loads -> {args.out}", file=sys.stderr)
    conn.close()


if __name__ == "__main__":
    sys.exit(main())
