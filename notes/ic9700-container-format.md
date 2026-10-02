# IC-9700 firmware container format — cold start, not cracked

See [notes/ic9700-container-format-history.md](ic9700-container-format-history.md) for the full
narrative, offsets, and negative results behind every finding below.

Separate product line from the IC-7300, genuine cold-start effort — no prior art, no existing
unpacker, nothing in `firmware/9700/` except raw `.dat`/`.zip` release files. Main CPU
confirmed as Renesas RZ/A1 series (R7S721001VCBG), same Cortex-A9 family as the IC-7300's
R7S721000 — see [[ic9700-hardware]].

**Ground truth, read directly off the user's own live IC-9700 (2026-08-30)** — the radio's own
firmware-info screen lists **6 independently-versioned components**: Main CPU 1.50, Sub CPU 1.00,
Front CPU 1.00, FPGA Program 1.08, FPGA Data 1.00, DV DSP 1.10. This is the single most important
fact this thread has had since the cold start — it confirms the container really does bundle
multiple independently-updated sub-images (not 2, not "5-6 boundaries" — see the corrections below),
and that three of them (Sub CPU, Front CPU, FPGA Data) have **never been revised past their initial
v1.00** as of this radio's current firmware.

**Then independently confirmed and massively extended via Icom's own public support pages**
(`icomjapan.com`/`icom.co.jp`, both the EN/EUR/USA page and the JP page — the JP page additionally
listed 2 early-2019 releases, v1.02/v1.03, since removed from the EN page but still present in this
project's local file archive): every one of the 20 publicly documented IC-9700 firmware releases
(v1.02 through v1.50) has its own page listing the exact post-update version of **all 6 components**.
This directly confirms the release-file naming convention (`J102`...`J150`, `E105`...`E150`) *is* the
**Main CPU version number** (`J150` = Main CPU v1.50) — Main CPU is the only component that increments
on every single release without exception, and the newest file in this project's dataset (`J150`)
exactly matches the version currently running on the user's own radio. Full per-release table below.

| Version | Date | FPGA Program | DV DSP |
|---|---|---|---|
| 1.02 | 2019/02/08 | 1.01 | 1.00 |
| 1.03 | 2019/03/08 | 1.02 | 1.01 |
| 1.05 | 2019/03/29 | 1.02 | 1.01 |
| 1.06 | 2019/04/19 | 1.03 | 1.02 |
| 1.10 | 2019/06/07 | 1.03 | 1.02 |
| 1.11 | 2019/06/14 | 1.03 | 1.02 |
| 1.13 | 2019/08/30 | 1.04 | 1.03 |
| 1.20 | 2019/10/11 | 1.05 | 1.04 |
| 1.21 | 2019/12/13 | 1.06 | 1.05 |
| 1.23 | 2020/04/03 | 1.06 | 1.06 |
| 1.24 | 2020/05/22 | 1.07 | 1.06 |
| 1.30 | 2021/02/26 | 1.07 | 1.06 |
| 1.31 | 2021/07/09 | 1.07 | 1.06 |
| 1.32 | 2022/08/05 | 1.07 | 1.06 |
| 1.40 | 2023/03/22 | 1.08 | 1.10 |
| 1.41 | 2023/04/06 | 1.08 | 1.10 |
| 1.42 | 2023/05/18 | 1.08 | 1.10 |
| 1.43 | 2023/07/21 | 1.08 | 1.10 |
| 1.44 | 2023/09/15 | 1.08 | 1.10 |
| 1.50 | 2025/08/21 | 1.08 | 1.10 |

(Sub CPU/Front CPU/FPGA Data omitted — confirmed `1.00` in every single one of these 20 releases,
never once revised across the product's whole public history to date.)

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
- **"Component 2" is itself a bundle of multiple components, not one image** — directly explained by
  the 6-component ground truth above (Sub CPU/Front CPU/FPGA Data never revised past v1.00 would
  produce exactly this "large fixed region" signature). Checked by diffing **consecutive** release
  pairs across the whole 19-release J sequence (not just oldest-vs-newest): component 2's body
  changes at only **7 of 18** transitions (`J102→J103`, `J105→J106`, `J111→J113`, `J113→J120`,
  `J120→J121`, `J121→J124`, `J132→J140`) and is **completely frozen** for the other 11, including
  every transition from `J140` through `J150` (the current release).
- **Validated against Icom's own published per-release data with a perfect, zero-discrepancy
  match**: every one of those 7 "changed" transitions corresponds to a real FPGA Program and/or DV
  DSP version bump in the table above, and every one of the 11 "unchanged" transitions corresponds to
  *both* staying flat (e.g. `J140`-`J144`-`J150` are all FPGA `1.08`/DSP `1.10`, matching the found
  freeze exactly; `J130`-`J132` are all FPGA `1.07`/DSP `1.06`, also matching). This is about as
  strong a validation as static analysis gets — the byte-diffing methodology and Icom's own
  changelog data agree on every single checkable transition across the product's full public history
  (2019–2025), with no exceptions found.
- **Attempted to isolate FPGA Program's byte range from DV DSP's using a transition where only one of
  them changed** (`E121→E123`: DV DSP `1.05→1.06`, FPGA Program unchanged; `E123→E124`: FPGA Program
  `1.06→1.07`, DV DSP unchanged) — partially informative, but complicated by a real methodological
  wrinkle: **overall file size shifts slightly release to release** (confirmed: `E121`/`E123`/`E124`
  are 15,626,632 / 15,626,956 / 15,628,630 bytes respectively), so a size change in an earlier
  component cascades into an apparent byte-diff across *everything packed after it*, even where the
  later component's own logical content didn't change. `E121→E123`'s diff span (`0x806038`-`0xedf038`,
  nearly the whole bundle) is too broad to be DV DSP's image alone under this container's basic
  concatenation layout — it's very plausibly this shift artifact, not DV DSP's real footprint.
  `E123→E124`'s diff (a 4KB stub, three small clusters `0x8e0038`-`0x8fa038` totalling ~94KB, then a
  large tail `0x9ca038`-`0xedf038`) is more localized but still likely includes shifted content in its
  own large tail. **Net result: real sub-component boundaries exist inside the bundle, but a raw
  byte-diff can't cleanly separate them while sizes vary — would need to account for the size delta
  explicitly (e.g. find the exact insertion/growth point first) rather than just diffing block-by-block.**
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

1. **Isolate the exact size-shift per transition and re-diff accounting for it**, rather than raw
   fixed-offset block diffing — for each of the 7 confirmed-changed transitions, compute the total
   file-size delta, then look for the specific point where inserted/removed bytes would explain it
   (a real component growing/shrinking), re-aligning everything *after* that point before diffing
   further. This should cleanly separate genuinely-changed sub-images from shift-artifact "noise" in
   a way the raw per-transition diffs (see above) couldn't — the single most promising concrete next
   step, now that the per-release component version ground truth (above) tells us exactly which
   transitions are "DV DSP only", "FPGA Program only", or "both" to test against.
2. **Pin down component 2's exact byte-level start/end** (currently known only to ~4KB granularity:
   starts `0x800038`, ends somewhere near `0xee3038`) and check whether its size, once exact, matches
   a real Cyclone V bitstream size for the specific FPGA part on this board (`5CEFA9F23I7N` — check
   the real Intel/Altera `.rbf`/`.sof` size for that exact device) — would meaningfully strengthen or
   kill the "component 2 = FPGA bitstream" hypothesis beyond the current size-only argument.
3. ~~Get per-release component version ground truth to validate the byte-diffing~~ — **done,
   2026-08-30, and better than hoped**: scraped Icom's own public support pages (EN and JP) for all
   20 published releases' full 6-component version breakdown (see the table above) — a perfect,
   zero-discrepancy match against every checkable byte-diff transition.
4. Web search for other Icom-radio RE projects that might share this compression scheme or vendor
   library — this container idiom (version string + size table + fixed slots) may not be
   IC-7300-specific. Tried 2026-08-30, came back empty (no public prior art found for any Icom
   amateur-radio firmware format) — worth retrying periodically, not worth repeating right away.
5. Wider LZSS-family parameter sweep (different min-match-length, control-byte read order, match
   encoding bit widths) than the 216 combinations already tried, against **both** real components now
   — with real candidate sizes (`0x30` for component 2) to validate against instead of guessing blind.
6. If the constant blob is a signature/certificate/key, identifying its format (RSA modulus size,
   ECDSA, etc.) might narrow down the vendor toolchain/era — weakened as a *signature* specifically
   by its being identical across 37 releases, but the format-ID angle stands regardless of what it
   turns out to be.
7. Decode the fixed 12-byte template shared by `0x400038`/`0x7f0038` (`11 13 14 14 15 04 28 19 19 1e
   1f 3c`) and check whether it (or a variant) recurs anywhere else — since it's fixed and
   non-version-specific, it won't reveal per-release content, but understanding what it *is* would
   help characterize the large fixed regions it sits inside.
8. Actual IC-9700 JTAG hardware access, bypassing the update-container problem entirely — same
   approach as [[hardware-debug-access]]'s IC-7300 plan, different radio. **Resolved, 2026-08-30**:
   JTAG connector *and* pinout confirmed identical to the IC-7300's own `J491` (`10FLT-SM2-TB`,
   standard ARM JTAG) — the already-ordered adapter should work for both radios as-is. See
   [[hardware-debug-access]] for the full pinout.

Treat this as a genuine cold-start RE effort if resuming, not a quick adaptation of existing
IC-7300 tooling.
