# `emu/` — a minimal IC-7300 firmware emulator

Runs real, unmodified firmware bytes on a CPU model instead of just reading them — lets this
project verify RE hypotheses by execution, and (see the roadmap below) eventually test a
custom/patched `body.bin` end-to-end offline, with zero hardware risk and no dependency on the
still-not-arrived JTAG adapter. See [[icom-custom-code-goal]] for why that matters to the
project overall, and the planning session (2026-09-08) this implements.

## Status

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

## A real correction this surfaced: the `0x2c`-byte flash-address shift

Building the flash image exposed (and resolved) a genuine, previously-unnoticed inconsistency
in [[base-loader]]'s own offset table — see that file's 2026-09-08 correction section for the
full derivation. Short version: every absolute flash address that note quoted was `0x2c` bytes
too high, because the container file's 44-byte header (version string + `size1..size7`) is
consumed as in-file metadata only and never itself lands in flash. The real mapping is:

```
flash_address = 0x18000000 + (container_file_offset - 0x2c)
```

`emu/flash_image.py` implements this (reusing `tools/icom_fw/container.py`'s existing
`CHECKSUM_REGION_START = 0x2c` constant, which turned out to already encode the same shift for
an unrelated reason — the checksum-region derivation). This means a real, unmodified
`7300_1XX.dat` container's bytes (past that one header) are already the flash image almost
as-is — no unpack/repack round-trip through `tools/icom_fw` needed just to *build* it (that tool
stays relevant for producing a *modified* container to test later, see the roadmap).

## Kit choice: Unicorn Engine

Chosen over a full custom QEMU machine, Renode, and Ghidra's own p-code emulator — see the
planning session's reasoning (2026-09-08) for the comparison. Short version: Unicorn reuses
QEMU's own battle-tested ARM/Thumb decoder (so CPU-model correctness isn't something to debug
from scratch), has first-class Python bindings matching this repo's whole existing tooling
style (`tools/icom_fw`, `arm_thumb_scan.py`, ...), and needs no board/device model of its own —
you get CPU + memory and write peripheral behavior yourself via MMIO callbacks, which maps
directly onto how this project already documents hardware register-by-register
(`notes/memory-map.md`, `notes/ic7300-signal-chain.md`).

Installed via `pip` into a project-local venv (`emu/.venv/`, gitignored) rather than the system
package manager — `python-unicorn` 2.1.4 is also available via `pacman` on this workstation if
the venv ever needs bypassing, and both are the same upstream version. Licensing note: GPLv2,
fine for this project's own non-distributed research use.

## Architecture

- **`core.py`** — thin Unicorn wrapper: CPU creation (Cortex-A9 model, `UC_CPU_ARM_CORTEX_A9`
  — confirmed available in this environment's installed bindings), memory mapping, run control.
  Nothing IC-7300-specific lives here.
- **`flash_image.py`** — builds the emulated 64 MB SPI-flash byte image from a real firmware
  container, per the corrected address mapping above.
- **`peripherals/registry.py`** — the extensibility seam: an address-range → handler-object
  table. Every future peripheral (SCIF UART, timer, GPIO/diode matrix, RIIC2/EEPROM, interrupt
  controller) is added by registering one more handler here; `board.py`/`core.py` never change.
  Deliberately the same shape as a QEMU MMIO device's `read`/`write` callbacks, so porting to a
  real QEMU machine later (if async IRQ delivery or timing accuracy is ever needed) is
  mechanical, not a rewrite.
- **`peripherals/stub.py`** — default "unimplemented" handler (logs the access, returns a
  configurable benign value), modeled on QEMU's own `unimplemented-device` — the fallback for
  anything not yet claimed, and the mechanism behind the MVP's criterion-3 clean-stop behavior.
  `Board.enable_stub_stop()` wires it to actually halt emulation on the *next* unmodeled access
  (off by default during `base.dat`'s own already-traced boot prologue, whose early hardware
  pokes are harmless and shouldn't interrupt reaching the MVP's real target).
- **`peripherals/spi_boot.py`** — the MVP's one real peripheral: the SPI status register at
  `0x3fefa048` bit 0, always reports "ready".
- **`board.py`** — wires core + flash image + peripheral registry into one "IC-7300 main CPU"
  board, and maps every I/O region [[memory-map]] documents as its own MMIO block (see that
  file's own address table — this replaced an earlier, narrower attempt that only mapped the
  two device windows `base.dat`'s own MMU translation table identity-maps, which turned out to
  miss real pre-MMU hardware pokes entirely).
- **`exceptions.py`** — real ARMv7-A exception entry (`SWI`/`UDEF`/aborts/`IRQ`/`FIQ`), living
  next to `core.py` rather than `board.py` since it's generic CPU architecture, not
  IC-7300-specific. See the Status section above for what this unlocked and the real Unicorn
  API trap it took to get `VBAR` reading correctly.
- **`mvp.py`** — the runnable MVP check described above.

## Extension roadmap

1. ~~**GPIO/port registers**~~, ~~**CPG**~~, ~~**GIC**~~, ~~**real ARM exception entry**~~ —
   **all done, 2026-09-08**, see the Status section above. Diode-matrix/EEPROM-specific
   behavior ([[diode-matrix]]) isn't modeled yet -- GPIO so far is the generic port register
   file only, not any specific pin's real-world meaning (no diode scan simulation, no
   RIIC2/EEPROM peripheral). The GIC has no real interrupt delivery/pending-state modeled,
   only distributor/CPU-interface register storage plus an always-spurious `ICCIAR` — enough
   for `body.bin`'s own init code, not enough for `IRQ`/`FIQ` to ever actually be taken yet.
2. **Immediate next stub hit**: `L2C.REG0_CACHE_ID` (`0x3ffff000`, ARM PL310-style L2 cache
   controller ID register) — likely another cheap, mostly-inert register-file addition like
   `CPG`, unless something branches on the ID value or a real cache-maintenance operation
   through this controller turns out to matter.
3. **SCIF UART** (one channel) piped to stdout — first real "see something happen" milestone;
   register layout already documented in [[ic7300-signal-chain]].
4. **Real timer interrupt delivery** — `peripherals/ostm.py`'s free-running counters exist but
   raise no `IRQ`; `exceptions.py` can now enter an `IRQ` handler correctly (untested against a
   real one so far, see its own verification-status note) but nothing yet *asserts* one. This
   is the concrete next step toward FreeRTOS's own tick handler and first real context switch.
   Open question to validate: whether Unicorn exposes a way to assert an external interrupt
   line asynchronously (rather than the synchronous `SWI` trap already working) — a firm "no"
   is the trigger to graduate to a real QEMU machine instead of fighting Unicorn's model.
5. **SD-card block device + enough VFS** to run [[firmware-update]]'s own update orchestrator
   inside the emulator — the big payoff: test a `tools/icom_fw`-repacked custom `body.bin` for
   "does it get accepted and boot" fully offline, ahead of (or instead of) the JTAG-gated live
   test in `sdk/roadmap.md`'s Phase 0.
6. **RIIC2/EEPROM and diode-matrix-specific pin behavior**, layered on top of the now-existing
   generic GPIO register file, to test region-code-gated behavior ([[diode-matrix]]) without
   hardware.
7. **Lowest priority, most hardware-specific**: front-panel SCIF3 link, touch controller,
   DSP/FPGA co-simulation.
