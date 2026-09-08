"""Workaround for a real Unicorn/QEMU Cortex-A9 model gap: the ARMv7 "hint" instructions
(`NOP`/`YIELD`/`WFE`/`WFI`/`SEV`) aren't all implemented.

Confirmed with an isolated test independent of this project's firmware: executing a bare
`WFE` (`e320f002`) on a fresh `Uc(UC_ARCH_ARM, ...)` with `UC_CPU_ARM_CORTEX_A9` selected
raises `UC_ERR_INSN_INVALID`, while `NOP` (`e320f000`) on the same setup executes fine. So
this genuinely is a CPU-model gap, not a mode/address bug on this project's side -- found
via `body.bin` executing a real `WFE` at `0x200b93a8` (very plausibly a spinlock/idle-wait
primitive) while extending the emulator, 2026-09-08.

Real hardware is *always* permitted to treat `WFE`/`WFI`/`YIELD`/`SEV` as a plain `NOP` per
the ARM architecture (a conforming implementation need not actually stall or signal
anything) -- so skipping the instruction (advancing `PC` past it, changing nothing else) is
architecturally valid, not a hack specific to this emulator's needs. Registered as a
`UC_HOOK_INSN_INVALID` handler: only instructions matching a known hint encoding are
skipped this way; anything else still propagates as a real error.
"""

from __future__ import annotations

from unicorn import Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_PC

_T_BIT = 0x20

# ARM encoding: 0xe320f00N for N in {0 (NOP), 1 (YIELD), 2 (WFE), 3 (WFI), 4 (SEV)}.
_ARM_HINT_PREFIX = 0xE320F000
_ARM_HINT_MASK = 0xFFFFFFF8

# Thumb (16-bit) encoding: 0xbf_0 for the same five hints, upper nibble of the low byte
# selects which one; condition/hint field must be 0 for these (vs. a real conditional IT).
_THUMB_HINT_PREFIX = 0xBF00
_THUMB_HINT_MASK = 0xFFF8


def install(uc: Uc, on_skipped=None) -> None:
    """Register the workaround on `uc`. `on_skipped`, if given, is called with the faulting
    PC every time an instruction actually gets skipped -- purely informational."""
    from unicorn import UC_HOOK_INSN_INVALID

    def _on_invalid(uc: Uc, _user_data) -> bool:
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = bool(uc.reg_read(UC_ARM_REG_CPSR) & _T_BIT)

        if thumb:
            halfword = int.from_bytes(uc.mem_read(pc, 2), "little")
            if (halfword & _THUMB_HINT_MASK) != _THUMB_HINT_PREFIX:
                return False
            uc.reg_write(UC_ARM_REG_PC, pc + 2)
        else:
            word = int.from_bytes(uc.mem_read(pc, 4), "little")
            if (word & _ARM_HINT_MASK) != _ARM_HINT_PREFIX:
                return False
            uc.reg_write(UC_ARM_REG_PC, pc + 4)

        if on_skipped is not None:
            on_skipped(pc)
        return True

    uc.hook_add(UC_HOOK_INSN_INVALID, _on_invalid)
