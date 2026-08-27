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
  bit-to-diode mapping, and several individual diode functions confirmed
  in code (D401/403/404/405/407/410/413/416), plus **official per-diode
  version-population data** from the service manual parts list (which
  diode is populated on which of the 7 named export variants) —
  including a sharp new lead on long-unresolved `D419` (Japan-only) and
  a flagged conflict with an earlier third-party claim about `D420`
  (`notes/diode-matrix.md`).
- ✅ **Full hardware BOM** for both the IC-7300 (all 5 boards: Main,
  Display, RF, PA, Tuner — `notes/ic7300-hardware.md`) and, as a side
  investigation, the IC-9700 (`notes/ic9700-hardware.md`), plus the
  IC-7300's complete SDR signal chain and a 17-page schematic sheet map
  (`notes/ic7300-signal-chain.md`).
- 🔎 **Open**: user-flagged raw/uncompressed bitmap material not yet pinned
  down to a specific offset (`notes/bitmaps.md`).
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
