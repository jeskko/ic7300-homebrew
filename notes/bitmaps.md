# Raw/uncompressed bitmap material (user-flagged, not yet independently pinned down)

User noted (2026-08-26) that there is clearly uncompressed material past
the compressed image — e.g. some bitmap images — that hasn't been
accounted for yet. Not yet clear whether this means:
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

## Next step
Ask the user directly where they saw this, then extract and try rendering
it (a quick raw-pixel viewer trying a few likely IC-7300 display
geometries/bit depths — the touchscreen is commonly quoted as 480×272 —
against candidate offsets) rather than continuing to guess from entropy
alone.
