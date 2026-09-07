# `icom_fw` — IC-7300 firmware container unpacker

A clean, tested rewrite of the existing ad-hoc `tunk.py`/`tunk3.py` scripts
(read-only originals at `/data/misc/icom/7300/`). See `/notes/` for the
reverse-engineered format this implements, and `/notes/multi-cpu-images.md`
in particular for why this rewrite exists: the original scripts silently
dropped ~1.46 MB (~37%) of every container.

No third-party dependencies — pure standard library, Python 3.9+.

## Usage

Unpack one firmware file:

```
python3 -m icom_fw.cli /data/misc/icom/7300/7300_142.dat scratch/out-142
```

Writes `body.bin` (main ARM image), `chunk1_font1.ttf`, `chunk2_font2.ttf`,
`chunk3.bin`, `chunk4.bin`, `chunk5_tail.bin`, and `manifest.json`
(offsets/sizes/warnings, plus an accounted-vs-total byte count).

Verify against all 10 known releases (diffs against the existing
`unpacked.dat` references, checks 100% of each container is accounted for):

```
python3 tools/verify_all.py
```

## Layout

```
tools/
  icom_fw/
    lzss.py        LZSS decompressor (see /notes/decompression-lzss.md)
    container.py   container parser (see /notes/container-format.md)
    cli.py          unpack one file -> component files + manifest.json
  verify_all.py     cross-check against all 10 releases + existing refs
  arm_thumb_scan.py    per-window ARM-vs-Thumb region classifier (objdump heuristic)
  superset_disasm.py   full per-address ARM+Thumb disassembly, persisted to SQLite
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
run with three prompts (start address, length, ARM or Thumb). One-time setup: Ghidra's Script Manager
(Window → Script Manager) → Script Directories icon → add this repo's `tools/ghidra_scripts` → Refresh;
it then shows up under the `ICOM.ARM-Thumb` category like any built-in script.
