#!/usr/bin/env python3
"""Tiny call-graph / xref layer over dis6x output (no CFG, no data flow).

Function starts = targets of calls: CALLP, or a B/BNOP whose delay slots contain an
ADDKPC/MVKL/MVKH writing B3 (the C6x return-address register), plus B.S2 Bx calls
resolved from an MVK/MVKH pair on Bx (indirect-through-constant).  Also emits the
_c_int00 entry given on the command line.

usage:
  c6x_xref.py <dis> funcs                 list function starts with #callers
  c6x_xref.py <dis> dp <hexoff>           uses of *+B14[off/4] (DP-relative word), by function
  c6x_xref.py <dis> callers <hexaddr>     call sites of a function
  c6x_xref.py <dis> callees <hexaddr>     calls made by the function starting at addr
  c6x_xref.py <dis> func <hexaddr>        print the function containing addr (to next start)
"""
import re, sys, bisect
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from c6x_consts import LINE

def load(path):
    ins, raw = [], []
    for ln in open(path):
        m = LINE.match(ln.rstrip())
        if m:
            a, w, par, pred, mn, ops = m.groups()
            ins.append((int(a, 16), mn, ops.strip(), pred or '', bool(par)))
            raw.append(ln.rstrip())
    return ins, raw

HEX = re.compile(r'0x([0-9a-f]+)')

def calls(ins):
    """yield (site, target) ; target None if unresolved register call"""
    regval = {}
    for i, (a, mn, ops, pred, par) in enumerate(ins):
        base = mn.split('.')[0]
        if base in ('MVK', 'MVKL', 'MVKH') and ',' in ops:
            v, r = ops.rsplit(',', 1)
            try:
                v = int(v.strip(), 0) & 0xffffffff
                if base == 'MVKH': regval[r.strip()] = (regval.get(r.strip(), 0) & 0xffff) | (v & 0xffff0000)
                else: regval[r.strip()] = v
            except ValueError: pass
        if base == 'CALLP':
            m = HEX.search(ops); yield a, int(m.group(1), 16) if m else None; continue
        if base in ('B', 'BNOP'):
            # a call writes B3 within the next ~6 instructions
            window = ins[i + 1:i + 8]
            if not any(w[1].split('.')[0] in ('ADDKPC', 'MVKL', 'MVKH', 'MVK') and w[2].endswith('B3') for w in window) and \
               not ops.startswith('B3'):
                continue
            if ops.startswith('B3'): continue          # return
            m = HEX.match(ops)
            if m: yield a, int(m.group(1), 16)
            else:
                r = ops.split(',')[0].strip()
                yield a, regval.get(r)

def build(path, extra=()):
    ins, raw = load(path)
    cs = list(calls(ins))
    starts = sorted(set(t for _, t in cs if t) | set(extra))
    return ins, raw, cs, starts

def owner(starts, a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 else None

def extra_starts(path):
    """Known starts not reachable as direct calls: <dis>.starts (one hex addr per line, '#' comments)."""
    try:
        return [int(l.split('#')[0], 16) for l in open(path + '.starts') if l.split('#')[0].strip()]
    except FileNotFoundError:
        return []

if __name__ == '__main__':
    import signal; signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    path, cmd = sys.argv[1], sys.argv[2]
    ins, raw, cs, starts = build(path, extra=extra_starts(path))
    if cmd == 'funcs':
        from collections import Counter
        n = Counter(t for _, t in cs)
        for s in starts: print(f'{s:08x} callers={n[s]}')
    elif cmd == 'dp':
        off = int(sys.argv[3], 16)
        scale = {'W': 4, 'H': 2, 'B': 1}
        for (a, mn, ops, pred, par) in ins:
            m = re.search(r'\*\+B14\[(\d+)\]', ops)
            b = mn.split('.')[0]
            if m and b[:2] in ('LD', 'ST'):
                sz = scale.get(b[2:].rstrip('U')[:1], 4)
                if int(m.group(1)) * sz == off: print(f'{owner(starts,a):08x}  {a:08x} {pred}{mn} {ops}')
    elif cmd == 'callers':
        t = int(sys.argv[3], 16)
        for s, tt in cs:
            if tt == t: print(f'{owner(starts,s):08x}  site {s:08x}')
    elif cmd == 'callees':
        f = int(sys.argv[3], 16); i = starts.index(f); end = starts[i + 1] if i + 1 < len(starts) else 1 << 32
        for s, t in cs:
            if f <= s < end: print(f'site {s:08x} -> {t:08x}' if t else f'site {s:08x} -> ?')
    elif cmd == 'func':
        a = int(sys.argv[3], 16); f = owner(starts, a)
        if f is None: sys.exit(f'{a:#x}: before first known function')
        i = starts.index(f)
        end = starts[i + 1] if i + 1 < len(starts) else 1 << 32
        for (ia, *_), r in zip(ins, raw):
            if f <= ia < end: print(r)
