"""RZ/A1H MTU2 (Multi-Function Timer Pulse Unit 2) -- plain register storage only.

Base address and the full 79-register layout (5 channels, `TCR`/`TMDR`/`TIOR`/`TIER`/`TSR`/
`TCNT`/`TGRx` per channel plus shared `TSTR`/`TSYR`/etc.) confirmed via `rza1.svd`
(`MTU2` base `0xFCFF0000`). Found via a write to `TCR_3` (`0xfcff0200`, channel 3's control
register) while extending the emulator, 2026-09-08, right after fixing the L2 cache
controller's `REG7_INV_WAY` hang (see `l2c.py`) -- `body.bin` moves on to configuring this
timer next.

**Plain read/write storage for every register, no behavior modeled at all** -- unlike
`ostm.py`, nothing here auto-increments or self-clears. This is a deliberate choice, not an
oversight: nothing here autonomously sets a status/compare-match flag the way real hardware
would, so there's no risk of an self-inflicted "poll a register we wrote a nonzero value to
and it never seems to clear" hang like `l2c.py`'s did -- a poll waiting on a *hardware-set*
bit (e.g. a `TSR_n` compare-match flag) would still hang here, since nothing sets one. Revisit
if that turns out to matter (a real `TCNT` free-run + `TGR` compare-match model, similar in
spirit to `ostm.py`).
"""

from __future__ import annotations

BASE = 0xFCFF0000
SIZE = 0x00000400  # covers every channel's registers (highest documented offset: 0x390)


class Mtu2:
    base = BASE
    size = SIZE

    def __init__(self) -> None:
        self._storage: dict[int, int] = {}

    def read(self, addr: int, size: int) -> int:
        return self._storage.get(addr, 0)

    def write(self, addr: int, size: int, value: int) -> None:
        self._storage[addr] = value & ((1 << (size * 8)) - 1)
