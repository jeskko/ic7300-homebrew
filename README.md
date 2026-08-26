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
- ✅ Boot ROM / vector table structure in `base.dat` mapped
  (`notes/base-loader.md`).
- 🔎 **Open**: where a second processor's firmware (companion chip
  `"SX3765"`) actually lives — current best lead is a previously-undiscovered
  ~1.46 MB compressed region at the end of every container
  (`notes/multi-cpu-images.md`).
- 🔎 **Open**: user-flagged raw/uncompressed bitmap material not yet pinned
  down to a specific offset (`notes/bitmaps.md`).
- ⏳ **Blocked on user**: Ghidra MCP bridge install — needs the user to
  close their current Ghidra session first (see below).

## Layout

- `notes/` — reverse-engineering findings, one topic per file, linked with
  `[[wiki-links]]`. Start at `notes/container-format.md`.
- `tools/` — `icom_fw`, a clean rewrite of the firmware unpacker, plus a
  cross-version verification script. See `tools/README.md`.
- `ghidra_project/` (git-ignored) — fresh Ghidra project, once created.

## Ghidra MCP setup (pending)

Plan: install [themixednuts/GhidraMCP](https://github.com/themixednuts/GhidraMCP)
v0.8.0 (targets Ghidra 12.1, matching the installed 12.1.2), replacing two
failed install attempts from earlier today (see git log / session notes).
Requires the user to close their currently-open Ghidra session first, then
restart Ghidra after the extension is installed.
