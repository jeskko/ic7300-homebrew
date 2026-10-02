# `emu/` — a minimal IC-7300 firmware emulator

Runs real, unmodified firmware bytes on a CPU model instead of just reading them — lets this
project verify RE hypotheses by execution, and (see the roadmap below) eventually test a
custom/patched `body.bin` end-to-end offline, with zero hardware risk and no dependency on the
still-not-arrived JTAG adapter. See `sdk/roadmap.md` for why that matters to the
project overall, and the planning session (2026-09-08) this implements.

## Status

**Superseded 2026-09-08 — no further work since, kept as a fast, working tool for non-interrupt
cases.** `emu/` reached its MVP: it boots a real firmware release through `base.dat`'s traced
boot sequence and lands exactly on `body.bin`'s real entry point (`0x20005000`), with RAM there
matching `tools/icom_fw`'s independent LZSS decompressor byte-for-byte. It was then extended,
same day, with real GPIO/port registers, OSTM0/OSTM1, CPG, the GIC, real ARMv7-A exception entry,
L2C, MTU2, and RIIC0-2 — all real, working peripheral models (see `emu/README-history.md` for
each module's derivation).

It then hit a genuine **Unicorn Engine correctness bug**: splitting execution across multiple
`count`-limited `emu_start()` calls (needed to inject a periodic timer interrupt into `body.bin`'s
`WFE`-based wait loop) corrupts the guest's ARM/Thumb state — confirmed independent of anything
IRQ-specific, reproducible with pure chunking alone. That gap is what `qemu-machine/` (a real
custom QEMU machine) was built to solve; see `qemu-machine/README.md`. `emu/` itself remains the
fast, already-working tool for anything that doesn't need interrupt delivery (the MVP's own
criteria 1+2 need none at all).

Run it:

```
emu/.venv/bin/python3 -m emu.mvp   # defaults to firmware/7300_142.dat
```

See [README-history.md](README-history.md) for the full pass-by-pass build narrative: the
Unicorn correctness-bug isolation, the real ARM exception-entry mechanics (including a real
Unicorn API trap reading `VBAR`), and every peripheral module's own derivation.

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
  IC-7300-specific. See `README-history.md` for what this unlocked, the real Unicorn API trap it
  took to get `VBAR` reading correctly, and the chunked-execution correctness bug found while
  trying to drive `trigger_irq()` periodically.
- **`hint_instructions.py`** — works around a real Unicorn/QEMU Cortex-A9 gap (`WFE`/`WFI`/
  `YIELD`/`SEV` aren't implemented) by skipping them as the architecturally-always-valid `NOP`
  interpretation. Lives next to `core.py`/`exceptions.py` for the same "generic CPU behavior,
  not IC-7300-specific" reason.
- **`mvp.py`** — the runnable MVP check described above.

## Extension roadmap

1. ~~**GPIO/port registers**~~, ~~**CPG**~~, ~~**GIC**~~, ~~**real ARM exception entry**~~,
   ~~**L2C**~~, ~~**MTU2**~~, ~~**RIIC0-2**~~, ~~**WFE/hint-instruction workaround**~~ — **all
   done, 2026-09-08**, see README-history.md. Diode-matrix/EEPROM-specific behavior
   ([[diode-matrix]]) isn't modeled yet -- GPIO/RIIC2 so far are the generic register files
   only, not any specific pin's real-world meaning or actual I2C protocol/EEPROM content. The
   GIC has no real interrupt delivery/pending-state modeled, only distributor/CPU-interface
   register storage plus an always-spurious `ICCIAR`.
2. **Real timer interrupt delivery — attempted, shelved, 2026-09-08**: blocked on a genuine
   Unicorn correctness bug (chunked `emu_start` calls corrupt ARM/Thumb state), not a modeling
   gap on this project's side -- see README-history.md for the full isolation. This is what
   `qemu-machine/` was built to solve instead (see that directory's own README); concrete next
   steps if this gets picked back up on the Unicorn side specifically, roughly in order of
   effort: (a) check whether a newer Unicorn release fixes the chunking bug; (b) investigate
   whether the bug is specific to `count`-limited stops vs. any repeated `emu_start` call (e.g.
   does alternating `until=`-based stops avoid it?).
3. **SCIF UART** (one channel) piped to stdout — first real "see something happen" milestone;
   register layout already documented in [[ic7300-signal-chain]]. Doesn't depend on interrupt
   delivery being solved first, so it's a reasonable place to make progress in the meantime.
4. **SD-card block device + enough VFS** to run [[firmware-update]]'s own update orchestrator
   inside the emulator — the big payoff: test a `tools/icom_fw`-repacked custom `body.bin` for
   "does it get accepted and boot" fully offline, ahead of (or instead of) the JTAG-gated live
   test in `sdk/roadmap.md`'s Phase 0. Also doesn't strictly need interrupt delivery, though a
   real driver may end up polling-vs-IRQ-waiting for card-ready in ways that need it eventually.
5. **RIIC2/EEPROM and diode-matrix-specific pin behavior**, layered on top of the now-existing
   generic GPIO register file, to test region-code-gated behavior ([[diode-matrix]]) without
   hardware.
6. **Lowest priority, most hardware-specific**: front-panel SCIF3 link, touch controller,
   DSP/FPGA co-simulation.

Note: `qemu-machine/` has since reached most of the same real peripherals in C (as real QEMU
devices) and gone well beyond this list (SCIF UART, SD-card protocol, front-panel/DSP/FPGA
links) — see `qemu-machine/README.md`. Items above describe `emu/`'s own, now-frozen state.
