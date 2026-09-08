"""RZ/A1H CPG (Clock Pulse Generator) register file.

Register names, offsets, and widths confirmed via `~/Downloads/rza1.svd` (the same SVD
`gpio.py` uses, and this project's Ghidra project imports per README.md's "RZ/A1H
peripheral SVD" bullet). `CPG` base `0xFCFE0010`, plus a separate deep-standby-related
cluster at `0xFCFF1800` (`RRAMKP`/`DSCTR`/`DSSSR`/`DSESR`/`DSFR`/`XTALCTR`).

Found via `STBCR5` (`0xfcfe0428`) while extending the emulator, 2026-09-08: the same
`FUN_2002b878` GPIO-pin-settle routine (see `gpio.py`/`ostm.py`'s docstrings) clears
`STBCR5` bit 0 (module clock out of standby) before its work and sets it back to 1
(standby again) afterward -- a plain, unconditional read-modify-write, never branched on.

Everything here is **plain read/write storage, all registers reset to `0`** -- no clock-
gating, PLL-ratio, or reset-control behavior is modeled. Real hardware almost certainly
resets some of these (`FRQCR`/`FRQCR2` especially -- the actual boot-time clock ratio) to
a non-zero value, but nothing in the boot path traced so far reads one of these registers
and branches on the result, so a real reset-value survey wasn't done. Revisit if that
changes.
"""

from __future__ import annotations

MAIN_CLUSTER_BASE = 0xFCFE0000
MAIN_CLUSTER_SIZE = 0x00000500  # covers FRQCR (0xfcfe0010) .. STBCR13 (0xfcfe0470)

DEEP_STANDBY_CLUSTER_BASE = 0xFCFF1800
DEEP_STANDBY_CLUSTER_SIZE = 0x00000020  # covers RRAMKP .. XTALCTR (0xfcff1810)

_REGISTERS = {
    # name: (absolute_address, size_bytes)
    "FRQCR": (0xFCFE0010, 2),
    "FRQCR2": (0xFCFE0014, 2),
    "CPUSTS": (0xFCFE0018, 1),
    "STBCR1": (0xFCFE0020, 1),
    "STBCR2": (0xFCFE0024, 1),
    "STBREQ1": (0xFCFE0030, 1),
    "STBREQ2": (0xFCFE0034, 1),
    "STBACK1": (0xFCFE0040, 1),
    "STBACK2": (0xFCFE0044, 1),
    "SYSCR1": (0xFCFE0400, 1),
    "SYSCR2": (0xFCFE0404, 1),
    "SYSCR3": (0xFCFE0408, 1),
    "STBCR3": (0xFCFE0420, 1),
    "STBCR4": (0xFCFE0424, 1),
    "STBCR5": (0xFCFE0428, 1),
    "STBCR6": (0xFCFE042C, 1),
    "STBCR7": (0xFCFE0430, 1),
    "STBCR8": (0xFCFE0434, 1),
    "STBCR9": (0xFCFE0438, 1),
    "STBCR10": (0xFCFE043C, 1),
    "STBCR11": (0xFCFE0440, 1),
    "STBCR12": (0xFCFE0444, 1),
    "SWRSTCR1": (0xFCFE0460, 1),
    "SWRSTCR2": (0xFCFE0464, 1),
    "SWRSTCR3": (0xFCFE0468, 1),
    "STBCR13": (0xFCFE0470, 1),
    "RRAMKP": (0xFCFF1800, 1),
    "DSCTR": (0xFCFF1802, 1),
    "DSSSR": (0xFCFF1804, 2),
    "DSESR": (0xFCFF1806, 2),
    "DSFR": (0xFCFF1808, 2),
    "XTALCTR": (0xFCFF1810, 1),
}


class Cpg:
    def __init__(self) -> None:
        self._by_addr = {addr: name for name, (addr, _size) in _REGISTERS.items()}
        self._storage: dict[int, int] = {}

    def name_for(self, addr: int) -> str:
        """Named register at `addr`, or a bare hex label if it's an unassigned address
        within this peripheral's claimed range (reserved/unused, per the SVD)."""
        return self._by_addr.get(addr, f"CPG+{addr:#06x}")

    def read(self, addr: int, size: int) -> int:
        return self._storage.get(addr, 0)

    def write(self, addr: int, size: int, value: int) -> None:
        self._storage[addr] = value & ((1 << (size * 8)) - 1)
