# `icom_fw` — IC-7300 firmware container unpacker/packer

A clean, tested rewrite of the existing ad-hoc `tunk.py`/`tunk3.py` scripts
(the `$ICOM_FW_DIR` originals). See `../notes/` for the
reverse-engineered format this implements, and `../notes/multi-cpu-images.md`
in particular for why this rewrite exists: the original scripts silently
dropped ~1.46 MB (~37%) of every container.

No third-party dependencies — pure standard library, Python 3.9+.

## Usage

Unpack one firmware file:

```
python3 -m icom_fw.cli unpack firmware/7300_142.dat scratch/out-142
```

Writes `body.bin` (main ARM image), `chunk1_font1.ttf`, `chunk2_font2.ttf`,
`chunk3.bin`, `chunk4.bin`, `chunk5_tail.bin`, and `manifest.json`
(offsets/sizes/warnings, plus an accounted-vs-total byte count).

Repack a container with a modified body — e.g. `sdk/roadmap.md`'s Phase 0
test (edit `body.bin`, get back a checksum-correct container to try flashing
via the normal SD-card update flow):

```
python3 -m icom_fw.cli pack firmware/7300_142.dat scratch/out-142/body.bin scratch/repacked_142.dat
```

Recompresses the given body with this project's own from-scratch LZSS
encoder (`icom_fw/lzss.py`'s `compress()` — produces a *valid* stream per
this format, not a byte-for-byte match to Icom's own compressor's output,
which doesn't matter for correctness), reinserts it into its fixed slot, and
recomputes the update mechanism's own MD5 checksum
(`notes/firmware-update.md`'s traced checksum region) — every other byte
(fonts, chunk3, and everything after) is copied verbatim from the original
container, untouched.

Verify the unpacker against all 10 known releases (diffs against the existing
`unpacked.dat` references, checks 100% of each container is accounted for):

```
python3 tools/verify_all.py
```

Verify the compressor+packer round-trips correctly against all 10 known
releases (patches a few bytes of each real body, repacks, re-parses, checks
the body/fonts/chunk3/tail/checksum all come out exactly as expected):

```
python3 tools/verify_pack.py
```

## Layout

```
tools/
  icom_fw/
    lzss.py        LZSS decompressor + compressor (see /notes/decompression-lzss.md)
    container.py   container parser + packer (see /notes/container-format.md)
    cli.py          unpack one file -> component files + manifest.json; pack a modified body back in
  verify_all.py     cross-check the unpacker against all 10 releases + existing refs
  verify_pack.py    cross-check the compressor+packer round-trip against all 10 releases
  arm_thumb_scan.py    per-window ARM-vs-Thumb region classifier (objdump heuristic)
  superset_disasm.py   full per-address ARM+Thumb disassembly, persisted to SQLite
  sjis_string_scan.py   whole-image Shift-JIS/ASCII string sweep, flags JP text with no adjacent English
```

## ARM/Thumb disassembly-ambiguity tooling

`body.bin` has no embedded mode markers, and Ghidra occasionally guesses the
wrong ARM/Thumb mode for a region (see `notes/kernel-rtos.md`'s tooling-gotcha
entry) or fails to find a real string/data reference simply because the
referencing instruction sits in a region Ghidra's own analysis hasn't
resolved correctly yet. Two complementary scripts address this, neither ever
touches the live Ghidra project — both are ground-truth locators, apply any
real fix by hand in the GUI:

- **`arm_thumb_scan.py`** — classifies fixed-size windows as `arm`/`thumb`/
  `ambiguous` using objdump's own bad-instruction-count in each mode. Cheap
  (~16s for the whole 3.7 MB image), good for "what mode does this region
  look like". Doesn't keep the actual instructions.
- **`superset_disasm.py`** — decodes *every* candidate address independently
  in both modes (every 4-byte-aligned address as ARM, every 2-byte-aligned
  address as Thumb — the standard "superset/shingled disassembly" technique
  for resolving this kind of ambiguity) and persists every attempt (address,
  mode, validity, mnemonic, operands, raw bytes, and — for `ldr Rd, [pc,
  #imm]` literal loads — the resolved target address) to a SQLite database.
  ~30s for the whole image. This is what makes cheap searches possible:
  ```
  python3 tools/superset_disasm.py scratch/unpacked/<rel>/body.bin \
      --base 0x20005000 --out scratch/superset_<rel>.sqlite

  # find every instruction (in either mode) that references a known address
  sqlite3 scratch/superset_142.sqlite \
      "select addr, mode, mnemonic, op_str from insns where target=0x2035a010"

  # all literal-pool loads in a region, either mode
  sqlite3 scratch/superset_142.sqlite \
      "select addr, mode, mnemonic, op_str from insns
       where mnemonic like 'ldr%' and op_str like '%pc%'
       and addr between 0x20140000 and 0x20150000"
  ```
  **Caveat found while validating this** (checked against the known
  `0x20056fd4` Thumb-fix spot): a *single* instruction's validity is a weak
  mode signal on its own, especially in ARM mode — most 4-byte words decode
  to *some* syntactically valid ARM instruction (conditional branches eat a
  huge slice of the encoding space), so `0x20056fd4` decodes "validly" as
  both `b #0x200175d4` (ARM) and `lsls r6, r7, #5` (Thumb) even though only
  Thumb is real. Use `arm_thumb_scan.py`'s windowed run-of-valid-instructions
  heuristic to judge *which* mode is actually right at a given address;
  use `superset_disasm.py`'s persisted table to then search/grep for
  literal-pool xrefs and other instruction patterns once you know the mode.
  Regenerate the `.sqlite` (gitignored, lives under `scratch/`) whenever the
  working firmware release changes.

**Applying a real fix** (once you know the mode from the two tools above) still needs Ghidra itself
to touch the live project — no MCP tool exposes this (checked both the currently-installed
`themixednuts/GhidraMCP` and, before it, `bethington/ghidra-mcp`, abandoned as unreliable; see
`README.md`'s Ghidra MCP section). `ghidra_scripts/FixArmThumbMode.java` is a native Ghidra script that
collapses the three manual GUI steps (Clear Code Bytes / Set Register TMode / Disassemble) into one
run. One-time setup: Ghidra's Script Manager (Window → Script Manager) → Script Directories icon → add
this repo's `tools/ghidra_scripts` → Refresh; it then shows up under the `ICOM.ARM-Thumb` category
like any built-in script.

Rather than prompting interactively, it reads its work from a fixed request file — `scratch/
armthumb_fix_requests.txt` — so Claude can write the addresses directly and you just click Run, no
copy-pasting. One line per fix: `<address> <length> <arm|thumb>`, optionally followed by a free-text
note:
```
0x20056fd4 16 thumb   known needs-Thumb spot, bookmark sweep 2026-08-29
0x2014b000 4096 thumb FreeType Thumb-2 region
```
Every line's outcome (OK or the error message) is appended, timestamped, to `scratch/
armthumb_fix_results.txt`; the request file is then replaced with a single "processed at &lt;time&gt;"
line so a stray re-run with no new content is an obvious no-op rather than silently reapplying old
fixes. Both files are gitignored (`scratch/`) — regenerate/rewrite as needed, nothing durable is lost
since the real record of a fix is the Ghidra database itself.

**2026-09-07 finding — exhaustive check of the whole image found zero live candidates.** Computed
every *direct* ARM↔Thumb boundary in `arm_thumb_regions_142.json` (adjacent windows disagreeing with
no `ambiguous`/`either` window between them) — only **7** exist in the whole 3.7 MB image, far fewer
than expected. Checked all 7 against Ghidra's actual live state (functions + listing, not just the
raw-file heuristic): every one is either (a) plain data Ghidra already correctly leaves undisassembled
(pointer tables, glyph/string tables — the raw-byte heuristic's mode call on data is a false positive,
not a real code-mode conflict), or (b) code Ghidra already disassembles correctly (confirmed
semantically, e.g. the `0x2014b000` FreeType-region boundary: real ARM code with proper push/pop
prologues and sensible local branch targets continues well past the point the window heuristic guessed
a switch to Thumb — the heuristic's per-window call was simply wrong there, not Ghidra's disassembly).
**Lesson**: an isolated single-mode window surrounded by `ambiguous` windows is a data-table false
positive far more often than a real bug — don't treat every heuristic-flagged boundary as a fix
candidate without checking Ghidra's live state first, the same way the "Bad Instruction" bookmark
sweep already taught. This was a genuine, well-supported negative result, not an unswept gap — the
project's "Bad Instruction" bookmarks (16 currently live) were also all checked and are stale leftovers,
not real bugs either. No known-broken address currently exists to test `FixArmThumbMode.java` against.

**Follow-up, same day — full sweep of every non-ambiguous region, thread now closed.** Checked all 51
remaining `arm`/`thumb`-classified regions from `arm_thumb_regions_142.json` (everything not already
excluded above) against Ghidra's live state — exhaustively, not a sample. Zero real mode-mismatch bugs,
and zero genuinely un-analyzed-but-plausible-code regions either. The two large ARM blocks
(`0x20005000`-`0x200ca000`, `0x200d1000`-`0x200fd000`, ~1MB combined) are confirmed correct throughout
via deep spot-checks and dense real functions. The remaining ~470KB (`0x2018e000`-`0x2035d000`) that the
classifier tagged as code is **data** — icon/glyph/font-bitmap and menu-lookup tables with periodic
repeating byte patterns that happen to fool the bad-instruction-count heuristic. Ghidra already
correctly leaves it undefined. **Known limitation of `arm_thumb_scan.py`, worth remembering if its
output is reused for anything else**: the bad-instruction-count heuristic mislabels certain low-entropy
repeating data as `arm`/`thumb` code — don't trust its labels at face value over that address range
without an independent check. Combined with the bookmark sweep and the direct-boundary sweep above,
**this closes out ARM/Thumb-bug-hunting across the entire currently-analyzed image** — no known-broken
address exists anywhere in it. Re-open only if newly-analyzed code (a region Ghidra hasn't touched yet)
shows a real problem later.

## Shift-JIS / bilingual-string sweep tooling

Built 2026-09-08 after a real, previously-unknown JP-only firmware feature (the "4630 kHz Emergency
Communication Mode" — see `notes/diode-matrix.md`) was found by hand, by noticing one Shift-JIS status
string had no adjacent English translation unlike every sibling row in the same small table. This tool
turns that technique into a repeatable whole-image sweep instead of a one-table manual check.

```
python3 tools/sjis_string_scan.py scratch/unpacked/142/body.bin \
    --out scratch/sjis_scan_142.sqlite --check 0x2032a014 0x2032a07a
```

**How it works**: a single left-to-right tokenizer walks the whole image once, greedily consuming runs of
printable ASCII and/or Shift-JIS lead/trail byte pairs that actually decode under the `shift_jis` codec
(precomputed into a lookup table so the hot loop is dict lookups, not repeated try/except). Runs under 3 real
characters are discarded (`--min-chars`, adjustable). Every kept run is classified `jp` (contains at least one
real kanji/kana codepoint — the plausibility filter that excludes byte pairs which decode "validly" as
Shift-JIS without ever landing on a real character), `ascii`, or `sjis_other` (valid pairs, no real kanji/kana
— audit-only, not a text candidate). Results persist to SQLite (`runs`, `jp_candidates` tables) — see the
script's own docstring for query examples. Always pass `--check <known-good addresses>` before trusting a
sweep's output on a new release; it exits nonzero if any expected hit is missing.

**Pairing check**: for each `jp` candidate, the tool looks at the NUL-delimited table *slot immediately
before* it (not the trivially-empty gap between the nearest preceding NUL and the candidate's own start —
an off-by-one that produced a real false negative during this tool's own calibration, see the
`find_preceding_field` docstring) for plausible English text, with a weaker fallback that checks any
plausible-English ASCII run within `--window` bytes on either side.

**Known limitation, found and fully characterized 2026-09-08 — read before trusting "no adjacent English"
output broadly**: the proximity-based pairing check is only valid for small, tightly-packed, genuinely
per-item-interleaved tables — confirmed against the `0x2032a000` status-indicator cluster (English slot,
Japanese slot, English slot, Japanese slot, ...). It does **not** hold across the much larger
`0x2035a000`-`0x2035f000` UI name/message pool, which instead uses either (a) long block-separated runs (a
whole run of English fields, then a separate whole run of Japanese fields for the same messages in the same
relative order — confirmed for the dialog/error-message section), or (b) a genuinely non-adjacent
pointer-record table (the `0x2032c91c`, 76-byte-stride bilingual message table documented in
`notes/firmware-update.md` — confirmed for at least one record, where the "unpaired" Japanese text's real
English pair sits in the *same table record*, just a different field, nowhere near it in the raw bytes).
Sweeping the 142 release found **18,474** `jp` candidates out of 126,603 total runs; only ~27% (4,920) fall
inside the known real string-pool address range (`0x2018e000`-`0x2035d000`) — ~46% land inside the two large
pure-ARM-code blocks and are almost certainly coincidental decodes of instruction bytes, not real text. Of
the in-range candidates, most of the very longest ones turned out to be a Shift-JIS glyph/character
enumeration table (`~0x20336000`-`0x20341000`), not UI strings. **Practical upshot**: treat an "unpaired"
flag as a real signal worth chasing only inside a small, fixed-stride, structurally-obvious table like
`0x2032a000`; outside that, cross-check via `references_to` and the record structure directly (as
`notes/diode-matrix.md`'s 12th session did) before trusting it. Full sweep narrative, the two false-positive
clusters chased and ruled out, and the real correction this tool's calibration step caught (an overturn of an
earlier session's own by-hand "no English counterpart" claim) are in `notes/diode-matrix-history.md`'s
12th-session entry.
