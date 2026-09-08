"""Generic ARMv7-A exception entry -- not IC-7300-specific, this is plain architecture
behavior (same reasoning as `core.py`'s own split from `board.py`).

Needed because Unicorn raises a bare `UC_ERR_EXCEPTION` and stops whenever the guest
executes something that traps (an `SWI`/`SVC` instruction, an undefined instruction, an
abort) -- it does *not* automatically perform the real CPU's exception-entry sequence
(bank a return address into the target mode's `LR`, save `CPSR` into that mode's `SPSR`,
switch mode/state, jump to the vector table). This module does that by hand, in a
`UC_HOOK_INTR` callback, so the guest's own vector table (already real, RAM-resident code
-- see notes/base-loader.md) takes over exactly as it would on real hardware.

Found needing this, 2026-09-08, while extending the emulator past the GIC: `body.bin`
executes a real `SWI`/`SVC` instruction (very likely FreeRTOS's own "start the first task"
mechanism, per notes/kernel-rtos.md's `swi_handler` -- not yet confirmed that specific
claim, just the general shape) partway through its own init, and Unicorn has no built-in
handling for it.

**Confirmed empirically against this exact Unicorn build** (not assumed): switching
`CPSR`'s mode bits and then reading/writing `LR`/`SPSR` via the plain `UC_ARM_REG_LR`/
`UC_ARM_REG_SPSR` constants transparently accesses the *current* mode's banked copy --
Unicorn's ARM model banks these internally, no per-mode register constants are exposed or
needed. `VBAR` is read via the `cpr_read` coprocessor-register API (`UcAArch32`, which
`Uc(UC_ARCH_ARM, ...)` already returns) rather than hardcoded, so this keeps working if
`body.bin` ever repoints it after `base.dat`'s initial write.

**Verification status, be precise about this**: only the `SWI` path (`EXCP_SWI`, Unicorn's
`intno == 2`) has actually been exercised against real firmware so far. The mode/vector/
return-address-offset table below for `UDEF`/prefetch-abort/data-abort/`IRQ`/`FIQ` is
standard ARM Architecture Reference Manual behavior (well-established, not IC-7300-
specific), included so hitting one of them doesn't hard-crash the emulator, but it has
**not** been tested against a real trap of those kinds yet -- treat the non-`SWI` rows as
"best effort, unverified" until one is actually exercised.
"""

from __future__ import annotations

from unicorn import Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_SPSR

MODE_MASK = 0x1F
T_BIT = 0x20  # Thumb state
I_BIT = 0x80  # IRQ disable
F_BIT = 0x40  # FIQ disable

MODE_UND = 0x1B
MODE_SVC = 0x13
MODE_ABT = 0x17
MODE_IRQ = 0x12
MODE_FIQ = 0x11

# EXCP_* numbering: QEMU's target/arm/cpu.h, which Unicorn's ARM core reuses directly
# (confirmed empirically: a real SWI in guest code fires this hook with intno == 2).
EXCP_UDEF = 1
EXCP_SWI = 2
EXCP_PREFETCH_ABORT = 3
EXCP_DATA_ABORT = 4
EXCP_IRQ = 5
EXCP_FIQ = 6

# (target_mode, vector_offset, lr_adjustment, sets_F_bit) -- see module docstring for the
# verification-status caveat on every row except EXCP_SWI.
_TABLE = {
    EXCP_UDEF: (MODE_UND, 0x04, 0, False),
    EXCP_SWI: (MODE_SVC, 0x08, 0, False),
    EXCP_PREFETCH_ABORT: (MODE_ABT, 0x0C, 4, False),
    EXCP_DATA_ABORT: (MODE_ABT, 0x10, 8, False),
    EXCP_IRQ: (MODE_IRQ, 0x18, 4, False),
    EXCP_FIQ: (MODE_FIQ, 0x1C, 4, True),
}


class ExceptionEntry:
    """Registers a `UC_HOOK_INTR` handler on `uc` that performs real ARM exception entry."""

    def __init__(self, uc: Uc, on_unhandled=print) -> None:
        from unicorn import UC_HOOK_INTR

        self.uc = uc
        self._on_unhandled = on_unhandled
        uc.hook_add(UC_HOOK_INTR, self._on_interrupt)

    def _read_vbar(self) -> int:
        # VBAR: coproc 15, opc1=0, CRn=12, CRm=0, opc2=0, 32-bit.
        #
        # The `el` argument below is `0`, not `1` -- confirmed empirically against this
        # exact Unicorn build, and worth flagging since it's counterintuitive: `cpr_read`'s
        # own signature names this parameter "el" ("the exception level the coprocessor
        # register belongs to"), and VBAR is an EL1 register, so `el=1` looks like the
        # obviously-correct call. It reads back 0 unconditionally, though (verified with an
        # isolated test: a guest `mcr`+`mrc` pair round-trips VBAR correctly, proving the
        # CPU model itself holds the right value -- only the *host-side* `cpr_read(..., el=1,
        # ...)` call fails to see it). `el=0` reads the real value instead. Underlying cause
        # not fully chased down (plausibly this binding's "el" argument actually selects a
        # security-state bank, not an exception level, despite its name/docstring -- but
        # that's inferred from this one observation, not confirmed against Unicorn's source).
        # Re-check this against a newer Unicorn release if it's ever upgraded.
        return self.uc.cpr_read(15, 0, 12, 0, 0, 0, False)

    def _on_interrupt(self, uc: Uc, intno: int, _user_data) -> None:
        entry = _TABLE.get(intno)
        if entry is None:
            self._on_unhandled(
                f"[exceptions] unhandled interrupt number {intno} at "
                f"pc={uc.reg_read(UC_ARM_REG_PC):#010x} -- leaving CPU state untouched, "
                "next emu_start call will likely raise UC_ERR_EXCEPTION again"
            )
            return

        target_mode, vector_offset, lr_adjust, sets_f = entry

        old_cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        return_address = uc.reg_read(UC_ARM_REG_PC) + lr_adjust

        new_cpsr = (old_cpsr & ~MODE_MASK) | target_mode
        new_cpsr &= ~T_BIT  # exception handlers always start in ARM state
        new_cpsr |= I_BIT  # every exception type masks IRQ on entry
        if sets_f:
            new_cpsr |= F_BIT

        # Order matters: switch banks first (via CPSR), then write LR/SPSR so they land
        # in the *new* mode's banked copies -- confirmed this is how Unicorn's register
        # model behaves, see module docstring.
        uc.reg_write(UC_ARM_REG_CPSR, new_cpsr)
        uc.reg_write(UC_ARM_REG_LR, return_address)
        uc.reg_write(UC_ARM_REG_SPSR, old_cpsr)

        vbar = self._read_vbar()
        uc.reg_write(UC_ARM_REG_PC, vbar + vector_offset)
