# `emu/` — history

Full pass-by-pass build narrative behind [README.md](README.md), which carries only the
current-state summary. Moved verbatim, in original order, as the active file grew past a
reasonable size.

## Archived from emu/README.md on 2026-09-24

## Status

### L2C, MTU2, RIIC0-2 added; real timer-IRQ delivery attempted and shelved, 2026-09-08 (fourth pass)

Continuing straight past the L2 cache controller stop from the previous pass:
**`peripherals/l2c.py`** (ARM PL310-style L2 cache controller, `0x3ffff000`) hit a real bug
almost immediately -- `body.bin`'s cache-invalidate-by-way bring-up (`REG7_INV_WAY`,
`0x3ffff77c`, confirmed via the two literal-pool pointers the code itself uses) polls that
register until it reads back `0` the way real PL310 hardware self-clears each way's bit as
invalidation completes; modeling it as plain storage (this module's first version) made
that poll genuinely infinite. Fixed by treating the whole `REG7_*` cache-maintenance block
as "any write completes instantly and always reads `0`" -- correct completion semantics,
not a target-specific hack. With that fixed, **`peripherals/mtu2.py`** (a 79-register,
5-channel general-purpose timer, plain storage only -- nothing autonomously sets a status
flag, so no `l2c`-style self-inflicted hang risk) and three **`peripherals/riic.py`**
instances (I2C bus controllers `RIIC0`/`RIIC1`/`RIIC2` -- `RIIC2` is the one wired to the
diode-matrix EEPROM per [[diode-matrix]], though no I2C protocol or EEPROM backing store is
modeled yet, just the register file) cleared quickly too.

Then boot hit a real Unicorn/QEMU Cortex-A9 model gap: `body.bin` executes a bare `WFE`
(`0xe320f002`), which this CPU model doesn't implement (`UC_ERR_INSN_INVALID`) -- confirmed
with an isolated test independent of this project's firmware (`NOP` on the same setup
executes fine, `WFE` doesn't). Fixed generically in **`hint_instructions.py`**: a
`UC_HOOK_INSN_INVALID` handler that recognizes the ARM/Thumb hint-instruction encodings
(`NOP`/`YIELD`/`WFE`/`WFI`/`SEV`) and skips them as a no-op -- architecturally valid on any
conforming implementation, not a target-specific hack either.

That unblocked execution into a **real WFE-based wait loop** that only a genuine periodic
timer interrupt can end -- exactly [[icom-custom-code-goal]]-adjacent territory the roadmap
already flagged as the next real threshold. Attempted it, and hit a genuine, reproducible
**Unicorn correctness bug**, confirmed independent of anything IRQ-specific: splitting
execution across multiple `count`-limited `emu_start` calls (needed to inject something
*between* chunks of guest execution, since a global per-instruction hook is prohibitively
slow -- confirmed separately, see below) corrupts the guest's ARM/Thumb state, crashing
code shortly afterward that runs cleanly under one unbroken `emu_start` call covering the
same instructions. Isolated with a minimal test that never calls `exceptions.trigger_irq`
at all -- pure chunking alone reproduces it. A per-instruction `UC_HOOK_CODE`-based
injection (no chunking, but incurring the same catastrophic slowdown `trace=True` already
showed) avoided the crash but landed somewhere not obviously correct either (re-entering
the IRQ vector), so that path isn't validated as actually working yet either.

**Net result this pass**: `exceptions.py` gained real, reusable IRQ-entry machinery
(`trigger_irq()`/`enter()`, refactored out of the existing SWI-entry code, independently
verified correct via an isolated `RFEIA` round-trip test) -- the underlying mechanism
works. But a practical way to *drive* it periodically without corrupting CPU state does
not exist yet in this codebase; a prototype `Board.run_ticked()` built on the
now-known-broken chunking pattern was deliberately **not** kept (see `board.py`'s comment
in its place). This is exactly the kind of finding the original plan flagged as the trigger
to reconsider Unicorn vs. a real QEMU machine for this specific need -- not resolved this
pass, see the roadmap.

`emu/mvp.py`'s stage 2 budget was shortened accordingly (past this point it only ever
times out uselessly in the WFE loop, so a short budget plus an honest "known WFE wait, not
a failure" message is more useful than a long one) -- and fixing that surfaced a real
reporting bug in the script itself: checking whether `board._stub.accesses` was non-empty
to decide "did stage 2 hit something new" was wrong, since stage 1's own already-absorbed
pokes leave stale entries there regardless of what stage 2 does. Fixed to check
`board._halted` (set only by the stub-stop callback) instead.

### Real ARM exception entry, CPG, GIC, and OSTM0 added, 2026-09-08 (same day, third pass)

Chasing the CPG stub hit below led somewhere much bigger: `body.bin` executes a genuine `SWI`
instruction (very likely FreeRTOS's own "start the first task" mechanism, per
[[kernel-rtos]]'s `swi_handler` -- the general shape matches, not yet individually
re-confirmed this session) partway through its own init, and Unicorn has no built-in handling
for ARM exceptions -- it just raises a bare `UC_ERR_EXCEPTION` and stops. **`exceptions.py`**
fixes this properly: a `UC_HOOK_INTR` handler that performs the real architectural
exception-entry sequence (bank a return address into the target mode's `LR`, save `CPSR` into
that mode's `SPSR`, switch mode/state, jump to `VBAR + vector_offset`) for `SWI`/`UDEF`/
prefetch-and-data-abort/`IRQ`/`FIQ`. This is genuine ARMv7-A architecture behavior, not
IC-7300-specific, so it lives alongside `core.py` rather than in `board.py`. Two real things
worth remembering from building it:
- **Unicorn banks `LR`/`SPSR` transparently per-mode** -- switch `CPSR`'s mode bits first, then
  read/write the plain `UC_ARM_REG_LR`/`UC_ARM_REG_SPSR` constants and they transparently hit
  the *current* mode's banked copy. Confirmed empirically (see `exceptions.py`'s docstring);
  no per-mode register constants exist in this Unicorn build's ARM bindings, and none are
  needed.
- **A real host-API trap**: reading `VBAR` via `Uc.cpr_read(15, 0, 12, 0, 0, el=1, ...)` (`el=1`
  looks obviously correct -- VBAR is an EL1 register, and that's what the API's own docstring
  calls the parameter) silently reads back `0` even when a guest `mcr`+`mrc` pair round-trips
  the real value correctly. `el=0` reads the true value instead. Root cause not fully chased
  down (plausibly this argument actually selects a security-state bank despite its name), but
  confirmed reproducible with an isolated test independent of this project's firmware --
  documented in `exceptions.py` in case a future Unicorn upgrade needs re-checking.

With exceptions handled, boot progresses vastly further -- straight past the `CPG.STBCR5` stub
hit below (implemented as **`peripherals/cpg.py`**, plain read/write storage for the whole
clock/standby/deep-standby register file, per the SVD -- nothing yet depends on real
clock-gating behavior), through **`peripherals/gic.py`** (the ARM GIC Distributor
`0xE8201000` + CPU Interface `0xE8202000`, both addresses already-confirmed ground truth from
[[memory-map]]/[[kernel-rtos]]'s prior sessions; `gic_distributor_disable`/`FUN_200b848c`,
already-decompiled real functions, configure it), and a second `Ostm` instance (`OSTM0`,
`peripherals/ostm.py` -- already generic from the first one). Stage 2 now stops at a new,
clean, standard location: a 4-byte read at `0x3ffff000`, confirmed via the SVD to be
**`L2C.REG0_CACHE_ID`** (the ARM PL310-style L2 cache controller's identification register) --
left as the next stub-hit signal, not implemented this pass.

### GPIO/port + OSTM1 timer added, 2026-09-08 (same day as the MVP)

Implemented the peripheral the MVP's criterion 3 stopped on: `peripherals/gpio.py`, the full
RZ/A1H `PORT_BASE`(`0xFCFE3000`)/`IBC_BASE`(`0xFCFE7000`) register cluster (P/PM/PMC/PPR/PFC/
PFCE/PFCAE/PNOT/PIBC/PBDC/PIPC/SNCR, all ports), with offsets cross-checked against both
`notes/ic7300-signal-chain.md`'s own derived formula and the RZ/A1H SVD
(`~/Downloads/rza1.svd`, the same file this project's Ghidra project imports) -- both agree
exactly. P/PM/PMC/PPR/PIBC's *roles* are hardware-confirmed (via `notes/ic7300-signal-chain.md`'s
`DRESD` trace and a fresh decompile of `FUN_2002b878`, see gpio.py's own docstring);
PSR/PMSR/PMCSR/PNOT's exact set/clear semantics are a structurally-motivated inference (the
classic masked set/clear register shape, matching each one's 32-bit-vs-16-bit-companion width in
the SVD), not independently confirmed -- flagged in the module for correction if real hardware
ever demonstrates otherwise.

Making GPIO alone progress further exposed a second peripheral immediately downstream: the exact
`body.bin` function this unblocks (`FUN_2002b878`, decompiled while implementing this) enables
`PIBC1`, then busy-waits on either `PPR1` bit 6 or **`OSTM1.CNT`** (`0xfcfec404`) reaching a
threshold -- confirmed via the SVD (`OSTM1` derived from `OSTM0`, based at `0xFCFEC400`, `CNT` at
`+0x4`) to be a real hardware free-running timer register, not a guess. Implementing GPIO without
also giving this timer *some* forward motion would have just traded one infinite loop (unmodeled
PPR1) for another (an unmodeled timer that never advances) -- so `peripherals/ostm.py` went in at
the same time: not a faithful cycle-accurate model, just a counter that jumps a large step per
read so a `CNT < threshold` polling guard resolves in a handful of polls instead of the real
tens-of-millions-of-cycles wait.

With both in place, `emu/mvp.py`'s stage 2 now progresses past `FUN_2002b878` entirely and stops
at a new, clean location: a 1-byte read at `0xfcfe0428`, confirmed via the SVD to be
**`CPG.STBCR5`** (Clock Pulse Generator, Standby Control Register 5 -- module clock-gating, a
different peripheral category than GPIO). Left as the next stub-hit signal, not implemented this
pass -- see the roadmap below.

### MVP reached, 2026-09-08

The MVP goal — boot a real firmware release through `base.dat`'s already-fully-traced 5-step
boot sequence and land on `body.bin`'s real entry point — **passes**, verified two independent
ways:

```
emu/.venv/bin/python3 -m emu.mvp   # defaults to /data/misc/icom/7300/7300_142.dat
```

1. **PC reaches `0x20005000`** (body.bin's real entry, per [[base-loader]]) without crashing on
   any required MMIO access.
2. **RAM there matches `tools/icom_fw`'s own independently-implemented LZSS decompressor's
   output byte-for-byte** (all 3,738,392 bytes, for v1.42) — strong evidence the CPU model and
   the flash-image construction are both correct, independent of trusting Unicorn's ARM decoder
   on its own.
3. **Execution continues cleanly into `body.bin` itself** until the first not-yet-modeled
   peripheral access (currently a 2-byte read at `0xfcfe7004`, in the GPIO port register area
   per [[ic7300-signal-chain]]'s `PORT_BASE = 0xfcfe7000`) — hit via the stub handler, not a
   crash. This is deliberately the MVP's designed stopping point, not a bug: see "Extension
   roadmap" below.

Also spot-checked independently: tracing actual executed PC values from `0x20005000` gives
`0x20005000 → 0x20005050 → 0x20005054 → ...`, exactly matching `arm-none-eabi-objdump`'s
disassembly of the same decompressed bytes (the vector table's `LDR PC,[PC,#0x18]` really does
resolve to the literal-pool value `0x20005050`).
