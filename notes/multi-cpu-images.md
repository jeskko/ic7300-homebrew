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

## Confirmed: dual A/B flash slots, `body.bin` itself uses both — not a companion-chip image

Traced this further in the fresh Ghidra project (`body.bin` rebased to
`0x20005000`, see [[base-loader]]). `body.bin` contains `FUN_20062c64`,
which runs the **exact same check** as `base.dat`
(`strncmp(0x187f0000, "SX3765 V1.00-003", 0x10)`, same literal string
embedded again at `0x20062d38`), then picks between two pairs of flash
pointers based on the result:

| | Slot A (`base.dat`'s `FLASH_1801`) | Slot B (`base.dat`'s `FLASH_1840`) |
|---|---|---|
| slot base | `0x18010000` | `0x18400000` |
| pointer 1 | `0x18210000` (base `+0x200000`) | `0x18600000` (base `+0x200000`) |
| pointer 2 | `0x18240000` (base `+0x230000`) | `0x18630000` (base `+0x230000`) |

Both slots use **identical relative offsets** (`+0x200000`, `+0x230000`),
and those land almost exactly on chunk1/chunk2 (the two `.ttf` fonts) per
[[container-format]]'s offsets (`0x21002c`/`0x24002c` from container
start, allowing for `base.dat`'s ~44-byte header offset and flash-sector
rounding) — i.e. `body.bin` uses these as direct XIP pointers to read font
glyphs straight out of whichever slot is currently active.

**Conclusion: the 64 MB SPI flash holds two complete, parallel copies of
the entire container** (main body + both fonts + chunk3/4/5), and
`"SX3765 Vx.xx-xxx"` at `0x187f0000` is a **generation/active-slot
marker** — checked identically by the boot loader (to pick which slot to
decompress+run) and by the running firmware itself (to pick which slot's
resources to read live). This is now strong evidence the marker string is
repurposed/generic, not a sign of an embedded companion-chip image —
supersedes the "probably" in the heading below, kept for history.

This gives the update mechanism's likely shape: write a new container
into the *inactive* slot, flip the `0x187f0000` marker, reboot. Still
unconfirmed: the actual SPI erase/program routine — `FUN_20062c64` has no
static callers found (same computed-call-table pattern as
`FUN_2017ccf8`/the kernel init-array walker in [[base-loader]], invisible
to xref scanning), so the entry point into the writer code is still
unlocated.

## Older working note (superseded by the section above, kept for history)

Read (read-only) the prior `icom_loader.rep` Ghidra project's `base.dat`
analysis — see [[base-loader]] for the full boot sequence. Its
`unpack_from_flash_to_mem` does this right before decompressing the main
body:

```
if flash[0x187f0000 : +0x10] == "SX3765 V1.00-003":
    source = 0x18400000
else:
    source = 0x18010000
decompress(source) -> RAM @ 0x20005000        # always this destination
```

`0x18400000 − 0x18010000 = 0x3f0000` ≈ **3.94 MB — almost exactly one
container's size.** Best current read: the 64 MB SPI flash holds **two
full copies of the update container back-to-back** (redundant/failsafe A/B
slots), and the `"SX3765 Vx.xx-xxx"` string at `0x187f0000` (which sits
between the two slots) is a boot-time marker used to pick the newer/valid
one — **not** evidence of a second processor's firmware embedded in the
container. This doesn't rule out a companion-chip image existing
somewhere else, but it does mean the `FLASH_187F`/`0x18400000` numbers
aren't that lead.

**Caveat — do not conflate with the old `tunk.py` note:** the note's
`"187f000"` (7 hex digits, `0x0187f000`) is a *different, ~16× smaller*
number than the loader's real `0x187f0000` (8 hex digits) — they share
leading digits by coincidence, not by being the same address. Checked
whether the note's own two numbers are internally consistent instead: they
are — `0x187f000 − 0x18212c8 = 0x5dd38` exactly, describing a
384,312-byte region from `0x18212c8` to `0x187f000`. That's still ~25 MB,
too big for `body.bin`/any container file, so it plausibly refers to a
*different* data source entirely (e.g. a raw SPI flash dump from one of
the other prior Ghidra projects) rather than to anything in our current
`scratch/unpacked/`. Unresolved — don't spend more time on this specific
number pair without finding what file it actually indexes into.

**Update — the note's other offset, `0x4f2c`, IS resolved:** see
[[firmware-update]]. It's the byte offset *inside the update file*
(not the decompressed body) where the updater reads the 16-byte value it
later flashes as the new `0x187f0000` marker. `0x18212c8`/`0x187f000`
remain the only unexplained pair.

## Checked: `vanah.rep` (prior project, `unpacked.dat` only) — no manual analysis to harvest

Peeked at the prior `vanah` Ghidra project (read-only,
`/data/misc/icom/7300/vanah.rep`), hoping for prior manual RE work on the
main body. It doesn't have any:

- Confirms the `0x20005000` base independently (loaded there back in Apr
  2024) — consistent with the [[base-loader]] finding above.
- But zero renamed functions, zero real comments, bookmarks are 100%
  auto-analysis noise (`Bad Instruction`, `Found Code`) — never manually
  worked, just imported and auto-analyzed then abandoned. Not a source of
  findings, only of the base-address confirmation.
- Its memory block is oddly oversized (`0x20005000`–`0x20645d33`, 6.55 MB,
  vs. the real ~3.74 MB decompressed body). Checked the extra ~2.7 MB:
  overwhelmingly `0xff` (erased-flash pattern) with only sparse scattered
  non-FF bytes, no coherent second image. Most likely the analyst just
  expanded the block and never filled it — **inconclusive, not evidence
  for or against a second embedded image.**

## DSP/FPGA/front-panel firmware investigation (this session, follow-up to [[hardware-debug-access]])

Confirmed real, separate versioning for more components than expected.
The radio's "Version Information" screen orchestrator (`FUN_20039714`)
populates **7 distinct version rows**, including confirmed labels
`"DSP(P)"`, `"DSP(D)"`, `"FPGA"` (found via string search at
`~0x20037800`) alongside a Main entry — each row pulled from a shared
live-status struct (`DAT_20037820`, itself never found being written —
same indirect-access limitation as everywhere else this session).

Found what looks like a **component descriptor table** near chunk3's
known flash address (`0x18250000`) at `~0x20328c80`: repeating
`{address, size, flags, ...}`-shaped records, several referencing
addresses in the `0x1825xxxx` range with size-like fields around
`0x1ffff`/`0x1f800` (~128KB) — too big to be chunk3 alone (previously
assumed small/config-shaped) and too small to be the DSP's full
firmware. **No static reference to the code that reads this table was
found** — same recurring limitation as the rest of this investigation.
Field semantics not confidently resolved; don't trust the byte-offset
interpretation above without confirming against actual reader code.

**Not yet answered**: where DSP (TMS320C6745) and FPGA (Cyclone IV)
firmware actually live in the container, and what code loads/updates
them. `chunk3.dat`/`chunk4.dat`/`chunk5`-tail remain the candidates, but
none confirmed. This is exactly the kind of question live memory access
(see [[hardware-debug-access]]) would resolve quickly — watching what
touches this table, or the chunk3/4/5 addresses, at runtime — versus
continued static guessing.

## Strong new lead: a genuinely separate write mechanism exists for the "3 extra chunks" (user's DSP/FPGA-link hypothesis)

User found via schematic: DSP's own flash (IC902, `EN25QH32A`) has its
DO/DI/CLK/CS lines present at connector `J901`, and some of the FPGA's
(`EP4CE55F231I7N`) pins (`DCLK`/`DATA0` — the standard Altera passive-serial
config inputs) connect via the `MAIN-6` interconnect to signals named
`SPDO`/`SPCK`. Hypothesis: the main CPU updates the DSP's flash over a
separate link bus during firmware update, and the FPGA's config bitstream
is fed by the DSP rather than directly by the main CPU. (Also noted: a
`23LC1024T` SPI SRAM sits near the FPGA too — almost certainly just a
volatile frame-buffer/scratch memory for display generation, not
firmware storage; doesn't bear on this hypothesis either way.)

Checked in `body.bin`: the "3 extra chunks" loop in `firmware_update_main`
(per-chunk-index dispatch via `DAT_200264b4[]`, see [[firmware-update]])
calls `FUN_20025044`, NOT `flash_write_chunked_from_file`. This is a
**genuinely different write mechanism**:
- 256-byte pages, write-then-verify, retry up to 5× per page (very
  different shape from `spi_flash_program`'s simple page-program loop)
- Its helper (`FUN_200b3040` → `FUN_200b10a0`) computes an address as
  `(param_1 & 0xffffff) + 0xe2000000` — **checked against the RZ/A1H
  hardware manual (full-text search): `0xe2000000` does not appear
  anywhere in it, including the DMAC chapter.** So this is likely a
  software-internal identifier/tag (possibly further transformed by
  `FUN_200b0f68`, the queue consumer, not yet traced — not a literal
  hardware peripheral base address like `SPI_BASE`/`PORT_BASE` are.
  Don't over-read the specific address value; the mechanism shape
  (separate from the main flash path, queued/async) is the solid part
  of this finding, not this particular constant.
- Submits that address into an **IRQ-disabled circular queue** (87
  entries) and kicks off async processing on the first submission —
  looks like a DMA/descriptor-queue-driven transfer, not simple bit-banged
  GPIO. Polling loops around it (`FUN_200b521c`, `FUN_200b3030`) are
  almost certainly completion checks for that async job.

**This is strong, concrete support for the "separate link bus" part of
the hypothesis** — real evidence of a genuinely distinct
peripheral/mechanism used specifically for these chunks, separate from
the main flash path. Not yet confirmed: which physical bus/chip this
actually targets (needs the RZ/A1H hardware manual for what's at
`0xe2000000`, or live JTAG access once available — see
[[hardware-debug-access]]) and whether it's the DSP flash specifically
vs. something else.

## Next steps
1. Disassemble the ~32 bytes around `out.dat` offset `0x5dd38` (both as
   ARM and, if that doesn't parse as code, treat it as the start of a
   distinct sub-image and look for a header/vector-table shape specific to
   the companion chip's own architecture, once identified) — lower
   priority now that the dual-slot theory above better explains the
   `0x187fxxxx`/`0x1840xxxx`/`0x1801xxxx` numbers.
2. Try decompressing `chunk4.dat`'s content again (LZSS or otherwise) to
   see if the high entropy resolves into something more code/data-shaped.
3. Ask the user directly what `snip` is, rather than continuing to guess.
0. **New, likely resolution of the "SX3765" identity question**: see
   [[hardware-debug-access]] — the IC-7300 schematic shows a front-panel
   controller IC with a partially-legible label that looks like
   `(UX-3765C)`, strongly suggesting "SX3765" is just that chip's part
   number, not a companion firmware image. Also newly found there: the
   real IF-DSP is a separate, different chip entirely (TI TMS320C6745),
   raising the question of where *its* firmware lives — worth checking
   before assuming the container-format investigation below covers
   everything relevant to "how many processor images exist."
4. ~~Check the `icom.gpr` project's `tunkki` program~~ — checked: it's just
   an early nickname for v1.42's `unpacked.dat` (`executablePath` confirms
   `7300_142/unpacked.dat`), based at `0x20005000`, same no-manual-analysis
   state as `vanah`. Not the flash dump. `icom.gpr`'s other two programs
   are earlier/superseded too: `out.dat` (Apr 4, the same content as
   `tunkki` but at the wrong base `0x0` — chronologically the first,
   pre-`0x20005000`-discovery import) and `7300_142.dat` (the raw
   container, imported flat at base `0`, 0 functions — never disassembled,
   reference-only). **None of the four prior Ghidra projects
   (`icom`, `icom_loader`, `vanah`, `oisko`) contain a raw full-flash dump
   or a companion-chip analysis** — the `0x18212c8`/`0x187f000` note pair's
   target file remains unidentified. Worth just asking the user directly
   next time, same as `snip`.
