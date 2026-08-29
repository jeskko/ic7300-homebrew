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
