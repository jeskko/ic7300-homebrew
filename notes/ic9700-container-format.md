# IC-9700 firmware container format — cold start, not cracked

See [notes/ic9700-container-format-history.md](ic9700-container-format-history.md) for the full
narrative, offsets, and negative results behind every finding below.

Separate product line from the IC-7300, genuine cold-start effort — no prior art, no existing
unpacker, nothing in `/data/misc/icom/9700/` except raw `.dat`/`.zip` release files. Main CPU
confirmed as Renesas RZ/A1 series (R7S721001VCBG), same Cortex-A9 family as the IC-7300's
R7S721000 — see [[ic9700-hardware]].

## Confirmed structural facts (checked across all 37 known releases, J102–J150/E105–E150)

- Header differs from the IC-7300's shape entirely (not just shifted) — embeds a literal ASCII
  build-tool version string (`"3.623.27"`-shaped) at `0x14` instead of IC-7300's numeric
  `size1..size7` fields.
- **Two previously-unexamined 4-byte fields at header offset `0x30`/`0x34` are genuine, real
  size-shaped values** (2026-08-30) — confirmed by checking all 37 releases: both vary with actual
  release content and track **exactly** with the already-known build-family grouping from the
  footer (e.g. E106/E110/E111/J106/J110/J111 all show the identical pair, matching their shared
  6-byte footer build-ID). `0x30` ≈ 7.2–7.3M, `0x34` ≈ 9.2–9.3M — plausibly a
  {compressed, decompressed} size pair for a sub-component, not yet confirmed (an LZSS decode
  attempt using these as a target size, see below, didn't validate it). A third field at `0x38` has
  a **constant high 3 bytes** (`0x4a60d6`) with only the low byte varying per-build in a narrow
  range — checksum-shaped, not decoded. A fourth field at `0x3c` (`0x569cdaee`) is **byte-identical
  across all 37 releases**, the same "fixed constant" character as the already-known blob below.
- A structured plaintext footer at EOF in every file: a 6-byte build-ID stamp (identical within a
  build across regional/point releases, e.g. shared by E106/E110/E111/J106/J110/J111), a `"N.NN"`
  version-ish field that increases roughly monotonically release-to-release, then
  `"-{J|E}001.00-0009"`. Confirmed **not** a whole-file checksum or size-derived value.
- **Correction, same day**: an earlier pass this session read a sweep for the ramp-filler pattern
  across the whole file (finding large ramp regions ending at `0x10038`, `0x3f0038`, `0x400038`,
  `0x7f0038`, `0x7fc038`, `0x800028`) as "5-6 per-chip component boundaries." **Checked properly
  against 4 different releases (J102/J124/J150/E150) and that's wrong**: every one of those ramp
  offsets, run-lengths, and post-ramp bytes is **byte-for-byte identical across all of them**,
  including the oldest (2019) vs. newest (2025) release. Fixed, non-version-specific content can't
  be a "component boundary" marking where per-release firmware for a different chip lives — it's
  more likely inert reserved space or a genuinely frozen sub-image nobody has updated in 6 years.
  `0x400038` and `0x7f0038` do share an identical 12-byte sequence after a 4-byte varying prefix
  (`11 13 14 14 15 04 28 19 19 1e 1f 3c`) — real, not coincidental — but this is a fixed template
  occurring at fixed locations, not evidence of per-release content there.
- **What actually varies release-to-release, found by direct byte-level diffing of the oldest
  (2019, `J102`) against later releases rather than trusting the ramp-sweep's framing**: exactly
  **two** real, per-release-varying regions — `0x10038`–`0x2a6038` (~2.71 MB, matches the
  already-known J/E-divergence boundary almost exactly) and **`0x800038`–`~0xee3038`
  (~7.2 MB, newly found)** — separated and followed by large stretches that are completely fixed
  across every release. `0x30`'s ≈7.2–7.3M value from the point above now has a much better-supported
  candidate meaning: **the size of this second, larger varying region**, not the first — a coarse
  4KB-granularity diff of that region measured ~7,213,056 bytes, within ~0.2% of `0x30`'s value
  (exact byte-level boundary not yet pinned down). Both real, per-release regions read as maximally
  high-entropy either way (~7.999 bits/byte at a 256KB sample) — no entropy-based distinction
  between them, that avenue is a dead end for telling them apart.
- Given the second region's size (~7.2 MB) is much larger than typical MCU/DSP firmware and
  [[ic9700-hardware]] documents a Cyclone V FPGA (`IC7601`) on this board, **the working hypothesis
  is component 1 (~2.7 MB) = main CPU firmware and component 2 (~7.2 MB) = the FPGA bitstream** —
  a size argument only, not yet independently confirmed. Tried compression against this fresh
  region too (both IC-7300's exact LZSS parameters and raw-DEFLATE, at the confirmed real start
  `0x800038`): both negative, same as component 1.
- Two small high-entropy islands break the *main header's own* filler ramp: `0x4038`–`0x48c8`
  (~2193 bytes) and `0x4f38`–`0x4f3f` (8 bytes). Both are **byte-for-byte identical across all 37
  releases** — rules out "per-release signature", more consistent with a fixed embedded
  key/certificate/lookup table.

## The actual blocker: compression scheme not identified

Real body content starts at `0x10038` (component 1) and `0x800038` (component 2, see above), both
high entropy, presumably compressed and/or encrypted — but nothing decodes either one:

- IC-7300's exact Okumura LZSS parameters ([[decompression-lzss]]): degenerate output. Retried
  2026-08-30 using the newly-found `0x30`/`0x34` header fields as a real target decompressed size
  (rather than an arbitrary guess) — still degenerate (output starts with a long run of zero bytes,
  consumed-byte count doesn't match the candidate compressed size either). A real negative, not
  just "didn't try the right size."
- A **216-combination systematic sweep** of the LZSS-family parameter space (window size, min-match
  length, ring-buffer cursor init, control-bit polarity/order, offset/length byte order): zero
  plausible results (entropy/repetition-based check, not just eyeballing).
- XOR-whitening the compressed stream against the constant blob (all 2193 byte rotations) before
  LZSS: also negative. Using the blob as a ring-buffer seed instead of zero-init: inconclusive by a
  weak metric, not real evidence — not confirmed.
- **zlib/raw-DEFLATE, checked thoroughly for the first time 2026-08-30** (motivated by finding real
  embedded zlib code elsewhere in the *IC-7300's* own firmware, a different but related Icom
  product — see [[kernel-rtos-history]]'s SLV5/graphics-stack thread): a clean 64KB-wide offset
  sweep for plain raw-DEFLATE found zero candidates producing more than a trivial garbage decode;
  XOR-whitening with the constant blob across all 2192 rotations before raw-DEFLATE, at the
  confirmed body-start offset, also zero candidates. A real, thorough negative — not worth
  re-trying without a new reason to suspect it.
- No standard compression magic bytes anywhere (gzip/bzip2/xz/zstd/lz4/zip/zlib all checked, no
  genuine hits).

**No way to resolve this from the `.dat` file alone** — the decompressor lives in the device's own
already-installed firmware (same pattern as the IC-7300, see [[firmware-update]]), which we can't
reach without cracking the compression first. No IC-9700 firmware dump, JTAG access, or other route
to installed firmware exists yet.

## If picked up again, roughly in order of promise

1. **Pin down component 2's exact byte-level start/end** (currently known only to ~4KB granularity:
   starts `0x800038`, ends somewhere near `0xee3038`) and check whether its size, once exact, matches
   a real Cyclone V bitstream size for the specific FPGA part on this board (`5CEFA9F23I7N` — check
   the real Intel/Altera `.rbf`/`.sof` size for that exact device) — would meaningfully strengthen or
   kill the "component 2 = FPGA bitstream" hypothesis beyond the current size-only argument.
2. Decode the fixed 12-byte template shared by `0x400038`/`0x7f0038` (`11 13 14 14 15 04 28 19 19 1e
   1f 3c`) and check whether it (or a variant) recurs anywhere else — since it's fixed and
   non-version-specific, it won't reveal per-release content, but understanding what it *is* would
   help characterize the large fixed regions it sits inside.
3. Web search for other Icom-radio RE projects that might share this compression scheme or vendor
   library — this container idiom (version string + size table + fixed slots) may not be
   IC-7300-specific. Tried 2026-08-30, came back empty (no public prior art found for any Icom
   amateur-radio firmware format) — worth retrying periodically, not worth repeating right away.
4. Wider LZSS-family parameter sweep (different min-match-length, control-byte read order, match
   encoding bit widths) than the 216 combinations already tried, against **both** real components now
   — with real candidate sizes (`0x30` for component 2) to validate against instead of guessing blind.
5. If the constant blob is a signature/certificate/key, identifying its format (RSA modulus size,
   ECDSA, etc.) might narrow down the vendor toolchain/era — weakened as a *signature* specifically
   by its being identical across 37 releases, but the format-ID angle stands regardless of what it
   turns out to be.
6. Actual IC-9700 JTAG hardware access, bypassing the update-container problem entirely — same
   approach as [[hardware-debug-access]]'s IC-7300 plan, different radio. **Progressed, 2026-08-30**:
   JTAG connector confirmed on the IC-9700's schematic, `10FLT-SM2-TB` — the exact same JST FLT-series
   part as the IC-7300's own `J491`. Pin assignment not yet checked (same connector part doesn't
   guarantee same pinout), but if it matches, the FT2232H adapter + FFC breakout already ordered for
   the IC-7300 would very plausibly work here too. See [[hardware-debug-access]]'s own new section on
   this.

Treat this as a genuine cold-start RE effort if resuming, not a quick adaptation of existing
IC-7300 tooling.
