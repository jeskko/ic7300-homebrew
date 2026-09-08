"""Wires the CPU core, flash image, and peripheral registry into one "IC-7300 main CPU"
board. Extension point for later work (swap peripheral sets, attach a GDB stub) without
touching `core.py` or `flash_image.py`.
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable

from unicorn import Uc

from . import flash_image as flash_image_mod
from .core import Cpu
from .peripherals.registry import PeripheralRegistry
from .peripherals.spi_boot import SpiBootStatus
from .peripherals.stub import StubPeripheral

RAM_BASE = 0x20000000
RAM_SIZE = 0x00A00000  # 10 MB -- notes/memory-map.md's "on-chip RAM page"

# Every documented I/O region from notes/memory-map.md's own table, mapped individually
# as MMIO. (An earlier version of this file instead mapped just the two "device/
# uncacheable" windows base.dat's own MMU translation table identity-maps -- that turned
# out to be the wrong source of truth: real hardware register pokes happen *before* the
# MMU is even enabled, e.g. base.dat's very first port/clock-enable writes at flash
# 0x18000094 target 0x3fffff80, which falls in a small I/O window the MMU table's own two
# device windows don't cover at all. memory-map.md's table is the complete, ground-truthed
# physical I/O map regardless of MMU state, so that's what this maps from.)
IO_REGIONS = [
    ("spi_status_and_neighbors", 0x3FEFA000, 0x00002000),  # 8 KB
    ("boot_gpio_pokes", 0x3FFFC000, 0x00004000),  # 16 KB -- e.g. the 0x3fffff80 write above
    ("io_e8000000", 0xE8000000, 0x00020000),  # 128 KB
    ("io_e8030000", 0xE8030000, 0x00020000),  # 128 KB
    ("io_e8100000", 0xE8100000, 0x00040000),  # 256 KB
    ("io_e8200000", 0xE8200000, 0x00030000),  # 192 KB
    ("io_fc000000", 0xFC000000, 0x00080000),  # 512 KB
    ("io_fcfe0000", 0xFCFE0000, 0x00020000),  # 128 KB -- port registers, per ic7300-signal-chain.md
    ("io_ffff0000", 0xFFFF0000, 0x00010000),  # 64 KB -- also the vector-table-at-boot location
]

# True flash address of the ARM exception vector table / CPU reset entry -- see
# notes/base-loader.md's 2026-09-08 correction (the raw file-offset math this used to
# read as 0x1800002c before the -0x2c shift was found).
RESET_VECTOR = flash_image_mod.FLASH_BASE  # 0x18000000


class Board:
    """A minimal IC-7300 main-CPU emulator: Cortex-A9 core + flash + RAM + peripherals."""

    def __init__(
        self,
        container_path: Path | str,
        trace: bool = False,
        stop_on_stub: bool = False,
    ) -> None:
        self.image = flash_image_mod.build_from_container(container_path)

        self._stub = StubPeripheral(quiet=True)
        if stop_on_stub:
            self.enable_stub_stop()
        self.registry = PeripheralRegistry(default_handler=self._stub)
        self.registry.register(
            "spi_boot_status", SpiBootStatus.base, SpiBootStatus.size, SpiBootStatus()
        )

        self._trace_log: list[int] = [] if trace else None  # type: ignore[assignment]
        self.cpu = Cpu(trace=self._on_trace if trace else None)

        self.cpu.map_rom(flash_image_mod.FLASH_BASE, bytes(self.image.data))
        self.cpu.map_ram(RAM_BASE, RAM_SIZE)
        for _name, base, size in IO_REGIONS:
            self.cpu.map_mmio(base, size, *self._mmio_callbacks(base))

        self.cpu.set_reset_state(RESET_VECTOR)

    def _mmio_callbacks(
        self, window_base: int
    ) -> tuple[Callable[[Uc, int, int, object], int], Callable[[Uc, int, int, int, object], None]]:
        def read_cb(uc: Uc, offset: int, size: int, _user_data: object) -> int:
            return self.registry.read(window_base + offset, size)

        def write_cb(uc: Uc, offset: int, size: int, value: int, _user_data: object) -> None:
            self.registry.write(window_base + offset, size, value)

        return read_cb, write_cb

    def _on_trace(self, address: int, size: int) -> None:
        self._trace_log.append(address)

    def _on_stub_access(self) -> None:
        self.cpu.uc.emu_stop()

    def enable_stub_stop(self) -> None:
        """Make the next unmodeled-peripheral access stop emulation cleanly.

        Deliberately not on by default during `base.dat`'s own already-fully-traced boot
        sequence (see notes/base-loader.md) -- its early hardware-bring-up pokes (e.g. the
        `0x3fffff80` config write hit while building this) are harmless writes the boot
        code never branches on the readback of, so letting the stub silently absorb them
        and keep going is the right behavior there. Callers doing genuinely new
        exploration (stage 2 of emu/mvp.py, or any future work past a known-traced point)
        should call this first.
        """
        self._stub._on_access = self._on_stub_access

    def run(self, until: int = 0, count: int = 0, timeout: int = 0) -> None:
        self.cpu.run(until=until, count=count, timeout=timeout)
