# IC-7300 firmware: emulator and homebrew SDK

A reverse-engineering workspace for the Icom IC-7300 HF transceiver that has grown two things you can
actually use:

- **A custom QEMU machine** that boots the real, unmodified firmware to a fully-drawn, interactively
  drivable main screen — a software IC-7300 on your desktop.
- **A homebrew SDK** for writing your own apps in C and running them on the radio (in the emulator
  today; the loader installs as a one-time firmware update) — a GUI "Hello world", a spinning 3D cube,
  Minesweeper, and an SSTV receiver all run.

Underneath both is an extensive set of reverse-engineering notes. For how the RE stands, see
[notes/status.md](notes/status.md); for the full narrative, the `*-history.md` files.

## Scope

This is hobbyist reverse-engineering and firmware modding of the author's own, physically-owned
IC-7300 — the same category as OpenWRT-style router re-flashing or game-console homebrew. The model
researched here has no built-in network interface at all (USB/serial only), and has since been
superseded on the market by the IC-7300MK2 (announced 2025). "No signature check on the firmware-update
path" only ever matters to someone with the radio physically in hand and an SD card.

> [!WARNING]
> **Nothing here has run on real hardware yet, and flashing it can brick your radio.**
> Everything in `sdk/` and `qemu-machine/` has been tested in the emulator only.
>
> **Before you flash any modified firmware, prepare and verify a way to re-flash a radio that no
> longer boots** — and test that method on your unit *before* you need it. Don't rely on the
> firmware's own SD-card updater for recovery: it runs inside the main firmware, so a modified image
> that crashes or hangs before the menu comes up leaves you no way to reach it. The A/B flash slots
> don't save you either: the boot loader starts whichever slot the update marked active, and no
> automatic fall-back to the previous slot has been found ([notes/firmware-update.md](notes/firmware-update.md)).
> The main CPU's JTAG header is the most likely recovery path
> ([notes/hardware-debug-access.md](notes/hardware-debug-access.md)), but nobody has yet restored a
> flash image through it. Until someone has, assume a bad flash means a trip to Icom service.
>
> You flash at your own risk.

## Firmware you supply

No Icom firmware, manuals, or schematics are redistributed here — supply your own legally-obtained
copies. The tooling reads firmware `.dat` files from the directory in `$ICOM_FW_DIR` (default
`firmware/` at the repo root). The default build target is `7300_142.dat` (v1.42). So either:

```
mkdir firmware && cp /path/to/7300_142.dat firmware/      # or:
export ICOM_FW_DIR=/path/to/your/firmware
```

## Prerequisites

- Linux, Python 3.10+, a C compiler and the usual build tools (for QEMU).
- `arm-none-eabi-gcc` / `binutils` — to build homebrew apps and the loader.
- `mtools` (`mmd`/`mcopy`) — to populate SD-card images.
- Python packages: `unicorn` (for `emu/`), plus `numpy`, `Pillow`, `scipy` for some tooling. The
  simplest setup is a venv at `emu/.venv` (the docs refer to `emu/.venv/bin/python3`).
- QEMU is built from a pinned upstream checkout by `qemu-machine/setup.sh` (no system QEMU needed).

## Run the emulator

```
qemu-machine/setup.sh                                      # one-time: clones + patches + builds QEMU
emu/.venv/bin/python3 qemu-machine/tools/build_flash.py    # -> qemu-machine/flash.bin (from your firmware)
emu/.venv/bin/python3 qemu-machine/tools/build_riic_eeprom_image.py   # -> riic2_eeprom.img

python3 qemu-machine/tools/run_gui.py                      # live GTK front-panel window
```

The GTK window is a working front panel: mouse = touchscreen, wheel = main dial, keyboard maps to the
physical keys. The band scope draws, CI-V works over a socket, and audio can be driven from stimulus
files. See `run_gui.py --help` and [qemu-machine/README.md](qemu-machine/README.md) for every flag,
the headless/QMP/GDB launch, and the confirmed-peripheral list.

## Write a homebrew app

An app is a plain-C program built into an `APP.BIN` you drop in `\homebrew\` on the SD card. It runs
under a one-time loader firmware that adds a "Homebrew Apps" row to MENU > SET > SD Card.

```
python3 sdk/tools/build_app.py -o HELLO.BIN sdk/examples/hello-gui/main.c
```

Apps get a full-screen double-buffered canvas over the radio's own UI, touch and key input, a ms clock,
a heap, and (ABI v4) a tap on the receive audio. The runtime runs your `main()` as a coroutine on the
UI thread, so blocking calls like `ui_message_box()` work without freezing the radio. Start from
[sdk/README.md](sdk/README.md) for the API tour and [sdk/loader/README.md](sdk/loader/README.md) for
building the loader and the full end-to-end test. Worked examples are under `sdk/examples/`
(`hello-gui`, `about-box`, `cube`, `minesweeper`, `sstv-rx`).

## Layout

- `notes/` — reverse-engineering findings, one topic per file, linked with `[[wiki-links]]`. Start at
  [notes/status.md](notes/status.md). Several topics are split into a lean active file plus a
  `<topic>-history.md` narrative companion — read the active file first. Local RE setup (Ghidra base
  address, the MCP server, firmware/manual paths) is in [notes/re-workflow.md](notes/re-workflow.md).
- `tools/` — `icom_fw`, a clean firmware (un)packer, plus cross-version verification and
  disassembly helpers. See [tools/README.md](tools/README.md).
- `emu/` — a minimal Unicorn Engine-based emulator; the fast tool for everything not
  interrupt-dependent. See [emu/README.md](emu/README.md).
- `qemu-machine/` — the custom QEMU machine (the main emulator). See
  [qemu-machine/README.md](qemu-machine/README.md).
- `sdk/` — the homebrew SDK: loader firmware, C runtime, headers, example apps, and design/API docs.
  See [sdk/README.md](sdk/README.md).

Older top-level status and narrative: [README-history.md](README-history.md).

## Licence

MIT — see [LICENSE](LICENSE), which also lists the exceptions: `qemu-machine/patches/` (changes to
QEMU itself) is GPL-2.0-or-later like QEMU, and material belonging to Icom (the firmware, icons
extracted from it, screenshots of its UI, firmware-generated EEPROM images) isn't ours to license.
The `qemu-machine/src/` device models are MIT, but a QEMU binary built with them is distributed
under QEMU's GPL.

Not affiliated with or endorsed by Icom. "IC-7300" and "Icom" are Icom Inc. trademarks.

## Credits

The SSTV mode timings and VIS codes in `sdk/examples/sstv-rx/` follow
[slowrx](https://github.com/windytan/slowrx) by Oona Räisänen (ISC licence), via
[slowrx-cli](https://github.com/sgarriga/slowrx-cli); the decoder itself is original. The QEMU
machine builds on [QEMU](https://www.qemu.org/) (GPL-2.0-or-later) and reuses its generic SD-card
and 24Cxx EEPROM models.
