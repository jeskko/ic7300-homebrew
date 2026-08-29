# Open question: how many processor images does the container hold?

## DSP firmware precisely located and unpacked (2026-08-29, next session, continuing the DSP comms thread)

**This closes the long-running "where is the DSP's firmware" question, first opened in this file's
`chunk4`/`chunk5-tail` entropy analysis many sessions ago.** Picked up directly from
`firmware_update_main`'s `local_9c[]` offset computation (already partially transcribed in
`notes/firmware-update.md`, never fully decoded) and traced it to an exact, byte-verified formula.

**The exact per-component formula, decompiled from `firmware_update_main` directly:**
```
size1..size7 = the container's existing 7 header fields (file offsets 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28)

component0_offset = size1 + 0x3c
component1_offset = component0_offset + size2 + 0x10
component2_offset = component1_offset + size4 + 0x10
```
Each component is `[LZSS-compressed payload][0x10-byte MD5 trailer]` back-to-back — compressed length is
size2/size4/size6 respectively (read + MD5-verified as a block), decompressed length is size3/size5/size7
respectively. The decompressor is a **second, independent instance of the exact same Okumura LZSS variant**
already documented for the main body (`chunk_lzss_decompress_init`/`_fill`, was `FUN_20024ef8`/`FUN_20024f30`
— found inside `chunk_transport_send_data`, decompressing 256 bytes at a time as it feeds each DSP transfer
page). Full derivation, including the exact decompiled source, is in `firmware_update_main`'s own Ghidra
plate comment now.

**Verified against a real v1.42 container (`/data/misc/icom/7300/7300_142.dat`), about as thoroughly as static
analysis allows**: computed all 3 offsets from the real header, sliced out the 3 compressed blobs, ran them
through `tools/icom_fw`'s existing LZSS decoder (already proven correct for the main body), and got — for
all 3 components — **exact byte-for-byte LZSS stream consumption** (no leftover bytes, no early exhaustion)
**and an exact MD5 match** against each component's own embedded trailer. This is as close to proof as this
project gets without live hardware: both independent checks the real firmware itself would perform both pass.

| Component (hypothesized) | File offset | Compressed | Decompressed |
|---|---|---|---|
| 0 — Front CPU | `0x252c2c` | `0x17e46` (97,862 B) | `0x27f08` (163,592 B) |
| 1 — DSP Program | `0x26aa82` | `0xb03ac` (721,836 B) | `0xaff08` (720,648 B) |
| 2 — DSP Data | `0x31ae3e` | `0xaa759` (698,201 B) | `0xd1d14` (859,412 B) |

(Component ordering is the existing working hypothesis from the `FUN_200a94c8` version-field-ordering
finding earlier in this file — matches structurally, not yet independently confirmed component-by-component.)

**This retracts/supersedes `tools/icom_fw/container.py`'s `chunk4`/`chunk5_tail` model for this region** —
that model's fixed-0x10000-decompressed "chunk4 right after chunk3" + "chunk5/tail decode-to-EOF" framing
was a reasonable first guess from `tunk3.py`'s byte accounting (and the 9th session's own caveat already
flagged this as suspect), but the real firmware doesn't recognize any chunk4/chunk5 boundary — these 3
components just happen to span across where that boundary falls. New tool: `tools/icom_fw/dsp_chunks.py`
(`python3 -m tools.icom_fw.dsp_chunks <container.dat> <out_dir>`) implements the correct model and reports
MD5 pass/fail per component. Extracted files live in `scratch/unpacked/142/`: `front_cpu.bin`,
`dsp_program.bin`, `dsp_data.bin` (plus each one's own `_compressed.bin`).

**Forensic look at the decompressed bytes — genuinely promising, not conclusive**:
- **Real internal structure, unlike the old (wrongly-scoped) `chunk4`/`chunk5-tail` entropy analysis**:
  `dsp_program.bin` starts with several *low*-entropy 1 KB blocks (as low as ~1.5 bits/byte) before ramping
  to a steady ~7.0 bits/byte plateau — a low-entropy header/vector-table region followed by a dense body is
  exactly the shape real executable images have, unlike the smooth, structureless ramps the old
  (mis-scoped) analysis found. `dsp_data.bin` shows the same pattern even more sharply (down to ~0.25
  bits/byte in a couple of blocks — long constant/padding runs), consistent with a sparse calibration/
  coefficient table rather than code.
- **`front_cpu.bin` starts with an ASCII string, `"TIPAcYSX"`** (ambiguous — not immediately recognizable),
  and both `front_cpu.bin` and `dsp_program.bin` end with a distinctive plain-ASCII numeric tag right
  before EOF (`"31101070"` and `"20001000"` respectively), preceded by `0xFF` padding — shaped like an
  embedded build/version tag, a common firmware-image convention. `dsp_data.bin` starts with `"31601130"`
  (also ASCII digits) followed by a long `0xFF` run.
- **One specific, hard-to-fake match**: `dsp_program.bin` contains the 16-bit value `0x5a82` **19 times**
  (2 of those as a full 32-bit word with a zero upper half) — this is the *exact* standard Q15 fixed-point
  representation of `√2⁄2` (`0.70710678… × 32768 = 23170 = 0x5a82`), an extremely common DSP constant (FFT
  twiddle factors, quadrature/Costas-loop trig, etc.). Suggestive, not proof by itself, but a genuinely
  specific, checkable match — not the kind of thing that shows up by chance in arbitrary data.

**Disassembly: checked concretely, not currently possible with any available tool.** The DSP is `IC901`,
a TI **TMS320C6745** (C674x core, VLIW). Checked this session:
- **Ghidra** (this project's own install, `/opt/ghidra`): no TMS320C6000/C674x processor module among its
  `Ghidra/Processors/*` directories (has `TI_MSP430`, an unrelated TI chip family, but nothing for the C6000
  DSP line).
- **binutils** (`objdump`/`arm-none-eabi-objdump` on this machine): no `tic6x` target registered; no
  `tic6x-elf-*` toolchain package found via `pacman`.
- **Capstone**: not installed, and doesn't support this architecture regardless.
- **LLVM** (`llc`): no C6x-shaped target in its registered target list.
- **Web search**: confirmed this is a known, longstanding gap — TMS320C6000 support has been an open,
  unfulfilled Ghidra feature request for years ([NationalSecurityAgency/ghidra#1807](https://github.com/NationalSecurityAgency/ghidra/issues/1807),
  [#5259](https://github.com/NationalSecurityAgency/ghidra/issues/5259)); no ready community Ghidra
  processor extension or other open-source disassembler for this specific TI DSP family turned up (there IS
  a community extension for the unrelated `tms320c24x` family, [banksy-git/ghidra-tms320c24x](https://github.com/banksy-git/ghidra-tms320c24x),
  demonstrating the *pattern* is done for other TI DSPs, just not this one).

**So the practical options, if this is picked up again**: (a) write a minimal custom disassembler from TI's
own public C6000 CPU and Instruction Set Reference (`SPRU189`) — the encoding is fully documented, just a
real, multi-session undertaking (VLIW fetch packets, 2 register files, cross-path stalls); (b) build a real
Ghidra processor module (SLEIGH) for it, a bigger version of the same task; (c) keep working the bytes
statically without a disassembler — pattern/constant hunting (like the `0x5a82` find above) can still turn
up real information without full instruction decoding; (d) live JTAG on the DSP itself, if that's ever
brought up as a target (not currently planned — the project's JTAG hardware targets the main CPU).

**RETRACTED, 2026-08-29 — (a)/(c)/(d) above are no longer the only options; real disassemblers do exist,
this session's checks above were checking "installed on this machine" rather than "exists upstream" (the
same mistake the RL78 thread's original handoff made and then corrected — see [[front-panel-firmware]]).
Checked properly this time, from actual upstream source, not just local package availability:**

- **GNU binutils has a real, working `tic6x` target** (`opcodes/tic6x-dis.c`, `bfd/elf32-tic6x.c` — added
  for TI's OMAP-L1x/AM1x DSP+ARM SoCs, which use this exact C674x-class core). Built stock `binutils-2.44`
  from `ftp.gnu.org` with `--target=tic6x-elf` — clean build, ~2 minutes, zero patches (same recipe as the
  RL78 build in [[front-panel-firmware]]). Installed at
  `~/.local/tic6x-binutils/bin/{tic6x-objdump,tic6x-readelf}` (outside the repo, reproducible from this
  recipe — not committed as a binary blob). **Usage note, easy to get wrong**: needs explicit
  `-EL`/`--endian=little` — without it, `objdump -m tic6x -b binary` silently assumes big-endian and
  produces near-total garbage (every word "undefined instruction"). With `-EL` against `dsp_program.bin` at
  a plateau offset (`0x4000`, well past the low-entropy header-ish region at the very start of the file),
  the decode looks genuinely convincing: real functional-unit annotations (`.D1`/`.D2`/`.M1`/`.M2`/`.L1`/
  `.L2`/`.S1`/`.S2`, including cross-path variants like `.L1X`/`.M2X`/`.D1T2`), real predicated-execution
  syntax (`[a2]`, `[!b1]`), and real parallel-execution bars (`||`) marking same-fetch-packet instructions —
  these are structural VLIW features a garbage/misaligned decode wouldn't produce convincingly. About
  25-30% of words in this window still show `<undefined instruction>` — most likely either genuine data
  words interleaved with code, or floating-point encodings specific to the C674x/C67x+ extension that this
  disassembler's OMAP-L1x-era Linux-toolchain origins may not fully cover (no `-M`/CPU-variant flag exists
  to select a silicon revision — checked, `-M help` produces no options for this target). Not chased further
  this session.
- **Capstone 5 also has real TI C6x support**: `CS_ARCH_TMS320C64X`, in mainline `capstone-engine/capstone`
  (not a fork), pip-installable in a throwaway venv (`python3 -m venv venv && venv/bin/pip install
  capstone`, no system-package changes needed). Also needs explicit `CS_MODE_LITTLE_ENDIAN`. Gives a
  plausible but less richly-annotated decode than binutils on the same bytes (real mnemonics like `addab`/
  `lddw`/`mpyluhs`/lda branch targets, but no functional-unit/predicate/parallel-bar detail) — binutils is
  the better tool of the two for actually reading this code.
- **Still confirmed absent**: no viable Ghidra Sleigh module. Re-checked via web search — the only related
  community repo, [gm-stack/tms320-ghidra](https://github.com/gm-stack/tms320-ghidra), targets the
  unrelated older TMS320C32 family and is explicitly unfinished (decode only, no P-code, no decompilation)
  even for that different chip. The Ghidra feature-request tickets cited above are still open.
- **Quick cross-check against the `dsp_data.bin`-is-FPGA-bitstream hypothesis** (section above): tried the
  same `tic6x-objdump -EL` treatment on a dense region of `dsp_data.bin` (offset `0x30000`) for comparison —
  came back with a similar ~30% undefined rate to `dsp_program.bin`'s, so this specific check is
  **inconclusive**, not a confirmation either way. Also noticed a few spots decoding as 2-byte-wide
  instructions there (address deltas of 2 instead of 4) — possibly TI's compact 16-bit instruction-set
  extension, possibly a local misalignment artifact; not investigated further.

**Next steps if this is picked up again**: disassemble a much larger span of `dsp_program.bin` with
`tic6x-objdump -EL` and look for the same kind of anchor this project used successfully elsewhere (the
`SCIF5` protocol's known command bytes/handshake shape, or literal `SCFTDR_5`-adjacent constants) to orient
inside the DSP's own code; consider whether the `-EL` flag alone is enough or whether the file needs
byte-swapping at a different granularity first (the file was correctly LZSS-decompressed and MD5-verified,
so this is purely an endianness/disassembler-invocation question, not a data-integrity one).

## Pointed the new DSP disassemblers at `front_cpu.bin` too — strong positive evidence it's real C674x code (2026-08-29, same day)

Following up on the "chunk 0 = Front CPU label is now actively doubtful" section above: ran both
`tic6x-objdump -EL` and Capstone's `CS_ARCH_TMS320C64X` against `front_cpu.bin` (skipping its ~34-byte
header) the same way just used on `dsp_program.bin`. First tried an automated alignment×endianness sweep
(undefined-instruction rate across 4 byte-phase shifts) — this turned out to be a **dead end as a
methodology**: running the identical sweep against `dsp_program.bin` (already established as real code) as
a calibration check showed the undefined rate is essentially *identical* across all 4 phase shifts there
too (29.6% at every shift) — meaning this specific metric isn't sensitive to alignment at all for this
decoder/data combination, and shouldn't be trusted as a discriminator. Recorded so a future session doesn't
repeat it expecting a clean signal.

**What actually is decisive: reading the decoded output qualitatively, the same way that convinced this
session on `dsp_program.bin`.** Checked four widely separated offsets (`0x1000`, `0x4000`, `0x8000`,
`0x10000`) and found consistent, specific, hard-to-fake structure at every one:
- Real **floating-point instruction sequences** — `spdp`/`dpsp` (single↔double conversion), `absdp`,
  `cmpltdp`/`cmpgtdp`, `mpydp`, `adddp`, `intspu` (int→single-precision) — used in coherent order (e.g.
  convert→absolute-value→compare at `0x1048`-`0x1064`; int-to-float→add→convert-back at `0x80c8`-`0x80e8`).
  These are C67x+/C674x-specific floating-point extension instructions, not generic fixed-point C64x ops —
  a specific match to this exact DSP's core, not just "some C6x chip."
- Repeated `mvk`/`mvkh` 32-bit-constant-building pairs (the standard TI C6x idiom for a 32-bit immediate),
  correctly paired at every one of the four offsets checked.
- A genuine local backward branch: `b .S1 0x100a4` at `0x100d0`, landing at a real, in-range, nearby address.
- `addkpc .S2 0x100a8,b3,0` at address `0x100a4` — the encoded target is exactly `self+4`, the textbook
  shape for a hardware call/return-address setup, not a coincidental value.

**Cross-validated by two independent disassemblers, not just one reading its own decode charitably**:
Capstone's separate `TMS320C64X` implementation agrees byte-for-byte with `tic6x-objdump` on decoded
immediates at the same addresses (e.g. at `0x8000`: both read a `mvk`/`mvkh` pair producing `0xef0` then
`0x11820000`). Two from-scratch implementations converging on the same interpretation across multiple
regions is much stronger evidence than either tool's output alone.

**This upgrades the "chunk 0 is probably not Front CPU firmware" conclusion above into a positive,
specific one: `front_cpu.bin` looks like genuine TMS320C674x object code**, not just "some other DSP-side
data blob" as a vague placeholder guess. Doesn't yet explain *what* this code does or why the update
mechanism treats it as a separate, distinctly-versioned component from `dsp_program.bin` — that's the
natural next question if this thread continues (is this a second overlay/segment of the same DSP program,
loaded to a different memory region? a completely separate DSP-side subsystem?) — not resolved this
session.

## Is `dsp_data.bin` (component2) actually the FPGA bitstream? Genuinely plausible, re-examined the evidence (2026-08-29, same session)

User's question, prompted by re-reading this file's own `IC902` history: the *current* model has `IC901`
(DSP) self-booting from its own flash (`IC902`) and then "likely driv[ing] the FPGA's config pins itself as
part of its own firmware" (see the `IC902` identity section below) — meaning there's no *other* traced path
anywhere in `body.bin` for how `IC1351` (the FPGA, `EP4CE55F23I7N`, Altera/Intel Cyclone IV E) ever gets
configured. If the DSP is really the one pushing bits into the FPGA, the natural question is whether
`component2` ("DSP Data," so far just an ordering guess) is actually that bitstream, relayed through the DSP
rather than reaching the FPGA any other way.

**Re-examined the whole-file byte histogram (not just the leading 64 bytes, which is what the original
"looks like small signed calibration constants" read was based on) — and it changes the picture:**

| File | dominant byte | next few (by count) | bytes in `0xf0-0xff` |
|---|---|---|---|
| `dsp_program.bin` | `0xff` (8.4%) | `0x00`/`0x08`/`0x80`/`0x11`/`0x01`/`0x99`/`0x10`/`0x22`/`0x89`/`0x12`/`0x9a`, all 1.4-2.2% — flat, no dominant value | 9.5% |
| `dsp_data.bin` | **`0x00` (20.1%)** | `0x22`/`0x44`/`0x11`/`0x42`/`0x14`/`0x21`/`0x84`/`0x40`/`0x88`/`0x24`/`0x20`, 2-3% each | 1.4% |

**`dsp_data.bin`'s whole-file distribution is dominated by `0x00` plus a specific, striking set of
follow-up bytes that are *all* 1-or-2-bits-set values** (`0x11`,`0x22`,`0x44`,`0x88`,`0x14`,`0x21`,`0x42`,
`0x84`,`0x24`,`0x40`,`0x20` — every single one). That's not what small-signed-calibration-constant data
would look like (which clusters around specific *numeric* values, not specifically low-bit-count byte
patterns); it's a well-known signature of **raw SRAM-FPGA configuration data for a design that doesn't use
anywhere near the full fabric** — LUT and routing-switch bits default to 0/single-bit-set for the (typically
large majority of) unused fabric, so real bitstreams for modest designs on a mid-size FPGA are often
dominated by zero and near-zero-Hamming-weight bytes exactly like this. `dsp_program.bin`, by contrast, has
a genuinely flat, spread-out distribution (no single byte value above ~2%) — much more consistent with real
VLIW instruction words (varied opcode/register fields) than with FPGA config data. This reverses the
original 64-byte-only read and is a real, checkable point in favor of the user's hypothesis, not against it.

**Size check against the real, published number for this exact FPGA**: Intel's Cyclone IV Device Handbook
(`cyiv-51008.pdf`) lists the EP4CE55's **uncompressed** raw binary file (`.rbf`) configuration size as
**14,889,560 bits = 1,861,195 bytes**. `dsp_data.bin` decompresses to 859,412 bytes — **46.2%** of that
figure. Not a match to the raw size, but Altera/Intel's own configuration flow supports an optional,
proprietary **bitstream compression** feature for exactly this FPGA family, commonly quoted as roughly
35-65% of the raw size depending on the design — 46% sits squarely inside that range. So the size is
consistent with "compressed Altera bitstream," not with "raw bitstream" or with a small calibration table.

**Net read, not proven but meaningfully re-weighted**: `component2` being (Altera-compressed) FPGA
configuration data, relayed to the FPGA by the DSP rather than delivered any other way, is now a genuinely
live, arguably *better*-supported hypothesis than the original "DSP Data" naming guess — which rested only
on matching the version-info screen's field *order*, a weaker form of evidence than either point above.
**What would still need checking to move this from "plausible" to "confirmed"**: no Altera-specific sync
pattern/preamble was searched for (RBF format has no universal fixed magic bytes the way Xilinx bitstreams
do, so this may not be conclusively findable this way); Altera's proprietary compression scheme itself
hasn't been identified or decoded (a separate, real task if pursued — likely documented in Altera's own
configuration handbook, not the LZSS scheme already solved for the container); and the base "component0/1/2
= Front CPU/DSP Program/DSP Data" ordering was itself never independently confirmed field-by-field, so even
"is component2 really *the* thing labeled DSP Data" carries some prior uncertainty on top of this. Also
worth keeping in mind: this doesn't fully explain the version screen still tracking "FPGA" as its own
labeled field distinct from "DSP Data," which the update-compatibility checker (`FUN_200a94c8`) also
separately memcmps — if `component2` really is the FPGA image, that comparison's `+0xb0` field (currently
attributed to "FPGA") and whatever feeds `component2`'s own version tag would need to reconcile, not yet
checked.

**`component0` ("Front CPU") is a separate, freshly-opened thread of its own now** — see
[[front-panel-firmware]] for the full handoff (not started yet, just planned): unlike the DSP, real
Ghidra RL78 support exists as a community extension, not yet installed/tried in this environment.

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

**Correction, 2026-08-27** (see [[ic7300-signal-chain]] for full
reasoning): the service manual's circuit description states the FPGA
loads its configuration from an "external EEPROM" at power-on — standard
Altera passive-serial behavior. Combined with IC902's already-traced
`DCLK`/`DATA0` routing, **IC902 is more likely the FPGA's own config
flash, not "the DSP's own flash"** as this section originally assumed
from physical proximity alone. Where the DSP's own program actually
comes from is still open — plausibly no dedicated flash at all,
boot-loaded into DSP RAM by the main CPU instead, which would fit neatly
with the separate async write mechanism found below. The hardware
evidence for a genuinely separate write mechanism (immediately below)
still stands; only the "whose flash is IC902" attribution changes.

**Further checked against the actual schematics, 2026-08-27** — real
tension found and now leaning resolved: the block-level diagram (sheet
4/MAIN-2) draws IC902 spatially right next to IC901 (the DSP), which
initially looked like it might undercut the correction above. But the
*detailed* component-level schematics (sheets 10/MAIN-4 and 12/MAIN-6)
show IC902's `SPDI`/`SPDO`/`SPCK`/`SPCS` lines routing through to a
signal cluster on the FPGA's sheet labeled `DONE`/`STAT`/`CFG`/`SPCK` —
`DONE` and `STAT`(US) are textbook Altera/Intel passive-serial
configuration handshake signal names, not something a general DSP boot
flash would need. Given that, the block diagram's visual proximity to
IC901 reads as sheet/board-layout grouping rather than a functional
link, and the *detailed* schematic evidence (an actual named signal path
to FPGA config handshake pins) is the stronger source. **Leaning
confirmed: IC902 = FPGA config flash**, not the DSP's — though still
worth a clean visual confirmation from the user given the small text
this reading depends on. Full page-by-page notes in
[[ic7300-signal-chain]].

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
0. ~~"SX3765" identity question~~ — **CONFIRMED, 2026-08-27**: the
   IC-7300 service manual's **Display Unit** IC list (user-supplied
   screenshot) gives `IC501 = R5F104LCAFB`, marked **`SX-3765C-1`** —
   this is a Renesas RL78-family MCU, and it's the display/front-panel
   unit's own controller chip. Exact match for the schematic's
   partially-legible `(UX-3765C)` label guessed at in
   [[hardware-debug-access]] (that was a misread of `S` as `U`). This
   settles it: **every `"SX3765 Vx.xx-yyy"` string in the firmware is a
   compatibility/version reference to the display unit's MCU, not an
   embedded firmware image for it** — consistent with (and now fully
   explains) the dual-slot marker/version-table findings elsewhere in
   this file. See [[ic7300-hardware]] for the full display-unit IC list.
   The real IF-DSP is a separate, different chip entirely (TI
   TMS320C6745, `IC901` — also now in [[ic7300-hardware]] with its exact
   part number and paired boot flash `IC902`), so *its* firmware's
   location in the container is still the open question, not this one.
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

## `chunk4`/`chunk5-tail` actually consumed at runtime — traced with Ghidra live (9th session)

User asked whether the parts of the container beyond what's decompressed to RAM (i.e. `chunk4`/
`chunk5-tail`, per [[container-format]]'s model) have been checked for *code that references them*.
Answer going in: no, only their raw entropy/shape had been examined. With Ghidra back up, traced this
properly rather than continuing to guess from entropy alone.

**Direct address search — negative, but expected.** Computed the flash addresses `chunk4`/`chunk5-tail`
would occupy in both update slots (using `tools/icom_fw`'s own parser against the real `7300_142.dat`:
`chunk4` at file offset `0x25302c`/49,270 compressed bytes, `chunk5-tail` at `0x25e7d2`/1,468,695
compressed bytes → slot A: `0x18252c1c`/`0x1825ec92`, slot B: `0x18642c1c`/`0x1864ec92`). Searched all
four as raw hex literals across the whole of `body.bin`'s RAM (`0x20005000`-`0x20395b17`): **zero
matches**. Not surprising in hindsight — [[firmware-update]] already established the updater computes
every offset dynamically from the `size1..size7` header fields at runtime rather than embedding fixed
absolute addresses, the same reason a literal-address search never found D419/D422's consumer either
(see [[diode-matrix]]).

**Chunk1/chunk2/chunk3 — checked directly via Ghidra's own xref database**, since these three (unlike
chunk4/5) are already loaded as memory blocks in the live project (`0x18210000`/`0x18240000`/`0x18250000`
in slot A, mirrored at `0x18600000`/`0x18630000`/`0x18640000` in slot B — confirms `README.md`'s note that
these were imported alongside `body.bin` in an earlier session). `references_to` on each:
- `chunk1`/`chunk2` (the two fonts): exactly one reference each, both inside `select_active_slot_resources`
  (`0x20062c64`, named in a prior session) — a **runtime** resource-locator (checks the `"SX3765
  V1.00-003"` marker, same repurposed tag used elsewhere, to pick slot A vs B) that hands back
  `{size, data pointer}` for both fonts. Not part of the update-write path at all — this runs whenever the
  running firmware needs a font, any time after boot.
- `chunk3`: **zero references anywhere.** Genuinely unread by any code Ghidra has resolved, in this
  version.

**The real find: `firmware_update_main`'s "3 more chunks" step doesn't touch chunk1/2/3 at all — its
offsets land inside chunk4+chunk5's byte range instead.** Decompiled `firmware_update_main`
(`0x20025ae4`) in full. `size1` (2,436,080 for v142) bounds the main "everything up to and including
chunk3" bulk flash write (`size1 - 0x10000` bytes) — chunk1/2/3 are flashed as part of *that* single write,
confirmed by the arithmetic (`size1` lands almost exactly at chunk3's real end, off by the same small
header-alignment constant seen elsewhere). The subsequent "3 more optional chunks" step reads `size2`
through `size7` (the six header fields *after* `size1`) to build **three separate sub-components**,
chained as `offset[n+1] = offset[n] + size[2n+1] + 0x10`:

| # | computed file offset | length used for MD5-verify read | length passed to the writer |
|---|---|---|---|
| 0 | `size1+0x3c` = 2,436,140 (16 B into `chunk4`) | `size2` = 97,862 | `size3` = 163,592 |
| 1 | 2,534,018 (inside `chunk5-tail`) | `size4` = 721,836 | `size5` = 720,648 |
| 2 | 3,255,870 (inside `chunk5-tail`) | `size6` = 698,201 | `size7` = 859,412 |

These three spans run from 2,436,140 to ~3,954,071 — essentially **all** of what `container.py` currently
models as one `chunk4` LZSS stream plus one `chunk5-tail` LZSS-to-EOF stream, just split differently (not
aligned to either chunk's LZSS boundaries at all). **This means `chunk4`/`chunk5-tail` are not inert data
— the real firmware's own updater explicitly reads, per-component MD5-verifies, and processes all three
sub-components, every update.** `container.py`'s byte-accounting is still correct (0 unaccounted bytes)
but its `chunk4`/`chunk5` split doesn't reflect how the real code understands this region — worth revising
once this is fully mapped.

**Where the data actually goes — traced 4 levels deep, stops short of a firm answer.**

**Correction (11th session) to this section as first written**: re-examined the raw instructions at the
call site (`0x20026080`-`0x20026090`) rather than trusting a shallow raw-byte read of `DAT_200264b4`.
There's **one more level of indirection** than originally stated: `ldr r0,[0x200264b4]` loads a *single*
pointer value (`0x2018d224`) from that literal-pool slot, then `ldr r1,[r0, r5, lsl #2]` dereferences
*that* address, indexed by the loop counter, to get the real per-iteration destination — i.e. the true
table of 3 destination pointers lives at `0x2018d224`, not at `0x200264b4` itself (the three consecutive
words `0x2018d224`/`0x203bb260`/`0x203bcea0` originally read *at* `0x200264b4` were mostly coincidental
adjacent data, not the real per-component destinations — only the first of those three, is actually used,
as the pointer to the real table). Reading `0x2018d224` directly in the static image gives `{0x0,
0x00050000, 0x00100000, 0x0}` — values far too small to be valid RAM addresses in this SoC's memory map.
**Most likely explanation**: this table is populated at runtime by some initialization step before
`firmware_update_main` ever runs, so the static (flashed) image just shows placeholder/zeroed content
here — meaning **we can't compute a fixed destination address for this data via static analysis alone**,
unlike `chunk1`/`chunk2`/`chunk3` (which do land at fixed, statically-computable flash addresses, already
checked via `references_to`). This is exactly the kind of thing live/JTAG verification would resolve
directly (watch what gets written to `0x2018d224` during boot, or single-step an actual update) rather
than continuing to guess from the static image.

**Chased "is there a writer?" harder (16th session)** — user asked directly whether anything updates this
address or something nearby, and whether that had actually been checked. It had, but only via Ghidra's
resolved xref database (one hit, a reader). Went further this time:
- **Raw hex search for the literal 4-byte value `0x2018d224` across the entire ~3.7 MB image**: exactly
  **one** occurrence, at `0x200264b4` (the reader already found) — rules out a missed/broken xref, the kind
  of gap this project has hit before with the ARM/Thumb disassembly-context bug. If a writer exists, it
  does not reference this address as a direct literal anywhere in `body.bin`.
- **Read the surrounding memory directly** rather than just the 16 bytes at the exact address: `0x2018d224`
  sits right at the boundary of a distinct, unrelated-looking table of function pointers and short tag
  bytes (`0x2018d1e0`-`0x2018d223`) that stops exactly there, with different, sparse-looking small-integer
  content continuing after `0x2018d230`. Doesn't change the reading of the specific 12 bytes used as the
  destination array (the code's own indexing confirms exactly `0x2018d224`/`+4`/`+8` are what's read), but
  confirms this sits in a largely un-typed data region Ghidra hasn't structured — consistent with a writer
  needing register-computed addressing that a literal search can never find, if one exists at all.
- **Checked for a bulk BSS-clear/init loop that might cover this address without ever encoding it as a
  literal** (the mechanism that would explain "zeroed in the static image, real value written generically
  at boot"). Traced the actual pre-kernel boot path precisely: `reset_handler` → `FUN_2002b878` (hardware
  register setup, not memory clearing) → `thunk_FUN_200b8690` → `FUN_200b848c`/`FUN_200b8630`. **Both turned
  out to be GIC (interrupt controller) initialization** — per-interrupt-line priority/target/enable setup
  matching the already-confirmed `0xE8202xxx` GIC registers (see [[kernel-rtos]]) — not a generic
  memory-clear routine at all. No bulk-clear loop with address bounds to check this table against was found
  on this path.
- **Worth flagging, not asserted as fact**: `FUN_20025044`'s call chain masks its "destination" argument to
  24 bits before tagging it (`FUN_200b3040`: `(param_1 & 0xffffff) + 0xe2000000`) — 24 bits matches this
  SoC's flash address space size, and the table's actual static values (`0`, `0x50000`, `0x100000`) are
  round enough to plausibly be **flash byte offsets** rather than RAM pointers. If so, no runtime writer
  would be needed at all — these could be genuine compile-time constants, and the reason none was found is
  that there isn't one. This isn't confirmed (the numbers don't obviously line up with the
  already-established slot-A/slot-B flash layout either), but it's a real alternative to "runtime-populated,
  needs JTAG" worth weighing against that assumption rather than defaulting to it.

The per-component writer, `FUN_20025044`, threads through `FUN_200b2fc8`/`FUN_200b3040` — which pack each
data byte into a tagged 32-bit word (`0xb0000000`-`0xb7000000`/`0xe2000000` in the top byte) and push it
via `FUN_200b10a0` into a **generic ring-buffer queue** (87 slots × 16 bytes) — then `FUN_200b0f68` (the
queue's drain/consumer side) sets an RTOS event-flag bit (`FUN_200b8308(0xa1)`) to wake whatever task
actually processes the queue.

**Correction (22nd session) — `FUN_200b0f68` does more than "just queue mechanics", but still not the
`0xb0`/`0xe2` tags specifically.** Picked up while chasing the RSPI2 lead in [[ic7300-signal-chain]]:
`FUN_200b0f68` actually has a real `switch` on each entry's tag byte with 5 concrete cases (`0`: signal
event `0xa1` and stop; `1`: a Port-8 reconfiguration gate; `2`: the SSIF/DMAC audio-transfer function;
`3`: a genuine RSPI2 SPI-transmit function, confirmed real and active — see [[ic7300-signal-chain]]; `4`:
a front-panel/SCIF3-adjacent function). **None of these 5 cases match `chunk4`/`chunk5`'s own tag range
(`0xb0`-`0xb7`/`0xe2`)** — so `chunk4`/`chunk5` entries, if pushed through this exact queue, hit none of
them and fall through unhandled by this function. This is a real, useful negative result: it rules out
RSPI2, the SSIF/audio-DMA path, and the front-panel UART as `chunk4`/`chunk5`'s consumer, without
identifying what actually is. **The consumer for the `0xb0`/`0xe2` tag range specifically is still not
identified** — whether this is a companion-chip command/data link or something else remains open, now with
three concrete candidates ruled out rather than zero.

**Concrete next step, if this thread is picked back up**: find the task that waits on the event-flag group
at `DAT_200b86a0+0x114` bit 1 and consumes this specific ring buffer — that task's own interpretation of
the `0xb0`/`0xe2`-tagged words is what would finally settle whether `chunk4`/`chunk5-tail` feed a
companion processor or something else entirely.

## `chunk4.bin` imported into Ghidra directly — confirmed NOT plain ARM code (10th session)

User imported `chunk4.bin` (decompressed, `scratch/unpacked/142/chunk4.bin`) as its own Ghidra program,
base `0x0`, ARM (forced by the import dialog, matching `body.bin`'s architecture). Ran auto-analysis:
**zero functions, zero disassembled instructions anywhere in the file** — not an entropy inference this
time, an empirical result from Ghidra's own ARM analyzer failing to find a single valid instruction
sequence. Rules out plain ARM code at this base address; doesn't rule out a different base (unlikely to
matter — see below) or a different architecture.

**Byte-level shape, checked directly**: entropy climbs smoothly from ~0.5 bits/byte at offset `0x0` to a
~6.7-6.9 bits/byte plateau by offset `0x1200`, then stays there for the rest of the file (measured in
512-byte blocks). No sharp header/payload boundary — a gradual ramp, not a jump. Scanned for common
format magic bytes (gzip, zlib, PNG, BMP, RIFF) at the start of the file: no genuine match (the two
apparent hits are deep inside the high-entropy region, coincidental, not header-position). This shape —
smooth statistical ramp rather than a sharp structural boundary — argues against this being a nested
compressed container *or* executable code for any architecture (a real vector table or reset handler
would show sharp structure immediately at offset 0, not a gradual entropy climb); more consistent with a
**data table whose values grow in magnitude/density** — a coefficient/calibration table is the most
plausible read, though nothing confirms which one specifically.

**Recommendation if this is picked up again**: given the shape argues against executable code generally
(not just ARM specifically), guessing further architectures (RL78, TMS320C6x) is unlikely to be
productive by itself — would want an actual reason to expect a specific format first. `chunk5-tail`
(the larger, originally-flagged candidate) hasn't had this same direct empirical check yet — worth doing
before drawing conclusions about the whole "second processor" hypothesis from `chunk4` alone.

## `chunk5_tail.bin` imported too — same empirical result, same conclusion (10th session, continued)

Same treatment: imported as its own program (base `0x0`, ARM), ran auto-analysis. **Zero functions, zero
disassembled instructions** — checked the listing at the start, the middle (`0xc0000`), and confirmed via
`functions.list` after analysis completed (fast, consistent with "nothing to disassemble" rather than
still-running, matching `chunk4`'s behavior exactly). Same conclusion as `chunk4`: not plain ARM code.

**Byte-level shape**: starts at a notably *higher* baseline than `chunk4` (~5.19 bits/byte at offset `0`,
vs `chunk4`'s ~0.5), climbs to a ~7.0-7.1 bits/byte plateau by ~offset `0x2000` (16 KB in), then holds
there with only minor dips (down to ~6.0-6.3) in the last few KB before EOF. Checked for sharp jumps
(>1.5 bits/byte between adjacent 2 KB blocks) across the *entire* 1.68 MB file: **zero** — this is one
continuous statistical regime start to finish, not several concatenated sub-files of different shapes.
No format magic bytes (gzip/zlib/PNG/BMP/RIFF/ZIP/ELF) at a header position either.

**Updated read on the "second processor firmware" hypothesis**: this makes the "plain, directly-
executable companion-chip image" version of the hypothesis noticeably less likely, not just for ARM but
in general — the same "real code shows sharp structure, this shows smooth statistical ramps" reasoning
applies, and at a higher, more strongly random-looking plateau than `chunk4`. Doesn't kill the broader
"second processor" idea outright (a *compressed or encrypted* image bound for another chip would still
look exactly like this from the outside — high, uniform entropy, no visible header — and the already-
confirmed outer LZSS layer decoding cleanly to an exact EOF is real, structured framing, not noise), but
the specific sub-hypothesis "decompress once and you'll find recognizable machine code" is now
empirically checked and doesn't hold for either `chunk4` or `chunk5-tail`, in this analysis at least. The
`FUN_20025044`/ring-buffer/event-flag delivery mechanism traced earlier in this file remains the more
promising thread if this is picked up again — finding *that* consumer task would settle the question on
firmer ground than further format-guessing on the bytes themselves.

## Real DSP/FPGA hardware links found — see [[ic7300-signal-chain]] (12th session)

User derived a full CPU↔DSP↔FPGA pin mapping from the schematics; cross-referenced against the RZ/A1H
manual and `body.bin`, this confirmed a **real, actively-used digital audio link between the main CPU and
the DSP** (RZ/A1H's SSIF0+SSIF1 peripherals, DMA-driven) and a **candidate SPI control link to the FPGA**
(RSPI channel 2) that's referenced only in generic init tables so far, not a dedicated driver. Full
mapping table and register-search results in [[ic7300-signal-chain]]'s "DSP/FPGA control signal mapping"
section — don't duplicate it here, but do check it before continuing this thread: the open task of finding
`FUN_20025044`'s ring-buffer consumer (above) should be checked against whether it ultimately calls into
the RSPI2 code found there, which would finally connect these two open questions.

## Strong string/UI evidence the "3 optional chunks" are Front CPU + DSP Data + DSP Program (13th session)

User's hunch: find the function that downloads a firmware update to the front panel, and it might explain
the mystery chunks. Didn't find that exact function, but found something almost as good — direct textual
proof of what the update mechanism actually tracks as separate components.

**The version-info screen lists five independently-versioned components**, found via string dump around
`0x2035e3f4`-`0x2035e420`: `FPGA:`, `Main CPU:`, `Front CPU:`, `DSP Data:`, `DSP Program:`. Nearby, two
distinct update-progress messages: `"Updating DSP/FPGA firmware."` (`0x2035e8f4`) and
`"Updating MAIN CPU firmware."` (`0x2035e910`) — confirming the update UI treats "DSP/FPGA" as one phase,
separate from the main CPU body/font/chunk3 write already traced in [[firmware-update]].

**Found the version-compatibility-check function itself**: `FUN_200a94c8` (screen/case `0x14` in the big
UI screen dispatcher `FUN_20080380`) directly `memcmp`s **five 4-byte version fields**
(`iVar2+0xa0`/`+0xa4`/`+0xa8`/`+0xac`/`+0xb0`, read from the parsed update file) against five stored
"currently installed" values (`DAT_200a9bb0+4`/`+0x10`/`+0x1c`/`+0x28`/`+0x34`, each a 12-byte record) —
and renders a mismatch warning naming each component (via `FUN_200ac6e0`) if any differ. This is a very
close structural match to the version-info screen's five labels above — strong (if not yet 100% confirmed
field-for-field) evidence the update container carries five separate version tags, one per
independently-tracked component, not just the two ("main body" and "DSP/FPGA blob") assumed so far.

**How this fits the already-traced mechanism**: [[diode-matrix]]/this file's earlier sessions found
`firmware_update_main` writes the main body + fonts + chunk3 in one bulk flash write (bounded by `size1`),
then separately processes **exactly three** more components via `FUN_20025044` (destinations still
unresolved — see the correction above) whose file offsets land inside `chunk4`+`chunk5-tail`'s byte range.
**Three non-main-CPU components** (`FUN_20025044`'s loop) lining up against **four** non-main-CPU labels
on the version screen (`FPGA`, `Front CPU`, `DSP Data`, `DSP Program`) is consistent with: FPGA's actual
bitstream comes from its own dedicated EEPROM (`IC902`, already established via the service manual — not
through this update file's chunk mechanism at all) while still getting a version *tag* checked here for
compatibility, and the three `FUN_20025044` components are **Front CPU firmware, DSP Data, and DSP
Program** — matching the "3 extra chunks" exactly. Plausible, well-supported, but **not yet confirmed
field-for-field** — would need to trace exactly which update-file byte offsets feed
`FUN_200a94c8`'s `iVar2+0xa0..0xb0` and check whether they line up with `firmware_update_main`'s already-
mapped `local_9c[]` offsets for the three components.

**Confirms the front-panel link is real infrastructure for this**, independently: [[ic7300-signal-chain]]
found a genuine, active SCIF3 UART driver for `LRXD`/`LTDX` (front-panel MCU link) in the same session that
led here — a real transport for "Front CPU" firmware to travel over, consistent with this hypothesis.

**Next steps, in order**: (1) trace backward from `FUN_200a94c8`'s `iVar2` parameter to find which SD-card
file offset it's read from, and check against `firmware_update_main`'s `local_9c[]` chunk offsets — this
would either confirm or refute the "3 chunks = Front CPU + DSP Data + DSP Program" mapping directly;
(2) if confirmed, re-examine `FUN_20025044`'s RAM-buffer destinations (once the runtime-populated pointer
table issue from the correction above is resolved, ideally via JTAG) to see which component goes out over
SCIF3 (front panel) vs. whatever channel reaches the DSP.

## The exact 5-field-to-component mapping, confirmed (22nd session) — the file-offset trace itself hits a real wall

Followed up on next step (1) above. **The component-to-offset mapping is now precisely nailed down**,
resolving the one part of the earlier hypothesis that was still just "a very close structural match":
read `DAT_200a9bb0`'s actual backing array in RAM (five `{flag, string_ptr, length}` records, 12 bytes
each) and dereferenced every string pointer directly. Result — `FUN_200a94c8`'s five compared 4-byte
fields map to components in this exact order:

| `iVar2` offset | Component (confirmed via live string read) |
|---|---|
| `+0xa0` | `Main CPU:` |
| `+0xa4` | `Front CPU:` |
| `+0xa8` | `DSP Program:` |
| `+0xac` | `DSP Data:` |
| `+0xb0` | `FPGA:` |

This directly refines the hypothesis: since `Main CPU` (the primary body, already handled by
`firmware_update_main`'s main bulk write) and `FPGA` (its own dedicated config EEPROM, per the service
manual) are the two components *not* among the "3 extra chunks", the three that remain —
`Front CPU` (`+0xa4`), `DSP Program` (`+0xa8`), `DSP Data` (`+0xac`) — sit at **consecutive, ascending
offsets**, the same natural ordering as `firmware_update_main`'s three sequentially-processed chunk
indices (`0`/`1`/`2`). The straightforward reading, if the two orderings correspond 1:1 (not yet proven,
see below): **chunk 0 = Front CPU, chunk 1 = DSP Program, chunk 2 = DSP Data** — a specific assignment,
not just an unordered set of three, and a genuinely stronger result than this file had before.

**What's still open, and why**: didn't manage to trace `iVar2` (`DAT_200a9ba4`, resolves to the fixed RAM
address `0x203ff76c`) back to an SD-card file offset this session — hit a real wall, not a shortcut taken:
- `references_to` on `0x200a9ba4` (the literal-pool slot holding this address) finds only the two reads
  already inside `FUN_200a94c8` itself — no writer anywhere in Ghidra's xref database.
- The raw value `0x203ff76c` recurs as a 4-byte match roughly **30 times** throughout `body.bin` — but an
  `objdump` grep for it being *loaded* via `pc`-relative addressing (`@ 0x203ff76c`) anywhere in the image
  returns **zero hits**. The ~30 matches are coincidental/unrelated 4-byte collisions or (more likely)
  private literal-pool copies used by *other*, unrelated code that happen to store this same address for
  a different purpose — not confirmed to be real accesses to this specific struct.
- Chased one promising-looking lead — a literal match on `0x203ff76c + 0x9c` (`0x203ff808`, the exact RAM
  address of the compared fields) — to two call sites (`FUN_20043a08`, `FUN_2008cff8`). **Both turned out
  to be false leads**: this codebase uses a `+0x9c`-relative-to-base convention very generically across
  many *unrelated* per-screen "candidate vs. current settings" structures (confirmed by reading
  `FUN_2008cff8` in full — a ~10 KB UI settings-comparison function handling several *other* screens'
  compare-and-highlight logic, using the identical `base+0x9c` idiom for entirely different settings).
  Recording this so a future session doesn't re-chase the same false lead.
- Checked all ~23 call sites of the generic "read N bytes from open file" primitive (`FUN_200bc6a4`)
  outside `firmware_update_main` itself for one passing a destination resolving to `0x203ff76c` — none
  found among the several traced by hand; the remainder are generic seek/read wrapper functions one level
  removed from their own callers, and fully enumerating all of them was not pursued to completion this
  session (diminishing returns without a faster way to bulk-check destination arguments).

**Assessment**: this now sits in the same category as the `chunk4`/`chunk5` destination table and several
task-descriptor questions elsewhere in this project — a runtime-populated global whose *writer* isn't
findable through the literal-pool/xref techniques that have worked well elsewhere, most likely because
it's filled via a bulk read whose destination is computed/passed through several layers of generic file-IO
wrapper functions rather than referenced as a direct literal. The component **identity** mapping above is
solid and new; the **byte-offset** confirmation the hypothesis ultimately needs is not — flagged accurately
rather than asserted. Live JTAG (watch what gets written to `0x203ff76c` while browsing the version-check
screen with a real update SD card inserted) would resolve this quickly and directly.

## `SCIF5` identified: the real physical DSP link, both for firmware update and live control (2026-08-29)

New target picked up: DSP interaction/firmware-update timing, specifically hoping to find where `DRESD`
gets released after boot. Didn't close that out, but found the actual **physical transport** the "3 extra
chunks" mechanism uses — a genuinely new discovery, not previously catalogued in this project's serial-link
inventory (SCIF0=CI-V, SCIF1=service-mode, SCIF3=front-panel).

**Full chain traced, `FUN_20025044`/`chunk_transport_send_reload_cmd` down to real hardware**:

```
FUN_20025044 (per-chunk producer) / chunk_transport_send_reload_cmd (reload trigger, cmd 0x87)
  -> FUN_200b10a0 (ring-buffer push -- 32-bit words tagged by top nibble: 0xB=3-byte payload
     triplet + channel, 0xE=total-size header)
  -> ring buffer drained by MTU2 channel 4's compare-match interrupt (GIC IRQ 161 = TGI4C per the
     RZ/A1H manual; event ID 0xa1 in this firmware's own registration scheme, confirmed via
     `register_event_handler(0xa1, FUN_200b2a14)`)
  -> FUN_200b2a14 (pop one ring-buffer entry) -> FUN_200b2440 (bit-reverses each of the 4
     payload bytes, then writes them one at a time to 0xE800980C)
  -> **SCFTDR_5 -- SCIF5's transmit FIFO data register** (confirmed against the RZ/A1H manual:
     SCSMR_5 base 0xE8009800, SCFTDR_5 at +0xC)
```

`DAT_200b2b28` (polled throughout this chain for a ready/done flag) = `0xFCFF022D` = `TSR_4`, MTU2 channel
4's own status register — the whole thing is a timer-paced software transmit loop over SCIF5, not a plain
blocking UART write.

**Confirms the update-timing question directly**: `FUN_20025044` and `chunk_transport_send_reload_cmd` each
have exactly **one caller**, both inside `firmware_update_main`. Nothing else in the traced firmware pushes
onto this transport for a chunk transfer. **DSP Program/DSP Data get sent to the DSP live, during the
update itself, over SCIF5 — not deferred to a post-restart check.**

**SCIF5's driver is armed on every boot, not just during updates.** `FUN_200b2bb0` (SCIF5 init: registers
event handlers `0xa1`/`0x9f`/`0xf1`/`0xf2`/`0xf3`, configures the MTU2 timer) is called unconditionally from
`cold_boot_hw_init`, right after `port_bulk_gpio_init_pass2()` (the `DRESD`-low write). Its only caller —
confirmed via `references_to`, this isn't a guess.

**This is not just an update channel — it carries live, ongoing control traffic.** `FUN_200b11c0`, running
regularly (looks timer/interrupt-driven, same family), continuously diffs a "current" parameter-state
struct against a "last-sent-to-DSP" shadow copy and pushes any changed field over the exact same
`FUN_200b10a0` ring buffer. This reads as a real-time parameter-sync bus (filter/mode/AGC-shaped settings
pushed to the DSP as the user changes them), not a one-shot mechanism — meaning **the DSP must be an
actively-running core receiving this traffic in normal operation**, not permanently held in reset.

**Boot-time DSP handshake, also over SCIF5**: right after `FUN_200b2bb0`/`FUN_200b48e4` (an `HSK1` wait,
same `PPR8` bit 9 signal already confirmed elsewhere), `FUN_200b5aa0` sends two specific command words
(`0x100007FF` and one more from `DAT_200b6370`) via the same `FUN_200b2440` primitive and waits for an ack
flag after each — reads as a real boot-time identification/handshake exchange with the DSP, all inside
`cold_boot_hw_init`'s unconditional call sequence.

**Open tension, not resolved this session**: this directly conflicts with the earlier, already-exhaustive
finding that `DRESD` (`P2_6`) is driven LOW once at boot by `port_bulk_gpio_init_pass2` and never touched
again anywhere in `body.bin` (see [[ic7300-signal-chain]]) — a DSP core in hardware reset cannot be
receiving live SCIF5 traffic. Checked and ruled out this session:
- **No inverter on the net** (user's schematic check, page 64/66): a 47kΩ pull-up to 3.3V on `DRESD`, a
  1kΩ series resistor (`R902`) straight to DSP pin 146 (`\RESET`) with the service-programming connector
  `J901` tapped off the same node, and a separate branch through a resistor array (`R927`, ~22Ω) to a
  47kΩ-pulldown-terminated `WP`/`DQ2` pin on `IC902` (a flash chip on this net — **note**: an earlier
  session's schematic read concluded `IC902` is the *FPGA's* config flash, not the DSP's; this reappearance
  on the `DRESD` net is worth reconciling, not asserted as a contradiction yet). No active component
  anywhere in this path — polarity is not flipped, `CPU LOW` really does mean `DSP pin sees LOW`.
- **Considered and checked: release via direction/tri-state instead of a data-register write** (the 47kΓ
  pull-up means the CPU need only stop *driving* the pin, not drive it HIGH, for the DSP to see it float
  back up) — would show up as a `PM2` (port 2 direction register, `0xFCFE3308`) write instead of a `P2`
  data-register write, a literal the earlier P2-only sweep would have missed entirely. Checked: the *only*
  literal reference to the whole `PMn` block base (`0xFCFE3300`) anywhere in `body.bin` is `FUN_2005fdb4`,
  and it touches `P2` bits 8-11 only — nowhere near bit 6. Ruled out.
- Checked the remaining not-yet-examined calls immediately around `cold_boot_hw_init`'s SCIF5-init sequence
  (`FUN_200b47f0`, `FUN_200b4800`, `FUN_2002af80`, `FUN_2002aed8`, `FUN_2007ed9c`, `FUN_2007ede0`) — all
  turned out to be RAM-state clears, a `scif3_driver_init()` call (front panel, unrelated), or ITRON
  semaphore-creation calls. None touch port registers.

**Where this leaves it**: either (a) the release genuinely isn't in the statically-traced call graph — a
real candidate for the same "computed/indirect, not a literal" blind spot that's beaten static analysis
elsewhere in this project (`chunk4`/`chunk5`'s destination table, `0x203ff76c`'s writer), or (b) `DRESD`'s
role needs re-examination given the `IC902`/flash-`WP` branch the schematic just revealed — worth checking
whether `DRESD` is better understood as a combined "hold DSP in reset **and** write-protect its companion
flash during programming" line, in which case the DSP might come out of reset far earlier/differently than
assumed, or via a mechanism that doesn't route through `P2` at all. Not guessed further; flagged for either
a fresh angle or live JTAG (watch `P2`/`PPR2` bit 6 directly while the radio boots).

## The "chunk 0 = Front CPU" label is now actively doubtful — no per-chunk SCIF3 path exists (2026-08-29)

Picked up the "next steps (2)" item from the 13th session's section above directly: "re-examine
`FUN_20025044`'s RAM-buffer destinations ... to see which component goes out over SCIF3 (front panel) vs.
whatever channel reaches the DSP." Answer, now confirmed at the code level rather than inferred: **none of
them go out over SCIF3. All three chunk indices — 0, 1, and 2 — are transported by the exact same code
path, and that path is 100% SCIF5.**

Traced the full call chain under `chunk_transport_send_data` (`FUN_20025044`) with no branch on chunk index
anywhere in it:
- `chunk_transport_send_data` → `dsp_page_transfer_verify` — its own header comment (this session) already
  frames this as a page-write acknowledgment protocol addressed *at the DSP*: it validates a checksum and
  an echoed-back destination address against the DSP's reply, and calls `scif5_send_and_wait_reply()`
  directly.
- The ring-buffer push helpers (`FUN_200b3040`, `FUN_200b304c`) both call `scif5_ring_push_word()` directly,
  tagging words with DSP command-class nibbles (`0xe2000000`/`0xe3000000`) — not a peripheral selector, a
  DSP-side command tag.
- `chunk_transport_send_reload_cmd` (the same-transport reload trigger, cmd `0x87`, sent specifically when
  chunk 0 or chunk 2 changed) uses the identical `scif5_ring_push_word`/handshake primitives.

No conditional anywhere in this chain picks a different UART for chunk index 0. The per-chunk-index value
threaded through as `param_2`/`page_addr` (from `DAT_200264b4[index]`, the runtime-populated pointer table
already flagged as unresolvable via static analysis in the correction above) is used purely as a
destination address *within whatever the DSP-side protocol addresses* — never as a code-path selector.

**This retracts the load-bearing part of the 13th session's "confirms the front-panel link is real
infrastructure for this" reasoning** (the paragraph right after the version-screen evidence): the existence
of a real SCIF3 driver elsewhere in the firmware was never actually wired to this transport — it was a
plausibility argument, not a traced connection, and it doesn't survive contact with the actual code. The
5-field version-label evidence (`Main CPU`/`Front CPU`/`DSP Program`/`DSP Data`/`FPGA`, and the ordering
argument built on it) is unaffected by this — that part stands on its own string/struct evidence — but the
assumption that "Front CPU" therefore travels to `IC501` over its own known link is now actively
contradicted, not just unconfirmed.

**Converges with an independent line of evidence from the same day**: the front-panel (RL78) firmware
thread ([[front-panel-firmware]]) found that `front_cpu.bin` (component0's decompressed payload) is
~134 KB of actual content — over 4x `R5F104LCAFB`'s Renesas-confirmed 32 KB code flash — and that its
byte-entropy profile sits much closer to `dsp_program.bin`'s dense, uniform VLIW-code profile than to
confirmed-real ARM code in `body.bin`. Two independently-derived findings (this session, different
methods — one from the update-mechanism's code, one from raw forensics on the extracted file) now point the
same direction.

**Best-supported reading now**: `component0` is very likely **not** Front CPU firmware at all — more
plausibly a third piece of DSP-side data, e.g. a second partition of the DSP's own external SPI boot flash
(`IC902`, already established as something the DSP reprograms on the main CPU's behalf during updates).
`R5F104LCAFB`'s 64-pin package almost certainly has no external memory bus to execute from such a shared
flash directly (RL78/G14's external-bus-capable variants are the larger pin-count packages), so a
"front panel boots from the same shared flash" rescue of the original hypothesis looks unlikely too —
not chased further this session, flagged as worth a datasheet check if it matters later.

**Genuinely open**: what `component0` actually is, if not Front CPU firmware. Not established this session.
The `FUN_200a94c8` version-field/label evidence still needs reconciling with this — either the
label-to-chunk-index correspondence itself is wrong (maybe the natural "ascending offset order" assumption
between the two independent orderings doesn't hold), or "Front CPU" firmware really does exist somewhere in
this update file under a different mechanism entirely, not part of the "3 extra chunks" loop at all. Worth
a fresh look before trusting either surviving half of the original hypothesis.

## Handoff: DSP comms/firmware thread, continuing in a new session (2026-08-29)

Picking this thread back up — start here, don't re-derive the sections above. Quick orientation first, then
concrete next steps in priority order.

**Confirmed, solid, don't re-check**:
- `SCIF5` (`0xE8009800`) is the real DSP link. Full chain: `chunk_transport_send_data` /
  `chunk_transport_send_reload_cmd` (firmware-update chunk transfer, both single-caller inside
  `firmware_update_main`) and `dsp_param_sync_tick` (continuous live parameter push, normal operation) all
  feed `scif5_ring_push_word`, drained by MTU2 ch.4's timer interrupt (event `0xa1` →
  `scif5_ring_pop_and_send` → `scif5_bitrev_transmit_word` → `SCFTDR_5`).
- `scif5_dsp_link_driver_init` runs unconditionally on every boot (`cold_boot_hw_init`'s only call to it),
  right after `port_bulk_gpio_init_pass2` (the `DRESD`-low write). Registers events `0xa1`/`0x9f`/`0xf1`/
  `0xf2`/`0xf3` — **only `0xa1` and `0x9f` have been traced** (both TX-side: pop-and-send /
  underrun-handler). `0xf1`/`0xf2`/`0xf3`'s handlers were never looked at.
- `dsp_boot_handshake` sends exactly 2 command words to the DSP over `SCIF5` right after driver init, at
  every boot, waiting for an ack after each — contents (`0x100007FF` and one more from `DAT_200b6370`,
  literal pool near `0x200b6368`) were **not decoded**, just observed to exist.
- `IC902` = the DSP's own SPI0 boot flash (user's direct pin trace to `IC901` `BOOT[4:0]`/SPI0 pins — solid,
  don't second-guess without new schematic evidence). Best current model: the DSP self-boots from it, and
  reprograms it itself in response to `SCIF5` commands during a firmware update.
- **Ruled out for `DRESD` release** (don't re-check): net inverters (none — user's schematic read, page
  64/66, purely resistive: 47kΩ pull-up, 1kΩ series to DSP pin 146, 22Ω array + 47kΩ pulldown to `IC902`'s
  `WP`); release via `PM2` direction/tri-state (only literal `PMn`-block reference in all of `body.bin` is
  `FUN_2005fdb4`, touches `P2` bits 8-11 only, nowhere near bit 6); every function called immediately around
  `scif5_dsp_link_driver_init` in `cold_boot_hw_init` (`FUN_200b47f0`, `FUN_200b4800`, `FUN_2002af80`,
  `FUN_2002aed8`, `FUN_2007ed9c`, `FUN_2007ede0` — all checked, none touch port registers).

**Next steps, roughly in order of promise**:

1. ~~**`0xf1`/`0xf2`/`0xf3` event handlers, never traced.**~~ — **RESOLVED, next session, see "SCIF5 RX path
   closed" section below.** All three route to the same function (`scif5_rx_isr`) — SCIF5's real RX path,
   feeding a genuine ack/retry state machine, not just a boot-complete flag.
2. **The other ~30 callers of `scif5_ring_push_word`** (`references_to` on it) beyond the ones already
   traced — mostly clustered `0x200b1194`-`0x200b14d0`, plus two more at `0x200b54c8`/`0x200b54e4` never
   even opened. **Partially advanced, next session, see "SCIF5 command API, first real look" below**: found
   the actual synchronous command/reply API (`scif5_send_and_wait_reply`, 14 call sites) sitting alongside
   this ring-buffer path, and confirmed `FUN_200b1540`'s command/ack lookup table — now renamed
   `dsp_cmd_table_init` — is the one-time boot-time builder of that table (values `0x106`, `0x1000000`,
   `0x24000000`, `0x25000000`, `0x41000000`-`0x4f000000`, `0x61000000`/`62000000`/`6b000000`, called from
   `cold_boot_hw_init` right after `scif5_dsp_link_driver_init`), not a response-code table. Still open:
   individually characterizing the ~14 `scif5_send_and_wait_reply` callers (mode/filter/AGC-shaped pushes,
   per the working hypothesis) and the original ~30 `scif5_ring_push_word` callers (a separate, slower,
   MTU2-paced TX path — normal-operation `dsp_param_sync_tick` traffic, not this synchronous command API).
3. **`DRESD` release, new angle**: everything checked so far was inside `body.bin`'s own `cold_boot_hw_init`
   and immediate neighborhood. Not yet checked: the **boot-ROM/`base.dat` stage**, which runs *before*
   `body.bin`'s entry point (see `notes/base-loader.md`) — if `DRESD` is released exactly once, very early,
   this earlier stage (not `cold_boot_hw_init`) is a real candidate that hasn't been looked at from this
   specific angle.
4. ~~**`SCIF5`'s physical `TxD5`/`RxD5` pin**~~ — **RESOLVED, next session.** Found by decompiling
   `scif5_dsp_link_driver_init` itself rather than continuing the schematic-candidate search: it directly
   configures `P8_0`/`P8_1`/`P8_2`'s `PFCn`/`PFCEn`/`PFCAEn`/`PMCn` registers (function code 3, `PMCn`
   genuinely enabled on all three) — these are the same 3 pins the schematic calls `DSPCK`/`DSPR`/`DSPX`
   (previously read as a second DSP McASP1 audio link; that reading is now very likely wrong — see
   `notes/ic7300-signal-chain.md`'s correction). Same method also resolved `SCIF1`'s long-open pin
   (`P6_13`+`P7_12`, split across two ports) — see `notes/kernel-rtos.md`'s "SCIF1 and SCIF5 physical pins
   resolved via each driver's own port-mux code" section for the full derivation, including a previously-
   missing register family (`PFCAEn`, `PORTn_base+0xA00`) this uncovered.
5. **Decode `dsp_boot_handshake`'s 2 command words** — lower priority than #1, but if #1's RX-side tracing
   doesn't pan out, manually decoding what `0x100007FF` means (top-byte command class `0x10`, distinct from
   the `0xB`/`0xE` classes already decoded for chunk transfer) against the same bit-reversal
   `scif5_bitrev_transmit_word` applies might reveal a recognizable TI HPI/boot-protocol command.

Once JTAG hardware arrives, this whole thread (watching `SCIF5`'s actual TX/RX bytes and `P2`/`PPR2` bit 6
live while the radio boots) would likely resolve faster than continued static tracing — flagged as an
option, not a requirement to wait for.

~~**`SCIF5`'s physical pin has the same "every candidate already claimed" problem as `SCIF1`.**~~ **RESOLVED,
next session — this whole candidate-pair approach was the wrong lens.** Register identity (`0xE8009800` =
`SCSMR_5`) is solid, and the schematic-candidate-pair search below correctly ruled out `P6_6`/`P6_7` and
`P8_11`/`P8_13` — but wrongly assumed `P8_1`/`P8_2` (`DSPR`/`DSPX`) were unavailable because "already McASP1
audio." They weren't: decompiling `scif5_dsp_link_driver_init` itself found it directly enables `P8_0`/
`P8_1`/`P8_2` (not just a pair — three pins) via the RZ/A1H's `PFCn`/`PFCEn`/`PFCAEn`/`PMCn` port-mux
registers, function code 3, all three with `PMCn` genuinely set. **`SCIF5` = `P8_0`/`P8_1`/`P8_2`** — the
"McASP1 audio" reading for these DSP-side pin names was very likely wrong (see
`notes/ic7300-signal-chain.md`'s correction); the real electrical function is this UART. Full derivation,
including the same method resolving `SCIF1`'s pin too, in `notes/kernel-rtos.md`'s "SCIF1 and SCIF5 physical
pins resolved via each driver's own port-mux code" section.

~~Register identity (`0xE8009800` = `SCSMR_5`) is solid regardless, but checked the RZ/A1H manual's
alt-function table for where `TxD5`/`RxD5` can physically land, against the user's own full CPU pinout
sweep: `P6_6`/`P6_7` (already `USSENI`/USB cluster and `LCD_ON`), `P8_1`/`P8_2` (already `DSPR`/`DSPX`,
McASP1's DSP audio serializers), `P8_11`/`P8_13` (already `FPDX`/`DCSR`) — every standard candidate pair is
already wired to a different, independently-confirmed function. Not resolved; same category as SCIF1's
still-open physical pin (an unlabeled/secondary alt-function on the real schematic, or a live JTAG register
read, would settle it).~~ (superseded above)

## SCIF5 RX path closed, command API first real look (2026-08-29, next session, continuing the DSP comms thread)

Picked up the handoff's #1 item directly: decompiled `scif5_dsp_link_driver_init`'s event-`0xf1`/`0xf2`/`0xf3`
targets (read `DAT_200b3140`'s literal value rather than guessing) and traced the whole chain forward.

**All three events (`0xf1`/`0xf2`/`0xf3`) route to the same function, `scif5_rx_isr`** (`0x200b2950`) — SCIF5's
real receive path. It polls SCIF5's RX-ready status (`FUN_20360b34` on `0xE8009808+0x14`), collects bytes one
at a time, and once 4 are in hand, un-bit-reverses them (the same per-byte algorithm `scif5_bitrev_transmit_word`
uses for TX, run in reverse) into a 32-bit reply word — stored at a shared struct field (`DAT_200b1c84+0x18`)
also read by the reply-classifier below. It then kicks `shared_job_ring_dispatch(1)` (the same generic
async-job dispatcher already documented for `chunk4`/`chunk5`/RSPI2/front-panel-UART) — job type 1 there is
"a DSP command is pending acknowledgment."

**Found the real ack/retry state machine behind that job type**:
- **`scif5_classify_reply`** (`0x200b0dc4`, was `FUN_200b0dc4`) reads the reply word and takes its **top
  byte's high nibble as a "reply class"** (0-15; class 9 aliases to class 1). Classes 1/2/8 are recognized:
  class 1 does an incremental step-towards-target using the reply's 2nd byte against a stored target
  (shape of a gain/AGC/level slew, not tied to a specific parameter yet); class 2 is a trivial ack; class 8
  records a 4-field, IQ/status-shaped payload, gated on a separate flag, consumer not yet traced. Returns
  "still pending" or "resolved" based on a retry-budget counter.
- **`scif5_arm_retry_timer(int)`** (`0x200b0cd4`, was `FUN_200b0cd4`) is the real **ACK-TIMEOUT RETRY**
  mechanism — called with a fresh command's kick-off and again whenever `scif5_classify_reply` says
  "still pending." Reprograms an MTU2-style compare/period register cluster with one of 2 timing profiles
  selected by its `param_1`. **Also toggles `PMC8` bits 2 and 11 as a mutually-exclusive pair** depending on
  that same `param_1` — `param_1==0` re-enables `P8_2` (SCIF5's normal 3rd static pin, confirmed in the pin
  section above); `param_1!=0` instead enables **`P8_11`** (the schematic's `FPDX`, previously read as
  FPGA-only). This is a second dynamic pin-toggle mechanism on this codebase, structurally similar to
  `SCIF1`'s TX/RX turnaround found in the prior detour (see `notes/kernel-rtos.md`) — but every sampled
  caller passes `param_1=0`, so the `P8_11` path is real in the code but not yet observed actually triggered.
- **`scif5_send_and_wait_reply`** (`0x200b237c`/`thunk_FUN_200b237c`) is the blocking wrapper tying it
  together: wait for a busy flag, call `scif5_arm_retry_timer` (forwarding its own caller's hidden parameter
  — Ghidra doesn't show this function taking any argument, but it clearly passes one through), wait for the
  reply-ready flag, return the decoded reply word. **14 call sites across the firmware** — this is the DSP's
  real synchronous command/reply API, i.e. most of handoff item 2.
- **`scif5_cmd_transmit_now(cmd_word)`** (`0x200b253c`) is a direct, immediate SCIF5 transmit (busy-wait,
  `scif5_bitrev_transmit_word`, arm a timeout deadline) — a different, faster path than the ring-buffer/MTU2-
  paced one used for `chunk4`/`chunk5` transfer and `dsp_param_sync_tick`. Callers build a command word (e.g.
  `0xe0000000`/`0xe0000001` in the 2 samples checked — a **new top-byte class, `0xE`, not in the default
  table below**), push it with this, then call `scif5_send_and_wait_reply` (up to a small retry count in the
  samples checked) and compare the reply's top nibble against an expected class (`9`, `0xE` seen).

**`FUN_200b1540`, renamed `dsp_cmd_table_init`, is confirmed (not just guessed) as the boot-time table
builder**: it's `cold_boot_hw_init`'s own direct call, right after `scif5_dsp_link_driver_init` (2 other init
calls between them, then `rspi2_driver_init` right after) — so this really is the "DSP driver is up, push its
full default/saved parameter set" boot step, not something reached later. It builds the 24-entry table at
`DAT_200b1cb8` (the literal words from the old handoff list), with **6 of the 24 slots populated from runtime
variables instead of fixed literals** (plausibly per-unit calibration/EEPROM-backed settings, not yet traced
further) — then fires exactly one `dsp_param_sync_tick()` call and waits for it to finish before marking
itself done.

**Net effect on the handoff list**: item 1 is closed. Item 2 has a real API surface now
(`scif5_send_and_wait_reply` + its ~14 callers) rather than being an undifferentiated pile of
`scif5_ring_push_word` call sites. Renamed/commented all 6 functions above in Ghidra; committed.

## SCIF5 command API, first real look: 7 of the 14 `scif5_send_and_wait_reply` callers characterized (2026-08-29, next session, continuing straight on)

Went through the suggested next step — the 14 callers one at a time — and 7 of them (spanning 6 command
words, `0xe0000000`-`0xe0000005`) turned out to be one clean, coherent mechanism, plus a 7th with a
different shape tying straight back to the firmware-update chunk transfer.

**The 6 `0xE0000000`-`0xE0000005` commands are a DSP identity/version-query protocol, not a bulk transfer
as first guessed.** Each of the 6 near-identical functions (`dsp_identity_query_cmd0`-`cmd5`) sends one
command via `scif5_cmd_transmit_now`, drains 2 throwaway replies, then polls up to 18 times for a reply
whose **top-byte high nibble is class `0xF`** ("done"), formatting the winning reply via
`dsp_identity_format_reply(dir, reply, record_base)` into a **6-byte record**:
`[byte2][.][byte1][byte0][ascii tens][ascii units]` — raw middle/low bytes of the reply word, a literal
`.` separator, and the top byte's low nibble (0-15) rendered as a 2-digit decimal string. This is exactly
the shape of a formatted version string (e.g. `"12.345XY"`-ish), not a data-block transfer.

**All 6 write into ONE shared struct, `DAT_200b1ca0`, as 3 consecutive 13-byte records** (found by tracing
which orchestrator calls which pair): commands 0/1 (dir 0/1) → offset `0x00`; commands 4/5 → offset `0x0d`;
commands 2/3 → offset `0x1a`. Each 13-byte record is 6+6 formatted bytes plus 1 "ready" flag, set by the
matching orchestrator (`dsp_identity_query_record0`/`record1`/`record2`) once both halves complete. The
apparent "3 separate destination buffers" seen earlier (`DAT_200b2b2c`/`DAT_200b2b30`) turned out to just
be runtime pointers into this same struct at `+0xd`/`+0x1a`, not distinct memory.

**Confirmed real boot step, not a menu/on-demand query**: all 3 orchestrators are called unconditionally
from `cold_boot_hw_init`, in one unbroken run of DSP bring-up calls:
```
scif5_dsp_link_driver_init → (2 other init calls) → dsp_cmd_table_init
  → dsp_identity_query_record0 → dsp_identity_query_record1 → dsp_identity_query_record2
  → rspi2_driver_init
```
i.e. right after the DSP's default command table is pushed, the CPU immediately asks the DSP for 3
identity/version fields, every boot, before moving on to unrelated RSPI2/FPGA setup.

**Working hypothesis, not proven by a direct xref yet**: these 3 records are DSP Program version / DSP
Data version / a 3rd field (hardware or silicon revision is a reasonable guess) — which would tie directly
to the already-documented version-compare fields in `FUN_200a94c8` (`+0xa8`=DSP Program, `+0xac`=DSP Data,
see this file's earlier session). Not yet chased: whether `FUN_200a94c8` (or the version-display screen)
actually reads `DAT_200b1ca0`'s 3 records — a concrete, cheap next check if this thread continues.

**7th function, different shape — `dsp_page_transfer_verify`** (`0x200b3054`, was `FUN_200b3054`): its
*only* caller is `chunk_transport_send_data` itself — this is the low-level **per-256-byte-page transfer
verification** step underneath the firmware-update chunk mechanism. Drains 4 throwaway replies, polls for a
reply of class `0xE`, then computes a running byte-sum checksum over a 256-byte page buffer (accumulator
persists across calls unless explicitly reset — i.e. a whole-transfer checksum, not per-page) and validates
it two ways against the reply: the checksum's two's-complement must match the reply's low byte, **and** the
page's destination address (`+0x100`) must match a 20-bit field the DSP echoes back in the reply — i.e. the
DSP confirms both data integrity and correct placement for every page it receives during an update.

**Correction**: the "6 remaining `scif5_send_and_wait_reply` callers" flagged above was a miscount — the 14
call sites split as 6 pairs (identity query, 2 call sites each) + 1 pair (`dsp_page_transfer_verify`) = 14
exactly. **All 14 are now characterized; none remain.**

## DSP command API, continued: `factory_file_load` confirmed as a real consumer of the identity records; `dsp_param_sync_tick` fully mapped (2026-08-29, next session, continuing straight on)

Did the two follow-ups from the prior entry — the direct-xref check on whether anything reads
`DAT_200b1ca0`'s 3 identity records, and the ~30 `scif5_ring_push_word` callers — and both paid off, though
not exactly where expected.

**The identity records feed a real "does this file match the current DSP" gate — in `factory_file_load`,
not `FUN_200a94c8`.** A raw hex search for `DAT_200b1ca0`'s resolved struct address (`0x203DEF00`) as a
literal anywhere in `body.bin` turned up `factory_file_load` (the `"C:\IC-7300\IC-7300_factory"`
mechanism from the earlier "Factory/service mode" thread, see `notes/kernel-rtos.md`) — **not**
`FUN_200a94c8` (that hypothesis is retracted; `DAT_200a9ba4`'s struct, `0x203ff76c`, is a completely
different, far more widely-referenced address with no literal-pool co-reference to `0x203DEF00` anywhere).
`factory_file_load`'s own decompile has `local_58[0..2] = DAT_20025644 + {0, 0xd, 0x1a}` — **exactly** the
3 identity-record offsets found in the prior entry — and compares each against a 4-byte field read from
the factory file, storing a pass/fail byte. **This directly confirms the user's own hypothesis this
session**: the DSP identity/version query is used to gate whether a stored file (here, the factory
data/calibration file specifically, not the general firmware-update path) matches what's currently
installed. Corrected `factory_file_load`'s Ghidra plate comment, which had previously described this
compare as against "a fixed reference table" without realizing one of the two checks in that loop is
against the live DSP identity struct.

**`dsp_param_sync_tick`'s full structure is now mapped**: it's a "diff current against last-synced
shadow" loop over `dsp_cmd_table_init`'s 24-entry table — for slots 1-22 (the header slot, index 0, is
handled by a sibling function, `dsp_param_sync_slot0`, same pattern; slot 23 isn't live-synced at all,
init-only), each slot's current value sits at table-offset `N`, its shadow copy at `N+0x60`; any mismatch
updates the shadow and pushes the new value via `scif5_ring_push_word`. **Exhaustively confirmed** (raw
hex search for the table's resolved base address, `0x20414c88`) that this literal appears **exactly once**
anywhere in the 3.7MB image — shared only by `dsp_cmd_table_init` and `dsp_param_sync_tick` themselves.
So no other function writes these 22 live parameter slots by referencing the table directly; whatever
actually changes a parameter between sync ticks does so through some other indirection (possibly the same
`DAT_200b1cd0`-style pointers `dsp_cmd_table_init` used for 6 of the slots' initial values, now suspected
to be live pointers into elsewhere rather than one-shot copies — not confirmed). **This is the natural next
step for individually naming the ~22 DSP parameters** (mode/filter/AGC/volume-shaped, per the working
hypothesis) — tracing each slot's real data source rather than the table itself, since the table's own
address is a dead end for that specific question.

**Net effect**: item 2 (the DSP command API) is now fully mapped at the *mechanism* level — the 3 command
families (identity query, page-transfer-verify, param-sync) and their respective transports
(`scif5_send_and_wait_reply`/`scif5_ring_push_word`) are all understood — but naming each of the ~22
individual live parameters is a separate, sizable task, not started. Renamed/commented 4 more functions in
Ghidra (`dsp_param_sync_slot0`, plus plate updates on `dsp_param_sync_tick`, `scif5_ring_push_word`,
`factory_file_load`); committed.

## `IC902` identity, corrected again: it's the DSP's own SPI boot flash, at the pin level (2026-08-29)

**Retracts the 2026-08-27 "confirmed: IC902 = FPGA config flash" conclusion above.** That call was based on
tracing IC902's `SPDI`/`SPDO`/`SPCK`/`SPCS` net *labels* through the schematic to a cluster also carrying
`DONE`/`STAT`/`CFG` (Altera passive-serial config handshake names) — real evidence, but one hop removed
from IC902 itself. This session the user traced IC902's actual `CS`/`DO`/`DI`/`CLK` pins directly to their
destination and got a much more direct answer: **`IC901` (the DSP, TMS320C6745) pins 9/17/18/11** —

| `IC902` pin | `IC901` (DSP) pin | DSP pin's dual function |
|---|---|---|
| `CS` | 9 | `\SPI0_SCS[0]` / `\UART0_RTS` / `EQEP0B` / `GP5[4]` / **`BOOT[4]`** |
| `DO` | 17 | `SPI0_SOMI[0]` / `EQEP0I` / `GP5[0]` / **`BOOT[0]`** |
| `DI` | 18 | `SPI0_SIMO[0]` / `EQEP0S` / `GP5[1]` / **`BOOT[1]`** |
| `CLK` | 11 | `SPI0_CLK` / `EQEP1I` / `GP5[2]` / **`BOOT[2]`** |

Every one of these DSP pins doubles as a `BOOT[n]` strapping input — TI C674x-family DSPs sample
`BOOT[4:0]` at reset release to select boot mode, and the same physical pins then serve as the SPI0
peripheral once that mode is selected. Wiring a flash chip straight onto exactly this pin group is the
textbook "boot from SPI flash" hardware configuration. This is about as direct as schematic evidence gets —
**`IC902` is `IC901`'s own dedicated boot flash**, and the DSP almost certainly self-boots autonomously
from it via its internal ROM bootloader, with no main-CPU involvement needed to load DSP program code after
reset is released.

**Reconciles rather than contradicts the FPGA-config-signal observation**: the 2026-08-27 finding (IC902's
net labels reaching a cluster with `DONE`/`STAT`/`CFG`) already came with a recorded hypothesis — "the
FPGA's config bitstream is fed by the DSP rather than directly by the main CPU" — that fits perfectly now:
`IC901`, once self-booted from `IC902`, likely drives the FPGA's config pins itself as part of its own
firmware, rather than `IC902` feeding the FPGA directly. Nothing here rules that out; it just moves the
FPGA-config relationship one hop later (DSP → FPGA, not flash → FPGA).

**This reframes the whole DSP-update picture and directly answers both of the session's open questions**:

- **DSP code update timing**: the "DSP Program" chunk `firmware_update_main` sends over `SCIF5` (see the
  section above) is very unlikely to be streamed straight into DSP RAM for direct execution — `IC901` is
  already running (it has to be, to receive and act on `SCIF5` traffic at all) on whatever program it
  self-booted from `IC902`'s *current* contents. The far more coherent model: `IC901`'s own running
  firmware includes a flash-programming routine that listens on `SCIF5`, receives the new image from the
  main CPU, and **reprograms `IC902` itself** — the DSP is the only device electrically able to program
  this flash at all, since `IC902` sits on the DSP's private SPI0 bus, not on any bus the main CPU can
  reach directly. That write happens live, during the update (matches the confirmed single-caller timing
  above) — but the new code doesn't start *running* until `IC901` itself next reboots and self-loads from
  the freshly-written flash, which most likely coincides with the same restart that follows the main CPU's
  own update (matching the "...restart. NEVER turn OFF..." warning), though whether that's the main CPU
  re-asserting `DRESD` or the DSP self-resetting once programming+verification completes isn't determined.
- **Where `DRESD` gets released**: still not found in the statically-traced main-CPU call graph (see above)
  — but this finding weakens the assumption that it *needs* to be released repeatedly or on any complex
  schedule. If `IC901` only needs taking out of reset once, ever, per power-on (and then runs and manages
  its own subsequent reboots/reprogramming autonomously via its own flash and its own logic, independent of
  further main-CPU `P2_6` writes), a single early release — however it happens — would be entirely
  sufficient to explain every SCIF5 observation in this session, live traffic included. Doesn't close the
  question, but narrows what kind of mechanism is worth still looking for.
