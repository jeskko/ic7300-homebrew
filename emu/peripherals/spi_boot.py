"""The one real peripheral the MVP needs: the SPI multi-I/O status register `base.dat`
polls before reading flash.

Address and bit confirmed 2026-09-08 by direct `arm-none-eabi-objdump` disassembly of the
real `base.dat` (see notes/base-loader.md's boot-sequence step 2): a tight loop re-reads
this register and masks+shifts out bit 0, looping until it reads 1. Always reporting
"ready" is sufficient for the MVP -- no real SPI transaction timing needs modeling to
reach body.bin's entry point.
"""

from __future__ import annotations

BASE = 0x3FEFA048
SIZE = 4
READY_BIT = 0x1


class SpiBootStatus:
    base = BASE
    size = SIZE

    def read(self, addr: int, size: int) -> int:
        return READY_BIT

    def write(self, addr: int, size: int, value: int) -> None:
        pass  # base.dat's boot-time poll loop never writes here; ignore if something does
