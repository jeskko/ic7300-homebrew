# IC-7300 firmware reverse engineering

Agentic RE workspace for the Icom IC-7300 HF transceiver firmware, working
alongside the existing (read-only) material at `/data/misc/icom/7300/` —
firmware images for all 10 official releases (v1.11–v1.42), service and
schematic manuals, a Renesas RZ/A1H SoC manual, hand-written notes, and
several prior Ghidra projects.

Nothing under `/data/misc/icom/7300/` is ever modified — this repo holds
our own notes, tooling, and (once created) a fresh Ghidra project, informed
by but not built on top of the prior work there.

## Status

- ✅ Container format understood well enough to account for **100%** of
  every firmware file's bytes across all 10 releases (see `tools/`,
  `notes/container-format.md`) — the original extraction script
  (`tunk3.py`) silently dropped ~37% of each file.
- ✅ LZSS decompression algorithm fully documented (`notes/decompression-lzss.md`).
- ✅ Boot ROM / vector table structure in `base.dat` mapped, and the full
  boot sequence traced in Ghidra through to the main firmware's entry
  point (`notes/base-loader.md`).
- ✅ Main firmware (`body.bin`) correctly based in Ghidra at `0x20005000`
  (RAM, not `0x18000000`/flash — a wrong initial guess corrected this
  session), with real backing memory for the flash-resident boot loader
  and both A/B update slots' font/chunk data (`notes/memory-map.md`).
- ✅ Firmware update mechanism fully traced end-to-end: SD-card source,
  dual A/B flash slots, SPI-NOR erase/program routines, and a preliminary
  (unconfirmed) finding that no cryptographic signature check exists in
  the update path (`notes/firmware-update.md`).
- ✅ Main-CPU kernel identified: **FreeRTOS**, Renesas RZ/A1H port (AN
  R01AN5093, matching source now in `scratch/r01an5093ej0170-rza1-swpkg/`),
  confirmed by direct comparison against the decompiled SWI handler —
  with real Icom customizations on top (unprivileged user-mode tasks,
  per-task ASID/MMU isolation) not present in the stock port
  (`notes/kernel-rtos.md`).
- ✅ **"SX3765" identity resolved**: it's the part marking on `IC501`
  (`R5F104LCAFB`, Renesas RL78), the Display Unit's own MCU — confirmed
  via the service manual's Display Unit parts list and independently
  visible on the Front Unit schematic sheet. Every `"SX3765 Vx.xx-yyy"`
  string in the firmware is a compatibility/version check against this
  chip, not an embedded second-processor firmware image
  (`notes/multi-cpu-images.md`).
- ✅ **Diode-matrix regional gating**: physical layout, scan mechanism,
  bit-to-diode mapping, and individual diode functions confirmed in code
  for 11 of 19 documented positions
  (D401/403/404/405/406/407/409/410/413/416/423 — the last three added a
  4th session in via raw ARM disassembly after the Ghidra MCP connection
  dropped, including strong direct evidence for D423 = Emergency Mode),
  plus **official per-diode version-population data** from the service
  manual parts list (which diode is populated on which of the 7 named
  export variants). 7 positions remain unresolved (D408/411/414/417
  fully unknown, D419/D420/D422 have hypotheses but no code consumer
  found across 4 sessions) — a newly-found likely clone/full-settings-export
  function is the top lead for finishing these
  (`notes/diode-matrix.md`).
- ✅ **Full hardware BOM** for both the IC-7300 (all 5 boards: Main,
  Display, RF, PA, Tuner — `notes/ic7300-hardware.md`) and, as a side
  investigation, the IC-9700 (`notes/ic9700-hardware.md`), plus the
  IC-7300's complete SDR signal chain and a 17-page schematic sheet map
  (`notes/ic7300-signal-chain.md`).
- ✅ **Full boot-time RTOS task catalog**: all tasks the scheduler ever activates identified,
  named, and disassembly-confirmed (SD-card menu, screen capture, audio buffering, a generic
  file-access RPC service, system monitor/DRESD-init, several small poll/queue tasks) — see
  `notes/kernel-rtos.md`'s task catalog and the "Full boot-time task catalog" section.
- ✅ **CI-V transport confirmed at the code level** (not just pins): SCIF0's driver implements
  real `FE`/`FE`/dst/src/cmd CI-V framing with destination-address filtering. **Open**: the
  actual frequency/mode command dispatcher consuming those frames hasn't been located.
- ✅ **Real service/factory mode fully decoded, end to end**: entry condition (front-panel
  MENU+FUNCTION held **and** the REMOTE/CI-V jack's contacts shorted, detected via a raw GPIO
  pin-read of the CI-V receive pin), the boot-time code that checks it, and the reduced-
  functionality mode it enters (only CI-V and a second, parallel calibration-shaped protocol
  on a separate UART channel stay active) — see `notes/kernel-rtos.md`'s "Factory/service
  mode" section. A structurally similar internal file-RPC service (mistakenly named
  `civ_command_dispatch_task` in an earlier session — retracted) was found along the way.
- ✅ **Hardware JTAG debug access** identified and a connector confirmed populated on the
  board — see `notes/hardware-debug-access.md`. Adapter hardware ordered, not yet arrived;
  once available, several open items above (the real CI-V dispatcher, some task-activation
  and mode-2/pin-mux questions) are flagged as better resolved live than by continued static
  guessing.
- ✅ **UI icon/bitmap resource format fully solved**: a 708-entry pointer table
  (`g_icon_table`, RAM `0x20335234`) indexes 32-byte header structs (offset/
  width/height) each followed by tightly-packed BGRA8888 pixel data, row
  stride padded to a 4-pixel boundary (both quirks found by re-reading the
  real consumer code, `icon_blit_by_id_v1`/`_v2`, after user-caught decode
  bugs); extracted and rendered all 708 icons cleanly (`tools/extract_icons.py`)
  and individually identified/renamed 150 of them in Ghidra — the complete
  touchscreen UI icon set (`TUNE`/`SPLIT`/filter labels/meters/arrows/menu
  icons/etc.), confirmed visually (`notes/bitmaps.md`, `notes/icon_table.csv`).
- ✅ **`SCIF5` identified as the real DSP command/data link** (a 5th serial
  channel, not previously catalogued): full transport chain traced from
  `firmware_update_main`'s "3 extra chunks" mechanism down to the literal
  `SCFTDR_5` register, MTU2-timer-paced; also carries live, ongoing
  parameter-sync traffic during normal operation, not just updates. Confirms
  DSP Program/DSP Data get written **live during the firmware update**, not
  deferred to a post-restart check — almost certainly by the DSP itself
  reprogramming its own boot flash (`IC902`, corrected this session from an
  earlier "FPGA config flash" misattribution — its `CS`/`DO`/`DI`/`CLK` pins
  trace directly to the DSP's own `BOOT[4:0]`/SPI0 strapping pins). **Open**:
  where `DRESD` (DSP reset) actually gets released — still not found in the
  traced main-CPU call graph despite this — see `notes/multi-cpu-images.md`.
- 🔎 **Open, side investigation**: IC-9700 (different radio, separate
  firmware format) — container structure mapped and compared across all
  37 known releases, but the compression/encryption scheme itself is
  **not cracked** after a thorough negative sweep (LZSS-family
  parameters, buffer-seeding, XOR-whitening all tried and ruled out) —
  see `notes/ic9700-container-format.md`. Genuine cold-start effort, no
  prior art existed for this radio going in.

## Layout

- `notes/` — reverse-engineering findings, one topic per file, linked with
  `[[wiki-links]]`. Start at `notes/container-format.md`.
- `tools/` — `icom_fw`, a clean rewrite of the firmware unpacker, plus a
  cross-version verification script. See `tools/README.md`.
- `ghidra_project/` (git-ignored) — fresh Ghidra project (`icom1`),
  created and in active use this session: `base.dat` + both A/B slots'
  `chunk1.ttf`/`chunk2.ttf`/`chunk3.dat` imported alongside `body.bin`.

## Ghidra MCP

Installed and in active use: [themixednuts/GhidraMCP](https://github.com/themixednuts/GhidraMCP)
v0.8.0, registered as the `ghidra` MCP server (see `.mcp.json`). Prior
projects at `/data/misc/icom/7300/` (`icom`, `icom_loader`, `vanah`,
`oisko`) have been reviewed read-only for pre-existing analysis to build
on; none contained a raw full-flash dump or companion-chip analysis.
