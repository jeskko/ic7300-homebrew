"""Minimal RZ/A1H OSTM (one-shot/free-running timer) model -- just enough for a polling
busy-wait to see time actually pass.

Register offsets confirmed against `/home/jvaarani/Downloads/rza1.svd` (`OSTM0` base
`0xFCFEC000`, `OSTM1` derived at `0xFCFEC400`; `CMP`=`+0x0`, `CNT`=`+0x4`, `TE`=`+0x10`,
`TS`=`+0x14`, `TT`=`+0x18`, `CTL`=`+0x20`). Found via this exact address while extending
the emulator, 2026-09-08: `FUN_2002b878`'s GPIO-pin-settle busy-wait (see gpio.py's
docstring) polls `OSTM1.CNT` (`0xfcfec404`) as a timeout guard alongside a PPR1 pin read.

This is **not** a faithful OSTM model -- real hardware counts actual elapsed bus cycles;
`CMP`/`CTL`/`TE`/`TS`/`TT` are plain read/write storage here with no real timer behavior
behind them. The one thing that matters for unblocking a polling loop like the one above
is that `CNT` visibly advances across repeated reads, so it's implemented as a counter
that jumps by a large step on every read -- enough that a `CNT < threshold` timeout guard
resolves in a handful of polls rather than needing the real hardware's full tick count
(which would mean tens of millions of instruction-heavy loop iterations in emulation).
Revisit with a real elapsed-cycle model if something ever depends on realistic timing.
"""

from __future__ import annotations

CNT_STEP = 0x00100000  # arbitrary, chosen only to resolve a polling timeout quickly


class Ostm:
    def __init__(self, base: int) -> None:
        self.base = base
        self.size = 0x24
        self._cmp = 0
        self._cnt = 0
        self._te = 0
        self._ctl = 0

    def read(self, addr: int, size: int) -> int:
        off = addr - self.base
        if off == 0x00:
            return self._cmp
        if off == 0x04:
            self._cnt = (self._cnt + CNT_STEP) & 0xFFFFFFFF
            return self._cnt
        if off == 0x10:
            return self._te
        if off == 0x20:
            return self._ctl
        return 0  # TS/TT are conventionally write-only; reading back isn't meaningful here

    def write(self, addr: int, size: int, value: int) -> None:
        off = addr - self.base
        if off == 0x00:
            self._cmp = value & 0xFFFFFFFF
        elif off == 0x14:  # TS: start
            self._te = 1
        elif off == 0x18:  # TT: stop
            self._te = 0
        elif off == 0x20:
            self._ctl = value & 0xFF
