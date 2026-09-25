# IC-7300 firmware reverse engineering

Agentic RE workspace for the Icom IC-7300 HF transceiver firmware, working
alongside the existing (read-only) material at `/data/misc/icom/7300/` —
firmware images for all 10 official releases (v1.11–v1.42), service and
schematic manuals, a Renesas RZ/A1H SoC manual, hand-written notes, and
several prior Ghidra projects.

Nothing under `/data/misc/icom/7300/` is ever modified — this repo holds
our own notes, tooling, and (once created) a fresh Ghidra project, informed
by but not built on top of the prior work there.

## Ultimate goal

**Stated 2026-08-30**: beyond documentation for its own sake, the end goal is the ability to write and
run our own code ("apps") on the radio. See `sdk/roadmap.md` for the survey and phased plan, and
`sdk/app-requirements.md` for what each of a few example apps (serial/display hello-world, a simple game,
an SSTV receiver) would still need — the short version: the firmware-update mechanism's only integrity
check is an unkeyed MD5 with no signature verification anywhere in the traced boot or update chain, so a
"flash once" custom-app loader is already technically feasible.

## Status

Where things stand, 2026-09-24: the container/compression/update-mechanism/boot-sequence groundwork is
all done, most of the firmware's static structure is mapped, and a custom QEMU machine now boots real,
unmodified firmware to a fully-drawn, interactively-drivable main screen.

**Firmware format and boot, fully understood:**
- Container format accounts for **100%** of every firmware file's byte across all 10 releases; LZSS
  compression fully documented with a working compressor and a checksum-correct container packer,
  round-trip-verified against all 10 real releases — `sdk/roadmap.md`'s Phase-0 tooling gap is filled
  (`notes/container-format.md`, `notes/decompression-lzss.md`, `tools/icom_fw/`).
- Boot ROM/vector table and the full boot sequence traced through to `body.bin`'s entry point
  (`0x20005000` in RAM, correctly based in Ghidra); firmware update mechanism traced end-to-end
  (SD card → dual A/B flash slots → SPI-NOR erase/program) with **no cryptographic signature check
  found anywhere in the traced path** (`notes/base-loader.md`, `notes/memory-map.md`,
  `notes/firmware-update.md`).

**Main firmware, deeply mapped:**
- Kernel identified as FreeRTOS (Renesas RZ/A1H port with real Icom customizations); the full
  boot-time RTOS task catalog is resolved — every task has a known body/purpose except two confirmed
  JTAG-only dead ends (`notes/kernel-rtos.md`).
- CI-V transport and the real 43-entry command dispatcher fully traced, including one genuine
  undocumented command (`0x2A`/`0x01`, gates the antenna-tuner engage hardware); real service/factory
  mode fully decoded end to end (`notes/kernel-rtos.md`).
- UI icon/bitmap resource format fully solved (708 icons extracted/rendered, 150 individually
  identified, `notes/bitmaps.md`); UI menu/touchscreen list-widget and physical-button dispatch chain
  traced end-to-end (`notes/ui-menu.md`).
- Diode-matrix regional gating: 12 of 19 positions confirmed in code plus official per-diode
  version-population data; `region_code`→country mapping resolved (`notes/diode-matrix.md`,
  `notes/band-plans.md`). 6 positions remain unresolved.
- Full hardware BOM for the IC-7300 (all 5 boards) and, as a side investigation, the IC-9700
  (`notes/ic7300-hardware.md`, `notes/ic9700-hardware.md`); hardware JTAG debug access identified and
  a connector confirmed populated — adapter hardware ordered, not yet arrived
  (`notes/hardware-debug-access.md`).
- RZ/A1H peripheral SVD imported into Ghidra (70 peripherals, real register names/structs).

**Multi-CPU images:**
- SCIF5 identified as the real DSP command/data link; DSP/Front-CPU/FPGA image identities fully
  resolved (Icom's internal "Program"/"Data" naming was swapped from what this project first assumed)
  (`notes/multi-cpu-images.md`).
- DSP disassembly working: the DSP is a TMS320C6745 (TI C674x VLIW); three independent disassemblers
  (mainline binutils `tic6x`, Capstone, TI's own `dis6x`) agree byte for byte.
- Front-panel MCU (RL78, `IC501`) firmware's own location is still unidentified, but the `SCIF3` link,
  the key-press path, the version handshake, and the update-progress dialogs are all fully traced
  (`notes/front-panel-firmware.md`) — a real negative result, not an open thread.
- IC-9700 (different radio, side investigation): container structure mapped across all 37 known
  releases, but the compression/encryption scheme is **not yet cracked**
  (`notes/ic9700-container-format.md`).

**Emulation (the current center of gravity):**
- `emu/` — a minimal Unicorn Engine-based emulator reached its MVP (boots real firmware to `body.bin`'s
  entry point), then hit a genuine Unicorn engine limitation delivering periodic timer interrupts.
  Superseded by `qemu-machine/` for anything interrupt-dependent; still the fast tool for everything
  else. See `emu/README.md`.
- `qemu-machine/` — a custom QEMU machine for the same SoC boots real, unmodified firmware all the way
  to a fully-drawn main screen (14.100.00 USB) and a stable idle steady state. The front panel is
  drivable from a desktop window (mouse/keyboard/wheel, `tools/fp.py`), the band scope draws (a fake
  FPGA model), the CPU↔DSP link (command + audio) is behaviourally modelled, and a modified `body.bin`
  repacks and boots end-to-end in the emulator (commit 774acce) — the first real test that a
  `tools/icom_fw`-repacked custom image is accepted and runs. See `qemu-machine/README.md`.
- **The emulated SD card works** (2026-09-25): the firmware's SD driver is on SDHI0, not MMCIF;
  with `qemu-machine/src/sdhi.c` it mounts a FAT32 card image, creates its folders and saves
  settings files. Heavy mounts stall under `-icount` (use `--icount off` for SD work).

**Open / next**: run the firmware's own SD updater on a repacked `.dat` in the emulator, then a
live SD-card firmware-update test on real hardware (`sdk/roadmap.md`'s Phase 0 payoff), or a
custom-code hook. Also open: 6 diode-matrix positions, the IC-9700 compression scheme, and where real
front-panel firmware would live if it exists at all.

Older status entries and investigation narrative: [README-history.md](README-history.md).

## Layout

- `notes/` — reverse-engineering findings, one topic per file, linked with
  `[[wiki-links]]`. Start at `notes/container-format.md`. Several topics
  (`kernel-rtos`, `multi-cpu-images`, `diode-matrix`, `bitmaps`,
  `front-panel-firmware`, `ic9700-container-format`) are split into a lean
  active file (current-state tables/summaries, open questions) plus a
  sibling `<topic>-history.md` holding the full session-by-session
  narrative — read the active file first, the history file only for "how
  did we get here". Follow this split for any topic file that grows past a
  few hundred lines: move narrative to a new `<topic>-history.md`, keep
  only current-state content in the active file.
- `tools/` — `icom_fw`, a clean rewrite of the firmware unpacker, plus a
  cross-version verification script. See `tools/README.md`.
- `emu/` — a minimal Unicorn Engine-based firmware emulator, MVP reached 2026-09-08. See
  `emu/README.md`.
- `qemu-machine/` — a custom QEMU machine for the same SoC, escalated to once `emu/` hit a
  real Unicorn engine limitation around interrupt delivery. See `qemu-machine/README.md`.
- `ghidra_project/` (git-ignored) — fresh Ghidra project (`icom1`),
  created and in active use this session: `base.dat` + both A/B slots'
  `chunk1.ttf`/`chunk2.ttf`/`chunk3.dat` imported alongside `body.bin`.

## Ghidra MCP

Installed and in active use: [themixednuts/GhidraMCP](https://github.com/themixednuts/GhidraMCP)
v0.8.0, registered as the `ghidra` MCP server (see `.mcp.json`). Prior
projects at `/data/misc/icom/7300/` (`icom`, `icom_loader`, `vanah`,
`oisko`) have been reviewed read-only for pre-existing analysis to build
on; none contained a raw full-flash dump or companion-chip analysis.
