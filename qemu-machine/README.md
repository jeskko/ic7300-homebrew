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
  at the right frequency and level), plus the RTTY data line (RTD, P8_7) from a fake FSK
  demodulator. Spec: `notes/dsp-protocol.md`.
- **The fake DSP plays audio stimulus files** (`src/ssif.c`, 2026-09-25) into any RX slot,
  replacing the tone: `af` = DX_REC L (RX audio: audio FFT, QSO recorder), `mic` = DX_REC R,
  `fmt` = DX_FMT L (demod output, read by the decoders), `fmt-r` = DX_FMT R. Formats: Sun .au or
  WAV, 16-bit PCM, any rate up to 96 kHz, first channel of several. Resampled to 96 kHz with a
  16-tap windowed sinc (passband 0.45 of the file's rate). Samples: `tools/gen_samples.py`
  synthesises `scratch/samples/{SSTV,RTTY,CW}.test.au` (12 kHz mono, known content).
  - Boot: `RZA1H_AF_FILE="path[,gain=G][,delay=S][,loop]"`, likewise `RZA1H_MIC_FILE`,
    `RZA1H_FMT_FILE`, `RZA1H_FMT_R_FILE`. fmt follows af unless it's set itself. A file starts S
    seconds of link time after it's armed, then plays once (silence after) or loops. The noise
    floor (`RZA1H_AF_NOISE`, 0.01) stays on.
  - Runtime, QMP on `/machine/ssif`: `qom-set` `af-file` (same syntax; re-setting restarts it,
    `none` clears it and brings the tone back), `af-tone` (`hz:level`), and read-only `af-status`
    (`waiting 1.0 s`, `playing 12.4/21.8 s (looped)`, `done`, `tone`, `off`). The same for
    `mic-`, `fmt-` and `fmt-r-`. A device reset rewinds files but keeps the settings.
  - Verified with `tools/audio_stimulus_check.py FILE [--runtime] [--gain G]`: the file arrives in
    the 48 kHz ring sample-accurately at unity gain (details in README-history.md).
  - `tools/run_gui.py --af-file/--fmt-file/--mic-file SPEC` passes the knobs through, and
    `--qmp PATH` opens a second QMP socket for `qom-set` while the window runs.
  - **RTTY decodes to text** (2026-09-25). The fake DSP's FSK demodulator (`src/ssif.c`)
    watches the fmt source while the mode is RTTY/RTTY-R. It runs mark/space tone detectors at
    `RZA1H_RTTY_MARK` (2125) and `RZA1H_RTTY_SHIFT` (170), and drives pin P8_7 (RTD; 1 = mark;
    inverted in RTTY-R). `mtu2.c` channel 1 gives the firmware its 1 ms TGI1A sampler. The
    firmware's own UART and Baudot table do the rest. QOM `/machine/ssif rtd-edges` counts RTD
    transitions.
  - `tools/decode_stimulus_test.py RTTY_FILE [--expect TEXT]` (19 checks, all pass). It shifts
    any RTTY recording to 2125/2295 Hz itself. The checks:
    - the RTTY decode screen's tuning scope peaks at mark/space and the waterfall draws;
    - the user's sample decodes to "WELCOME TO WIKIPEDIA, THE FREE ENCYCLOPEDIA THAT";
    - RTTY-R decodes the mirrored sample to the same text, and garbles the normal one;
    - the FM TSQL CTCSS detector flags 88.5 Hz and rejects 85.4/91.5/100 Hz.

    Paths: `notes/dsp-protocol.md`, "DX_FMT consumers and the RTTY receive path".
  - **Known issue: the firmware's audio pump misses DMA blocks** (found 2026-09-25 by the SSTV
    app). Even under `-icount shift=1` the emulated firmware leaves about 0.1–0.4% of the
    0.75 ms SSIF blocks untaken before the next one lands (`RZA1H_DEBUG=dmac` counts them as
    overruns), from boot on, with or without an app. Without `-icount` it's about 19%.
    Whether real hardware drops any is unknown. The SDK's audio runtime detects and fills the
    lost blocks (`sdk/runtime/audio.c`); anything else reading the rings sees the gaps.
- **The SD card slot works** (`src/sdhi.c`, 2026-09-25): `body.bin` drives the card through
  **SDHI0** (`0xE804E000`, Renesas' SD driver library), not MMCIF — that's why the old `mmc.c`
  was never touched. The card is upstream QEMU's `sd-card` on the SDHI's SD bus
  (`-drive if=sd,format=raw,file=IMG`, power-of-two size; `run_gui.py`/`screenshot.py --sd IMG`,
  `tools/build_sdcard.py` builds MBR+FAT32 images). Sector transfers use DMAC ch7, paced by an
  SDHI DMA-request line. Verified live: full card bring-up, mount (SD icon shown), the firmware
  creating its `IC-7300/{Decode/Rtty,Voice,Setting,Capture,VoiceTx}` folder tree, SET > SD Card >
  SD Card Info showing the correct capacity/free space, and Save Setting writing
  `IC-7300/Setting/Set20000001_01.dat` (8224 bytes, `fsck.fat` clean). Two side fixes: the
  CS0-CS5 external bus (`0x00000000`-`0x17FFFFFF`) is now mapped as ignore-writes/read-zero (a
  voice-recorder stop routine writes through a NULL pointer on card insert, which used to be a
  fatal abort), and SD_INFO1.INFO7 = 1 means *writable* to this firmware.
  **Known issue:** under `-icount` (either pacing) a heavy mount — one that scans the whole FAT,
  e.g. a card that already has the folder tree — stalls at a random point: the guest idles in
  WFE (a real halt in QEMU 11) waiting for an SD event, and QEMU's icount idle warp stops
  delivering the device's timer expiry promptly. Without `-icount` the same card mounts and
  works fully. So use `--icount off` for SD work until this is fixed.
- **The firmware's own SD-card updater works end to end** (2026-09-25): SET > SD Card >
  Firmware Update on a card holding a repacked container (`tools/icom_fw` + `build_sdcard.py`)
  runs the stock updater — MD5 check, slot-B erase/program, active-slot marker, "Firmware
  updating has completed" — then the firmware's watchdog restart, and the radio comes back up
  running the modified image (a `1.42`→`9.99` version-string edit shows on the splash and on
  SET > Others > Information > Version). New devices: `spibsc.c` (SPIBSC0 manual SPI mode + the
  EN25Q64 boot flash; `-global rza1h-spibsc.save-file=PATH` persists flash), `wdt.c`
  (watchdog reset, WOVF kept across it), `stbc.c` (STBREQ/STBACK module-standby handshake, which
  the graphics shutdown on power-off waits on). The restart path, traced: the dialog sets
  `0x20390306`, the power-off path (`power_state_pwrk_wait_and_bringup`) turns the POWER LED /
  backlight off over SCIF3 (`fe 01 00 00 00 fd`), makes `ui_graphics_lifecycle_task` tear down
  EGL (which stops the GPU via STBREQ2 bit 0 / VDC5 bit 5), writes `Fup_AutoEnd_3765` and arms
  the watchdog. The tools' GPIO path is now `/machine/gpio` (was an auto-numbered
  `device[14]`). A plain PWRK-hold power-off still doesn't complete (not investigated).
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

**Open / next**: the same SD update on real hardware (`sdk/roadmap.md` Phase 0, now fully
rehearsed in the emulator), a custom-code hook, the `-icount` SD stall above, and PWRK power-off. DSP-side static code analysis is done (`notes/dsp-protocol.md`,
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
| MTU2 | `mtu2.c` | Real: channel 3's `TGI3A` (the 500 µs system tick, GIC 154, live re-arm), channel 4's `TGI4A`/`TGI4C` (GIC 159/161), channel 1's `TGI1A` (the RTTY bit sampler, GIC 146, live TCNT_1 with TPSC prescaler), two more purely-polled compare-match events. Real 32 MHz clock. Every other channel/register plain storage |
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR/STI/TI/TEI/RI/SPI protocol, real bit-rate-generator-paced timing. RIIC2 backs a real `hw/nvram/eeprom_at24c.c` EEPROM slave (16KB, addr 0x50) loaded with the firmware's own captured factory-default image (`tools/build_riic_eeprom_image.py`) — only RIIC2 exercised by any traced boot path so far |
| SCIF0-7 (UART) | `scif.c` | Real TX (baud-rate-accurate pacing) with per-channel level-triggered TXI; real RXI backing two virtual responders — a front-panel one on channel 3, a DSP-link one on channel 5. Per-channel bus logger: `RZA1H_DEBUG=scif<N>` |
| IF-DSP behind SCIF5 | `fake_dsp.c` | Behavioural model built from the DSP's own code: per-opcode state, the 7 class-tagged TX slots, identity replies from the version tags, realistic 2-frame command latency. `RZA1H_DEBUG=dsp` |
| FPGA behind RSPI2 | `fake_fpga.c` | Behavioural model of the band-scope sweep protocol: 7-byte register file, 475-sample sweep reply, sweep-rate knobs (`RZA1H_FPGA_SWEEP_HZ`/`_FLOOR`/`_SIGNALS`). `RZA1H_DEBUG=fpga` |
| SSIF0/1 (DSP audio, I2S) | `ssif.c` | Real register model (SSISR.IIRQ, FIFO data regs); RX content is the fake DSP's synthetic tone/noise or a stimulus file (`RZA1H_AF_FILE`, QOM `/machine/ssif`), TX (DR_AF) logged as peak levels. `RZA1H_DEBUG=ssif` |
| RX-8803LC RTC | `rx8803.c` | Real RIIC1 I2C slave; backs `body.bin`'s live idle-state RTC traffic |
| SPIBSC0 + boot flash | `spibsc.c` | XIP ROM plus manual SPI mode: WREN/RDSR/WRSR/RDID/read/page program/4K,64K,chip erase on an EN25Q64 model; optional save-file |
| WDT | `wdt.c` | Keyed WTCSR/WTCNT/WRCSR, overflow → system reset with RSTE, WOVF survives the reset |
| STBREQ/STBACK | `stbc.c` | Module-standby handshake, STBACKn = STBREQn |
| SDHI0 (SD host) | `sdhi.c` | The IC-7300's SD slot: manual chapter 50 register model, upstream `sd-card` on its SD bus, card detect/WP, PIO and DMA (ch7) data, GIC 302-304 |
| MMCIF (SD/MMC host) | `mmc.c` | Standalone protocol model with its own virtual card; `body.bin` doesn't use MMCIF (its SD card is on SDHI0) |
| DMAC (DMA controller) | `dmac.c` | Real channels 0-7: honours `CHCFG.SAD/DAD` (fixed vs incrementing address), `ptimer`-based completion, streaming mode backs the SSIF audio pumps and the band-switch shift-register writes |
| RSPI2 (Serial Peripheral I/F ch.2) | `rspi2.c` | Real TX + RX: SPDR2 writes clock a byte into an RX queue, SPRI2 (GIC 277) is level-triggered; clocks the FPGA sweep protocol |
| VDC50 (LCD/display controller) + LVDS | `vdc5.c` | Register storage plus a real 60 Hz frame-timing interrupt source (output vsync/VLINE status, GIC 75-97); a QEMU graphic console composites the graphics planes GR0 < GR1 < GR2 < GR3 (read-enabled planes with DISP_SEL CURRENT/BLEND, drawn opaque) (`tools/run_gui.py`); GRn_UPDATE bits clear at each frame, as on hardware. Stock boot is pixel-identical to the reference screenshot |
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
  - `ram_marker_sweep.py` — which RAM does the firmware ever write? Patterns a range at reset,
    drives a feature scenario (scope, modes, menus, keyer, RTTY decode, TX, QSO recorder, SD
    save/load, and SDK apps as a positive control), and diffs after each step. Result 2026-09-25:
    `0x20601000`–`0x2080afff` is never written (`notes/memory-map.md`).
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
1. **Phase 0 on real hardware** — the firmware's own SD updater installs and boots a modified
   image in the emulator (see Status); the `-icount` stall on heavy SD mounts is still open.
2. The CI-V `27 00` scope-waveform output and fixed-mode (VFO-offset) scope behaviour are not yet
   checked against the fake FPGA.
3. **SD card drops out during Save + Load Setting.** The firmware shows "SD Card was removed.".
   Afterwards directory listing fails (the homebrew picker shows no apps), Voice TX REC reports
   "not formatted in FAT/FAT32", and SD Card Info won't open. Save alone was fine in an
   interactive run. Found 2026-09-25 by `ram_marker_sweep.py`, which runs save/load last because
   of it. Not investigated.
4. **A one-off hang after a CI-V band change** (`05` to 7.1 MHz) with `--icount off`: the
   waterfall kept scrolling, but CI-V and the UI stopped. Not reproduced in 16 further band-change
   rounds, with or without the sweep's RAM markers. Not investigated.
5. TX audio playback (DR_AF) and the DX_FMT decoders are unexercised — needs recorder or
   voice-memory playback driven from the front panel.
