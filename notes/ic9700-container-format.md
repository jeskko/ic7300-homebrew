# IC-9700 firmware container format — cold start, not cracked

See [notes/ic9700-container-format-history.md](ic9700-container-format-history.md) for the full
narrative, offsets, and negative results behind every finding below.

Separate product line from the IC-7300, genuine cold-start effort — no prior art, no existing
unpacker, nothing in `/data/misc/icom/9700/` except raw `.dat`/`.zip` release files. Main CPU
confirmed as Renesas RZ/A1 series (R7S721001VCBG), same Cortex-A9 family as the IC-7300's
R7S721000 — see [[ic9700-hardware]].

## Confirmed structural facts (checked across all 37 known releases, J102–J150/E105–E150)

- Container layout is byte-identical in shape across every release, 2019–2025: header at `0x0`, a
  flash-erase-style filler ramp `0x1000`–`0x10000` (not real content), real high-entropy "body"
  content starting at exactly `0x10038` in every file, a `0x800000` fixed-slot boundary constant
  (J/E variants diverge `0x10000`–`0x2a6000`, converge again at `0x800000`).
- Header differs from the IC-7300's shape entirely (not just shifted) — embeds a literal ASCII
  build-tool version string (`"3.623.27"`-shaped) at `0x14` instead of IC-7300's numeric
  `size1..size7` fields.
- Two small high-entropy islands break the filler ramp: `0x4038`–`0x48c8` (~2193 bytes) and
  `0x4f38`–`0x4f3f` (8 bytes). Both are **byte-for-byte identical across all 37 releases** — rules
  out "per-release signature", more consistent with a fixed embedded key/certificate/lookup table.
- A structured plaintext footer at EOF in every file: a 6-byte build-ID stamp (identical within a
  build across regional/point releases, e.g. shared by E106/E110/E111/J106/J110/J111), a `"N.NN"`
  version-ish field that increases roughly monotonically release-to-release, then
  `"-{J|E}001.00-0009"`. Confirmed **not** a whole-file checksum or size-derived value.

## The actual blocker: compression scheme not identified

Real body content starts at `0x10038`, high entropy, presumably compressed and/or encrypted — but
nothing decodes it:

- IC-7300's exact Okumura LZSS parameters ([[decompression-lzss]]): degenerate output.
- A **216-combination systematic sweep** of the LZSS-family parameter space (window size, min-match
  length, ring-buffer cursor init, control-bit polarity/order, offset/length byte order): zero
  plausible results (entropy/repetition-based check, not just eyeballing).
- XOR-whitening the compressed stream against the constant blob (all 2193 byte rotations) before
  LZSS: also negative. Using the blob as a ring-buffer seed instead of zero-init: inconclusive by a
  weak metric, not real evidence — not confirmed.
- No standard compression magic bytes anywhere (gzip/bzip2/xz/zstd/lz4/zip/zlib all checked, no
  genuine hits).

**No way to resolve this from the `.dat` file alone** — the decompressor lives in the device's own
already-installed firmware (same pattern as the IC-7300, see [[firmware-update]]), which we can't
reach without cracking the compression first. No IC-9700 firmware dump, JTAG access, or other route
to installed firmware exists yet.

## If picked up again, roughly in order of promise

1. Web search for other Icom-radio RE projects that might share this compression scheme or vendor
   library — this container idiom (version string + size table + fixed slots) may not be
   IC-7300-specific.
2. Wider LZSS-family parameter sweep (different min-match-length, control-byte read order, match
   encoding bit widths) than the 216 combinations already tried.
3. If the constant blob is a signature/certificate/key, identifying its format (RSA modulus size,
   ECDSA, etc.) might narrow down the vendor toolchain/era — weakened as a *signature* specifically
   by its being identical across 37 releases, but the format-ID angle stands regardless of what it
   turns out to be.
4. Actual IC-9700 JTAG hardware access, bypassing the update-container problem entirely — same
   approach as [[hardware-debug-access]]'s IC-7300 plan, different radio.

Treat this as a genuine cold-start RE effort if resuming, not a quick adaptation of existing
IC-7300 tooling.
