# Open question: how many processor images does the container hold?

## Headline finding: `tunk3.py` silently drops ~1.46 MB of the container

Built and ran a from-scratch decoder (`tools/icom_fw/`, see
[[container-format]]) against the real files and tracked exactly how many
bytes each step of `tunk3.py`'s logic actually consumes, instead of trusting
the hardcoded offsets. Result: **after chunk3 + chunk4 (the last things
`tunk3.py` reads), there is a large unread region running to the end of the
file that the existing script never looks at**:

| ver | file size | end of chunk4 | **unaccounted trailing region** |
|---|---|---|---|
| 1.11 | 3,946,563 | `0x25ef58` | **1,460,459 bytes** (~37% of the file) |
| 1.42 | 3,954,089 | `0x25ec92` | **1,468,695 bytes** (~37% of the file) |

This trailing region:
- has **no leading/trailing zero padding** (not simple alignment filler),
- has **high entropy (~7.25 bits/byte)**, similar to `chunk4.dat` and
  `snip` and clearly unlike real ARM code/data (`out.dat` measures ~5.8),
- does **not** start with a plausible length-prefix (first 4 bytes read as
  LE u32 are not a sane byte count) or an ARM vector-table pattern like
  `base.dat` does — so it isn't simply "one more `size-prefix + raw data`
  slot" or "one more `base.dat`-shaped sub-image header" in the same shape
  as everything else in the container.

**This is now the single strongest candidate location for an embedded
second-processor (companion-chip / DSP) firmware image** — it's large
enough, it's unaccounted for, and its high entropy is consistent with it
still being compressed (LZSS or otherwise) rather than raw. It supersedes
`chunk3.dat`/`chunk4.dat`/`snip` as the primary thing to chase next.

## Confirmed: the trailing region is a 5th, EOF-terminated LZSS stream

Fed the trailing region into the *same* LZSS decoder, letting it run until
input is exhausted instead of assuming a fixed output length (this mirrors
`tunk.py`'s own `except IndexError: return` fallback path, which now looks
less like an error case and more like the intended termination mode for
this final chunk):

| ver | leftover (compressed) bytes | decoded (decompressed) bytes | decode consumes up to |
|---|---|---|---|
| 1.11 | 1,460,459 | 1,666,533 | **exactly `0x3c3843` = end of file** |
| 1.42 | 1,468,695 | 1,679,655 | **exactly `0x3c55a9` = end of file** |

The decode lands on the *exact last byte of the container* in both cases —
essentially conclusive proof this is real, correctly-shaped LZSS data
(garbage/misaligned input would not by chance walk off the end of a
multi-megabyte file to the very last byte). So: **the container has a 5th
component, call it `chunk5`/`tail`, which is LZSS-compressed like the main
body and `chunk4`, but — unlike every other chunk — has no length prefix
anywhere; its decompressed length is implicit in "decode until input runs
out."**

Its decoded content: starts with a long run of zero bytes (looks like a
zeroed BSS/data section, not noise) then transitions to dense, still
high-entropy (~7.23–7.24 bits/byte) bytes through to the end — i.e. it
doesn't look like plain ARM code (entropy stays much higher than `out.dat`'s
~5.8 throughout), but the clean zero-run start and the exact-EOF alignment
both argue against this being random/misinterpreted data. Plausible reads:
a companion-chip/DSP firmware image, a coefficient/lookup table, or a
further-compressed/encrypted sub-image.

`size1` (`2436080`, constant across all versions, per [[firmware-versions]])
does not equal this chunk's compressed or decompressed size in either
direction — ruled out as its length field. So its size is genuinely only
knowable by decoding it (or by locating whatever code parses it in the ARM
loader).

**This `chunk5`/tail stream, not `chunk3.dat`/`chunk4.dat`/`snip`, is now
the primary target for the "second processor image" question.** Next
concrete steps: disassemble/inspect the decoded output for a
vector-table-shaped header the way [[base-loader]] found one for the main
CPU (its ISA is presumably different if it belongs to a companion chip —
try ARM first since that's the only ISA confirmed on this board so far,
but don't assume); and once MCP is up, check whether the ARM loader code in
`base.dat` references a *second* fixed decode length/target address after
the main body, which would confirm this is a deliberate, loader-known
chunk rather than an accidental byte-alignment coincidence.


The IC-7300 has more than one processor (main Renesas RZ/A1H application
CPU, plus at least one companion chip referred to as **"SX3765"** in
strings found inside the firmware). It's not yet established how/whether a
second processor's firmware is packaged inside the single `7300_1XX.dat`
container, or delivered some other way entirely. This file tracks the
evidence gathered so far — **not yet a conclusion**.

## Evidence for a companion-chip version table (frozen after v1.14)

Per [[firmware-versions]], `size2`/`size6`/`size7` and the 16-byte version
string in the container header all stopped changing after v1.14, while the
main body's decompressed `length` kept changing every release through
v1.42. That's consistent with Icom shipping updates to the main RZ/A1H
application continuously, but freezing whatever `size2`/`size6`/`size7`
track after v1.14.

## `SX3765` strings found in the decompressed main body (`out.dat`, v1.42)

Ground-truthed with `strings -t x` against the read-only v1.42 decompressed
image (`/data/misc/icom/7300/out.dat`):

```
     3f  SX3765 V4.96-000
  1d63a9  SX3765 V4.81-000
  5dd38   SX3765 V1.00-003      <- the ONE entry in "Vx.xx-yyy" (not "V4.xx-000") form
  680bc   SX3765 V4.41-000
  68774   SX3765 V3.41-000
  69020   SX3765 V4.61-000
  69708   SX3765 V4.41-000
  69fa0   SX3765 V4.71-000
  6ab90   SX3765 V4.61-000
  6b77c   SX3765 V4.81-000
  6be68   SX3765 V4.71-000
 18877f   "Partial" SX3765 V0.30-000 SX3765 V4.81-000 SX3765 V0.9H-000
 354780   SX3765 V3.41-000
 354794   SX3765 V4.41-000
 3547a8   SX3765 V4.61-000
 3547bc   SX3765 V4.71-000
 3547d0   SX3765 V4.81-000
```

Reading: most entries share the `SX3765 V4.xx-000` (or `V0.xx`) shape and
cluster in twos/threes at several offsets, with `"Partial"` sitting right
next to one cluster (`0x18877f`) — this reads like a **compatibility/version
table** used by a firmware-updater UI ("Partial" update messaging, a list
of acceptable/known companion-chip firmware versions to check against),
*not* one embedded firmware image per occurrence.

**The one outlier is `SX3765 V1.00-003` at file offset `0x5dd38`** — a
different version-string format from all the others, appearing only once.
`tunk.py`'s inline notes contain the fragment `"187f000 5dd38 - image
alkais 18212c8"` (Finnish: "image starts at...") which literally names this
same offset (`5dd38`) — so the user had already flagged this exact spot as
a candidate embedded-image start, before these notes existed. **This is the
strongest lead for an actual embedded companion-chip firmware image**, as
opposed to just a string in a compatibility table. Not yet disassembled or
otherwise confirmed — the surrounding bytes at `0x5dd38` haven't been
checked for a header/vector-table shape yet.

Caveat: `tunk.py`'s other two offset notes (`0x4590`, `0x4f2c`, both
captioned "SX3765 V1.00-003") do **not** match what's actually at those
offsets in `out.dat` v1.42 today (plain ARM code, no such string nearby) —
either that note was written against a different firmware version's
decompressed image, or against a different extraction (`tunk.py` writes to
`out2.dat`, a separate/scratch output, and takes a CLI offset argument, so
it was likely used interactively across several trial offsets during
exploration). Don't trust those two offsets without re-deriving them.

## `chunk4.dat` and `snip`: high entropy, not yet explained

Shannon entropy, measured directly:
- `out.dat` (real decompressed ARM code+data): **~5.8 bits/byte** — normal
  for native code/data.
- `chunk4.dat` (already LZSS-*decompressed* per [[container-format]]):
  **~7.2 bits/byte** — unusually high for something that's supposedly
  already decompressed. Candidates: a second (nested) compression or
  encryption layer, or inherently dense binary content (e.g. a lookup
  table, audio/DSP coefficient data, or the actual companion-chip firmware
  image itself, encrypted/signed the way many Icom companion-chip updates
  are).
- `snip` (1.5 MB, no file-type magic, no notes explaining its origin):
  **~7.3 bits/byte**, and **does not appear as a substring anywhere in
  `out.dat` or `chunk4.dat`** (checked both a header and a mid-file 64-byte
  needle). So it isn't simply a carve-out of the current container's
  content — plausible origins: a raw flash dump taken directly from
  hardware (the service manual notes two external flash ICs, IC391 64 MB
  and IC902 32 MB, see [[memory-map]]), from a different firmware version
  than the ones on hand, or something unrelated entirely. **Needs to be
  asked about directly** rather than reverse-derived — the user may simply
  remember what this was.

## Next steps
1. Disassemble the ~32 bytes around `out.dat` offset `0x5dd38` (both as
   ARM and, if that doesn't parse as code, treat it as the start of a
   distinct sub-image and look for a header/vector-table shape specific to
   the companion chip's own architecture, once identified).
2. Try decompressing `chunk4.dat`'s content again (LZSS or otherwise) to
   see if the high entropy resolves into something more code/data-shaped.
3. Ask the user directly what `snip` is, rather than continuing to guess.
