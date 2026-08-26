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
- 🔎 **Open**: where a second processor's firmware (companion chip
  `"SX3765"`) actually lives — investigation so far favors it being a
  repurposed marker string for the A/B update slots rather than an
  embedded image; the container's `chunk5`/tail region remains the one
  unexplained candidate (`notes/multi-cpu-images.md`).
- 🔎 **Open**: user-flagged raw/uncompressed bitmap material not yet pinned
  down to a specific offset (`notes/bitmaps.md`).

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
