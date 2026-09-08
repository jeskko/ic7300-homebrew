"""Thin wrapper around Unicorn: CPU creation, memory mapping, run control.

Nothing IC-7300-specific lives here -- that's `board.py`'s job. Keeping this split is
what lets `board.py` (or a future second board) evolve independently of the underlying
emulation engine.
"""

from __future__ import annotations

from typing import Callable

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_PC, UC_CPU_ARM_CORTEX_A9

# ARM state, SVC mode, IRQ+FIQ masked -- the documented Cortex-A reset convention.
CPSR_RESET = 0x000000D3


class Cpu:
    def __init__(self, trace: Callable[[int, int], None] | None = None) -> None:
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.cpu_model_selected = True
        try:
            self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
        except Exception as exc:  # pragma: no cover -- environment-dependent fallback
            self.cpu_model_selected = False
            print(
                f"[core] warning: could not select Cortex-A9 CPU model ({exc}); "
                "falling back to Unicorn's default ARM model -- see emu/README.md's "
                "open-risks section"
            )

        if trace is not None:
            self.uc.hook_add(UC_HOOK_CODE, lambda uc, addr, size, _ud: trace(addr, size))

    def map_rom(self, address: int, data: bytes) -> None:
        from unicorn import UC_PROT_EXEC, UC_PROT_READ

        self.uc.mem_map(address, len(data), UC_PROT_READ | UC_PROT_EXEC)
        self.uc.mem_write(address, data)

    def map_ram(self, address: int, size: int) -> None:
        from unicorn import UC_PROT_EXEC, UC_PROT_READ, UC_PROT_WRITE

        self.uc.mem_map(address, size, UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC)

    def map_mmio(
        self,
        address: int,
        size: int,
        read_cb: Callable[[Uc, int, int, object], int],
        write_cb: Callable[[Uc, int, int, int, object], None],
    ) -> None:
        self.uc.mmio_map(address, size, read_cb, None, write_cb, None)

    def set_reset_state(self, pc: int) -> None:
        self.uc.reg_write(UC_ARM_REG_CPSR, CPSR_RESET)
        self.uc.reg_write(UC_ARM_REG_PC, pc)

    def read_ram(self, address: int, size: int) -> bytes:
        return bytes(self.uc.mem_read(address, size))

    @property
    def pc(self) -> int:
        return self.uc.reg_read(UC_ARM_REG_PC)

    def run(self, until: int = 0, count: int = 0, timeout: int = 0) -> None:
        self.uc.emu_start(self.pc, until, timeout, count)
