# `qemu-machine/` — a custom QEMU machine for the IC-7300's RZ/A1H

Why this exists: `emu/` (the Unicorn-based emulator) hit a real wall — `body.bin` reaches a
genuine `WFE`-based wait loop that only a correctly-delivered periodic timer interrupt can
end, and driving Unicorn's own (already-correct) IRQ-entry code periodically via repeated
`count`-limited `emu_start` calls triggers a real, reproducible Unicorn engine bug (confirmed
independent of anything IRQ-specific — pure chunking alone corrupts ARM/Thumb state). See
`emu/README.md`'s Status section for the full story and the effort evaluation that led here.
This is not a replacement for `emu/` — that stays the fast, already-fully-working tool for
everything not interrupt-dependent (criteria 1+2 of `emu/mvp.py` need no interrupt delivery at
all) — this is the escalation path for the one thing it can't do.

See [README-history.md](README-history.md) for the full session-by-session narrative and
evidence trail behind everything below — this file carries only the current state and the
active resume point.

## Status

`qemu-machine` boots real, unmodified IC-7300 firmware (v1.42) through the full RZ/A1H boot
sequence to a fully-drawn main screen (14.100.00 USB FIL2), reaches a stable idle steady state
on both power-on branches (auto-boot and PWRK-hold), and stays there indefinitely. Boot to main
screen: ~8 s emulated / ~12 s wall under `-icount shift=1,sleep=off` (was 97 s wall before the
2026-09-24 tick fix — see below), or ~32 s wall under plain `shift=1` (real-time paced).
Measured with `tools/bench_boot.py`.

- **The front panel is fully drivable** from the desktop window (`tools/run_gui.py`, a GTK
  window): mouse/touch, keyboard (the front-panel key map prints at start, also in `fp_keymap`
  in `src/scif.c`), and the scroll wheel (MAIN DIAL, MULTI with Shift, TWIN PBT with Ctrl).
  Verified live through QMP `input-send-event`: MENU opens/EXIT closes the on-screen menu, wheel
  notches tune. `tools/fp.py` drives the same control socket headlessly (`press`, `dial`,
  `touch`, `state`, `keys`).
- **The band scope draws**, fed by a behavioural fake-FPGA model (`src/fake_fpga.c`) of the
  RSPI2 sweep protocol — synthetic carriers visible in the spectrum and waterfall after `27 10
  01` over CI-V. Spec: `notes/fpga-link.md`.
- **The CPU↔DSP link is modelled**: command/status over SCIF5 (`src/fake_dsp.c` — per-opcode
  state, class-tagged replies, identity replies, realistic 2-frame latency) and audio over
  SSIF0/1 (`src/ssif.c` — 96 kHz I2S; a synthetic tone reaches the firmware's own RX-audio ring
  at the right frequency and level). Spec: `notes/dsp-protocol.md`.
- **A modified `body.bin` repacks and boots end-to-end** in the emulator (commit 774acce) — the
  first real test that a `tools/icom_fw`-repacked custom image is accepted and runs.
- **The emulated system tick was fixed, and boot got much faster as a result**: the 500 µs tick
  (MTU2 TGI3A) was firing 8.2× too rarely because of a compare-match re-arm modelling bug that
  also caused an icount vCPU↔main-loop handoff storm under `-icount`; fixing both dropped boot
  time from 97 s to 8 s wall (see `mtu2.c`/`ostm.c`).
- Main-screen rendering (a real OpenVG rasterizer + path tessellator), the VDC5 60 Hz
  frame-timing interrupt, the RIIC2 EEPROM loaded with the firmware's own factory-default image,
  and the CI-V/frequency/band-switch-latch path are all working — see the peripheral table below
  for what backs each one.

**Open / next**: a live SD-card firmware update on real hardware (`sdk/roadmap.md`'s Phase 0
payoff — `body.bin`'s own MMCIF driver has never been reached by a traced boot path yet), or a
custom-code hook. DSP-side static code analysis is done (`notes/dsp-protocol.md`,
`notes/civ-dsp-fpga-catalogue.md`); the CI-V `27 00` scope-waveform output and fixed-mode
(VFO-offset) scope behaviour are not yet checked. TX audio playback (DR_AF) and the DX_FMT
decoders are unexercised — there's no front-panel key-injection-driven recorder/voice-memory
test yet.

Older status entries and the full session-by-session narrative: [README-history.md](README-history.md).

## Confirmed peripherals

| Device | File | Status |
|---|---|---|
| GIC (Distributor + CPU I/F) | `rz_a1h.c` (QEMU's own `arm_gic`) | Real, working |
| OSTM0 / OSTM1 | `ostm.c` | Real timer + real IRQ. OSTM0 is `body.bin`'s real tick source (GIC 134, `CMP`=32000) |
| SPI boot status | `spi_boot.c` | Real — the one register `base.dat`'s SPI-ready poll needs |
| GPIO/port registers | `gpio.c` | Real masked set/clear, `PNOT` toggle, live `PPR` pin levels; also hosts the 74AHC595 band-switch shift-register model (`RZA1H_DEBUG=sr595`, decoded to schematic names MSTB1/2, DSTB, PSTB) |
| L2C (PL310 cache controller) | `l2c.c` | Real (`CACHE_ID`/`CACHE_TYPE`/`REG7` self-clear semantics) |
| CPG | `rz_a1h.c`'s `add_plain_ram_region()` | Plain storage, no behavior — nothing traced needs more yet |
| MTU2 | `mtu2.c` | Real: channel 3's `TGI3A` (the 500 µs system tick, GIC 154, live re-arm), channel 4's `TGI4A`/`TGI4C` (GIC 159/161), two more purely-polled compare-match events. Real 32 MHz clock. Every other channel/register plain storage |
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR/STI/TI/TEI/RI/SPI protocol, real bit-rate-generator-paced timing. RIIC2 backs a real `hw/nvram/eeprom_at24c.c` EEPROM slave (16KB, addr 0x50) loaded with the firmware's own captured factory-default image (`tools/build_riic_eeprom_image.py`) — only RIIC2 exercised by any traced boot path so far |
| SCIF0-7 (UART) | `scif.c` | Real TX (baud-rate-accurate pacing) with per-channel level-triggered TXI; real RXI backing two virtual responders — a front-panel one on channel 3, a DSP-link one on channel 5. Per-channel bus logger: `RZA1H_DEBUG=scif<N>` |
| IF-DSP behind SCIF5 | `fake_dsp.c` | Behavioural model built from the DSP's own code: per-opcode state, the 7 class-tagged TX slots, identity replies from the version tags, realistic 2-frame command latency. `RZA1H_DEBUG=dsp` |
| FPGA behind RSPI2 | `fake_fpga.c` | Behavioural model of the band-scope sweep protocol: 7-byte register file, 475-sample sweep reply, sweep-rate knobs (`RZA1H_FPGA_SWEEP_HZ`/`_FLOOR`/`_SIGNALS`). `RZA1H_DEBUG=fpga` |
| SSIF0/1 (DSP audio, I2S) | `ssif.c` | Real register model (SSISR.IIRQ, FIFO data regs); RX content is the fake DSP's synthetic tone/noise, TX (DR_AF) logged as peak levels. `RZA1H_DEBUG=ssif` |
| RX-8803LC RTC | `rx8803.c` | Real RIIC1 I2C slave; backs `body.bin`'s live idle-state RTC traffic |
| MMCIF (SD/MMC host) | `mmc.c` | Real command/response/data protocol + virtual SD card, validated standalone — `body.bin`'s own driver not yet reached by any traced boot path |
| DMAC (DMA controller) | `dmac.c` | Real channels 0-7: honours `CHCFG.SAD/DAD` (fixed vs incrementing address), `ptimer`-based completion, streaming mode backs the SSIF audio pumps and the band-switch shift-register writes |
| RSPI2 (Serial Peripheral I/F ch.2) | `rspi2.c` | Real TX + RX: SPDR2 writes clock a byte into an RX queue, SPRI2 (GIC 277) is level-triggered; clocks the FPGA sweep protocol |
| VDC50 (LCD/display controller) + LVDS | `vdc5.c` | Register storage plus a real 60 Hz frame-timing interrupt source (output vsync/VLINE status, GIC 75-97); a QEMU graphic console scans out the active GR plane (`tools/run_gui.py`) |
| OpenVG (R-GPVG 2.6.2 graphics processor) | `openvg.c` | Completion interrupts (GIC 130-133) plus a real rasterizer: command-FIFO decoder (fills, blits, affine image draws, cover draws) and the path-tessellator command-list engine (vector paths incl. all font text, 4x4 AA non-zero fill). Fragment programs not interpreted |
| ADC (10-bit wired A/D converter) | `adc.c` | DRA-DRH all return a fixed mid-scale reading; no IRQ, matching the real driver's continuous-scan/never-polls-completion behavior. Feeds `dsp_param_table_rebuild_from_settings` |

## Directory layout

- **`src/`** — our own C sources (tracked in git): `rz_a1h.h`/`rz_a1h.c` (shared addresses, the
  machine itself), one file per peripheral in the table above, and `rza1h_debug.h` — a shared
  debug-logging helper (`RZA1H_DEBUG=<device>` or `RZA1H_DEBUG=all`) that every peripheral calls
  at its real "boundary" events (command dispatch, IRQ raise, phase transitions), not routine
  register passthrough. Deliberately separate from QEMU's own `-d unimp`/`-d guest_errors`, which
  stays a clean "genuinely unimplemented access" signal.
- **`tools/`** — the actively-used set:
  - `gdbrsp.py` — raw GDB remote-serial-protocol client (registers, memory, breakpoints,
    continue/step/interrupt); the reliable way to drive and inspect this machine dynamically.
  - `build_flash.py` — builds the flat flash image `rz_a1h.c` loads via `-kernel` (wraps
    `emu/flash_image.py`).
  - `build_riic_eeprom_image.py` — builds the RIIC2 EEPROM image from the firmware's own
    captured factory-default dump; run before any boot test that needs to reach real
    `cold_boot_hw_init`-era code.
  - `run_gui.py` — the live GTK front-panel window (`--display gtk/sdl/none`, plus the flags
    listed in the Status section's history: `--fast`, `--icount`, `--no-pwrk`, `--civ`, `--tone`,
    `--fpga-sweep-hz`, `--debug`, etc.).
  - `fp.py` — headless front-panel control client (press/dial/touch/state/keys) over the GTK
    window's control chardev.
  - `civ.py` / `civ_dsp_sweep.py` — CI-V client tooling.
  - `screenshot.py` — boot N seconds, dump the LCD plane (or arbitrary guest surfaces) as PNG;
    supports GDB-based RAM pokes before capture (`--poke ADDR=VAL`).
  - `bench_boot.py` — wall-time-to-pixel-identical-screen benchmark, the tool used for every
    speed number in this file.
  - `build_sdcard.py` — builds a real FAT16 SD card image with an update container at the
    documented path.
  - `qmp_read_mem.py` — general-purpose, fully GDB-free physical-memory reader via QMP.
  - `force_call_fup.py` / `test_mmc.py` / `test_irq.py` / `trial_irq.py` — standalone protocol
    and IRQ-delivery validation, reused as regression checks.
  - A large set of one-off `trace_*.py`/`test_*.py` investigation scripts from earlier debugging
    threads (ring-overflow, DMAC races, IRQ-mask tracing, OpenVG command-traffic tracing, GIC/SGI
    state, etc.) — kept for their reusable bracketing/polling patterns even though the specific
    stalls they chased are resolved; see README-history.md for what each one found.
- **`patches/`** — `hw-arm-build.patch` (registers our files in the vendored QEMU checkout;
  applied automatically by `setup.sh`). `irq-mask-trace.patch` / `rr-loop-trace.patch` are
  one-off diagnostic patches against core QEMU, NOT applied by default — reusable via their own
  `tools/apply_*.sh` scripts if a similar investigation comes up again.
- **`setup.sh`** — idempotent: clones QEMU `v11.1.1` (shallow) if missing, applies the patch,
  symlinks `src/*.c` in, configures (`arm-softmmu` only), builds.
- Gitignored: `qemu-src/`, `flash.bin`, `riic2_eeprom.img` (all regenerated by the tools above).

## Running it

```
qemu-machine/setup.sh                                        # one-time (or after a source edit)
emu/.venv/bin/python3 qemu-machine/tools/build_flash.py       # produces qemu-machine/flash.bin
emu/.venv/bin/python3 qemu-machine/tools/build_riic_eeprom_image.py  # produces riic2_eeprom.img
qemu-machine/qemu-src/build/qemu-system-arm -M rz-a1h -nographic \
    -kernel qemu-machine/flash.bin -serial none -monitor none \
    -global rza1h-riic.image=qemu-machine/riic2_eeprom.img \
    -icount shift=1 \   # or shift=1,sleep=off: same determinism, ~4x faster boot
    -qmp unix:/tmp/qemu.sock,server,nowait   # or -s -S for GDB
```

Or, for the live GTK window with front-panel input: `python3 qemu-machine/tools/run_gui.py`
(see its `--help` for the flags above).

`-icount shift=1` is this machine's recommended default (2 ns/insn ≈ the real 400 MHz
Cortex-A9). Boot reaches `main_idle_loop` and a stable idle steady state on both power-on
branches (auto-boot and PWRK-hold).

Omit the `-global rza1h-riic.image=...` line to boot with an empty virtual EEPROM instead — a
real, valid configuration, but boot will stop much earlier, at the pre-cold-boot-branch
watchdog/`wfi` point documented in README-history.md, rather than reaching the main screen.

Inspecting live state: `tools/gdbrsp.py`'s raw GDB remote-serial client (`c`/`s`/`?`/`g`/`G`/
`m`/`M`, plus `set_breakpoint`/`remove_breakpoint`) is the reliable way to both inspect and
drive execution — see `tools/test_irq.py` or `tools/test_fup_scheduling.py` for worked
examples. QMP's `human-monitor-command` → `info registers` also still works for a read-only
spot-check.

## Extension roadmap

**Done** (full derivation for all of this in README-history.md): the GDB scripting reliability
gap (`gdbrsp.py`); finding what really arms `body.bin`'s tick source (OSTM0); porting every
peripheral above from the Unicorn/Python prototypes to real C QEMU devices; SCIF UART TX+RX with
the front-panel and DSP virtual responders; reaching a stable idle steady state on both power-on
branches; OpenVG rendering (the main screen fully draws); the fake FPGA (band scope) and fake DSP
(command + audio link); the front panel driven live from a desktop window; and the 2026-09-24
system-tick fix that took boot from 97 s to 8 s wall.

**Open:**
1. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — `body.bin`'s own MMCIF driver
   has still never been reached by any traced boot path; reaching it (e.g. via the SD-update
   flow) remains the actual Phase-0 payoff.
2. The CI-V `27 00` scope-waveform output and fixed-mode (VFO-offset) scope behaviour are not yet
   checked against the fake FPGA.
3. TX audio playback (DR_AF) and the DX_FMT decoders are unexercised — needs recorder or
   voice-memory playback driven from the front panel.
