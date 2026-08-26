# Firmware container format (`7300_1XX.dat`)

Derived by tracing `tunk3.py` (the user's existing extractor script,
read-only original at `/data/misc/icom/7300/tunk3.py`). This is the
starting hypothesis for the rewritten `tools/` unpacker — **not yet
independently verified against all 10 releases**, see the open questions
below and [[firmware-versions]].

## Layout as currently understood

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x0000` | 16 B | version string | Shift-JIS, e.g. `3wfU3.112.003.16`. **Stops changing after v1.14** — not a reliable per-build discriminator, see [[firmware-versions]]. |
| `0x0010` | 4 B × 7 | `size1`..`size7` | Little-endian u32 each. Purpose of each field is **not yet confirmed** — `tunk3.py` only prints them, doesn't use them for offset math anywhere. This is a prime suspect for where the real chunk boundaries/counts are actually encoded, instead of the hardcoded offsets below. |
| `0x1002c` | 4 B | `length` | LE u32 — exact decompressed byte count of the main body (matches [[firmware-versions]]' `unpacked.dat` sizes, e.g. `3738328` for v1.11). |
| `0x10030` | variable | main body, LZSS-compressed | Decompress `length` bytes per [[decompression-lzss]] → this is `unpacked.dat`/`out.dat`, the main ARM firmware image. |
| `0x21002c` | 4 B + N | chunk1 | LE u32 size prefix, then N raw (uncompressed) bytes → `chunk1.ttf`, a TrueType font. |
| `0x24002c` | 4 B + N | chunk2 | Same shape → `chunk2.ttf`, another TrueType font. |
| `0x25002c` | 4 B + N | chunk3 | Same shape → `chunk3.dat`, raw, **purpose unidentified**. |
| (continues immediately after chunk3) | 0x10000 fixed | chunk4 | **LZSS-compressed** (unlike chunks 1–3), fixed 65536-byte output regardless of container version → `chunk4.dat`, **purpose unidentified**. |
| (continues immediately after chunk4) | to EOF | **chunk5 / tail** ⚠️ **not read by `tunk3.py` at all** | **LZSS-compressed, ~1.46 MB**, no length prefix — decode runs until input is exhausted, landing exactly on the container's last byte (verified for v1.11 and v1.42). See [[multi-cpu-images]] — this is the current #1 candidate for an embedded second-processor firmware image. |

## Ground-truthed: `base.dat`/`data.dat` are a straight split of the container

Verified directly (byte-for-byte `cmp`, v1.11) against the read-only
originals — **not** guessed from the scripts:

- `7300_1XX/base.dat` (66000 B = `0x101d0`, constant size across all 10
  versions) is **byte-identical to `container[0x0:0x101d0]`** i.e.
  `container[0:66000]` — the version string, the `size1..size7` table, and
  the ARM boot loader/vector table (see [[base-loader]]).
- `7300_1XX/data.dat` is **byte-identical to `container[0x10030:]`** — i.e.
  everything from the LZSS main-body start onward: main body + chunk1 font
  + chunk2 font + chunk3 + chunk4, all still packed together exactly as
  `tunk3.py` expects. (Note the 416-byte overlap between the two: bytes
  `0x10030..0x101d0` exist in both `base.dat` and `data.dat` — `base.dat`'s
  last 416 bytes are boot-loader code that happens to run past where the
  compressed body begins in the container, they aren't part of the LZSS
  stream itself since `tunk3.py` starts decoding from `0x10030` in the
  *container*, i.e. from `data.dat` offset `0`.)

This means `base.dat`/`data.dat` per version are pure derived artifacts (a
two-way split at a fixed offset), not independently meaningful containers —
confirms there's exactly one split point (`0x10030`) worth caring about at
the top level, and that per-version `base.dat` (constant 66000 B across all
10 releases) is a good, stable target for locating the LZSS decompressor
and the boot vector table (see [[base-loader]]) once in Ghidra.

## Known risk: hardcoded absolute offsets

`tunk3.py` uses fixed absolute offsets (`0x21002c`, `0x24002c`, `0x25002c`)
for chunks 1–3, rather than deriving them from where the compressed main
body actually ends or from the `size1..size7` header fields. Per
[[firmware-versions]], the compressed main-body size varies by version
(overall `.dat` file size deltas of several KB between releases, per
`foo.txt`), while these fixed offsets give roughly 2 MB of headroom after
the body starts at `0x10030` — so it *probably* still works across all 10
releases, but this has only been confirmed for v1.42 (`out.dat` ==
`7300_142/unpacked.dat` by md5, per the directory survey). **The rewritten
unpacker should derive these offsets properly (or at minimum verify the
fixed offsets byte-for-byte against every version) rather than trust the
hardcoded constants blindly.**

## Open questions (goals for the rewritten unpacker)

1. What do `size1`..`size7` actually bound? Do any of them equal
   `length`, or the chunk1/2/3 sizes, or the compressed-body size (as
   opposed to `length` which is the *decompressed* size)? Cross-reference
   against actual parsed values for all 10 versions.
2. Where does the LZSS-compressed main-body stream *actually* end (i.e.
   its compressed byte length, not just decompressed `length`)? Right now
   nothing in `tunk3.py` computes this — the decompressor just stops
   emitting once `length` output bytes are produced, and the leftover file
   position is never checked against `0x21002c`.
3. **Multi-CPU question** (see [[multi-cpu-images]]): `tunk.py`'s
   commented notes place `"SX3765 V1.00-003"` strings at offsets `0x4590`
   and `0x4f2c` — inside the *decompressed* main body, i.e. very early in
   `out.dat`/`unpacked.dat`. That's a strong signal that a second
   processor's firmware image is embedded within the first ~20 KB of the
   decompressed main body, not necessarily in `chunk3.dat`/`chunk4.dat`/
   `snip` as first assumed. Needs to be mapped out explicitly.
