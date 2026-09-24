# Base-configuration diode matrix (P5 GPIO)

Physical layout deciphered by the user from PCB images (omitted from the
schematics deliberately) — 3 rows × 8 columns, diode numbers per
position:

| | col1 (P5_7) | col2 (P5_6) | col3 (P5_5) | col4 (P5_4) | col5 (P5_3) | col6 (P5_2) | col7 (P5_1) | col8 (P5_0) |
|---|---|---|---|---|---|---|---|---|
| **top (P5_10)** | N/A | N/A | N/A | N/A | N/A | 409 | 406 | 403 |
| **middle (P5_9)** | 423 | 420 | 417 | 414 | 411 | 408 | 405 | 402 |
| **bottom (P5_8)** | 422 | 419 | 416 | 413 | 410 | 407 | 404 | 401 |

Rows = `P5_8`/`P5_9`/`P5_10`, columns = `P5_0`–`P5_7`, all on the main
CPU's GPIO Port 5. EEPROM for general settings storage is separate:
`IC351` (`GT24C128B`), `SCL`/`SDA` → CPU pins `P1_4`/`P1_5` (`ECK`/`EDT`,
RIIC2 — see [[memory-map]]).

See [notes/diode-matrix-history.md](diode-matrix-history.md) for the full
session-by-session narrative and evidence trail behind everything below.

## Living reference: what each diode does (update this as findings change)

Bit numbering per the confirmed scan-result layout: row-bottom bit =
`8-col`, row-middle bit = `16-col`, row-top bit = `24-col`.

| Diode | Position | Bit | Status | Function |
|---|---|---|---|---|
| D401 | bottom, col8 | 0 | ✅ confirmed | Enables the region-restriction check itself (gate for D404/407/410/413's region code taking effect). When D401 is absent, the entire RX+TX range-restriction mechanism degenerates to "every frequency is valid" |
| D402 | middle, col8 | 8 | ✅ confirmed | **Forces the region 5/6 (export/general-coverage) variant's 40m upper band edge to the full 7.300 MHz.** Their raw stored tables have a narrower 40m allocation (region 5: 7.000-7.100 MHz; region 6: 7.000-7.200 MHz) — any edge strictly between 7.000000 and 7.300000 MHz gets snapped to 7.000/7.300 MHz by `FUN_2003bd80`/`FUN_2003be94`'s shared clamp logic. D402 **absent** (its state on every currently-documented shipping variant) → clamp active → both regions get the full 7.0-7.3 MHz 40m allocation on real hardware. D402 present (never seen on real hardware) would disable the clamp, restricting 40m to the narrower stored value instead |
| D403 | top, col8 | 16 | ✅ confirmed | Selects Type 1 (present) vs Type 2 (absent) market designation (`FUN_200134b4`/`get_type1_type2_designation` returns ASCII `'1'`/`'2'`); also gates a 51st, non-standard CTCSS tone (150.0 Hz). **Ruled out** for 60m/5MHz via the one consumer traced — that path is CTCSS, not band access. **Fully traced, 9th session**: `get_type1_type2_designation` has exactly 3 real callers (`FUN_2000b98c`, `FUN_200189e8`, `FUN_20065dec`), all 3 use its return purely as the 49-vs-50-tone CTCSS-cycle bound passed to the same generic "select item N with wraparound" helpers (`FUN_20017c50`/`FUN_20006398`) — no other distinct consumer exists anywhere |
| D404 | bottom, col7 | 1 | ✅ confirmed | Region-code bit, weight 8 (see region table below) |
| D405 | middle, col7 | 9 | ✅ confirmed | **Gates a specific ~5.255 MHz (60m-area) frequency in `FUN_2003bd80`'s range table** — D405 present excludes it, D405 absent includes it. Resolves the D403-vs-D405 conflict from external sources in favor of D405 |
| D406 | top, col7 | 17 | ✅ confirmed | Input to `FUN_2003c530`'s post-scan classification — present → classification byte = 1, takes priority over D409. **Consumer found, 9th session**: the classification byte (mirrored to `*DAT_2003c858+0x29` = `0x203def29`) is read by `dsp_param_table_rebuild_from_settings` (`0x200b232c`), shifted left 6 bits and packed into DSP command-table word index 3 (opcode `0x22`), pushed live to the DSP over SCIF5 every tick by `dsp_param_sync_tick()` — a real, continuously-synced DSP configuration input, not just local UI state |
| D407 | bottom, col6 | 2 | ✅ confirmed | Region-code bit, weight 4 |
| D408 | middle, col6 | 10 | ❓ unknown | **Exhaustively searched** — no feature-gate consumer found anywhere in the full firmware. Only touched by the mechanical bit-reversal echo (`FUN_2003c70c`). Genuinely populated on every shipping version (unlike the confirmed-N/A pads), so its lack of any software consumer is a real open question, not just "not found yet" |
| D409 | top, col6 | 18 | ✅ confirmed | Input to `FUN_2003c530`'s post-scan classification — present (and D406 absent) → classification byte = 2 |
| D410 | bottom, col5 | 3 | ✅ confirmed | Region-code bit, weight 2 |
| D411 | middle, col5 | 11 | ❓ unknown | **Exhaustively searched across all 10 known firmware versions** — no consumer found |
| D413 | bottom, col4 | 4 | ✅ confirmed | Region-code bit, weight 1 |
| D414 | middle, col4 | 12 | ❓ unknown | **Exhaustively searched across all 10 known firmware versions** — no consumer found |
| D416 | bottom, col3 | 5 | ✅ confirmed | Gates the general-coverage RX unlock (0.030–74.8 MHz, 13-segment table), combined with region code 5 or 6. Also gates a separate 2-entry lookup (`DAT_2003c85c`, values 2/3 at indices 13/15) — **consumer found, 9th session, but the gate is provably dead code**: it feeds `FUN_2000a5f0(10)`, one branch of a generic ~10/11-item menu-cycle "is item N enabled" gate (called from at least 5 places, shape-consistent with a settings cycling selector); but `DAT_2003c85c` is indexed by the confirmed region_code (`DAT_2003c800+2`, written by `FUN_2003c0ec` as `diode_region_code_lookup`'s result), whose confirmed valid range is 0-7 — indices 13/15 (the table's only non-zero entries) can never be reached by any real diode combination, so this D416-gated path always evaluates to disabled in practice |
| D417 | middle, col3 | 13 | ❓ unknown | **Exhaustively searched across all 10 known firmware versions** — no consumer found. Populated only on EUR/ITR/ESP (`[#03][#05][#06]`) per parts list — **note: this row previously misread `#06` as KOR; `#06` = ESP (Spain), confirmed against this file's own version-number table above.** EUR/ITR/ESP is exactly Icom's official "HF/50/70 MHz" (70 MHz/4m unlocked) band-access group — but **17th session: ruled out as the 70 MHz gate**, with real ROM band-table evidence. 70 MHz access is baked directly into `region_code` 2/3/4's TX/RX tables (`0x20198b88`/`0x20198bec`/`0x20198c50`) — no D417/bit-13 test exists anywhere in that selection chain. The country correlation is very likely coincidental (same 3 PCBs get both D417 and region codes 2/3/4), not causal. D417's actual function is still unknown |
| D419 | bottom, col2 | 6 | ✅ confirmed | **Selects the TX frequency-range table in `FUN_2003bd34`/`FUN_2003be94`, together with D422.** Present (D422 absent) → continuous TX 0.1–74.8 MHz, exactly the mod-guide's "open TX" figure. Populated on all versions per parts list |
| D420 | middle, col2 | 14 | ✅ confirmed (16th session) | **Gates visibility of the "4630kHz" Emergency-mode checkbox itself.** Direct raw-bit test (`*DAT_2003ea4c & 0x4000`, bit 14 = D420) in `settings_item_diode_region_gate` (`0x2003e108`, ex-`FUN_2003e108`) — D420 absent (bit clear) → item excluded from the settings list entirely (never inserted, not just disabled); D420 present (bit set) → included. Confirmed Japan-only (`Only [#01]`) per parts list, so this reads as "4630kHz Emergency Communication Mode is JP-exclusive," matching the manual/domain-knowledge framing from the start. See the new 16th-session section below for the full call chain |
| D422 | bottom, col1 | 7 | ✅ confirmed | **Selects the TX frequency-range table in `FUN_2003bd34`/`FUN_2003be94`, together with D419.** Present (D419 absent) → continuous TX 1.6–54 MHz (fills the HF/6m gap only, not the full 0.1–74.8 MHz range the user's external claim attributed to D422 alone) |
| D423 | middle, col1 | 15 | ✅ confirmed | Real, direct input (bit 15) to `FUN_2003dcc0` (`is_feature_enabled_for_region`/`FUN_2003df34` is a separate, sibling function gating bits 0/5 only for a disjoint set of item codes — see 10th-session correction below) — gates item-code overrides including at least `0x22/0x32/0x4b/0x71/0x73/0x79` and the `0x8f-0x93/0x94/0xe5` range. **10th session**: found a second, more concrete role — `FUN_2003dcc0` is also the factory-reset defaults-table applicator (see "Factory reset / restore-defaults mechanism" below), and D423 present forces item `0x73`'s reset default to `1` there. **11th-session correction**: item `0x73`'s own name string (read directly out of the same 326-item table, offset `+0x28`) is **`"Display Language"`**, not a frequency or Emergency-Mode toggle — so this specific D423 consumer picks the factory-reset *display-language* default (almost certainly forcing Japanese on a JP-model unit), not the Emergency Mode feature itself. The "Emergency Mode" name is still real and still firmware-confirmed (see the new "4630 kHz Emergency Communication Mode" section below), but the *link* from that named feature back to a specific `is_feature_enabled_for_region`/`FUN_2003dcc0` item code remains unresolved — don't read the 10th session's "sits directly in the same gatekeeper" framing as having identified *which* item code is Emergency Mode; it hadn't |

`D412`/`D415`/`D418`/`D421`: pads exist on the physical board (confirmed
by user) but are omitted from published diode-matrix references/photos —
don't assume they're unused; possibly reserved for R&D/factory use. Not
"N/A" in the sense of not existing, just undocumented. `D402`/`D406`/`D409`
join this "never populated on any documented shipping variant" group per
the parts list (see below), even though D402/D406/D409 are all confirmed
*live* code inputs — they're just apparently never asserted on real
hardware in production.

**D408/D411/D414/D417's own "no consumer found" mystery — a real, narrowing
PCB-level negative check (2026-09-09)**: the user checked the actual PCB
photos directly for these four diodes, since the schematic omits the diode
matrix entirely — found no visible trace/routing leading anywhere beyond
the matrix scan itself (no separate signal line, no second consumer trace
distinguishable from the other, already-confirmed diodes in the same
matrix). This doesn't prove there's no software consumer (a trace being
invisible in a photo isn't the same as a trace not existing, and the
already-confirmed diodes' own real consumers are all *firmware-side*
logic reading the matrix's scanned bit pattern, not separate physical
traces either — so this check couldn't have found a consumer even for a
diode that DOES have one). But it does rule out the one hardware-side
hypothesis that would have made further firmware searching moot (a
second, undocumented physical connection carrying these bits somewhere
the matrix-scan code never touches) — reinforcing that if these four
diodes do anything, the answer is still purely in firmware, not on the
board. Narrows the standing "vestigial vs. undiscovered code" question
without resolving it either way.

## Official per-version population data (service manual Parts List)

Source: service manual §5 Parts List, Main Unit, page 5-1 — per-diode
`[#NN]` version tags against each `D4xx` line. Version numbers per
[[ic7300-signal-chain]]'s official table: `USA #02`, `EUR #03`,
`ITR #05` (Italy), `ESP #06` (Spain), `TPE #07` (Taiwan), `KOR #08`
(Korea), `EXP #12`. This parts list also uses `#01`, not present in that
table — almost certainly `JAP` (Japan domestic).

Final values below are from a direct `pdftotext -layout` machine-text
extraction of `IC-7300_Servicio.pdf`, which corrected 5 rows (D405,
D416/D417, D419/D420) that an earlier page-screenshot read had
misread/swapped — see the history file for the full correction story.

| Diode | Population per parts list | Confidence |
|---|---|---|
| D401 | all versions (no tag) | high |
| D403 | all versions (no tag) | high |
| **D404** | **`Only [#12]`** — EXP alone | high |
| D405 | all versions (no tag) | high |
| D407 | `[#05]`, `[#06]`, `[#07]`, `[#08]` — ITR/ESP/TPE/KOR | high |
| D408 | all versions (no tag) | high |
| D410 | `[#03]`, `[#07]`, `[#08]` — EUR/TPE/KOR | high |
| D411 | all versions (no tag) | high |
| D413 | `[#06]`, `[#08]`, `[#12]` — ESP/KOR/EXP | high |
| D414 | all versions (no tag) | high |
| D416 | all versions (no tag) | high |
| D417 | `[#03]`, `[#05]`, `[#06]` — EUR/ITR/ESP | high |
| D419 | all versions (no tag) | high |
| **D420** | **`Only [#01]`** — JAP alone | high |
| D422 | all versions (no tag) | high |
| **D423** | **`Only [#01]`** — JAP alone | high |

`D402`/`D406`/`D409`/`D412`/`D415`/`D418`/`D421` don't appear in this
parts list at all — not populated on *any* currently-documented variant.

**Derived region-code hypothesis — NOT verified, arithmetic only.**
Combining this table with the confirmed 4-bit region-code weights
(`D404`=8, `D407`=4, `D410`=2, `D413`=1) gives an apparent code per
version: `USA`→0, `EUR`→2, `ITR`→4, `ESP`→5, `TPE`→6, `KOR`→7, `EXP`→9.
Two of those (`USA`=0, `EXP`=9) fall **outside** the confirmed valid
range (1–7, from `DAT_2003c7fc`'s 16-entry table) — re-confirmed not a
parts-list misread, so either the bit-weight formula or the region-code
mapping itself has a wrinkle not yet found. Don't treat this derived
table as fact; still worth checking directly against a known board.

## The scan routine: `FUN_2003bb88` (body.bin, RAM)

Uses the RZ/A1H manual's documented Port 5 data register, `P5` at
`0xFCFE3014` (`PM5`/`PMC5` mode-control registers are at
`0xFCFE3314`/`0xFCFE3414`, not directly referenced by this routine, so
pin direction setup for P5 happens elsewhere, not traced).

```c
uVar1 = P5;                          // save current P5 value
for (bVar4 = 0; bVar4 < 8; bVar4++) {
    P5 = sVar7;                      // sVar7 = 1,2,4,...,128 — one-hot column drive
    // debounced read loop: wait for P5 bits 8/9/10 (rows) to read stable
    local_28 = (local_28 & mask) | (row_top<<24 | row_mid<<16 | row_bot<<8);
    local_28 = local_28 >> 1;        // shift accumulator right every iteration
    sVar7 <<= 1;
}
P5 = uVar1;                          // restore original P5 value
*DAT_2003c7f8 = local_28;            // store final scan result
```

Drives columns `P5_0`→`P5_7` one at a time (one-hot), reads rows
`P5_8`/`P5_9`/`P5_10` each time with a debounce/stability loop, packs the
result into a 32-bit accumulator via repeated right-shift.

## Confirmed bit layout of the scan result (`DAT_2003c7f8`)

Final result packs as **bits `[0-7]` = row-bottom (`P5_8`)** results for
columns 8,7,6,5,4,3,2,1 (bit0=col8, bit7=col1), **bits `[8-15]` =
row-middle (`P5_9`)** same column order, **bits `[16-23]` = row-top
(`P5_10`)** same order (only col6/7/8 meaningful — cols 1-5 are the
schematic's N/A positions). Mapped onto the user's diode numbers:

| Bit | Diode | Row/col |
|---|---|---|
| 0 | 401 | bottom, col8 |
| 1 | 404 | bottom, col7 |
| 2 | 407 | bottom, col6 |
| 3 | 410 | bottom, col5 |
| 4 | 413 | bottom, col4 |
| 5 | 416 | bottom, col3 |
| 6 | 419 | bottom, col2 |
| 7 | 422 | bottom, col1 |
| 8 | 402 | middle, col8 |
| ... | ... | (same pattern, +402 per bit within the row) |
| 16 | 403 | top, col8 |
| 17 | 406 | top, col7 |
| 18 | 409 | top, col6 |

## Confirmed consumers of the live scan value (`DAT_2003c7f8`, aliased as `DAT_2003c800`/`DAT_2003ea4c`/`DAT_2003c858`)

- **`FUN_2003bca8(value)`** — extracts bits 1-4 (weighted 8,4,2,1;
  diodes 404/407/410/413) into a 4-bit index, looks up `DAT_2003c7fc`
  → `region_code`. Table contents (decoded): `[0,1,2,0,3,4,5,6,0,7,0,0,0,0,0,0]`
  — only 7 of 16 combinations map to a non-zero region code (1-7).
- **`FUN_2003c634`/`FUN_2003c6c0`** — test bit 0 (D401, "does the region
  check apply at all") and bit 5 (D416 + region 5/6 → force-disable
  override).
- **`FUN_2003c5d4`** — tests bit 5 (D416) standalone; gates a second
  lookup table `DAT_2003c85c` = `[0×13, 2, 0, 3]`, indexed by the region
  code (`DAT_2003c800+2`). Its one caller, `FUN_2000a5f0(10)`, is a
  branch of a generic ~10/11-item menu-cycle enable-gate — see Open
  Question 3, resolved (dead in practice: the table's only non-zero
  entries, indices 13/15, are outside the confirmed 0-7 region-code
  range).
- **`FUN_2003c5c4`** — returns bit 16 (D403) as a raw value (see
  `FUN_200134b4`/`get_type1_type2_designation` Type1/Type2 designation
  above). Its only caller's (3) callers are now fully traced — see Open
  Question 4, resolved: all 3 just bound a CTCSS-tone-cycle.
- **`FUN_2003bc68`** — persists the scan value to EEPROM every boot
  under parameter tag `0x3e44` (via the generic `FUN_2001e510`/
  `FUN_2001e484` get/set API), confirming the diode matrix and general
  EEPROM settings storage share the same parameter-ID-keyed API.
- **`FUN_2003bd80`** (RX)/**`FUN_2003be94`** (TX) — bit 5 (D416,
  inverted, selects per-region table), bit 8 (D402, inverted, 7.0-7.3 MHz
  40m clamp — see table above), bit 9 (D405, inverted, excludes
  ~5.255 MHz).
- **`FUN_2003bd34`** — bits 6/7 (D419/D422) TX-table selector, 0-3 (see
  table above).
- **`FUN_2003c530`** — bits 17/18 (D406/D409) → post-scan classification
  byte, mirrored to `*DAT_2003c858 + 0x29` (= `0x203def29`, inside the
  live DSP-config struct). **Consumer found, 9th session** — see Open
  Question 5, resolved: `dsp_param_table_rebuild_from_settings`
  (`0x200b232c`) reads it, packs it (shifted `<<6`) into DSP command-word
  index 3 (opcode `0x22`) alongside 3 other 2-bit fields, and
  `dsp_param_sync_tick()` pushes it live to the DSP over SCIF5.
- **`FUN_2003df34`/`FUN_2003dcc0`** — bit 15 (D423), the master
  "is_feature_enabled_for_region" gatekeeper (item codes `0x24`-`0xf6`);
  also dispatches on the region code (1-7) and D416+region-5/6 (see
  band-edge table below).
- **`FUN_2003c70c`** — mechanically reverses all 24 scan bits into
  natural column order for a likely full-settings clone/export path
  (`FUN_2003ddd4` dumps 326 item codes via `FUN_2003dcc0`) — echoes
  every bit's raw state into that blob, but branches/gates on none of
  them.

**No consumer found** for D408, D411, D414, D417, D420 despite this —
checked identically across all 10 known firmware versions (`111`
through `142`). **Superseded for D420, 16th session**: a real consumer exists
(`settings_item_diode_region_gate`, `0x2003e108`, tests `*DAT_2003ea4c & 0x4000`) that every
one of these `references_to`-based alias sweeps should have caught (`DAT_2003ea4c` is
explicitly one of the 5 swept addresses) and didn't — **directly verified this session**:
`references_to(0x2003ea4c)` returns only 4 hits (`0x2003dc84`/`0x2003dce8`/`0x2003df3c`/
`0x2003e23c`), and `0x2003e108` is not among them even though its decompile plainly shows
the read. A real, confirmed gap in Ghidra's static xref database for this load site, not a
methodology mistake by any prior session — worth remembering for any future "no consumer
found" negative in this file: an exhaustive `references_to` sweep is only as complete as
Ghidra's own reference analysis, which can silently miss a real read. See the 16th-session
section below for how this consumer was actually found (via the settings-list "walker"
function, not via any alias sweep). **Re-confirmed fresh, 9th session**, via 3 independent
methods, all agreeing: (1) `references_to` on all 5 alias addresses
(`DAT_2003c7f8`/`DAT_2003c800`/`DAT_2003ea4c`/`DAT_2003c858`/
`DAT_2003c85c`) returns the exact same closed set of consumers as
before, zero new hits; (2) a fresh SQL sweep of
`scratch/superset_142.sqlite` (the full ARM+Thumb superset disassembly,
independent of Ghidra's own analysis) for literal-pool loads targeting
these 5 addresses returns the identical closed set; (3) a whole-image
`movw`/`movt` immediate-pair search for these addresses (in case some
function forms the pointer without a literal-pool load) — zero hits
anywhere. Also spot-checked this session's newly-found UI-thread leads
specifically: the touchscreen widget's per-item availability callback
(`FUN_2004f250`) only tests the widget's own table byte, no diode
reference; none of `g_system_command_table`'s 279 handler-function
addresses fall inside the diode-matrix code cluster
(`0x2003b000`-`0x2003f000`). Bits 19-23 (the non-existent top-row N/A
pads) are likewise only ever touched by the `FUN_2003c70c` mechanical
echo, never gated on.

## Living reference: region code from diodes 404/407/410/413

4-bit index = `8×D404 + 4×D407 + 2×D410 + 1×D413` (1 = diode present, 0
= absent), looked up in `DAT_2003c7fc` → `region_code`. Only 7 of 16
combinations produce a non-zero code:

| D404 | D407 | D410 | D413 | Index | Region code | Notes |
|---|---|---|---|---|---|---|
| — | — | — | — | 0 | 0 (default) | |
| — | — | — | ✓ | 1 | 1 | |
| — | — | ✓ | — | 2 | 2 | |
| — | — | ✓ | ✓ | 3 | 0 | |
| — | ✓ | — | — | 4 | 3 | |
| — | ✓ | — | ✓ | 5 | 4 | |
| — | ✓ | ✓ | — | 6 | **5** | Combined with D416 → general-coverage RX unlock |
| — | ✓ | ✓ | ✓ | 7 | **6** | Combined with D416 → general-coverage RX unlock |
| ✓ | — | — | — | 8 | 0 | |
| ✓ | — | — | ✓ | 9 | 7 | |
| ✓ | * | * | * | 10-15 | 0 | All remaining combinations with D404 present (except idx 9) |

Region-code TX/RX tables decoded so far: region 0 = textbook USA plan
(160/80/60(5.255-5.405)/40(7.0-7.3 full)/30/20/17/15/12/10/6m); region 1
identical to region 0; region 7 differs only in 160m's lower edge
(1.81 MHz vs 1.80 MHz) — plausibly EXP (the only version whose diode
combination can reach region-code 7). Regions 2/3/4 not yet pulled.
Country/market name correlation (JAP/USA/EUR/ITR/ESP/TPE/KOR/EXP) to
region codes 1-7 is otherwise still open — see Open Questions.

`get_type1_type2_designation` (`FUN_200134b4`, diode 403) returns ASCII
`'1'`/`'2'` — Icom's real "Type 1"/"Type 2" market designation, the same
one printed on their spec sheets/manuals.

## Found the menu-item table

`get_next_hidden_menu_item` (`FUN_2000d380`) indexes a table at
`DAT_2000e230` (`0x2018a698`) by menu item number (0-based, 216 items
total), 4 bytes/entry: `[format_type_byte, flag_byte, region_code_lo,
region_code_hi]`. `region_code` is passed to `is_feature_enabled_for_region`.

`format_type_byte` is a **value-formatting type code**, not a "gate
category" as first read: `0`=byte, `1`=word, `4`/`5`=BCD-like time/
frequency scaling, `8`=raw blob copy, `0xa`=min/max pair (52 items,
indices 111-215 — this is the band-edge table below), `0xb`=8 items
(indices 103-171). The correlation with region-gating is real but
indirect — band-edge settings need both a min/max format *and* region
gating, one doesn't imply the other. This table builds a byte-packed
**value representation** (CI-V get/set or EEPROM serialization), not a
display string — menu item *names* are a separate, still-unlocated
table (see [[firmware-update]] for the "Set Mode" string table lead).

## Band-edge table — the general-coverage RX unlock, confirms D416

`is_feature_enabled_for_region`'s 13-row×4-column table
(`FUN_2000edb0`/`FUN_2000f330`, base `DAT_2000f0c4` = `0x20190ecc`, row
`r` col `0` code = `0x8f + r*8`, min/max at struct offsets `+12`/`+16`):

| Row | Min (MHz) | Max (MHz) |
|---|---|---|
| 0 | 0.030 | 1.595 |
| 1 | 1.600 | 1.995 |
| 2 | 2.000 | 5.995 |
| 3 | 6.000 | 7.995 |
| 4 | 8.000 | 10.995 |
| 5 | 11.000 | 14.995 |
| 6 | 15.000 | 19.995 |
| 7 | 20.000 | 21.995 |
| 8 | 22.000 | 25.995 |
| 9 | 26.000 | 29.995 |
| 10 | 30.000 | 44.995 |
| 11 | 45.000 | 59.995 |
| 12 | 60.000 | **74.795** |

Row 0's min and row 12's max match the user's quoted D416 range
("0.030-74.8MHz") almost exactly — this is the **entire
general-coverage receiver spectrum split into 13 contiguous gated
segments**, not separate ham bands (row 2 happens to contain the
60m/5.3MHz allocation, but that's incidental). Several row-start codes
(`0x8f`, `0x90-0x93`, `0x95-0x96`, `0xdf-0xf6`) require **both** D416
present **and** region code exactly 5 or 6 (`FUN_2003bd04`) to return
enabled — direct confirmation D416 gates this table (necessary but not
sufficient — also needs region 5/6). One row-start code (`0x97`, row 1)
appeared to hit an unconditional deny regardless of diode state; not all
13 rows individually re-verified against the diode416+region5/6 rule.

Real-world confirmation: the service manual's official
`MODEL/VERSION/VERSION NUMBER/OPERATABLE BANDS` table (matching a
3rd-party photo of 8 labeled versions: `JAP #01`, `USA #02`, `EUR #03`,
`ITR #05`, `ESP #06`, `TPE #07`, `KOR #08`, `EXP #12`) shows
`EUR`/`ITR`/`ESP` get `HF/50/70 MHz` (70 MHz/4m unlocked) while the rest
get `HF/50 MHz` only — consistent with [[ic7300-signal-chain]]'s spec
numbers (70.000000-70.500000 MHz TX "depending on the transceiver
version", 60m as 5.255000-5.405000 MHz matching D405, general-coverage
RX as 0.030000-74.800000 MHz matching D416).

## Exhausted searches / dead ends (compressed)

- **D408** (7th session): exhaustive whole-firmware search (direct masks,
  `ubfx` at every plausible width/offset, register-mediated masks,
  EEPROM-persisted path) — no consumer found; only the mechanical
  bit-reversal echo touches it.
- **D411/D414/D417/D420** (8th session): same exhaustive sweep repeated
  across all 10 known firmware versions (`111`-`142`) — identical zero
  result on every version; not a gap introduced/fixed at some point in
  the update history.
- **D422** (2nd session, before its 5th-session resolution): three
  hypotheses ruled out — RX-table dispatch-table siblings, TX-denial
  strings (`"TX Inhibit"`, `"User Band Edge"`), and a structural parallel
  to the RX min/max-pair table.
- **D419** (3rd session, before its 5th-session resolution): not found
  in `FUN_2003c530`'s full call chain, nor via the EEPROM-persisted
  `0x3e44` path.
- **Bits 19-23** (non-existent top-row N/A pads, diodes 412/415/418/421's
  positions): checked across the entire firmware — genuinely unused,
  only mechanically echoed by `FUN_2003c70c`, never gated on by any real
  feature check.
- **P5 direction/mode registers**: checked all references to `PM5`/`PMC5`
  (`0xFCFE3300`/`0xFCFE3400` bases) and `PIBC5`/`PBDC5` (`0xFCFE7000`
  base) — none show a clean, simple `+0x14` (P5) offset. Very likely
  folded into one of two large generic bulk port-init functions
  (`FUN_200b4320`, `FUN_200b4494`) that configure dozens of ports
  together via bit-masked address computation — not reliably decodable
  by hand, best resolved via JTAG single-stepping.

## Factory reset / restore-defaults mechanism (10th session)

The real manual (`IC-7300_ENG_FM_12b.pdf` pp. 14-3/14-4) documents `MENU > SET > Others > Reset` →
**Partial reset** and **All reset**. This project had never previously located either function; found and
decompiled the whole chain this session (full evidence trail in
[notes/diode-matrix-history.md](diode-matrix-history.md)'s 10th-session entry — this section is the
condensed reference).

**Trigger chain**: pressing "Partial Reset"/"All Reset" in the touchscreen list-widget (same generic
infrastructure `notes/ui-menu.md` documents) shows a confirm dialog (`ui_show_message_dialog`, message IDs
`0x57`/`0x59` respectively, resolved via the confirm-string table at `0x2032c91c`, 76-byte stride, indexed by
message ID — also home to `0x54` "Reset All Edges?" and the generic, reused `0x52` "Reset to the default
settings?"). On confirm, each writes a value (`3` for Partial, `5` for All — Partial's write directly traced
to `*0x20390310 = 3` via `FUN_2002b818`; All's path to writing `5` into the same shared variable not traced to
a single instruction) into a "system mode request" byte that turns out to be the *same* variable
`system_mode_request_dispatch` (`0x2002a6b8`, already documented in `notes/kernel-rtos-history.md`'s 30th
session) polls and switches on. Its `case 3`/`case 5` each write an EEPROM record (tag `16000`) and then call
**`FUN_2002a4c8(reset_type)`** — `0` for Partial, `1` for All — the real bulk-reset function.

**The defaults table**: `FUN_2002a4c8` calls `FUN_2003ddd4(reset_type)`, which loops all 326 item codes
(`0`–`0x145`) calling `FUN_2003dcc0(item_code, reset_type)`. `FUN_2003dcc0` indexes a **64-byte-stride,
326-entry table at `0x20190ecc`** (the same base address this file's own confirmed 13-row band-edge min/max
table already uses, there as `DAT_2000f0c4` — one canonical per-item-code record table serving both roles) —
each record holds a live-value pointer, a type byte, and a literal default value, normally just copied
straight into the live setting. `FUN_2003dcc0` also unconditionally calls `FUN_20045800()` — the exact same
"wipe all 11 User Band Edge slots to `0xFFFFFFFF`" function found independently via the User-Band-Edge
screen's own "Reset All Edges" button (see below) — for **both** Partial and All reset, which appears to
contradict the manual's claim that Partial reset preserves User Band Edge data; not resolved, flagged as an
open discrepancy.

**Six item codes get a special-cased default instead of the table value** — this is where diodes/region_code
come back in:

| Item code | Special-cased when | Forced default |
|---|---|---|
| `0x73` | **D423** (bit 15 of the diode-scan-value alias `DAT_2003ea4c`) present | `1` |
| `0x22` | region_code == 0 (`diode_region_code_lookup(...) == 0`) | `3` |
| `0x32` | region_code == 0 | `1` |
| `0x79` | region_code == 0 | `0x6c` (108) |
| `0x4b` | Partial reset only (`reset_type == 0`) | copied from a live-mirror byte, not the table — not traced further |

None of the six checked item codes' gating tests bits 10/11/12/13/14 (D408/D411/D414/D417/D420) — see those
diodes' rows above; this is a genuinely new code path and it reconfirms, rather than merely repeats, the
existing negative result for all five.

**A seventh, `reset_type`-gated (not diode/region-gated) special case, found later while tracing
`SET → DISPLAY → MY CALL` for `notes/ui-menu.md`/`qemu-machine/`'s own boot-splash investigation**:
`FUN_2003dcc0`'s own outer condition for applying the *normal* table-default copy is
`(reset_type == 0) || (item_code != 0x71 && (item_code - 0x8f) unsigned > 0x67)` — i.e. for
`reset_type == 1` (All reset), item `0x71` (`MY CALL`, confirmed live-value pointer `0x203de53c` —
see `notes/ui-menu.md`) and the whole item-code range `0x90`-`0xf6` are skipped entirely (left
untouched), while for `reset_type == 0` (Partial reset) every item in that range, including `0x71`,
gets its normal default applied like anything else. **Read literally against this section's own
already-confirmed `0`=Partial/`1`=All labelling, that means All Reset preserves the configured
callsign and Partial Reset clears it — backwards from the intuitive expectation** (a full/all reset
wiping personal identity info, a partial reset leaving it alone). Not resolved — either real hardware
genuinely behaves this way (worth checking against the operating manual's own reset-scope table, pp.
14-3/14-4, next time it's in hand) or the `0`/`1` reset-type labelling itself needs re-verifying. The
`0x90`-`0xf6` range hasn't been individually characterized item-by-item; `0x71` is the only one
identified by name so far.

**"Reset All Edges" (User Band Edge screen)** — a separate, smaller, fully self-contained reset function,
not diode-gated: the screen's own "Reset All Edges" button (`FUN_2003331c`) shows a `0x54` confirm dialog
whose callback (`FUN_20045e68`) calls `FUN_20045800()`, writing `0xFFFFFFFF` to each of the 11 user band-edge
slots and recomputing the "current" slot index. This is the same `FUN_20045800()` the bulk Partial/All Reset
path above also calls unconditionally.

**Not found this session**: the hardware `CLEAR`+`V/M`-at-power-on forced-reset path the manual also
documents. `cold_boot_hw_init` does run three real, already-documented (`notes/kernel-rtos.md`, 30th session)
boot-time button-combo checks (`boot_check_mode1_combo`/`_mode5_combo`/`_challenge_response`) right where
the reset-request variable gets zero-initialized at cold boot, but all three are **service-mode entry**
combos (`MENU`+`FUNCTION`+shorted REMOTE jack, and two others) — not `CLEAR`+`V/M`, and none of them touch the
Partial/All-Reset variable. Genuinely not located this session, not just unconfirmed — open for a future
session or JTAG.

## 4630 kHz "Emergency Communication Mode" / Tuner — real feature, full chain traced, D420 confirmed as its gate (11th-16th sessions)

The user's domain-knowledge lead (Japan's IC-7300 variant has a special emergency-frequency behavior tied
to 4630 kHz, CW-only, layered on top of an all-regions "reduced power + relaxed tuner matching"
Emergency-Mode shape) is **confirmed real and fully traced end to end**. Full session-by-session narrative
(11th-16th) in the history file; this is the condensed, current state.

**The screen and its two items.** `MENU > SET > Others > Emergency` (非常通信) is one screen with two
independent checkboxes, per the real Japanese manual (user-supplied): `"4630kHz"` (forces CW, TX allowed
on 4630 kHz for emergencies) and `"Tuner"`/`"チューナー"` (expands the tuner's matching range to start
tuning even at SWR ≥ 3, and — specifically on the IC-7300 — limits max TX output to 50 W). A firmware
string (message `0x5b` in the `0x2032c91c` confirm-dialog table) independently confirms the 50 W claim in
these exact words. Flow: check a box → `OK` (warning dialog) → `"Restart to Set"` → mode commits. This is
a genuine, named, JP-flavored firmware feature (非常通信モード/"Emergency Communication Mode", real
bilingual strings at `~0x2032a000`/`~0x2035axxx`), not a coincidental string match.

**Full UI→commit chain, real code, ARM-ground-truth-verified**:
- Both warning dialogs (`0x5a` 4630kHz, `0x5b` Tuner) share one OK-callback,
  `emergency_screen_warning_ok_callback` (`0x20041cd0`), which just sets a "confirmed" flag.
- The actual commit happens at the separate "Restart to Set" tap: `emergency_mode_restart_commit`
  (`0x2000de64`) reads the pending 0/1 value, writes it to `0x2039021d`/`e` (the live checkbox-state
  bytes), and calls `FUN_2002b818(4)` — the same "system mode request" mechanism Partial/All Reset use
  with values 3/5 — which sets mode-request value 4.
- `system_mode_request_dispatch`'s mode-4 branch (already documented, `notes/kernel-rtos-history.md`'s
  30th session) commits the pending value into persistent status bits at `0x203de175` (bit 3 = 4630kHz,
  bit 2 = Tuner — the byte that also drives `FUN_2009060c`'s status-bar "4630kHz / TUNER" indicator, whose
  case 4/5/6 dispatch and bilingual field sourcing (`EMERGENCY MODE`/非常通信モード + a suffix) is fully
  traced) and runs a full task-restart sequence.
- **The "restart" is a genuine software-only soft restart, not a hardware reset — confirmed structurally,
  not by absence.** `cold_boot_hw_init` has exactly one caller in the whole image, unreachable from this
  chain; neither of this firmware's two watchdog-reset primitives (`FUN_20029ca4`/`FUN_20052bd0`) is ever
  invoked either. It's pure task teardown/reinit within the same continuously-running process — RAM,
  including the persistent status bits, is never cleared or reloaded. (This also closes an earlier "no
  EEPROM write found" question as moot: there was never anything to persist across.)
- `emergency_screen_checkbox_state_sync` (`0x2002ae6c`) runs the reverse direction, seeding the checkboxes
  from the live persistent bits when the screen opens — confirms `0x2039021d`/`e` are UI scratch, not
  themselves EEPROM-backed.

**D420 confirmed as the "4630kHz" checkbox's real visibility gate** — a direct raw diode-bit test, not a
`region_code`/`is_feature_enabled_for_region()` test, and the first ever consumer found for D420 across 16
sessions of searching. The gate sits well upstream of everything the tap-handler/render-dispatch/
checkbox-state code paths do (all independently confirmed clean of any diode/region test): the Emergency
screen's item list is built by **`settings_list_builder`** (`0x2003e5f0`), which calls
**`settings_item_visibility_filter`** (`0x2003e29c`) per item — an item that fails is never appended to
the list at all, not merely disabled — which for every category falls through to
**`settings_item_diode_region_gate`** (`0x2003e108`):
```c
else if (cVar1 == '\x03') {
    if (*(short *)(param_1 + 2) == 5) {                 // catalog index 5 = "4630kHz"
        if ((*DAT_2003ea4c & 0x4000) == 0) { uVar4 = 1; }   // D420 (bit 14) absent -> EXCLUDE
        else                                { uVar4 = 0; } // D420 present -> include
    }
    ...
}
```
`DAT_2003ea4c` is one of this file's four confirmed diode-scan-value aliases; bit `0x4000` = bit 14 = D420
per the confirmed scan-bit layout table. Catalog index 5 = "4630kHz", index 6 = "Tuner" (which hits no
case in the gate and is therefore always included, unconditionally) — both indices independently nailed
down via the checkbox-state-byte identity in `settings_list_item_kind_renderer` (`0x20042458`), the
per-item kind-based renderer found the session before the walker. The Emergency screen's own registry
category (`0x22`, 3 items: `val`=5/6/7) is tied to this specific screen by two convergent, non-inferential
matches (`settings_list_builder(0)` is called directly from `emergency_screen_warning_ok_callback`; the
item shape matches indices 5/6 exactly) rather than a literal string/label xref — strongly evidenced, the
one honest gap left in an otherwise fully byte-verified chain. Why 16 sessions of `references_to` sweeps
missed this: the walker's own key pointers are compile-time-fixed into a shared "current settings screen"
singleton whose *private* literal-pool copies live ~0x4400 bytes away in ROM from the symbols this project
had been sweeping — plus a genuine gap in Ghidra's own static xref database for the `DAT_2003ea4c` load
site itself (`references_to` on it misses the real consumer). Worth remembering for any future
"exhaustive sweep, zero hits" negative in this file.

**What's still open**: D423's own role (if any) in this specific chain remains unconfirmed — the broader
Emergency-Mode indicator/status-bit toggle machinery (`FUN_2009060c`, `FUN_20037c10`,
`system_mode_request_dispatch`'s mode-4 branch, `factory_reset_apply_defaults`) was checked end to end and
is region-independent code, consistent with (not proof of) Tuner-mode being all-regions; who writes
`DAT_2002b46c`/`DAT_2002b470` (read-only everywhere traced) is the same unreached-mechanism dead end
`notes/kernel-rtos-history.md`'s 30th session already hit for the mode-request byte itself. The
CW-mode-force/SWR≥3 threshold mechanics were not conclusively located (one partial lead,
`FUN_200132c4`, consumes 4630kHz's status bit but its exact semantics weren't confirmed). The separate,
all-regions `SET > Others > Emergency` *menu-category* item (distinct from this 非常通信 feature — see the
user's correction — sharing only the English word "Emergency") is structurally placed (a 24-byte-stride
table near `0x20190200`, generic page renderer) but its own gating was not traced. No raw Hz-integer
literal `4630000` (or kHz/float variants) exists anywhere in the image, and no "CW mode only" text was
found near the 4630 kHz strings — the CW-only restriction, if real, isn't spelled out in UI text.

## 17th-22nd sessions — 70 MHz/4m band and Japan's band plan: both purely `region_code`-driven, not gated by D417/D420/D423

Full band tables, all derivations, and the complete narrative now live in
[notes/band-plans.md](band-plans.md) (created 18th session as the consolidated region/band-plan
reference) and its own history file — summarized here for the diode-matrix-specific angle.

- **70 MHz ("4m") access is purely `region_code`-driven — D417 tests nowhere in the chain.** `region_code`
  (0-7, from D404/407/410/413 only) selects a per-region ROM band table via a plain pointer-array index;
  exactly region codes 2/3/4 have a 70 MHz row, matching the exactly-3-country `EUR`/`ITR`/`ESP`
  "HF/50/70 MHz" group — D417 (populated only on those same 3 variants) plays no role in the lookup or any
  reachable code path. Read as coincidental population overlap (same 3 PCBs), not a causal link. **D417's
  own real function remains unconfirmed** — new leads for a future session: the ~110 KB unexplored range
  of the (Ghidra-mis-bounded) `settings_list_builder` function, a non-band-edge EUR/ITR/ESP-specific
  behavior (compliance string, duty-cycle/power table), or live JTAG toggling of bit 13.
- **The region_code-to-country mapping bug (Open Questions 1/2, years open) is fixed.** The old "derived
  arithmetic hypothesis" computed the raw 4-bit diode-weight index (0-15) and used it directly as
  `region_code`, silently skipping a real lookup-table indirection (`region_code_lookup_table`,
  `0x20198a40`) — that's why it produced out-of-range values. Redone through the real lookup: `EUR`=2,
  `ITR`=3, `ESP`=4, `TPE`=5, `KOR`=6, `EXP`=7, `USA`/`JAP`=0 — see band-plans.md's own mapping table for
  the full per-variant derivation. Region 5 (TPE)/6 (KOR)'s full band tables were also hand-decoded and
  byte-verified for the first time this pass.
- **Japan's real, JARL-matching band plan exists in ROM** (`tx_band_table_jp_narrow_region0_override`,
  `0x20198940`) but **is not gated by D420 or D423 anywhere in the table-selection code** — it loads
  identically for USA and JAP whenever `region_code==0`. The real enforcement path and real consumer were
  both eventually traced (across 3 further sessions): a single classifier, `classify_frequency_to_band`
  (`0x20013218`, 21 call sites), underlies broad frequency validation everywhere and has a second mode
  that classifies against the JP table specifically; its one real caller is `band_edge_beep_check_and_fire`
  (Icom's real "Band Edge Beep" menu feature), which only uses JP-table mode when a flag exceeds 1 — and
  that flag is **factory-reset item `0x22`'s own default, forced to mode `3` whenever `region_code==0`**,
  i.e. for USA and JAP hardware alike. **D420/D423 play no role anywhere in this chain** — full derivation
  in `notes/band-plans.md`'s own "Japan's real band plan" section and its history file.
## Open questions

1. ~~Country/market name correlation to internal region codes 1-7~~ — **resolved, 18th session**
   (externally corroborated, 19th session): `USA`/`JAP`→0 (share a table — Japan's real distinguishing
   diodes are D420/D423, not a unique region_code), `EUR`→2, `ITR`→3, `ESP`→4, `TPE`→5, `KOR`→6, `EXP`→7.
   The old arithmetic mapping was skipping a real lookup-table indirection (`region_code_lookup_table`,
   `0x20198a40`), which is why it produced out-of-range values. EUR/ITR/ESP/KOR are independently confirmed
   against real published national band plans (exact numeric matches); `TPE`=5 still rests on
   diode-derivation alone. `region_code` 1 remains unclaimed by any of the 8 documented variants — see
   Open Question 2. Full derivation and every region's band table: `notes/band-plans.md`.
2. ~~Icom's public "Version #" numbering goes at least to 12, but the internal 4-bit region code only
   reaches 7~~ — **mostly resolved, 18th session**: the lookup table deliberately maps every combination
   not used by a real named variant to region_code 0 (by design, not a gap). The one genuine remaining gap
   is `region_code` 1, unclaimed by any of the 8 documented variants — either an undocumented 9th variant
   exists (Icom's numbering has gaps at `#04`/`#09`/`#10`/`#11`) or region_code 1 is defined but unused.
3. ~~What `DAT_2003c85c` (diode 416's second lookup, indexed by `DAT_2003c800+2`) actually drives~~ —
   **resolved, 9th session.** `DAT_2003c800+2` is the region_code byte. `DAT_2003c85c`'s lookup feeds
   `FUN_2000a5f0(10)`, one branch of a generic ~10/11-item menu-cycle "is item N enabled" gate — but since
   the table's only non-zero entries are at indices 13/15 and region_code never exceeds 7, **this
   D416-gated path can never fire on any real diode combination** — a real consumer, but dead in practice.
4. ~~Where diode 403's raw bit value (`FUN_2003c5c4`'s return) actually gets used~~ — **resolved, 9th
   session.** Its only caller, `get_type1_type2_designation`, has exactly 3 callers, all using the
   `'1'`/`'2'` return purely as a 49-vs-50-tone CTCSS-cycle bound. No other consumer exists — D403 doesn't
   reach band/TX logic through this path.
5. ~~What the `FUN_2003c530` bit-17/18 (D406/D409) classification byte is read by~~ — **resolved, 9th
   session.** It's mirrored into the live "DSP shadow config" struct (`0x203def00`) and pushed live to the
   DSP chip over SCIF5 every tick by `dsp_param_sync_tick()` — a real, continuously-synced DSP
   configuration input, not local UI/CPU-side state.
6. Which of the ~40+ numeric item codes (`0x24`-`0xf6`) in `FUN_2003df34`'s gatekeeper corresponds to which
   named feature/menu item — specifically, whether any of them is Japan's "Emergency Mode" (an early D423
   hypothesis). **Still open**, despite extensive searching across 4 sessions (9th-13th): no static link
   was found from the `~0x2032f000`-`0x20360000` name-string pool to either the 216-item value table or the
   326-item factory-reset table's item codes (the same "computed-table-access wall" this project has hit
   elsewhere) — checked `0x22`/`0x32`/`0x71`/`0x73`/`0x79`/`0x94`/`0xe5` individually, none read
   "Emergency"/4630/tuner (`0x73` = "Display Language" instead). The real Emergency Communication Mode
   feature is now firmware-confirmed directly via strings and a full UI chain (see the 4630 kHz section
   above) but was never tied to a specific item code in either table — the likeliest explanation is that
   it isn't gated through either table at all. Full multi-session search trail in the history file.
7. `chunk5_tail.bin` (~1.6-1.7 MB, LZSS-compressed, likely a second processor's firmware image) and
   `base.dat`/font-resource blocks remain completely unchecked for diode consumers — low-probability
   locations, but not literally verified for D408/D411/D414/D417/D420.
8. ~~Why P5's direction/mode registers weren't found being set~~ — partially resolved, see "Exhausted
   searches" above; final answer needs JTAG (see [[hardware-debug-access]]).
9. **10th session.** `FUN_2002a4c8`'s Partial/All-reset defaults pass calls `FUN_20045800()` (wipes all 11
   User Band Edge slots) **unconditionally, for both reset types** — but the real manual says Partial reset
   preserves User Band Edge. Not reconciled: either something else restores/skips this specifically for
   Partial reset (not found), the call is gated on something upstream of what was traced, or the manual's
   wording and the firmware's real behavior simply don't match. Worth a fresh, targeted look.
10. **10th session.** The hardware `CLEAR`+`V/M`-held-at-power-on forced-reset path the manual documents
    was not found. The three real boot-time button-combo checks in `cold_boot_hw_init` are all
    **service-mode entry** combos, not this one, and none touch the Partial/All-Reset request variable.
    Genuinely unfound, not just unconfirmed — a real target for a future session or JTAG.
11. ~~D420's role~~ — **RESOLVED, 16th session.** Found the settings-list walker
    (`settings_list_builder`) and its per-item filter chain down to `settings_item_diode_region_gate`,
    which tests D420 (bit 14, `0x4000`) directly to decide whether the "4630kHz" checkbox is included in
    the Emergency screen's item list at all. See the "4630 kHz Emergency Communication Mode / Tuner"
    section above and D420's living-reference row for the full, verified chain. Full multi-session search
    trail (14th-16th) in the history file.

All remaining unresolved diodes (D408, D411, D414, D417) are good candidates for live JTAG verification
(toggle the position, watch what changes) rather than further static searching — the same conclusion
earlier sessions reached for D419/D422 before those turned out to have real consumers, so none of this is
necessarily final.

**New, 13th session.** `notes/kernel-rtos.md` does **not** actually document an EEPROM-settings-load-at-
boot mechanism (zero mentions of "eeprom" in that file). The real EEPROM catalogue lives in
`notes/eeprom-catalogue.md`, whose one documented combined-settings loader (`FUN_2006cb84`) does not cover
the `0x203de174` struct region the Emergency-Mode status bits live in (ruled out by address arithmetic).
Where (or whether) `0x203de175` bits 2/3 are EEPROM-backed at all remains open.
