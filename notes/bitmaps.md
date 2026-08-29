# UI icon/bitmap resources — SOLVED

See [notes/bitmaps-history.md](bitmaps-history.md) for the full session-by-session narrative,
including two dead-end approaches (raw-frame-buffer width guessing) and two real decode bugs found
and fixed along the way (channel order, row stride) — every image rendered before that fix is
unreliable, don't reuse it as a reference.

This closes the 2026-08-26 open question about uncompressed material seen past the compressed
image: it's the touchscreen UI's icon/glyph resource set, format and structure both fully known.

## Format and structure (final, confirmed)

- **`g_icon_table`** — RAM `0x20335234`, 708 entries × 4 bytes, a pointer array indexed by icon ID.
  Ghidra's own auto-analysis had already bookmarked this as `Address table[708]`, unconnected to
  bitmap data until this investigation.
- Each entry points to a 32-byte **`icon_header`** struct, immediately followed by that icon's
  pixel data (no gap, no per-asset size field):
  ```
  +0x00  ?
  +0x04  u32 data_offset   (always 0x20 — the header size itself; data starts right after the header)
  +0x08  ?
  +0x0c  u16 width
  +0x0e  u16 height
  +0x10..0x1f  ?
  +data_offset  pixel data, BGRA8888 (4 bytes/pixel), row stride = ((width+3)>>2)<<4
                (width padded UP to a 4-pixel/16-byte boundary before ×4 — NOT simply width*4)
  ```
- **Consumers**: `icon_blit_by_id_v1` (`0x200ae4d4`) and `icon_blit_by_id_v2` (`0x200b0294`, adds
  x/y scale params) — both bounds-check the icon ID against `708` (`0x2c4`), then pass
  `header + data_offset` / `width` / `height` into an internal 2D command-queue blitter. This is
  where the BGRA-not-RGBA and padded-stride facts were confirmed, after two rounds of user-caught
  decode bugs in the extraction tooling (see history).
- **319 unique icons** back 708 table slots: 299 simple pairs (`table[i]` and `table[i+354]` point
  to the literal same header — hypothesis: one bank per rendering context, e.g. main-VFO vs.
  sub-VFO strip), 18 small tick-mark-shaped icons shared across 4-8 slots each (meter/scale-segment
  building blocks), and 2 true singletons (ids `292`/`646`, both 47×43, not yet identified).

## Tools

- `tools/extract_icons.py` — decodes the full table from a raw `body.bin`; per-icon PNGs
  (`--out-dir`) or a single contact-sheet montage (`--montage`). Table address/count are parameters
  (default `0x20335234`/708 for this firmware version) in case they shift between releases.
- `tools/label_icons.py` — a montage with icon_id printed on each cell, for visual review only (the
  addressable labeling lives in Ghidra + `notes/icon_table.csv`, never baked into an image).

## Current state

150 of 319 unique icons individually identified and renamed in Ghidra (`icon_TUNE_off/active/...`,
the full main-menu set, meter-select labels, filter-selector states, transport controls, status
badges, and more) — see `notes/icon_table.csv` for the full 708-row id/address/dimensions/label
table and `notes/icon_table.png` for a corrected contact-sheet montage of the whole set.

## Open questions

- Plain `"E"` box (id `3`) — meaning not identified.
- The tick-mark-shaped meter-segment family — structurally understood (shared 4-8 ways across
  slots) but not individually named.
- Two singleton icons (ids `292`/`646`, 47×43) — not identified.
- ~230 narrow vertical bars (guess: S-meter/ALC/SWR/PWR-style level-meter segments) and wide thin
  bars up to 480px (guess: band-scope/frequency-scale rulers, matching the touchscreen's 480px
  width) plus ~90×90 diagonal-stripe-with-corner-bracket icons (guess: band-scope corner decoration
  or an out-of-band warning overlay) — all left open for the user rather than guessed further.
