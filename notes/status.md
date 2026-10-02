# Project status

Where the reverse-engineering stands. The top-level [README.md](../README.md) is the usage guide
(emulator, SDK); this file is the RE scoreboard. Older status entries and the full narrative live in
[../README-history.md](../README-history.md) and the per-topic `*-history.md` files.

Last updated: 2026-09-25.

## Summary

The container/compression/update-mechanism/boot-sequence groundwork is done, most of the firmware's
static structure is mapped, and a custom QEMU machine boots real, unmodified firmware to a fully-drawn,
interactively-drivable main screen. On top of that, a homebrew SDK runs custom C apps from the SD card,
live-tested in the emulator.

## Firmware format and boot — fully understood

- Container format accounts for **100%** of every firmware file's bytes across all 10 releases; LZSS
  compression fully documented with a working compressor and a checksum-correct container packer,
  round-trip-verified against all 10 real releases ([container-format.md](container-format.md),
  [decompression-lzss.md](decompression-lzss.md), `tools/icom_fw/`).
- Boot ROM/vector table and the full boot sequence traced through to `body.bin`'s entry point
  (`0x20005000` in RAM, correctly based in Ghidra); firmware update mechanism traced end-to-end
  (SD card → dual A/B flash slots → SPI-NOR erase/program) with **no cryptographic signature check
  found anywhere in the traced path** — the only integrity check is an unkeyed MD5
  ([base-loader.md](base-loader.md), [memory-map.md](memory-map.md),
  [firmware-update.md](firmware-update.md)).

## Main firmware — deeply mapped

- Kernel identified as FreeRTOS (Renesas RZ/A1H port with real Icom customizations); the full
  boot-time RTOS task catalog is resolved — every task has a known body/purpose except two confirmed
  JTAG-only dead ends ([kernel-rtos.md](kernel-rtos.md)).
- CI-V transport and the real 43-entry command dispatcher fully traced, including one genuine
  undocumented command (`0x2A`/`0x01`, gates the antenna-tuner engage hardware); real service/factory
  mode fully decoded end to end ([kernel-rtos.md](kernel-rtos.md)).
- UI icon/bitmap resource format fully solved (708 icons extracted/rendered, 150 individually
  identified, [bitmaps.md](bitmaps.md)); UI menu/touchscreen list-widget and physical-button dispatch
  chain traced end-to-end ([ui-menu.md](ui-menu.md)).
- Diode-matrix regional gating: 12 of 19 positions confirmed in code plus official per-diode
  version-population data; `region_code`→country mapping resolved ([diode-matrix.md](diode-matrix.md),
  [band-plans.md](band-plans.md)). 6 positions remain unresolved.
- Full hardware BOM for the IC-7300 (all 5 boards) and, as a side investigation, the IC-9700
  ([ic7300-hardware.md](ic7300-hardware.md), [ic9700-hardware.md](ic9700-hardware.md)); hardware JTAG
  debug access identified and a connector confirmed populated
  ([hardware-debug-access.md](hardware-debug-access.md)).
- RZ/A1H peripheral SVD imported into Ghidra (70 peripherals, real register names/structs).

## Multi-CPU images

- SCIF5 identified as the real DSP command/data link; DSP/Front-CPU/FPGA image identities fully
  resolved ([multi-cpu-images.md](multi-cpu-images.md)).
- DSP disassembly working: the DSP is a TMS320C6745 (TI C674x VLIW); three independent disassemblers
  (mainline binutils `tic6x`, Capstone, TI's own `dis6x`) agree byte for byte.
- Front-panel MCU (RL78, `IC501`) firmware's own location is still unidentified, but the `SCIF3` link,
  the key-press path, the version handshake, and the update-progress dialogs are all fully traced
  ([front-panel-firmware.md](front-panel-firmware.md)) — a real negative result, not an open thread.
- IC-9700 (different radio, side investigation): container structure mapped across all 37 known
  releases, but the compression/encryption scheme is **not yet cracked**
  ([ic9700-container-format.md](ic9700-container-format.md)).

## Emulation

See [../qemu-machine/README.md](../qemu-machine/README.md) and [../emu/README.md](../emu/README.md).

- `emu/` — a minimal Unicorn Engine-based emulator; boots real firmware to `body.bin`'s entry point,
  then hit a genuine Unicorn limitation delivering periodic timer interrupts. Superseded by
  `qemu-machine/` for anything interrupt-dependent; still the fast tool for everything else.
- `qemu-machine/` — a custom QEMU machine for the same SoC boots real, unmodified firmware all the way
  to a fully-drawn main screen (14.100.00 USB) and a stable idle steady state. The front panel is
  drivable from a desktop window (mouse/keyboard/wheel, `tools/fp.py`), the band scope draws (a fake
  FPGA model), the CPU↔DSP link (command + audio) is behaviourally modelled, and the emulated SD card
  mounts a FAT image, creates folders, and saves settings files.
- The firmware's own SD updater installs and boots a modified image in the emulator: MD5 ok, slot B
  written, marker committed, watchdog restart, boots the new slot — Phase 0 fully rehearsed in
  emulation ([firmware-update.md](firmware-update.md)).

## Homebrew SDK

See [../sdk/README.md](../sdk/README.md) and [../sdk/loader/README.md](../sdk/loader/README.md).

- `sdk/loader/` is a one-time loader firmware ("Homebrew Apps" row in SET > SD Card, an idle tick, a
  header-checked `APP.BIN` ABI). Apps are plain C (`sdk/tools/build_app.py`); blocking UI calls work
  via a coroutine runtime. Any number of apps sit in `\homebrew\` on the card and are listed in a
  firmware list screen (up to 14). Apps can take over the whole screen (a full-screen canvas on the
  top VDC5 plane with touch input), get a 448 KB heap, and (ABI v4) tap RX audio.
- Example apps, all live-tested in the emulator: `hello-gui` (firmware dialog), `about-box`, `cube`
  (spinning 3D cube), `minesweeper` (App 3, playable 10×10), `sstv-rx` (App 4, Scottie/Martin SSTV
  receiver decoding to an image).

## Open / next

- **Phase 0 on real hardware** — a live SD-card firmware-update test of a repacked `.dat` on the actual
  radio (`sdk/roadmap.md` Phase 0 payoff), and then running the SDK loader there. Nothing in `sdk/` has
  run on real hardware yet; it is all emulator-tested.
- `-icount` stall on heavy SD mounts (use `--icount off` for SD work).
- The CI-V `27 00` scope-waveform output and fixed-mode (VFO-offset) scope behaviour are not yet checked
  against the fake FPGA.
- 6 diode-matrix positions remain unresolved; the IC-9700 compression scheme is uncracked; where real
  front-panel firmware would live (if it exists at all) is unresolved.
- A handful of emulator-only glitches noted in [../qemu-machine/README.md](../qemu-machine/README.md)
  (SD drop-out during Save + Load Setting; a one-off hang after a CI-V band change).
