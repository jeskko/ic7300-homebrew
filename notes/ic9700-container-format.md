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
- **The container has (at least) 5 major component-boundary regions, not just the 2 previously
  known** (2026-08-30 finding, via a systematic sweep for the flash-erase-style decrementing-ramp
  filler pattern across the *whole* file, not just the header): large ramp regions end at `0x10038`
  (already known), `0x3f0038`, `0x400038`, `0x7f0038`, `0x7fc038`, and `0x800028`
  (`0x800000`+header, already known as a J/E-convergence point). Each of the newly-found boundaries
  breaks the ramp with the identical tail bytes `fb fa f9 f8 f7 f6 f5 f4` seen at the main header's
  own ramp-end, then a short structured region before real high-entropy content resumes — the same
  shape as the already-documented `0x800000` component header, just not previously noticed because
  nobody had swept past the first ~1MB of the file for this pattern before.
- **Two of those boundaries (`0x400038` and `0x7f0038`) share an *identical* multi-byte sequence**
  right after their ramps end (`11 13 14 14 15 04 28 19 19 1e 1f 3c`) — checked directly, not a
  coincidence of two independent high-entropy regions. This is real evidence of a fixed, repeating
  per-component header/descriptor template used at multiple points in the file, not a one-off
  structure. Not yet decoded or matched against the other boundaries' own post-ramp bytes (`0x3f0038`
  and `0x7fc038` look different from each other and from this pair — not yet characterized).
- Two small high-entropy islands break the *main header's own* filler ramp: `0x4038`–`0x48c8`
  (~2193 bytes) and `0x4f38`–`0x4f3f` (8 bytes). Both are **byte-for-byte identical across all 37
  releases** — rules out "per-release signature", more consistent with a fixed embedded
  key/certificate/lookup table.

## The actual blocker: compression scheme not identified

Real body content starts at `0x10038`, high entropy, presumably compressed and/or encrypted — but
nothing decodes it:

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

1. **Fully map the newly-found component boundaries** — decode/compare the short structured region
   right after each ramp ends (`0x3f0038`, `0x400038`, `0x7f0038`, `0x7fc038`, `0x800028`), not just
   the two that were found to match exactly. Given the file is ~15.6MB and these boundaries roughly
   bracket 4MB/8MB-ish regions, this container is very plausibly segmented per-chip (main CPU / FPGA
   bitstream / other MCU / DSP, mirroring the already-confirmed IC-7300 multi-component pattern in
   [[multi-cpu-images]]) rather than being one monolithic compressed blob — worth checking against
   [[ic9700-hardware]]'s chip list (main CPU, Cyclone V FPGA, STM32 MCU, TMS320C55x DSP) for a
   size/count match before assuming anything about which region is which.
2. Web search for other Icom-radio RE projects that might share this compression scheme or vendor
   library — this container idiom (version string + size table + fixed slots) may not be
   IC-7300-specific. Tried 2026-08-30, came back empty (no public prior art found for any Icom
   amateur-radio firmware format) — worth retrying periodically, not worth repeating right away.
3. Wider LZSS-family parameter sweep (different min-match-length, control-byte read order, match
   encoding bit widths) than the 216 combinations already tried — now with real candidate sizes
   (`0x30`/`0x34`) to validate against instead of guessing blind.
4. If the constant blob is a signature/certificate/key, identifying its format (RSA modulus size,
   ECDSA, etc.) might narrow down the vendor toolchain/era — weakened as a *signature* specifically
   by its being identical across 37 releases, but the format-ID angle stands regardless of what it
   turns out to be.
5. Actual IC-9700 JTAG hardware access, bypassing the update-container problem entirely — same
   approach as [[hardware-debug-access]]'s IC-7300 plan, different radio. **In progress, 2026-08-30**:
   user is checking the schematic for a JTAG connector on the IC-9700 board (nothing scoped yet —
   unlike the IC-7300, no connector has been identified as populated on this board at all).

Treat this as a genuine cold-start RE effort if resuming, not a quick adaptation of existing
IC-7300 tooling.
