"""Minimal ARM GIC (Generic Interrupt Controller) model -- Distributor + CPU Interface.

Base addresses are already-confirmed ground truth from this project's own prior sessions, not
new derivation: `notes/memory-map.md` pins the Distributor at `0xE8201000` (`gic_distributor_disable`
directly dereferences it) and `notes/kernel-rtos.md` pins the CPU Interface at `0xE8202000`
(`INTC_ICCIAR_ADDR = 0xE820200C` / `INTC_ICCEOIR_ADDR = 0xE8202010`, matched against the real
Renesas FreeRTOS port in `scratch/r01an5093ej0170-rza1-swpkg/`). Register offsets within each
(`ICDDCR`/`ICDICTR`/`ICDIIDR`/... and `ICCICR`/`ICCPMR`/`ICCIAR`/`ICCEOIR`/...) are the standard
ARM GICv1/PL390 layout -- also confirmed present in `rza1.svd`'s `INTC` peripheral.

Found via `gic_distributor_disable`/`FUN_200b848c` (already-decompiled, real functions -- see
notes/memory-map.md) while extending the emulator, 2026-09-08: distributor init reads `ICDICTR`
to size a configuration loop over `ITLinesNumber` (bits 4:0, encoding `(N+1)*32` interrupt lines).

**Not a faithful GIC**: no real interrupt delivery, pending/active state, or priority arbitration
is modeled. `ICDICTR`'s `ITLinesNumber` is a plausible placeholder (`6`, i.e. 224 lines -- a
reasonable superset of a real RZ/A1H's interrupt count, not independently confirmed from the
manual) chosen only so the real distributor-init loop's bound is sane; everything else is plain
read/write storage, except `ICCIAR` (CPU Interface Interrupt Acknowledge Register), which always
returns the architected "no interrupt pending" spurious ID (`1023`) since nothing here ever
raises one -- the architecturally correct response for a system with zero active interrupt
sources, not a guess.
"""

from __future__ import annotations

DISTRIBUTOR_BASE = 0xE8201000
DISTRIBUTOR_SIZE = 0x00001000

CPU_INTERFACE_BASE = 0xE8202000
CPU_INTERFACE_SIZE = 0x00001000

_ICDICTR_OFFSET = 0x004
IT_LINES_NUMBER = 6  # -> (6+1)*32 = 224 lines; see module docstring

_ICCIAR_OFFSET = 0x00C
SPURIOUS_INTERRUPT_ID = 0x3FF  # 1023, architected "no pending interrupt" response


class GicDistributor:
    base = DISTRIBUTOR_BASE
    size = DISTRIBUTOR_SIZE

    def __init__(self) -> None:
        self._storage: dict[int, int] = {}

    def read(self, addr: int, size: int) -> int:
        off = addr - self.base
        if off == _ICDICTR_OFFSET:
            return IT_LINES_NUMBER & 0x1F
        return self._storage.get(addr, 0)

    def write(self, addr: int, size: int, value: int) -> None:
        off = addr - self.base
        if off == _ICDICTR_OFFSET:
            return  # read-only
        self._storage[addr] = value & ((1 << (size * 8)) - 1)


class GicCpuInterface:
    base = CPU_INTERFACE_BASE
    size = CPU_INTERFACE_SIZE

    def __init__(self) -> None:
        self._storage: dict[int, int] = {}

    def read(self, addr: int, size: int) -> int:
        off = addr - self.base
        if off == _ICCIAR_OFFSET:
            return SPURIOUS_INTERRUPT_ID
        return self._storage.get(addr, 0)

    def write(self, addr: int, size: int, value: int) -> None:
        off = addr - self.base
        if off == _ICCIAR_OFFSET:
            return  # read-only
        self._storage[addr] = value & ((1 << (size * 8)) - 1)
