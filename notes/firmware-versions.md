# Firmware releases — header field survey

Parsed directly from all 10 read-only originals in `firmware/`
(`7300_1XX.dat`) using the layout in [[container-format]]. This supersedes
`foo.txt`'s hand-collected hex diffs (which mixed in `base.hex` dumps) —
figures below are computed straight from the actual files.

| ver | file size | version string | size1 | size2 | size3 | size4 | size5 | size6 | size7 | `length` (main body, decompressed) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1.11 | 3,946,563 | `3wfU3.092.003.13` | 2436080 | 97307 | 163592 | 721836 | 720648 | 691230 | 847169 | 3738328 |
| 1.12 | 3,960,355 | `3wfU3.102.003.14` | 2436080 | 97670 | 163592 | 721836 | 720648 | 704659 | 853193 | 3738328 |
| 1.13 | 3,955,508 | `3wfU3.112.003.15` | 2436080 | 97862 | 163592 | 721836 | 720648 | 699620 | 852941 | 3738328 |
| 1.14 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738340 |
| 1.20 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738340 |
| 1.21 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738340 |
| 1.30 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738388 |
| 1.40 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738388 |
| 1.41 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738388 |
| 1.42 | 3,954,089 | `3wfU3.112.003.16` | 2436080 | 97862 | 163592 | 721836 | 720648 | 698201 | 859412 | 3738392 |

## Patterns

- `size1`, `size3`, `size4`, `size5` are **byte-for-byte constant across
  every release** (`2436080`, `163592`, `721836`, `720648`) — these look
  like fixed structural constants (slot capacities or similar), not
  per-build data sizes. Not matched to the real chunk1/chunk2/chunk3 sizes
  below (checked — no match, and that check still stands, see next
  section), but **`size3`/`size4`/`size5` *are* now matched to something
  else in the container**: [[multi-cpu-images]]'s later, fully-verified
  container dissection (`size1`/`size2`/`size4`/`size6`/`size7` used
  directly as offset/length arithmetic for 3 LZSS-compressed
  "components" beyond the main body) gives, for v1.42, component sizes of
  compressed/decompressed `97862`/`163592` (component0), `721836`/`720648`
  (component1, `dsp_program.bin`), and `698201`/`859412` (component2,
  `dsp_data.bin`) — an exact match to this table's `size2`/`size3`,
  `size4`/`size5`, and `size6`/`size7` respectively. So `size3` = component0's
  decompressed size and `size4`/`size5` = component1 (DSP program)'s
  compressed/decompressed sizes — genuinely fixed because that component
  never changed size after v1.14 (see below), not because they're slot
  capacities. `size1` still isn't tied to a specific sub-blob's size
  directly, but is now known to be used as the base offset
  (`component0_offset = size1 + 0x3c`) from which the 3 components are
  located — see [[multi-cpu-images]].
- `size2`, `size6`, `size7`, and the **version string itself** all changed
  release-to-release only through **v1.11 → v1.14**, then **froze solid**
  for every release from v1.14 through v1.42 (the last official release).
  `length` (main body decompressed size), by contrast, keeps changing on
  nearly every release (+12, +48, +4 bytes across 1.14/1.20/1.30/1.40/1.42).
  This is the strongest evidence so far for [[multi-cpu-images]]: it looks
  like Icom stopped updating a secondary component (whatever `size2`,
  `size6`, `size7`, and the version string track) after v1.14, while
  continuing to update the main RZ/A1H application (`length`) every
  release through v1.42.

  **Now confirmed, precisely — and the "all 3 froze simultaneously at
  v1.14" reading above was slightly wrong** (2026-08-30): [[multi-cpu-images]]
  identified all 3 components' real Icom-official identities by correlating
  their content-change points against Icom's own published per-release
  `DSP Program`/`DSP Data`/`FPGA` version fields (a perfect, zero-discrepancy
  match). The real per-component freeze points are **not** all the same
  release:
  - `size2`/`size3` (component0) = **`DSP Program`** — changes `1.11→1.12→1.13`
    (`1.05→1.06→1.07`), **frozen from v1.13 onward**, one release earlier
    than the other two.
  - `size4`/`size5` (component1) = **`DSP Data`** — **never changes at all**
    across this project's entire local dataset (`v1.11`–`v1.42`, pinned at
    Icom's own `1.00` the whole time) — not "froze after v1.14," it was
    already frozen at the very first locally-held release. This component
    is also confirmed genuine executable TMS320C674x object code, despite
    the "Data" label — see [[multi-cpu-images]] for that open naming
    puzzle.
  - `size6`/`size7` (component2) = **`FPGA`** — changes `1.11→1.12→1.13→1.14`
    (`1.10→1.11→1.12→1.13`), **frozen from v1.14 onward** — this is the one
    component that actually matches the "froze at v1.14" framing above.

  So the accurate statement is: **3 different components, 2 different real
  freeze points** (`DSP Program` at v1.13, `FPGA` at v1.14, `DSP Data` frozen
  for the entire known history) — not one simultaneous v1.14 cutover across
  all 3, as the original wording implied.

## Real chunk sizes vs. fixed slot offsets

Actual chunk1 (font)/chunk2 (font)/chunk3 sizes, read from their
size-prefix fields (`container[0x21002c]`, `container[0x24002c]`,
`container[0x25002c]`):

| ver | chunk1 (font1) | chunk2 (font2) | chunk3 |
|---|---|---|---|
| 1.11–1.14 | 174528 | 25636 | 11244 |
| 1.20–1.30 | 175656 | 25636 | 11244 |

## Cross-check against Icom's official published release list (2026-08-30)

While chasing the IC-9700's per-component version history (see
[[ic9700-container-format]]), also fetched Icom's official EN/`icomjapan.com`
firmware-history page for the IC-7300. Two things worth recording:

- **The EN/`icomjapan.com` page lists only 9 IC-7300 firmware releases (v1.12
  through v1.42)** — it does not list v1.11 at all. **Resolved**: v1.11 is on
  the JP/`icom.co.jp` page instead (dated 2016/02/18, the oldest entry
  there) — same pattern already seen on the IC-9700 side, where the JP page
  carries early releases (v1.02/v1.03) the EN page has dropped. Not a real
  discrepancy, just each region's page having trimmed different early history
  from its own display list; the release genuinely existed and is a normal
  general-release build, confirmed by both being a real local file and now
  being listed on Icom's own JP page.
- **The same fetched page also lists several unrelated IC-PW2 (linear
  amplifier, a completely different product) firmware entries** mixed in
  after the IC-7300 rows — this is a page-layout quirk on Icom's side (both
  products' histories share one page/table), not an IC-7300 data issue. No
  IC-PW2 entries belong in this file.
| 1.40–1.42 | 176700 | 25636 | 11244 |

None of these match `size1..size7` either. But the **offsets themselves are
fixed, oversized slots**, confirmed by construction:
- `0x21002c − 0x10030 = 0x1FFFFC` (≈2 MiB) — the main-body slot is a fixed
  ~2 MiB budget, regardless of the actual compressed size.
- `0x24002c − 0x21002c = 0x30000` (192 KiB) — chunk1's fixed slot, vs. its
  actual ~170 KiB content.
- `0x25002c − 0x24002c = 0x10000` (64 KiB) — chunk2's fixed slot, vs. its
  actual ~25 KiB content.
- chunk3 and chunk4 (the trailing LZSS-compressed 0x10000-byte block), by
  contrast, are **tightly packed with no gap** — chunk4 starts immediately
  after chunk3's actual data, no fixed-offset seek involved (see
  `tunk3.py`'s code — no `seek()` call between reading chunk3 and starting
  the chunk4 `unpack()`).

**Conclusion**: this is a fixed-slot container format for the first four
regions (body/chunk1/chunk2/chunk3-start), each over-provisioned to a round
power-of-two-ish size, followed by one tightly-packed compressed tail
(chunk4). The rewritten unpacker in `tools/` should assert this shape
(fixed-offset reads for slots 1–4, immediately-following read for chunk4)
rather than silently trust it — see [[container-format]].

**Correction, later session**: the "chunk4"/single-trailing-tail framing above (from
`tools/icom_fw/container.py`'s original byte-accounting model) is superseded by
[[multi-cpu-images]]'s fuller, byte-verified dissection of this same trailing region: it isn't one
tightly-packed compressed tail, but **3 separate LZSS-compressed components** (`component0`,
`dsp_program.bin`, `dsp_data.bin`), located via `size1`/`size2`/`size4` header-field arithmetic, not
a single contiguous read. [[multi-cpu-images]] notes explicitly that "the real firmware recognizes
no chunk4/chunk5 boundary; these 3 components just happen to span across where that boundary
falls" — i.e. the old chunk4 concept wasn't wrong about *where* the data starts, just about there
being only one blob there instead of three. The "tightly packed, no fixed-offset seek" observation
above still holds for locating the *start* of this region relative to chunk3; what happens inside
it is better described by [[multi-cpu-images]]'s component0/1/2 model now.
