"""Address-range -> peripheral-handler dispatch.

The one seam this whole emulator's extensibility is built around (see emu/README.md):
every future peripheral (SCIF UART, timer, GPIO/diode matrix, RIIC2/EEPROM, ...) is added
by registering one more handler here. `emu/board.py` never needs to change to add one.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol


class PeripheralHandler(Protocol):
    def read(self, addr: int, size: int) -> int: ...
    def write(self, addr: int, size: int, value: int) -> None: ...


@dataclass
class _Entry:
    name: str
    base: int
    size: int
    handler: PeripheralHandler


class PeripheralRegistry:
    """Dispatches MMIO accesses to whichever registered handler claims the address.

    Falls back to `default_handler` (normally `peripherals.stub.StubPeripheral`) for any
    address nothing has claimed yet -- see `stub.py` for why that's a safe default rather
    than a crash.
    """

    def __init__(self, default_handler: PeripheralHandler) -> None:
        self._entries: list[_Entry] = []
        self._default = default_handler

    def register(self, name: str, base: int, size: int, handler: PeripheralHandler) -> None:
        for e in self._entries:
            if base < e.base + e.size and e.base < base + size:
                raise ValueError(
                    f"peripheral {name!r} ({base:#x}+{size:#x}) overlaps "
                    f"already-registered {e.name!r} ({e.base:#x}+{e.size:#x})"
                )
        self._entries.append(_Entry(name, base, size, handler))

    def _find(self, addr: int) -> PeripheralHandler:
        for e in self._entries:
            if e.base <= addr < e.base + e.size:
                return e.handler
        return self._default

    def read(self, addr: int, size: int) -> int:
        return self._find(addr).read(addr, size)

    def write(self, addr: int, size: int, value: int) -> None:
        self._find(addr).write(addr, size, value)
