# UI icon/bitmap resources — full session history

Full narrative and evidence trail for the "raw/uncompressed bitmap material" investigation. See
[notes/bitmaps.md](bitmaps.md) for the current, condensed understanding (final format, structures,
tools, and what's still open) — this file is the permanent record, kept verbatim for the exact
reasoning, addresses, and dead ends behind each conclusion, including approaches later superseded
by better evidence.

## Original open question (2026-08-26)

User noted that there is clearly uncompressed material past the compressed image — e.g. some bitmap
images — that hasn't been accounted for yet. Not yet clear whether this means:
(a) raw bitmap resources embedded *within* the decompressed main body
    (`out.dat`) — normal for UI icons/splash screens/waterfall assets, or
(b) raw bitmap data living in one of the still-unidentified chunks
    (`chunk3.dat`, `chunk5`/tail, `snip`) themselves being partially
    uncompressed rather than fully LZSS streams end-to-end.

**Need the specific offset/version/chunk from the user to confirm** —
noting what's been checked so far so we don't redo it:

## What a quick entropy scan turned up (v1.42, `out.dat`)

A windowed (4 KiB) Shannon-entropy scan of the decompressed main body shows
mostly ~6.0–6.9 bits/byte (typical code+data mix) except one distinctly
higher band:

- **`0x331000`–`0x33e000`** (~52 KiB): entropy jumps to **7.4–7.7
  bits/byte**, well above the surrounding code/data. This is the most
  likely candidate region so far for the bitmap material the user
  mentioned, or possibly still-compressed data that survived the initial
  container-level decompression (an image codec's own compression, or a
  second LZSS layer). Not yet extracted or visually rendered.

Everything else scanned (`out.dat` broadly, and `chunk5`/tail's decoded
content, see [[multi-cpu-images]]) sits in the 6.2–6.9 band throughout —
consistent with code/mixed data, not obviously raw bitmap pixel data
(which for a small-palette or greyscale UI asset would usually show up as
*lower* entropy than this, not higher — raw bitmaps only read this high if
they're deep-color/photographic, which seems unlikely for a radio's
control-panel UI).

## Candidate region found incidentally, 2026-08-29 (byte-format confirmed, not yet rendered)

Found while chasing an unrelated ARM/Thumb disassembly-context bug (see `notes/kernel-rtos.md`'s "Whole-
program Thumb-conflict hunt, part 2"): a coarse `objdump`-based scan of `body.bin` flagged large stretches
around RAM `0x201ad000`-`0x2035d000` (file offset `0x1a8000`-`0x358000`) as neither clean ARM nor clean
Thumb code — spot-checking several samples showed why: **highly repetitive 4-byte groups that read exactly
like solid-fill 8-bit RGBA pixel runs**, e.g.:

- `0x201b5000`: `FF FF 00 FF` repeated — R=FF G=FF B=00 A=FF (opaque yellow)
- `0x202c1000`: `FF 7B 7D 7B` repeated — a muted solid fill
- `0x20261000`: `85 85 00 85` repeated — an olive/translucent fill

User independently confirmed (2026-08-29) that 8-bit RGBA bitmap material really does exist in `body.bin` —
matches this byte-pattern read directly. This region **overlaps and extends** the earlier entropy-scan
candidate above (`0x331000`-`0x33e000` file offset = RAM `0x20336000`-`0x20343000` sits inside it) — the
entropy scan's "surprisingly high" reading and this session's "surprisingly repetitive" reading are two
faces of the same kind of material: real bitmap data has both solid-fill regions (low local entropy, what
this session found) and detailed/dithered/anti-aliased regions (high local entropy, what the earlier scan
found), which is exactly what a real UI icon or splash-screen asset looks like.

**Tried and failed**: rendering a large slice (`0x1a8000`-`0x220000`, ~480 KB) as one uniform RGBA buffer at
several candidate widths (480/320/272/256/160/128/96/64/32, the touchscreen's 480 px width included) — none
produced a coherent image, just horizontal banding/noise that persisted across every width tried. That
banding-regardless-of-width pattern is itself informative: it means this almost certainly is **not one
uniform frame buffer**, but a **packed sequence of many small discrete bitmap assets** (icons/glyphs), each
probably with its own small header (width/height/format) ahead of its pixel data — a single arbitrary global
stride was never going to line up with that.

**Next step, concretely scoped**: don't keep guessing strides. Either (a) find the actual resource-loader
code that reads this region in Ghidra (a function taking a resource ID/offset and a destination frame
buffer, likely called from the LCD/UI drawing subsystem) to get real per-asset dimensions and confirm the
RGBA8888 format guess, or (b) if the user can point at a specific known icon (its approximate on-screen
size/shape), search this byte range for a header-sized run of small integers matching that size as a
starting anchor.

## Real icons rendered, real 708-entry reference table found (2026-08-29, same day)

User asked to locate actual glyphs/icons and check for a header/footer or a reference table. Found both, to
different degrees of completeness:

**Real icons, visually confirmed, RGBA8888 format nailed down (later corrected to BGRA8888 — see below).**
At RAM `0x201b4000` (file offset `0x1af000`), a run of icons renders **perfectly cleanly at a fixed 24px
width** (96-byte row stride) — a solid black up-arrow, an orange up-arrow, and bracket/pipe glyphs, all with
proper anti-aliased edges (partial-alpha border pixels). This is unambiguous, no longer a guess: real UI
icon assets, 4 bytes/pixel (confirmed opaque black pixels read `00 00 00 ff`, channel-order call corrected
later).

**No explicit header/footer field found.** The bytes immediately before `0x201b4000` are not a header —
they're a smooth 8-step alpha ramp (`ff d3 91 73 4c 2e 16 07 00`), i.e. the anti-aliased trailing edge of
the *previous* packed icon (a round/circular one, judging by the gradient). Icons are packed **back-to-back
with zero padding and no per-asset size field** — consistent with fixed dimensions supplied by the caller
(icon ID → hardcoded width/height in code) rather than self-describing image data.

**Found the reference table the user asked about.** A literal-pool search for pointers landing inside the
confirmed icon region turned up a cluster at RAM `~0x20335750`, which — checked against Ghidra's own prior
auto-analysis — sits inside a table Ghidra had *already* auto-detected and bookmarked as `Address table[708]`
at `0x20335234` (never previously connected to the bitmap-data investigation). Dumped and checked all 708
entries directly from the raw image: **every single one** lands inside `0x2010xxxx`-`0x20302f8c`, i.e.
squarely inside the same broad "not code" region this session already flagged as bitmap material — 210 in
the `0x2010xxxx`-`0x201fffff` band, 497 in `0x2020xxxx`-`0x202fffff`, 1 at the top edge. This is almost
certainly the **full icon/glyph resource index** — one pointer per asset, 708 assets total. A few entries
(`0x201af21c`, `0x201ac47c`, `0x201b1fbc`) sit right next to the confirmed-clean icon sheet, reinforcing the
connection.

**Update, same session — SOLVED. Header struct found, consumer code found, all 708 icons extracted
cleanly.** The "different width per icon" problem above was solved by *not* treating table entries as raw
pixel-data pointers — they're pointers to a per-icon **header struct**. Found by searching for direct literal
references to the table's own base address (`0x20335234`, not its entries) and decompiling the two functions
that read it:

```
g_icon_table (RAM 0x20335234, 708 entries x 4 bytes = pointer array)
    entry[icon_id] -> icon_header

icon_header (32-byte struct, pixel data packed immediately after):
    +0x00  ?
    +0x04  u32 data_offset   (self-relative to pixel data; EVERY one of the 708
                               entries has data_offset == 0x20, i.e. the header
                               size itself -- no padding, data starts right
                               after the header)
    +0x08  ?
    +0x0c  u16 width
    +0x0e  u16 height
    +0x10..0x1f  ?
    +data_offset  pixel data, width*height*4 bytes (format corrected below),
                  row stride corrected below (not simply width*4)
```

Consumer functions, renamed in Ghidra: **`icon_blit_by_id_v1`** (`0x200ae4d4`) and **`icon_blit_by_id_v2`**
(`0x200b0294`, adds explicit x/y scale params) — both do `header = g_icon_table[icon_id]` with a bounds
check against `0x2c4` (**708 decimal, matches the table size exactly** — confirms the table boundary), then
pass `header + header->data_offset` / `header->width` / `header->height` into an internal display/2D
command-queue blitter (`FUN_200ffdf2`/`FUN_200ffe66`/etc. — not chased further, doesn't matter for extraction).

**All 708 entries decoded and rendered — 708/708 produced sane, in-range dimensions, zero failures** (using
the pre-correction RGBA/width*4 assumptions — see the decode-bug section below for why these first renders
were later found to be channel-swapped and shear-distorted, though still legible enough to identify content).
The contact-sheet montage is an unambiguous UI icon set: `TUNE`/`E-TUN`/`TX`/`SPLIT` mode-indicator labels
(each in 2-3 color/state variants), `ATT 10dB`/`ATT 30dB`, `S` (S-meter?), `CENTER`/`FIX`/`SCROLL-C`/
`SCROLL-F`, `BPF`/`SFT`/`SHARP`/`SOFT` filter labels, circled digits `1`/`2`, play/stop/arrow glyphs,
up/down/left/right triangles, scale tick-mark strips, solid-color level/progress bars, and diagonal
hazard-stripe graphics (band-edge/warning overlays) — the complete touchscreen UI's icon set, essentially
confirmed at a glance. The whole table appears to repeat with a second, visually near-identical pass partway
through (rows ~18-35 mirror rows ~0-17) — likely a highlighted/selected-state duplicate set, not investigated
further at the time (see the labeling-pass section below, which found the real structural explanation).

**Reusable tool**: `tools/extract_icons.py` — decodes the full table from a raw `body.bin`, writes either
one PNG per icon (`--out-dir`) or a single contact-sheet montage (`--montage`). Table address/count are
parameters (default `0x20335234`/708, this firmware version) in case they shift between firmware releases.

**Closed at the time**: this appeared to fully resolve the "raw/uncompressed bitmap material" open item from
2026-08-26 — format, location, and structure all seemingly known, extraction automated. (The channel-order
and stride bugs below were found afterward, so treat every image produced up to this point as suspect.)
Ghidra renames: `g_icon_table`, `icon_blit_by_id_v1`, `icon_blit_by_id_v2`, both with plate comments
documenting the struct layout.

## Icon labeling pass (2026-08-29, same day)

Went through all 708 table entries to identify and name individual icons — not cosmetic labels on the
extracted images, but real names on the *addresses* (Ghidra symbols on each `icon_header`) and in a lookup
file, so future code analysis immediately recognizes what an `icon_blit_by_id(..., N, ...)` call is drawing.

**Structural correction to the note above**: it's not a duplicate pixel-data pass — `table[i]` and
`table[i+354]` point to the **literal same** `icon_header` address for every one of 299 icons (confirmed via
exact pixel-hash grouping, not eyeballing). Hypothesis, not confirmed: one bank per rendering context (e.g.
main-VFO vs. sub-VFO icon strip). 319 unique icons total: 299 simple pairs, 18 small (1px-13px wide) tick-
mark-shaped icons shared across 4-8 table slots each (reusable meter/scale-segment building blocks), and 2
true singletons (ids `292`/`646`, both `47x43`, not yet identified).

**31 unique icons (62 table slots) confidently identified from their own rendered text**, renamed in Ghidra:
`icon_TUNE_off/active/disabled`, `icon_E_TUN_off/active`, `icon_TX_off/active/disabled/dotted`, `icon_SPLIT`,
`icon_BW`, `icon_SFT`, `icon_BPF`, `icon_ATT_OFF/10dB/20dB/30dB`, `icon_play_stop_filled/outline`,
`icon_CENTER`, `icon_FIX`, `icon_SCROLL_C`, `icon_SCROLL_F`,
`icon_levelbar_{black,gold,cyan}_{104x13,68x13}`, `icon_circled_1/2`. Full id/address/dimensions/label table
for **all 708 entries** (confident labels plus flagged best-guess categories for the rest) in
`notes/icon_table.csv`.

**Left open, asked the user rather than guessed**: several icons with text too small to read even zoomed
(ids `21`-`26`, `53`-`60`, `112`/`113`, `156`/`157`, `185`), an unlabeled `"E"` box (id `3`), a `"△TX"`
triangle+TX glyph (id `55`), diagonal-stripe/spark graphics with no text (ids `17`/`18`/`37`/`38`/`61`/`98`/
`155`/`263`/`264`/`314`/`346`), and two large unconfirmed families: ~230 narrow vertical bars in varying
colors/heights (guess: S-meter/ALC/SWR/PWR-style level-meter segments) and wide thin bars up to 480px wide
(guess: band-scope/frequency-scale rulers — 480px matches the touchscreen's documented resolution) plus
~90x90 diagonal-stripe-with-corner-bracket icons (guess: band-scope corner decoration or an out-of-band
warning overlay). Answers, once in, go into `notes/icon_table.csv` and further Ghidra renames.

Extraction tooling: `tools/extract_icons.py` (per-icon PNGs / contact-sheet montage), `tools/label_icons.py`
(a montage with icon_id printed on each cell, for visual review only — the addressable labeling lives in
Ghidra + `notes/icon_table.csv`, never baked into an image).

## Two real decode bugs found and fixed, full relabel (2026-08-29, same day)

User (correctly) didn't trust the first labeling pass's images: "processed with wrong offset and/or wrong
rgb order... blue and red seem to be swapped... the ones with garbled diagonal features are processed with
wrong width." Both calls were right, and both were fixed by going back to the actual consumer code
(`icon_blit_by_id_v1`, `0x200ae4d4`) instead of guessing further:

1. **Channel order is BGRA, not RGBA.** Confirmed by rendering `icon_TX_active` both ways — the RGBA
   interpretation shows a blue "TX", swapping R/B shows red, which is the only sane color for a transmit
   indicator. Every earlier render had every color channel-swapped. **This is the final, confirmed format**
   — see [notes/bitmaps.md](bitmaps.md) for the current-state summary.
2. **Row stride is not `width*4`.** Re-reading `icon_blit_by_id_v1`'s decompilation more carefully found the
   real formula it passes to the blitter: `((width + 3) >> 2) << 4` — width padded UP to a 4-pixel (16-byte)
   boundary before multiplying by 4 bytes/pixel. Identical to `width*4` when width is already a multiple of
   4 (why TUNE/TX/SPLIT etc., all 4-aligned widths, looked fine from the start), but silently wrong for any
   other width (59, 90, 137, ...) — the exact "diagonal shear" the user spotted, worse over taller icons.
   Fixing this turned three previously-"garbled" icons into a perfectly clean `SHARP`/`SOFT` filter-selector,
   a `SCOPE` menu icon, and a `Vd 10-16V` voltage-meter gauge.

Both fixes are now baked into `tools/extract_icons.py` and `tools/label_icons.py` (see their docstrings for
the full formula/reasoning) — every earlier render in this file's history should be treated as unreliable;
only images generated after this point are trustworthy.

**Second lesson, this session's own mistake, not the user's**: even after fixing the decode, several of the
*first* corrected labels were wrong anyway — not a decode bug, but hand-transcription error reading which
`icon_id` a given cell in a labeled contact sheet actually corresponded to (e.g. misreading the whole
`SCOPE`/`AUDIO`/`VOICE`/... menu-icon block as `211`-`222` when the real ids are `221`-`232`; misreading the
warning-triangle/error-X icons as `291`/`292` when they're really `301`/`302`). Caught by rendering **every**
planned rename individually (small crop + claimed label side by side, `id` baked into the crop from the raw
data, not copied by hand) and visually cross-checking each one before touching Ghidra — this is now the
standing method for any future icon-labeling work, not just a one-off recovery. A blanket full-sheet read
is fine for a first pass / spotting content categories; never trust its exact `id` numbers into a rename
without this per-item recheck.

**Final result, individually verified**: 150 unique icons (out of 319) confidently identified and renamed in
Ghidra (up from the first pass's incorrect 31) — the complete main menu (`icon_SCOPE_menu_icon` through
`icon_SET_menu_icon`), all meter-select labels (`S`/`Po`/`SWR`/`ALC`/`COMP`/`Id`/`Vd`, plus their `S/xxx`
combined forms), the `Vd 10-16V` and `TEMP COOL/HOT` gauges, the full `SHARP`/`SOFT` filter-selector state
set, `AUTOTUNE`, `RIT`, the out-of-band-TX warning glyph, transport controls (play/pause/stop/rewind/FF),
`R`/`T` status badges, `warning_triangle`/`error_cancel`, and more — see `notes/icon_table.csv` for the full
708-row table and `notes/icon_table.png` for a corrected contact-sheet montage of the whole set (per the
user's request, to accompany the CSV once confidence was actually earned).
