# Firmware container format (`7300_1XX.dat`)

Derived by tracing `tunk3.py` (the user's existing extractor script,
read-only original at `/data/misc/icom/7300/tunk3.py`) — the starting
hypothesis for the rewritten `tools/` unpacker. **Now independently
verified against all 10 releases** ([[firmware-versions]]'s full
cross-version survey), and the region beyond chunk3 that `tunk3.py` never
read correctly (its "chunk4"/tail model) has since been fully decoded —
see the sections below. Only a couple of small open questions remain, see
the end of this file.

**Packing (the inverse direction), 2026-09-08**: `tools/icom_fw/container.py`'s `pack()` (with a
from-scratch LZSS encoder in `lzss.py`'s `compress()`) rebuilds a checksum-correct container from a
modified decompressed body — everything outside the body's fixed slot is copied verbatim, and the
checksum region documented below (`size1`'s role) is recomputed correctly. Round-trip-verified against
all 10 releases (`tools/verify_pack.py`). See `tools/README.md` and `sdk/roadmap.md`'s Phase 0.

## Layout as currently understood

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x0000` | 16 B | version string | Shift-JIS, e.g. `3wfU3.112.003.16`. **Stops changing after v1.14** — not a reliable per-build discriminator, see [[firmware-versions]]. |
| `0x0010` | 4 B × 7 | `size1`..`size7` | Little-endian u32 each. `tunk3.py` only prints them, doesn't use them for offset math anywhere — but the real firmware does. **Meaning of every field now confirmed**, see the section right after this table. |
| `0x1002c` | 4 B | `length` | LE u32 — exact decompressed byte count of the main body (matches [[firmware-versions]]' `unpacked.dat` sizes, e.g. `3738328` for v1.11). |
| `0x10030` | variable | main body, LZSS-compressed | Decompress `length` bytes per [[decompression-lzss]] → this is `unpacked.dat`/`out.dat`, the main ARM firmware image. |
| `0x21002c` | 4 B + N | chunk1 | LE u32 size prefix, then N raw (uncompressed) bytes → `chunk1.ttf`, a TrueType font. |
| `0x24002c` | 4 B + N | chunk2 | Same shape → `chunk2.ttf`, another TrueType font. |
| `0x25002c` | 4 B + N | chunk3 | Same shape → `chunk3.dat`, raw, **purpose unidentified**. |
| (continues immediately after chunk3) | — | **"chunk4"/"chunk5-tail"** — legacy `tunk3.py`-era names, **superseded** | This region is **not** a fixed-0x10000-block-then-decode-to-EOF pair as `tunk3.py` guessed (and as this table used to claim) — that split doesn't correspond to any boundary the real firmware recognizes. It's really **3 separately-bounded, MD5-verified LZSS components**, whose exact file offsets are computed from `size1`..`size7` (see the section right after this table). Confirmed byte-exact (LZSS consumption + MD5 match) against a real v1.42 container. Extracted via `tools/icom_fw/dsp_chunks.py`, not `tunk3.py`. Current best identity per [[multi-cpu-images]]: component1/2 are the DSP's program/data images (TMS320C674x, `IC901`); component0 (the old "chunk4", historically guessed to be Front CPU firmware) is confirmed genuine DSP-class object code too, not front-panel firmware — its exact relationship to component1 is still open. |

## `size1`..`size7` field meanings — CONFIRMED (2026-08-29)

All 7 header fields are now understood, cross-checked against
[[firmware-versions]]'s per-release survey and `firmware_update_main`'s
real offset arithmetic (see [[firmware-update]]):

| Field | Role | Cross-version behavior |
|---|---|---|
| `size1` | Total byte length of the **boot-loader + main body + chunk1 + chunk2 + chunk3** region as a fixed, over-provisioned slot (`component0_offset = size1 + 0x3c`; the update flasher also writes exactly `[0x2c, size1+0x2c)` as one checksummed blob). **Not** the actual compressed-body size — it's a fixed slot budget, which is why it never changes. | Byte-for-byte constant (`2,436,080`) across all 10 releases. |
| `size2` | Component0's compressed length | Varies release-to-release only through v1.11→v1.14, then frozen through v1.42 |
| `size3` | Component0's decompressed length | Constant across all releases (`163,592`) |
| `size4` | Component1's (DSP Program) compressed length | Constant across all releases (`721,836`) |
| `size5` | Component1's decompressed length | Constant across all releases (`720,648`) |
| `size6` | Component2's (DSP Data) compressed length | Varies through v1.14, then frozen |
| `size7` | Component2's decompressed length | Varies through v1.14, then frozen |

`component0_offset = size1 + 0x3c`, `component1_offset = component0_offset
+ size2 + 0x10`, `component2_offset = component1_offset + size4 + 0x10` —
see [[multi-cpu-images]] for the full derivation and what each component
actually is. The `size2`/`size6`/`size7`-froze-after-v1.14 pattern (while
the main body's decompressed `length` keeps changing every release) is the
strongest evidence that Icom stopped updating whatever component0/2 are
after v1.14 while continuing to update the main RZ/A1H application —
see [[firmware-versions]].

## Ground-truthed: `base.dat`/`data.dat` are a straight split of the container

Verified directly (byte-for-byte `cmp`, v1.11) against the read-only
originals — **not** guessed from the scripts:

- `7300_1XX/base.dat` (66000 B = `0x101d0`, constant size across all 10
  versions) is **byte-identical to `container[0x0:0x101d0]`** i.e.
  `container[0:66000]` — the version string, the `size1..size7` table, and
  the ARM boot loader/vector table (see [[base-loader]]).
- `7300_1XX/data.dat` is **byte-identical to `container[0x10030:]`** — i.e.
  everything from the LZSS main-body start onward: main body + chunk1 font
  + chunk2 font + chunk3 + the 3-component DSP/data region (the old
  "chunk4"/"chunk5-tail"), all still packed together exactly as `tunk3.py`
  expects. (Note the 416-byte overlap between the two: bytes
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

## Resolved: `tunk3.py`'s fixed offsets are genuine fixed slots, not a lucky guess

`tunk3.py` uses fixed absolute offsets (`0x21002c`, `0x24002c`, `0x25002c`)
for chunks 1–3, rather than deriving them from where the compressed main
body actually ends or from the `size1..size7` header fields. [[firmware-versions]]'s
full cross-version survey (all 10 releases, not just v1.42) confirms this
is architecturally correct, not a fragile assumption: the main-body/chunk1/
chunk2 regions really are **fixed-size, over-provisioned slots** (main body
gets a ~2 MiB budget at `0x10030`-`0x21002c` regardless of actual compressed
size, chunk1 a 192 KiB slot, chunk2 a 64 KiB slot), each padded well beyond
its real content — chunk3/chunk4 by contrast are tightly packed with no
gap, matching `tunk3.py`'s own no-seek-between-them code. `size1` is the
fixed total covering the boot-loader+body+chunk1+chunk2+chunk3 span (see the
field-meanings table above) and is itself constant across every release,
independent confirmation this really is a static slot layout, not something
computed per-build. **The rewritten unpacker's fixed-offset approach was the
right call all along**; the real firmware update code (see
[[firmware-update]]) also derives its own chunk offsets from `size1`
(`chunk_offset = size1 + 0x3c`, etc.) rather than hardcoding raw addresses,
independently confirming these header fields really are the size/boundary
fields they were suspected to be.

## Resolved: the "SX3765" multi-CPU question

The old open question here — whether `"SX3765 V1.00-003"` strings found at
`0x4590`/`0x4f2c` in the decompressed main body meant a second processor's
firmware was embedded early in `out.dat`/`unpacked.dat` — is **resolved,
and the answer is no**: `SX3765` is the part marking on `IC501`
(`R5F104LCAFB`, Renesas RL78), the front panel's own MCU. Every
`"SX3765 Vx.xx-yyy"` string in the firmware is a compatibility/version
check against that chip, not an embedded second-processor firmware image.
The real second/third-processor components (DSP program/data, and a still-
unidentified third piece) live in the region documented above (the old
"chunk4"/"chunk5-tail"), not here. Full detail in [[multi-cpu-images]].

## Open questions

1. `chunk3`'s actual purpose is still unidentified — zero code references
   found anywhere in the traced firmware for the version checked (see
   [[multi-cpu-images]]'s open questions).
2. Component0's exact identity/relationship to component1 (DSP Program) is
   still open — confirmed genuine TMS320C674x object code, not front-panel
   firmware, but what it specifically is remains unresolved. See
   [[multi-cpu-images]].
