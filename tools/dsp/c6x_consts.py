#!/usr/bin/env python3
"""Recover 32-bit constants built by MVK/MVKL + MVKH pairs in dis6x output.

usage: c6x_consts.py <file.dis> [lo hi]   -> prints "insn_addr reg value" for every
MVKH completing a constant (value in [lo,hi] if given, hex).  Pairing is by
destination register within a 16-instruction window (the compiler keeps them close;
a stray pairing across a branch target is possible, so treat hits as leads).
"""
import re, sys

LINE = re.compile(r'^([0-9a-f]{8})\s+([0-9a-f]{4,8})\s+(\|\|)?\s*(\[!?[AB]\d+\]\s+)?(\S+)\s*(.*)$')

def parse(path):
    out = []
    for ln in open(path):
        m = LINE.match(ln.rstrip())
        if m:
            a, _, par, pred, mn, ops = m.groups()
            out.append((int(a, 16), mn, ops.strip(), pred))
    return out

def consts(insns):
    last = {}  # reg -> (index, low16 signed-extended value)
    for i, (a, mn, ops, pred) in enumerate(insns):
        base = mn.split('.')[0]
        if base in ('MVK', 'MVKL') and ',' in ops:
            v, r = [x.strip() for x in ops.rsplit(',', 1)]
            try: last[r] = (i, int(v, 0) & 0xffffffff)
            except ValueError: pass
        elif base == 'MVKH' and ',' in ops:
            v, r = [x.strip() for x in ops.rsplit(',', 1)]
            try: hi = int(v, 0) & 0xffff0000
            except ValueError: continue
            if r in last and i - last[r][0] <= 16:
                yield a, r, hi | (last[r][1] & 0xffff)
            else:
                yield a, r, hi   # MVKH alone (low half unknown/zero)

if __name__ == '__main__':
    ins = parse(sys.argv[1])
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xffffffff
    for a, r, v in consts(ins):
        if lo <= v <= hi: print(f'{a:08x} {r:4} {v:08x}')
