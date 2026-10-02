"""RZ/A1H RIIC (I2C bus interface) -- plain register storage only, no I2C protocol modeled.

Register layout confirmed via `rza1.svd` (`RIIC0` base `0xFCFEE000`; `RIIC1`/
`RIIC2` are declared `derivedFrom="RIIC0"` at `0xFCFEE400`/`0xFCFEE800` -- matching
[[diode-matrix]]'s already-documented base addresses for these three real controllers, one
per notes/ic7300-hardware.md's I2C device map). One instance of this class is created per
real controller (see `board.py`).

Found via a write to `RIIC2.CR1` (`0xfcfee800`, offset `0x0`) while extending the emulator,
2026-09-08 -- `body.bin` moving on to I2C bus setup after the MTU2 timer. `RIIC2` is the
controller wired to the diode-matrix EEPROM (`IC351`, per [[diode-matrix]]), so this is the
real next step toward the roadmap's "RIIC2/EEPROM" item -- **not attempted yet**: no I2C
transaction (start condition, address match, ACK/data shifting) or EEPROM backing store is
modeled here, only the register file, so a driver that actually waits on a real transaction
completing (a status-register bit only real hardware would set) will hang the same way
`l2c.py`'s `REG7_INV_WAY` did before that fix -- watch for that specifically before assuming
this is "done" the way `mtu2.py` turned out to be.
"""

from __future__ import annotations

SIZE = 0x00000044  # covers CR1 (+0x0) .. DRR (+0x40)


class Riic:
    def __init__(self, base: int) -> None:
        self.base = base
        self.size = SIZE
        self._storage: dict[int, int] = {}

    def read(self, addr: int, size: int) -> int:
        return self._storage.get(addr, 0)

    def write(self, addr: int, size: int, value: int) -> None:
        self._storage[addr] = value & ((1 << (size * 8)) - 1)
