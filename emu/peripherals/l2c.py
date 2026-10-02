"""ARM PL310-style L2 cache controller (RZ/A1H's `L2C`).

Register names/offsets confirmed via `rza1.svd` (`L2C` base `0x3ffff000`) --
the same SVD `gpio.py`/`cpg.py`/`gic.py` use. This is a standard ARM peripheral (PL310),
not an Icom-specific design, unlike the vendor port/clock registers elsewhere in this
project.

Found via `REG0_CACHE_ID` (`0x3ffff000`) while extending the emulator, 2026-09-08 -- the
first read `body.bin` makes here, right after clearing its GIC/exception-entry setup
(see emu/README.md's Status section), consistent with a typical L2 cache controller
bring-up sequence (identify -> configure `REG1_AUX_CONTROL` -> enable via `REG1_CONTROL`).

**Confirmed real bug found and fixed the same session**: that bring-up sequence continues
into `REG7_INV_WAY` (`0x3ffff77c`, confirmed via a direct read of the two literal-pool
pointers the code uses, `0x200b9384`/`0x200b9388` -- not a guess), writing a "which ways
to invalidate" bitmask and then polling the *same register* until it reads back `0` (real
PL310 hardware self-clears each way's bit as its invalidation completes). Modeling this
register as plain storage -- this module's first version -- made that poll loop truly
infinite (whatever was last written stays forever), the emulator's first real hang. Fixed
by treating every `REG7_*` cache-maintenance-operation register (`CACHE_SYNC`/`INV_PA`/
`INV_WAY`/`CLEAN_*`) as "write triggers the operation, which completes instantly and
reads back `0`" -- architecturally correct completion semantics (real hardware just isn't
instant), not a hack specific to this one register.

**Not a faithful cache model** beyond that: no actual caching behavior, and
`REG1_CONTROL`'s enable bit isn't backed by anything. `REG0_CACHE_ID`/`REG0_CACHE_TYPE`
return real architected PL310 constants (a fixed implementer+part-number ID, and a
plausible cache-geometry type value) rather than zero, since an identification read is the
one thing a driver might plausibly branch on; every other non-`REG7` register is plain
read/write storage.
"""

from __future__ import annotations

BASE = 0x3FFFF000
SIZE = 0x00001000

_CACHE_ID_OFFSET = 0x000
_CACHE_TYPE_OFFSET = 0x004

# The REG7 cache-maintenance-operation block (CACHE_SYNC/INV_PA/INV_WAY/CLEAN_*/
# CLEAN_INV_*) -- see module docstring for why these self-clear instead of holding state.
_REG7_RANGE = range(0x700, 0x800)

# Standard ARM PL310 architected values (implementer ARM=0x41, part number 0x0C8/0x9 for
# PL310, a real-world revision suffix) -- not IC-7300-specific, just "a real PL310's ID".
CACHE_ID = 0x410000C8
CACHE_TYPE = 0x1C100100


class L2CacheController:
    base = BASE
    size = SIZE

    def __init__(self) -> None:
        self._storage: dict[int, int] = {}

    def read(self, addr: int, size: int) -> int:
        off = addr - self.base
        if off == _CACHE_ID_OFFSET:
            return CACHE_ID
        if off == _CACHE_TYPE_OFFSET:
            return CACHE_TYPE
        if off in _REG7_RANGE:
            return 0  # operation always "already complete" -- see module docstring
        return self._storage.get(addr, 0)

    def write(self, addr: int, size: int, value: int) -> None:
        off = addr - self.base
        if off in (_CACHE_ID_OFFSET, _CACHE_TYPE_OFFSET):
            return  # read-only
        if off in _REG7_RANGE:
            return  # triggers an operation that completes instantly; nothing to store
        self._storage[addr] = value & 0xFFFFFFFF
