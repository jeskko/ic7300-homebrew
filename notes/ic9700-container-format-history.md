# IC-9700 firmware container format — full session history

Full narrative and evidence trail behind [notes/ic9700-container-format.md](ic9700-container-format.md),
which carries only the current-state summary and open questions. Kept verbatim for the exact reasoning,
offsets, and negative results behind each conclusion — this is a genuine cold-start effort with no prior
art, so the specific things already ruled out matter as much as what's confirmed.

## Initial recon (unresolved at the time, still unresolved)

**Status: cold start, not cracked.** Unlike the IC-7300 side of this repo,
there was no prior art to lean on here — no existing unpacker script, no
pre-decompressed reference image, nothing in `/data/misc/icom/9700/`
except raw `.dat`/`.zip` release files (confirmed by search). This note
exists so a future session doesn't repeat the same dead ends.

Files examined: `9700J150.dat` (Japan, 15,613,952 B) and `9700E150.dat`
(Europe, 15,629,913 B), the two SD-card update images for the IC-9700 —
a separate product line from the IC-7300. **Main CPU confirmed as
Renesas RZ/A1 series (R7S721001VCBG), same Cortex-A9 family as the
IC-7300's R7S721000** — see [[ic9700-hardware]] for the full parts list
and why this container almost certainly packs firmware for multiple
chips (main CPU + FPGA + separate MCU + DSP), not just one image.

### What's confirmed

- **Header struct differs from IC-7300's**, not just shifted — at the
  offset where IC-7300 has numeric `size1..size7` fields (`0x10` +),
  IC-9700 embeds a literal ASCII substring `"3.623.27"` (a build-tool
  version, presumably) at `0x14`. IC-7300's `0x1002c` body-length field
  reads as garbage here (4.2 billion) — confirms the whole struct shape
  moved, this isn't a simple offset translation job.
- **J and E variants are byte-identical from `0x1000` to `0x10000`** —
  same "common header/boot region shared across regional variants"
  pattern as IC-7300's `base.dat`, confirmed via block-level diff.
- Unlike IC-7300's `base.dat` (mostly real ARM bootloader code across
  its 66000 B), **this shared `0x1000`–`0x10000` region is almost
  entirely flash-erase-style filler** — a smooth decrementing byte-value
  ramp (`2b 2a 29 28 ... 00 ff fe fd ...`, wrapping mod 256), not real
  content. Verified precisely by walking 256-byte blocks and checking
  for continuation of the ramp from the previous block's last byte.
  Only two small islands of genuine high-entropy content break the
  ramp:
  - `0x4038`–`0x48c8` (~2193 bytes) — high entropy, no readable strings,
    no recognizable ARM/Thumb instruction patterns found by inspection.
    Isolated placement in an otherwise near-empty region + high entropy
    is more consistent with an embedded **signature or certificate**
    than boot code — IC-9700 (2019) postdates the IC-7300 and may have
    added real update-signing IC-7300 never had (see
    [[firmware-update]] for the 7300's confirmed no-signature, MD5-only
    mechanism — worth checking whether 9700 differs here, since a
    header-adjacent isolated crypto-sized blob is exactly what real
    signing would look like). **Not confirmed, just the best-fitting
    hypothesis at the time** — see the cross-version comparison below,
    which ruled this out.
  - `0x4f38`–`0x4f3f` (8 bytes) — smaller high-entropy fragment, purpose
    unknown.
  - Both islands are within the J/E-shared region, so whatever they are,
    they're common to both regional variants.
- **Real "body" content starts at `0x10038`** (right after the ramp
  filler ends) — high entropy, presumably compressed and/or encrypted.
- Also present in the shared header: a value `0x800000` (8388608)
  appears as a literal field in both J and E headers, **and** a
  block-level J/E diff shows the two variants come back byte-identical
  again at exactly that same offset (`0x800000`) after a large
  region-specific stretch (`0x10000`–`0x2a6000` differs, `0x2a6000`–
  `0x800000` is identical again) — strong evidence `0x800000` is a real
  fixed-slot boundary constant, same design idiom as IC-7300's hardcoded
  chunk offsets (see [[container-format]]).

### What's NOT resolved — the actual blocker

Tried decompressing at `0x10038` (and every 4-byte-aligned offset from
`0x10000`–`0x10040`) with IC-7300's exact Okumura LZSS parameters
([[decompression-lzss]]) — degenerate, mostly-zero output every time,
not plausible. Also tried structural variants at `0x10038`: inverted
literal/match control-bit polarity, swapped match offset/length byte
order, alternate window sizes and initial ring-buffer cursor positions.
None produced plausible output. So either:
- the compression algorithm itself differs from IC-7300's (used a
  different vendor library, or Icom changed it for this product line),
  or
- the real body doesn't actually start at `0x10038` and the true offset/
  length field lives somewhere not yet identified in the new header
  shape.

**No way to resolve this from the `.dat` file alone.** The code that
would actually decompress this container lives inside the *device's own
already-installed firmware* (exactly parallel to how IC-7300's update
logic lived in `body.bin`, not in the update `.dat` itself — see
[[firmware-update]]) — which is precisely the thing we can't reach
without cracking the compression first. There's no shortcut through
"load the `.dat` into Ghidra and find the decompressor" — the
decompressor isn't in this file. Confirmed no existing IC-9700 firmware
dump, JTAG access, or any other route to the installed firmware exists
yet.

## Cross-version comparison (2026-08-27, all 37 known releases)

Checked all available `.dat` files: J102, J103, J105, J106, J110, J111,
J113, J120, J121, J124, J130–J132, J140–J144, J150 (19 files) and E105,
E106, E110, E111, E113, E120, E121, E123, E124, E130–E132, E140–E144,
E150 (18 files) — every version in `/data/misc/icom/9700/`.

- **Container layout is byte-identical in shape across all 37 releases**
  spanning 2019–2025: same ramp-filler region, same island positions
  (`0x0`–`0x200`, `0x4038`–`0x48c8`, `0x4f38`-ish, real body starting at
  exactly `0x10038` in every single file), same `0x800000` slot-boundary
  constant. This is now a confirmed structural fact, not a
  single-version guess — worth trusting for any future unpacker attempt.
- **The `0x4038`–`0x48c8` blob (2193 bytes) is byte-for-byte identical
  (same SHA-256) in all 37 files**, both regions, across 6 years of
  releases. This rules out the "per-release signature" hypothesis from
  the initial recon — a real signature over changing firmware content
  would necessarily differ release to release. More consistent with a
  fixed embedded key, certificate, or lookup table used the same way
  every time.
- The `"N.NNN.NN"`-shaped build-tool version string at `0x14` (e.g.
  `3.623.27`) changes release to release and tracks consistently between
  matching J/E release pairs (e.g. both J140–J144 and E140–E144 show
  `3.623.27`) — genuine toolchain version tracking, not per-region.

**Tried and inconclusive**: seeded the LZSS ring buffer (at `0x10038`)
with the constant blob instead of Ghidra's/IC-7300's zero-init, on the
hypothesis that it might be a decompression seed rather than just inert
data. Output looked less degenerate by crude printable-ratio/repetition
metrics, but inspecting it directly showed why: it's mostly the seed
blob's own bytes being echoed back through match-copies, not genuinely
decoded content — an artifact of the test method (any structured
non-zero seed will score better on those metrics without actually being
correct), not real evidence for the seed hypothesis. Don't treat this as
a lead without a better validation method (e.g. checking for a
plausible ARM vector table shape, not just entropy/printable-ratio).

## Trailer / footer structure (2026-08-27)

Every one of the 37 files ends with a small structured plaintext footer,
found by inspecting raw tail bytes (not by trusting a printable-ASCII
regex — that gave false positives from coincidental byte values earlier
in this same investigation, worth remembering as a recurring trap with
this dataset). Exact layout, working backward from EOF:

```
... <6 bytes, "build family" field, see below>
    <4 ASCII bytes, "N.NN" version-ish field, e.g. "5.43">
    "-" <region letter, 'J' or 'E'> "001.00-0009"
    <1 non-printable byte (0x13)>                    -- EOF
```

- The `"N.NN"` field (e.g. `3.12` → `5.43` across J102 → J150) increases
  roughly monotonically with release order but isn't pure decimal — one
  value is `"3.8A"`, using a hex letter. Some kind of build/spec counter,
  not decoded further. **Confirmed identical between matching J/E pairs**
  at the same release (e.g. J150 and E150 both show `5.43`).
- The **6 bytes immediately before that field are identical across
  groups of releases that share the same underlying build** — e.g.
  E106/E110/E111/J106/J110/J111 (six files, both regions, three point
  releases) all share the exact same 6 bytes, while E113/J113 (a
  different build) share a different constant. **Initially misread this
  as tracking absolute file size** (sizes happened to cluster the same
  way for several groups) — checked properly and that's wrong: J and E
  files in the same group have *different* absolute sizes, so it's not
  a size-derived value. More consistent with a genuine build-ID stamp
  written once per internal Icom build and carried through unchanged
  across cosmetic re-releases/regional repackaging, similar in spirit to
  a git commit hash baked into every artifact of one build.
- **Not a whole-file checksum**: confirmed full-file SHA-256 differs
  between files sharing the same 6-byte value (their content genuinely
  differs — the version field alone proves that), and tried CRC32 of
  the file size (LE and BE encodings) against the field with no match.
  Whatever function produces these 6 bytes, it isn't simply "checksum of
  the whole file" or "derived from the size" — not resolved further.
- No standard compression magic bytes anywhere in the file (checked
  gzip/bzip2/xz/zstd/lz4/zip/zlib headers — zero genuine hits; the
  handful of 2-byte "zlib header" coincidences found are just noise in
  high-entropy data, not real headers).

## Compression algorithm: broad LZSS-family sweep, all negative (2026-08-27)

Systematically tried decompressing at `0x10038` across every combination
of: window size (`0x800`/`0x1000`/`0x2000`), minimum match length
(2/3/4), initial ring-buffer cursor (start/end-relative/last-byte),
literal-vs-match control-bit polarity, match offset/length byte order,
and control-bit read order (LSB-first vs MSB-first) — **216 total
combinations, using entropy + byte-repetition as a materially better
plausibility check than the crude printable-ASCII-ratio scorer used in
the initial recon session**. Genuine decoded ARM code should land around
4.5–7.0 bits/byte entropy with low single-byte repetition; **zero of the
216 combinations landed in that range.** This is a real negative result,
not just "didn't get lucky" — strongly suggests either:
- a fundamentally different compression family (not a sliding-window
  LZ77/LZSS token scheme at all), or
- a whitening/XOR pass over the compressed byte stream that must be
  undone *before* any LZSS-style decode would produce sane output —
  which would also give the constant `0x4038`–`0x48c8` blob (see above
  in this file) a concrete functional role as an XOR key/keystream
  rather than inert data or a ring-buffer seed.

  **Tested and also negative**: XOR-whitened the compressed stream with
  the blob as a repeating keystream, across all 2193 possible byte
  rotations of the blob, then ran standard LZSS on each — zero produced
  plausible output (same entropy/repetition check as above). Combined
  with the buffer-seed test also being inconclusive/unproductive, the
  blob-as-decompression-key idea has no supporting evidence at this
  point despite being a reasonable guess from its position and
  constancy. Don't re-try XOR-with-blob variants without a new reason to
  suspect them.

## If picked up again

Realistic paths forward, roughly in order of promise:
1. Check whether the compression scheme is shared with *other* Icom
   radios that might already have public dumps/tools (this container
   format idiom — version string + size table + fixed slots — might not
   be IC-7300-specific; worth a web search for other Icom firmware RE
   projects before assuming this needs solving from scratch again).
2. More systematic brute-force over LZSS-family parameter space
   (different min-match-length, different control-byte read order,
   different match encoding bit widths) rather than the handful of
   variants tried above.
3. If the `0x4038`–`0x48c8` blob really is a signature, its size/format
   might be identifiable (RSA modulus size, ECDSA, etc.) and could hint
   at the vendor toolchain/era, which might in turn narrow down what
   compression library they'd have paired it with. (Weakened by the
   cross-version finding that it's byte-identical across 37 releases —
   a real per-build signature would vary — but the format-identification
   idea itself still stands regardless of what the blob turns out to be.)
4. Actual IC-9700 hardware access (JTAG, if a unit is available) to dump
   installed firmware directly, bypassing the update-container problem
   entirely — same approach as the IC-7300 JTAG plan in
   [[hardware-debug-access]], but for a different radio.

This was a "quick tangent" that turned out to need a genuine cold-start
RE effort, not a quick adaptation of the existing IC-7300 tooling — flag
that honestly if resuming, rather than treating it as a small follow-up.

## Another go at the compression, 2026-08-30: zlib ruled out, but a real structural breakthrough instead

User's ask, after an intermission probing the live IC-9700 over the network: pick this thread back up,
we have both EN and JP versions of many releases to work with (all 37 already on disk, unchanged from the
earlier cross-version sweep).

### zlib/raw-DEFLATE: a well-motivated new hypothesis, thoroughly checked, negative

New reason to suspect zlib specifically that didn't exist during the original 216-combination LZSS-family
sweep: a completely separate investigation this same day found **real, genuine zlib deflate/inflate code
embedded in the IC-7300's own firmware** (see [[kernel-rtos-history]]'s SLV5/graphics-stack thread — a
higher-level wrapper's error strings like "unexpected zlib return code" plus zlib's own verbatim internal
algorithm error strings, both found in `body.bin`). Since Icom's toolchain evidently uses zlib somewhere in
this era, and the IC-9700 postdates the IC-7300, it's a reasonable guess IC-9700's update container might
use it too — especially since "no standard compression magic bytes" was already established (a **raw**
DEFLATE stream, without the 2-byte zlib wrapper header, has no magic bytes at all, so that earlier check
wouldn't have ruled this out).

Checked properly with Python's `zlib` module against `9700J150.dat`:
- A clean 64KB-wide offset sweep (`0x10000`-`0x20000`, every byte offset) for plain raw-DEFLATE, requiring
  >4KB of clean decoded output: **zero candidates**.
- XOR-whitening the compressed stream against the constant blob (`0x4038`-`0x48c8`) across all 2192 byte
  rotations, at the confirmed real body-start offset (`0x10038`): **zero candidates** producing more than a
  trivial/degenerate decode. (One single-rotation hit at a different offset turned out to be a classic false
  positive — `eof=True` after only 64 bytes of a highly repetitive 5-byte pattern, exactly the kind of
  degenerate result this file's own earlier notes already warned about.)

**Real, thorough negative.** Not worth retrying without new evidence — recorded so a future session doesn't
redo this exact test.

### The actual breakthrough: real header fields never noticed before, and many more component boundaries than known

While double-checking the zlib test's target offset by eye, did a plain hex dump of the file header
(`0x0`-`0x40`) that the original cold-start session never happened to render this way. Found two 4-byte
fields at `0x30`/`0x34` sitting in what had previously been assumed to be inert space between the
already-known `0x14` version string and the `0x1c` `0x800000` constant — nobody had looked at `0x30`-`0x3f`
as structured numeric fields before.

**Checked across all 37 releases, not just eyeballed on one file**:
```
9700E106.dat  @0x30=7217626 (0x6e21da)  @0x34=9267776 (0x8d6a40)  @0x38=0x4a60d6b2  @0x3c=0x569cdaee
9700E110.dat  @0x30=7217626 (0x6e21da)  @0x34=9267776 (0x8d6a40)  @0x38=0x4a60d6b7  @0x3c=0x569cdaee
9700E111.dat  @0x30=7217626 (0x6e21da)  @0x34=9267776 (0x8d6a40)  @0x38=0x4a60d6b1  @0x3c=0x569cdaee
9700J106.dat  @0x30=7217626 (0x6e21da)  @0x34=9267776 (0x8d6a40)  @0x38=0x4a60d6b7  @0x3c=0x569cdaee
9700J110.dat  @0x30=7217626 (0x6e21da)  @0x34=9267776 (0x8d6a40)  @0x38=0x4a60d6ac  @0x3c=0x569cdaee
9700J111.dat  @0x30=7217626 (0x6e21da)  @0x34=9267776 (0x8d6a40)  @0x38=0x4a60d6b6  @0x3c=0x569cdaee
```
This exact six-file group is **the same build-family grouping already established from the footer's 6-byte
build-ID stamp** in the original cold-start session — `@0x30`/`@0x34` track it perfectly, varying together
release-to-release (roughly with overall file size) while `@0x38` varies even within an identical-build
group (checksum-shaped: constant high 3 bytes `0x4a60d6`, only the low byte differs, narrow range) and
`@0x3c` never varies at all across any of the 37 files (`0x569cdaee` everywhere — same "fixed constant"
character as the already-known blob).

**Tried using `@0x30`/`@0x34` as real candidate {compressed, decompressed} sizes for the IC-7300's own LZSS
decoder** (`tools/icom_fw/lzss.py`, which supports a target `out_len` and reports bytes actually consumed):
neither assignment (`out_len=@0x34`/expect consumed≈`@0x30`, or the reverse) produced a real decode — output
starts with a long run of zero bytes (a classic degenerate-ring-buffer-echo signature, not real content) and
consumed byte counts don't match either candidate. A cleaner, more targeted negative than the original
216-combination blind sweep, but still negative — these fields probably describe something real, just not
"feed directly into IC-7300's own LZSS with these as sizes."

**The bigger find**: swept the *entire* file (not just the header) for the same flash-erase-style
decrementing-byte-value ramp pattern already known from the header, using a proper run-detection scan rather
than assuming it only exists near the start. Found large (thousands-of-bytes) ramp regions — real component
boundaries, not noise — ending at:
- `0x10038` (already known — this is where "the body" was always assumed to start)
- `0x3f0038` (**new**)
- `0x400038` (**new** — only `0x4000`/16KB past the previous one)
- `0x7f0038` (**new**)
- `0x7fc038` (**new**)
- `0x800028` (already known as the J/E-convergence boundary, `0x800000`, but its own short ramp/header shape
  hadn't been individually characterized before — see below)

Every one of these breaks its ramp with the identical tail bytes `fb fa f9 f8 f7 f6 f5 f4` — matching the
main header's own ramp-end exactly — then a short region of structured-looking bytes before real
high-entropy content resumes, the same shape as the already-documented `0x800000` boundary. **This
completely changes the picture from "one big compressed blob from `0x10038` to somewhere past `0x2a6000`,
converging with the other region again at the already-known `0x800000` slot boundary" to "a container
segmented into at least 5-6 pieces"**, most plausibly one per physically-separate chip needing its own
firmware/bitstream image — directly analogous to the IC-7300's own already-solved multi-component update
container ([[multi-cpu-images]]) and consistent with [[ic9700-hardware]]'s chip list (main CPU, Cyclone V
FPGA, STM32 MCU, TMS320C55x DSP — four independently-programmable components, a very natural fit for
~5 boundaries marking ~4-5 segments).

**Strongest single piece of evidence this session**: dumped the bytes immediately following each ramp's end
and found `0x400038` and `0x7f0038` share an **identical** 13-byte sequence
(`11 13 14 14 15 04 28 19 19 1e 1f 3c`) despite being over 4MB apart in the file. This is not coincidental
noise in two independent high-entropy regions — it's direct evidence of a **fixed, repeating per-component
header/descriptor template** reused at multiple points in the file. `0x3f0038`'s and `0x7fc038`'s own
post-ramp bytes look different from this pair and from each other — not yet characterized, a natural next
step (do they also match some other template, or is each genuinely distinct?).

**Where this leaves the thread**: compression is still not cracked, but the container's true shape is now
understood to be meaningfully more complex than previously documented, with a concrete, well-evidenced lead
(the repeating header template) that a future session could decode without needing to solve the compression
first — knowing the real component boundaries and having a byte-identical template to compare against other
occurrences is a solid, self-contained next step. See [[ic9700-container-format]] for the condensed current
state.

## Correction, same day: the "5-6 component boundaries" framing was wrong — here's what's actually there

Re-checked the ramp-boundary finding above against 4 different releases spanning the full 2019-2025 range
(`J102`, `J124`, `J150`, `E150`) before building further on it, the same discipline that caught the RIIC
xref-tool mislabeling in an earlier session. **Every single ramp offset, run-length, and post-ramp byte
sequence found above is byte-for-byte identical across all four files, including the oldest vs. newest.**
That's a direct contradiction of "these mark where per-release firmware for a different chip lives" — fixed,
non-version-specific content can't be a per-release component boundary. The `0x400038`/`0x7f0038` 12-byte
template match is still real (confirmed, not coincidental), but it's a fixed template at fixed locations,
not evidence of per-chip segmentation.

**Found what actually varies, by going back to first principles**: direct byte-level diff of `9700J102.dat`
(oldest, 2019) against `9700J150.dat` (newest, 2025) in 4KB blocks from `0x10038` onward, collapsed into
contiguous differing ranges:
```
0x10038 - 0x2a6038   (2,711,552 bytes)   -- matches the already-known J/E-divergence boundary almost exactly
0x800038 - 0x801038  (4,096 bytes)        -- component 2's own small per-release header/version field
0x802038 - 0xee3038  (7,213,056 bytes)    -- a second large varying region, never noticed before
```
Everything else — including all of the "5-6 boundary" ramp regions from the earlier (wrong) framing, and
the large stretch `0x2a6038`-`0x800038` — is completely fixed across every release tested. So is the region
right after `0xee3038` up to close to EOF (the small remaining difference there is already-understood
footer content — build-ID stamp and version string, not a new mystery).

**This directly explains `0x30`'s value** (≈7.2-7.3M, varying release to release, tracking the known
build-family grouping): it's within ~0.2% of the measured size of the *second* varying region
(7,213,056 bytes measured at 4KB granularity vs. `0x30`'s ~7,225,222 for this same release) — a much better
fit than my own first guess that it described component 1. Checked `0x34` and component-1's real size
(2,711,552 = `0x296000`) against the header too — no match, so `0x34`'s exact role is still open.

**Tried compression against this newly-found second component too** (confirmed real start `0x800038`, not
guessed): both IC-7300's exact LZSS parameters and raw-DEFLATE, same as component 1 — both negative.
Checked entropy of both components at a large-enough sample (256KB) to be meaningful: both converge to
~7.999 bits/byte, indistinguishable from each other — the entropy angle doesn't help tell them apart or
support any particular hypothesis about which is more "compressed" vs. "encrypted."

**Updated working picture**: this container has exactly two real, per-release-varying regions — a ~2.7MB
one (`0x10038`, very likely main CPU firmware, matches the existing J/E-divergence fact) and a ~7.2MB one
(`0x800038`, newly found) — separated and followed by large fixed, non-varying stretches whose purpose is
still unclear (reserved space? a genuinely frozen secondary image nobody has updated in 6 years?). The
second region's size is much larger than a typical MCU/DSP firmware image and matches the right order of
magnitude for an FPGA configuration bitstream — [[ic9700-hardware]] already documents a Cyclone V
(`IC7601`) on this board — making "component 2 = FPGA bitstream" a reasonable working hypothesis, but this
is a size argument only, not independently confirmed (the real Cyclone V part's actual bitstream size
hasn't been checked against the measured ~7.2MB).

Recorded here as a visible correction rather than silently editing the earlier entry, per how this project
tracks mistakes — the ramp-sweep technique itself was sound and did find something real (the fixed
template), just not what it was first read as.
