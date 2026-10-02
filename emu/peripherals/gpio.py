"""RZ/A1H GPIO/port register block.

Register offsets below are ground-truthed two independent ways: `rza1.svd`
(the RZ/A1H SVD this project's Ghidra project also imports, per README.md's "RZ/A1H peripheral SVD"
bullet) gives every register's exact address, and `notes/ic7300-signal-chain.md`'s own
independently-derived formula (`PORTn_base=0xFCFE3000`, per-register-type strides `+0x100`/`+0x200`/...,
`+n*4` per port) matches the SVD numbers exactly. The *roles* of P/PM/PMC/PPR/PIBC are confirmed by
directly decompiling real `body.bin` code (see notes/ic7300-signal-chain.md's `DRESD`/`P2` section, and
`FUN_2002b878` -- traced 2026-09-08 while extending this emulator, see emu/README.md): P is the output
data latch, PM is direction (1=input/0=output), PMC selects GPIO vs. alternate function, PPR reads the
real pin level, PIBC enables the digital input buffer for a pin (confirmed by `FUN_2002b878`'s exact
"set PIBC1 bit, wait, restore PIBC1" pattern around a PPR1 read).

The *exact* read/write semantics of PSR/PMSR/PMCSR/PNOT are **not** independently confirmed (this
project's manual excerpts and the SVD don't include prose descriptions, only names+offsets) -- what's
implemented below is a structurally well-motivated inference, not a confirmed fact: PSR/PMSR/PMCSR are
each a 32-bit register (per the SVD) paired with a 16-bit plain register (P/PM/PMC respectively) of half
the width -- the classic Renesas/ARM "masked set/clear" register shape (lower 16 bits = bits to set to 1,
upper 16 bits = bits to clear to 0), which is what's implemented here. PNOT (plain 16-bit, "notification")
is implemented as a toggle-on-write. Flag these four for correction if real hardware ever demonstrates
different behavior (JTAG would settle it in one read/write test).

PFC/PFCE/PFCAE (alternate-function select) and PBDC/PIPC (bidirectional/pull-up control) are implemented
as plain read/write storage with no side effects -- nothing in this emulator yet depends on their exact
role, and no confirmed source describes their bit semantics.
"""

from __future__ import annotations

PORT_BASE = 0xFCFE3000
IBC_BASE = 0xFCFE7000  # PORT_BASE + 0x4000, per the SVD/notes formula

# Which ports actually have which register (confirmed via the SVD -- not every port has every
# register, e.g. port 0 has no plain P/PM/PFC/... registers at all, only PPR0/PMC0/PIBC0/PMCSR0).
PORTS_WITH_P = range(1, 12)  # P1..P11
PORTS_WITH_PPR = range(0, 12)  # PPR0..PPR11
PORTS_WITH_PMC = range(0, 12)  # PMC0..PMC11
PORTS_WITH_PMCSR = range(0, 12)  # PMCSR0..PMCSR11
PORTS_WITH_PIBC = range(0, 12)  # PIBC0..PIBC11
PORTS_WITH_OTHER = range(1, 12)  # PM/PSR/PMSR/PFC/PFCE/PFCAE/PNOT/PBDC/PIPC: 1..11 only

# (register_name_prefix, base_offset_from_PORT_BASE, valid_ports, width_bytes)
_PLAIN_16 = [
    ("P", 0x000, PORTS_WITH_P),
    ("PPR", 0x200, PORTS_WITH_PPR),
    ("PM", 0x300, PORTS_WITH_OTHER),
    ("PMC", 0x400, PORTS_WITH_PMC),
    ("PFC", 0x500, PORTS_WITH_OTHER),
    ("PFCE", 0x600, PORTS_WITH_OTHER),
    ("PNOT", 0x700, PORTS_WITH_OTHER),
    ("PFCAE", 0xA00, PORTS_WITH_OTHER),
]
_SET_CLEAR_32 = [
    ("PSR", 0x100, PORTS_WITH_OTHER, "P"),
    ("PMSR", 0x800, PORTS_WITH_OTHER, "PM"),
    ("PMCSR", 0x900, PORTS_WITH_PMCSR, "PMC"),
]
_IBC_PLAIN_16 = [
    ("PIBC", 0x000, PORTS_WITH_PIBC),
    ("PBDC", 0x100, PORTS_WITH_OTHER),
    ("PIPC", 0x200, PORTS_WITH_OTHER),
]

SNCR_ADDR = PORT_BASE + 0xC00


class GpioBlock:
    """Handles the whole `PORT_BASE`/`IBC_BASE` register cluster as one peripheral.

    Registered as a single, fairly wide MMIO claim (see `board.py`) rather than one entry per
    register -- there are well over a hundred of these -- with an internal address decode built
    from the tables above.
    """

    def __init__(self) -> None:
        self._p: dict[int, int] = {n: 0 for n in PORTS_WITH_P}
        self._pm: dict[int, int] = {n: 0xFFFF for n in PORTS_WITH_OTHER}  # reset: all input
        self._pmc: dict[int, int] = {n: 0 for n in PORTS_WITH_PMC}  # reset: plain GPIO
        self._pin_level: dict[int, int] = {n: 0 for n in PORTS_WITH_PPR}  # simulated pin state
        self._storage: dict[int, int] = {}  # every other plain register, keyed by absolute addr

        self._by_addr: dict[int, tuple[str, int]] = {}
        for prefix, off, ports in _PLAIN_16:
            for n in ports:
                self._by_addr[PORT_BASE + off + n * 4] = (prefix, n)
        for prefix, off, ports, _target in _SET_CLEAR_32:
            for n in ports:
                self._by_addr[PORT_BASE + off + n * 4] = (prefix, n)
        for prefix, off, ports in _IBC_PLAIN_16:
            for n in ports:
                self._by_addr[IBC_BASE + off + n * 4] = (prefix, n)
        self._by_addr[SNCR_ADDR] = ("SNCR", None)

        self._set_clear_targets = {name: target for name, _off, _ports, target in _SET_CLEAR_32}

    def set_pin_level(self, port: int, bit: int, value: bool) -> None:
        """Test/exploration hook: simulate an external signal on a PPR-readable pin.

        Not used by anything yet -- exposed so a future test scenario (a boot-strap pin, a jack
        detect line, ...) can drive a specific PPR bit without needing real hardware.
        """
        cur = self._pin_level.get(port, 0)
        mask = 1 << bit
        self._pin_level[port] = (cur | mask) if value else (cur & ~mask)

    def read(self, addr: int, size: int) -> int:
        entry = self._by_addr.get(addr)
        if entry is None:
            return 0  # unassigned register within this block's claimed range -- reserved/unused
        name, n = entry

        if name == "P":
            return self._p[n]
        if name == "PPR":
            return self._pin_level[n]
        if name == "PM":
            return self._pm[n]
        if name == "PMC":
            return self._pmc[n]
        if name in self._set_clear_targets:
            # Write-oriented registers; reading back isn't independently confirmed to mean
            # anything in particular, so just mirror the underlying target register.
            target = self._set_clear_targets[name]
            return self._read_named(target, n)
        return self._storage.get(addr, 0)

    def write(self, addr: int, size: int, value: int) -> None:
        entry = self._by_addr.get(addr)
        if entry is None:
            return
        name, n = entry

        if name == "P":
            self._p[n] = value & 0xFFFF
        elif name == "PPR":
            pass  # read-only real pin level; ignore writes
        elif name == "PM":
            self._pm[n] = value & 0xFFFF
        elif name == "PMC":
            self._pmc[n] = value & 0xFFFF
        elif name == "PNOT":
            self._p[n] = self._p[n] ^ (value & 0xFFFF)
        elif name in self._set_clear_targets:
            self._apply_set_clear(self._set_clear_targets[name], n, value)
        else:
            self._storage[addr] = value & ((1 << (size * 8)) - 1)

    def _read_named(self, name: str, n: int) -> int:
        if name == "P":
            return self._p[n]
        if name == "PM":
            return self._pm[n]
        if name == "PMC":
            return self._pmc[n]
        raise AssertionError(name)  # pragma: no cover -- exhaustive over _SET_CLEAR_32's targets

    def _apply_set_clear(self, target: str, n: int, value: int) -> None:
        set_bits = value & 0xFFFF
        clear_bits = (value >> 16) & 0xFFFF
        if target == "P":
            self._p[n] = (self._p[n] | set_bits) & ~clear_bits & 0xFFFF
        elif target == "PM":
            self._pm[n] = (self._pm[n] | set_bits) & ~clear_bits & 0xFFFF
        elif target == "PMC":
            self._pmc[n] = (self._pmc[n] | set_bits) & ~clear_bits & 0xFFFF
