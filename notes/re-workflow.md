# Reverse-engineering setup and workflow

This is the local tooling the notes in this directory were produced with. None of it is needed to
build or run the emulator or the SDK (see the top-level [README.md](../README.md) for those) — it
matters only if you want to reproduce or extend the static analysis.

## Firmware and reference material

The notes are written against the 10 official IC-7300 firmware releases (v1.11–v1.42) plus Icom's
service and schematic manuals and the Renesas RZ/A1H SoC manual. None of that is redistributed here —
supply your own copies. The build/verify tooling looks for the firmware `.dat` files in the directory
given by `$ICOM_FW_DIR` (default `firmware/` at the repo root); see the README's "Firmware you supply".

Notes that cite a manual use a repo-relative `docs/<file>.pdf` placeholder for Icom's/Renesas's PDFs
and `rza1.svd` for the Renesas SVD — point these at your own copies.

## Ghidra

- `body.bin` is loaded at base **`0x20005000`** (address = file offset + `0x20005000`). Any raw-file
  pointer scan must use this base, not `0x20000000`.
- The working project imports `base.dat` + both A/B slots' `chunk1.ttf`/`chunk2.ttf`/`chunk3.dat`
  alongside `body.bin`. The project DB itself is regenerated locally and git-ignored
  (`ghidra_project/`).
- The RZ/A1H peripheral SVD is imported (70 peripherals with real register names/structs).

### Ghidra MCP (optional)

The analysis was driven partly through [themixednuts/GhidraMCP](https://github.com/themixednuts/GhidraMCP)
v0.8.0, registered as the `ghidra` MCP server (see `.mcp.json`, which points at a local
`http://127.0.0.1:8080/mcp`). This is a convenience for agent-driven analysis, not a requirement —
the notes can be reproduced in the Ghidra GUI directly.

## Note-keeping conventions

- One topic per file, linked with `[[wiki-links]]`.
- `X.md` holds current state (facts, living tables, open questions). Session narrative goes in a
  sibling `X-history.md`; fold only the delta into `X.md`. Move text into the history file rather than
  deleting it. Split a file into a `-history.md` companion once it grows past a few hundred lines.
- Verify claims against listings or live runs before recording them as confirmed.
