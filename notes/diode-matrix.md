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
| D417 | middle, col3 | 13 | ❓ unknown | **Exhaustively searched across all 10 known firmware versions** — no consumer found. Populated only on EUR/ITR/KOR (`[#03][#05][#06]`) per parts list |
| D419 | bottom, col2 | 6 | ✅ confirmed | **Selects the TX frequency-range table in `FUN_2003bd34`/`FUN_2003be94`, together with D422.** Present (D422 absent) → continuous TX 0.1–74.8 MHz, exactly the mod-guide's "open TX" figure. Populated on all versions per parts list |
| D420 | middle, col2 | 14 | ❓ unconfirmed | User hypothesis: language-related. **Exhaustively searched across all 10 known firmware versions** — no consumer found. **Confirmed Japan-only (`Only [#01]`) per parts list** — matches D423, supporting the user's original "D420/D423 both JP-only" domain-knowledge lead |
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
| D417 | `[#03]`, `[#05]`, `[#06]` — EUR/ITR/KOR | high |
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
through `142`). **Re-confirmed fresh, 9th session**, via 3 independent
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

## 4630 kHz "Emergency Communication Mode" — real, firmware-confirmed feature (11th session)

The user's domain-knowledge lead (Japan's IC-7300 variant has a special emergency-frequency behavior tied to
4630 kHz, CW-only, layered on top of the all-regions "reduced power + relaxed tuner matching" Emergency-Mode
shape) is **confirmed to exist in the firmware as a real, named feature** — this is not a coincidental string
match. Full evidence trail in `notes/diode-matrix-history.md`'s 11th-session entry; condensed here.

**Real strings found** (all inside the same `~0x2035a000`-`0x2035f000` UI name/message-string pool this file's
Open Question 6 already tracks):
- `"You can transmit on 4630 kHz for "` (`0x2035e518`) + `"Emergencies."` (`0x2035ea7c`) — concatenated at
  render time into **"You can transmit on 4630 kHz for Emergencies."**, a real UI message string.
- Three standalone `"4630kHz"` strings (`0x20359ae0`, `0x2035fac0`, `0x2032a036`).
- `"EMERGENCY"` (`0x2035a5c4`) / `"Emergency"` (`0x2035a81c`) as standalone strings.
- **`"EMERGENCY"`** also appears as a plain item in what reads as a real **menu-category name list** at
  `0x2035a500`-ish: `...RX..TX..TX DELAY....DISPLAY.EMERGENCY...KEYER MEMORY....RTTY MEMORY.RTTY...` — sitting
  in sequence with real, known IC-7300 SET-mode category names, strongly suggesting "Emergency" is a genuine
  named menu category (plausibly `MENU > SET > Others > Emergency`), not just a status message.
  **Correction, per the user's own domain knowledge**: this `SET > Others > Emergency` category is a
  **different, separate feature from the JP-only 4630 kHz mode below** — plausibly the standard (all-regions)
  "reduced power + relaxed tuner matching" Emergency-Mode-shaped behavior the user described, not the JP-only
  4630 kHz/CW one. Don't conflate the two just because both use the English word "Emergency" — they are
  presented here as two separate bullets/threads for a reason, and this note previously ran the risk of reading
  as if they might be the same menu item. Whether this generic "Emergency" category itself is diode-gated
  (region_code, or none at all if it's genuinely universal) is unexamined — a real, distinct open item, not
  covered by anything below.
- A dedicated **bilingual status-indicator string cluster** at `0x2032a000`: Shift-JIS `"非常通信モード"`
  ("Emergency Communication Mode") with **no English counterpart** (blank field where a translation would sit
  — every other row in the same cluster, e.g. `"TUNER"`/`"チューナー"`, has both), plus `"4630kHz"`, `"TUNER"`,
  and two combined display strings `"4630kHz / TUNER"` and `"4630kHz / チューナー"`. The missing English
  translation is itself circumstantial evidence this is a JP-only display item, consistent with D423/D420 being
  JP-only diodes.

**Real code consumer found for the status-indicator cluster**: `FUN_2009060c` (a status-bar/icon-text
renderer) switches on a byte read from a global UI-status struct (base `DAT_20090a18` → `0x2040376c` this
build, field `+0x7f8` selects the icon "kind", `+0x7f9` selects English/Japanese by a separate display-language
byte). **`case 6`** is the "4630 kHz / Tuner" indicator — it picks between the English-less Japanese string
cluster above based on the language byte, confirmed by walking the actual ARM listing at `0x20090790`-`0x200907cc`
(`ldr r2,[0x20090a40]` etc., literal-pool loads resolving into the `0x2032a0xx` cluster). This confirms the
indicator is live UI code, not dead/unreferenced data.

**What was not found, honestly** (real negative results, not gaps left unchecked):
- **No raw Hz-integer literal `4630000`** (`0x46A6D0`) anywhere in the image — checked via `mcp__ghidra__memory`
  decimal search across the whole program. Sanity-checked the search method itself against three *known-good*
  band-edge literals from this file's own tables (`7300000`, `7000000`, `5255000` all hit cleanly, at the
  expected `0x20193xxx`/`0x20198xxx` table addresses) — so this is a real, clean negative for that specific
  representation, not a search-method failure. Also tried `4630` (kHz-scale) and `4.630`/`4630000.0` (float/
  double) — zero hits on all of them.
- **No "CW mode only" (or similar) text** found anywhere near the 4630 kHz strings — searched `"CW mode"`,
  `"only in CW"` directly, plus manually read ~700 bytes of surrounding string-pool context. The user's
  CW-only recollection is not disconfirmed, just **not independently corroborated by any firmware string** —
  the mode restriction, if real, isn't spelled out in UI text the way the frequency and tuner/emergency wording
  is.
- **No conclusive trace from D423 (or region_code) to this feature's trigger code.** The `case 6` icon-kind
  byte (`iVar2+0x7f8`) is a UI-status field, not (as far as traced) itself gated by any diode-scan-value alias —
  a search for its setter(s) turned up only what looks like an unrelated code cluster (byte-for-byte address
  coincidence, not a real match; flagged honestly rather than forced into a narrative). **Item `0x73`, the one
  concretely-known D423 consumer, is ruled out** (see the D423 living-table correction above — it's "Display
  Language", not this feature). None of the other D423-gated item codes checked this session (`0x22`, `0x32`,
  `0x71`, `0x79`, `0x94`, `0xe5`) have name strings or record shapes ( checked via the same 326-item table
  read used for item `0x73`) matching "Emergency"/"4630"/tuner either — `0x94`/`0xe5` in particular are
  word-typed records with plausible-looking frequency-shaped values but no name string at all, and `0xe5`'s own
  gating in `is_feature_enabled_for_region` tests bits 0/5 (D401/D416) only, matching that function's already-
  documented general-coverage-style gating, not D423.
- **The broader, all-regions "reduced power + relaxed tuner matching" Emergency-Mode-shaped behavior the user
  described was not investigated this session** — no time was spent searching for TX-power-scaling tables or
  tuner SWR/matching-tolerance constants. Genuinely open, not a negative result either way; a real next step
  for a future session (start from `notes/kernel-rtos.md`'s `tuner_engage_gpio_toggle`/
  `tuner_freq_and_txstate_precheck` section per the task brief that prompted this one).

**Net assessment**: this is a real, named, JP-flavored ("Emergency Communication Mode" / 非常通信モード)
firmware feature tied concretely to 4630 kHz and to the tuner — the user's lead is confirmed to exist, not a
coincidence or misremembering. What remains unconfirmed is the precise gating mechanism (D423-specific vs.
something else), the CW-only restriction specifically, and any link to a broader all-regions reduced-power
mode. Worth a dedicated follow-up session with JTAG or a deeper static trace of `FUN_2009060c`'s callers and
whatever sets its icon-kind byte to 6.

**Correction, straight from the user**: the standalone `"EMERGENCY"`/`"Emergency"` SET-menu-category strings
(`0x2035a5c4`/`0x2035a81c`) are **not** the same feature as this 4630 kHz/非常通信モード one — two genuinely
separate things that happen to share the English word "Emergency." Treat the menu-category "Emergency" as its
own, likely all-regions, still-untraced feature (see the correction inline above), and this section's
非常通信モード/4630kHz/tuner cluster as the specific JP-only one. Don't merge them in any future write-up.

## 12th session — systematic Shift-JIS sweep tool; corrects the "no English counterpart" claim; gating chain traced to a still-unwritten trigger variable

Built `tools/sjis_string_scan.py` (documented in `tools/README.md`) to turn the 11th session's by-hand "JP
string with no adjacent English = signal" technique into a repeatable whole-image sweep, per this session's
task brief. Ran it against `scratch/unpacked/142/body.bin`. Full numbers, methodology, and the tool's own
documented caveats are in the new `tools/README.md` section; the diode-relevant findings are below.

**Correction to the 11th session's headline claim**: 非常通信モード (`0x2032a014`) **does have a real English
pair** — `"EMERGENCY MODE"` (`0x20329ff2`) — contrary to that session's "no English counterpart" read. Hex-
dumping the full `0x20329f48`-`0x2032a0e0` cluster directly (not just the immediate two fields the 11th
session's by-hand read stopped at) shows a flat sequence of fixed **0x22-byte (34-byte)** NUL-terminated,
space-padded slots, consistently alternating English-then-Japanese: `"- ADJUST -"` / `"ALL RESET"` /
`"オールリセット"` / `"PARTIAL RESET"` / `"パーシャルリセット"` / **`"EMERGENCY MODE"`** / **`"非常通信モード"`**
/ `"4630kHz"` (language-neutral, stands alone) / `"TUNER"` / `"チューナー"` / `"4630kHz / TUNER"` /
`"4630kHz / チューナー"`. The 11th session's method checked only the single NUL-delimited span immediately
before 非常通信モード's own text — which is trivially empty by construction, since that text begins
immediately after its own field's opening NUL — and read that emptiness as "no translation." The real
preceding *slot* (bounded by the *next* NUL back, i.e. skip one field further) is "EMERGENCY MODE". First
implementation of the sweep tool's own pairing check made the identical off-by-one mistake before being
caught by cross-checking the two known-good hits against a hand-verified expectation and fixed (see the
tool's own `find_preceding_field` docstring for the exact before/after).

**This is independently confirmed by live code, not just structural pattern-matching.** `FUN_2009060c` (the
status-bar/icon-text renderer this file already knew handles this cluster's `case 6`) actually has a `switch`
over 7 cases (0-6); cases 1/2 use `DAT_20090a2c`/`DAT_20090a30` + `lang_byte*0x22` to select
ALL RESET/オールリセット and PARTIAL RESET/パーシャルリセット respectively (`lang_byte`=0 → English slot,
=1 → next slot = Japanese) — confirming `+lang*0x22` is the function's own real English/Japanese selection
convention, not something this session invented. **Cases 4, 5, and 6 all read the exact same "value" field**,
`DAT_20090a20 + lang_byte*0x22` — and `DAT_20090a20`'s stored content is `0x20329ff2`, i.e. **the address of
"EMERGENCY MODE" itself** (`+0x22` = `0x2032a014` = 非常通信モード for `lang_byte==1`). So the same shared
code path that already-confirmed-real cases 1/2 use for their bilingual pairs uses this exact field for the
Emergency indicator too — case 4 combines it with the constant `"4630kHz"` suffix, case 5 with `"TUNER"`
(bilingual pair), and case 6 (the already-confirmed one) with the pre-combined `"4630kHz / TUNER"` pair. All
three are genuinely the same "Emergency Mode" value, just with a different trailing "kind" fragment appended.

**New: found and traced the actual case-selector (icon "kind" byte) setter, closing a gap the 11th session
explicitly flagged as a dead end.** The byte at `iVar2+0x7f8` (`iVar2` = `*DAT_20090a18` = `0x2040376c` this
build, so the byte lives at `0x20403f64`) is written by `FUN_20037c10`, which derives cases 4/5/6 from bits
2 (`0x4`) and 3 (`0x8`) of a *different* status byte at `0x203de175` this build (aliased in the live project
as `DAT_2002b45c[1]` and `*(DAT_20038290+1)`/`*(DAT_200382e4)` depending which function reads it — same
established "several DAT_ aliases, one physical struct" pattern this file already uses for the diode-scan
value):
- bit 2 set, bit 3 clear → case 5 (`"EMERGENCY MODE"` + `"TUNER"` / `"非常通信モード"` + `"チューナー"`)
- bit 3 set, bit 2 clear → case 4 (`"EMERGENCY MODE"` + `"4630kHz"` / `"非常通信モード"` + `"4630kHz"`)
- both set → case 6, the already-confirmed full combo (`"EMERGENCY MODE"` + `"4630kHz / TUNER"` /
  `"非常通信モード"` + `"4630kHz / チューナー"`)
- neither set → indicator not shown (falls through to whatever case 0-3 last set, e.g. a reset indicator)

**Both bits trace to already-documented functions, tightening the chain further**:
- `factory_reset_apply_defaults` (`0x2002a4c8`, named 10th session) clears both bits (`pbVar1[1] &= 0xa3`,
  which clears bits 2/3/4/6) as part of Partial/All Reset — confirming `DAT_2002b45c` (its own `pbVar1`) is
  the same struct base as `0x203de174`.
- `system_mode_request_dispatch` (`0x2002a6b8`, named/fully documented 30th kernel-rtos session) *sets* both
  bits, in its `uVar11==4` branch (mode-request value 4, not previously called out by name in that function's
  own comment): bit 3 is toggled via `FUN_2002a5f8(...)` when the byte's current bit-3 state doesn't match a
  target read from `*DAT_2002b46c`; bit 2 is set directly from `*DAT_2002b470 & 1`.

**Hits the identical, already-documented dead end one level further down, not a new one**: `DAT_2002b46c` and
`DAT_2002b470` are read-only everywhere in the image (2 READ references each, both inside
`system_mode_request_dispatch`/its sibling, zero WRITEs) — the same "populated via some message/event
mechanism this session didn't trace, or needs live JTAG" wall `notes/kernel-rtos-history.md`'s 30th session
already hit trying to find who writes `DAT_2002a158` (the mode-request byte itself, request value 4 being
this exact branch). Not re-litigating that dead end — just confirming this newly-found branch shares it, one
variable further into the same unreached mechanism.

**D408/D411/D414/D417/D420 checked against this entire newly-traced chain — none found, real negative.**
Read the full decompiled bodies of all four functions in this chain (`FUN_2009060c`, `FUN_20037c10`,
`system_mode_request_dispatch`, `factory_reset_apply_defaults`) end to end: zero references anywhere to any
diode-scan-value alias (`DAT_2003c7f8`/`DAT_2003c800`/`DAT_2003ea4c`/`DAT_2003c858`) or to `region_code`. This
whole Emergency-Mode-indicator toggle mechanism is, as far as this chain goes, region-independent code — which
is at least consistent with (though doesn't prove) the user's own separate point that the broader "reduced
power + relaxed tuner matching" behavior might be an all-regions feature, not something D423/D420-gated. It
does NOT rule out the *display string* or the *4630 kHz-specific* framing being JP-only some other way (e.g.
gated earlier, at whatever populates `DAT_2002a158`/`DAT_2002b46c`/`DAT_2002b470` — still unreached).

**One more string confirmed as the same already-known feature, not a new one** — worth recording as a real
demonstration of the sweep tool's own documented pairing-heuristic limits (see `tools/README.md`): the sweep
flagged `0x2035fac0` (`"4630kHz非常通信用周波数での送信ができ..."`) as having no adjacent English, but
`references_to` on it resolves to `0x2032e3fc` — field `+0x28` of record index 90 in the already-documented
`0x2032c91c`, 76-byte-stride bilingual message table (`notes/firmware-update.md`). That same record's `+0x04`
field is `0x2035e518`, the already-known `"You can transmit on 4630 kHz for "` string, and `+0x08` is
`"Emergencies."` — i.e. this Japanese text **is** genuinely paired with the already-confirmed English message,
just through this pointer-table mechanism rather than physical proximity, which is exactly the kind of
"paired but not adjacent" case the sweep tool's own documentation warns it can miss.

**Bounded check on the separate, still-open "Emergency" SET-menu category** (`0x2035a5c4`/`0x2035a81c`, per
the user's correction above that this is a distinct feature): both strings resolve via `references_to` to a
single record (`0x2035a5c4`→`0x2019024c`'s pointer field, `0x2035a81c`→`0x20190250`'s) inside a **0x18-byte
(24-byte) fixed-stride table** starting around `0x20190200` — each record is `{packed byte flags/type-tag
(mostly `0x09`), 4 name-pointer fields, 1 callback function pointer}`. The Emergency record's callback,
`0x20042f3c`, is a generic "render a page of up to 4 sub-items" SET-menu-category handler shared by several
neighboring category records (not specific to Emergency) — decompiled it, and it's parameterized entirely by
a page-type byte read from elsewhere (`DAT_20042658+0xb`), not by anything in the Emergency record itself.
Genuinely did not trace further (would need the specific item-list data this generic renderer walks for the
Emergency page specifically, out of scope for the remaining session budget) — this is a real, if partial,
structural placement (its home table + a generic, non-diagnostic renderer), not a gating answer. Still open,
not diode-tested by what was found; don't assume diode-gated per the user's own caution.

**Files touched this session**: `tools/sjis_string_scan.py` (new), `tools/README.md` (new section documenting
it), `notes/diode-matrix.md` (this section, Open Question 6 addendum below), `notes/diode-matrix-history.md`
(12th-session narrative entry). No git commit made — left for the user's own review. No ARM/Thumb
disassembly gaps were hit this session (all code inspected was already disassembled by Ghidra), so nothing
was queued in `scratch/armthumb_fix_requests.txt`.

## Emergency Mode / Tuner (all-regions) — the shared 非常通信 screen (13th session)

**Ground truth from the user, pasted directly from the real Japanese manual** (non-negotiable — this
supersedes any structural guess in the 11th/12th-session entries above about "4630kHz" and "Emergency
Mode" possibly being separate menu items): `MENU > SET > Others > Emergency` (非常通信) is **one screen**
holding **two independent checkboxes** — `"4630kHz"` and `"Tuner"` (`"チューナー"`) — not two categories.
Flow for either: touch the checkbox → touch `OK` (dismisses a warning dialog) → touch
`"≪再起動してセット≫"` ("Restart to Set", a separate button) → **the radio actually reboots** → the mode is
now active. Un-checking both and repeating the same flow cancels it. The manual states outright: 4630kHz
mode **forcibly switches the operating mode to CW**; Tuner mode **expands the tuner's matching range so it
starts tuning even at SWR ≥ 3** (normally requires SWR ≤ 3) and, **specifically for the IC-7300, limits max
TX output to 50 W**. This directly matches — and firmware-confirms — the user's original domain-knowledge
claim from before this project started tracing either feature.

**New firmware string found this session, independently confirming the manual's "50 W" claim**: message ID
`0x5b` in the `0x2032c91c` (76-byte-stride) confirm-dialog table reads, in full: *"Expands max. matching
ratio for Emergencies. Output power is limited to max. 50W. Danger! Never get close to the antenna during
TX."* — assembled from 4 fragments at `0x2035f7dc`/`0x2035f8a0`/`0x2035f7b8`/`0x2035e6f8`. This is the
Tuner-mode warning dialog, sibling to the already-known message `0x5a` ("You can transmit on 4630 kHz for
Emergencies.", the 4630kHz warning dialog). Both dialogs sit in the same message-ID range as the
already-documented Partial Reset (`0x57`)/All Reset (`0x59`) confirms, right next to each other — real,
concrete evidence this is the same generic "Others" list-widget infrastructure the Reset thread already
mapped, just two more items in it.

**Real code chain traced, verified against ARM ground truth (Ghidra listing gap hit and fixed the
established way)**:
- Both warning dialogs are wired via `ui_show_message_dialog(0x5b, emergency_screen_warning_ok_callback, 0)`
  (`0x20041d38`) and `ui_show_message_dialog(0x5a, emergency_screen_warning_ok_callback, 0)` (`0x20041d4c`)
  — **the exact same confirm-callback for both**, confirmed twice independently via
  `arm-none-eabi-objdump` against `scratch/unpacked/142/body.bin` (Ghidra's own listing/decompile silently
  degrades `0x20041cfc`-`0x20041d68` to raw undefined bytes — a real ARM/Thumb-adjacent disassembly gap,
  queued in `scratch/armthumb_fix_requests.txt` for the user to apply). `emergency_screen_warning_ok_callback`
  (renamed from `FUN_20041cd0`) just sets a generic "confirmed" flag (`*(DAT_20042664+6)` = `*0x2039021e` = 1)
  — it does **not** itself distinguish which checkbox was confirmed; both items share this one OK-dismiss
  step.
- The actual bit-specific commit happens at the **separate** "restart to set" button tap, traced to
  `emergency_mode_restart_commit` (renamed from `FUN_2000de64`) — a peer, in the same generic "Others"
  OK-handler dispatch table (`0x2018b1a8`-ish literal-pool cluster) as the already-documented Partial Reset
  trigger (`FUN_2000de50`, unconditionally `FUN_2002b818(3)`). `emergency_mode_restart_commit`: guarded by
  `*DAT_2000e238 & 0x4000` (probably "Emergency screen is open"), reads a pending 0/1 value from
  `*(DAT_2000d23c+3)`, commits it via `*DAT_2000e24c` — confirmed to be the **exact same physical address**
  as `*DAT_2002b46c` (`0x2039021d`), the "4630kHz target" byte `system_mode_request_dispatch`'s already-
  documented `uVar11==4` branch reads — then calls `FUN_2002b818(4)`, which writes mode-request value `4`
  into `DAT_2002a158`/`DAT_2002b4fc` (the same "system mode request" variable Partial/All Reset use with
  values 3/5) and runs the same soft-restart machinery. **This confirms the predicted shape concretely, for
  the 4630kHz side**: menu OK/restart button → stages a pending value → requests system-mode-4 →
  `system_mode_request_dispatch`'s mode-4 branch (already documented, 12th session) commits it into the
  persistent status bits (`0x203de175` bit 3) and runs the full task-restart sequence.
- **`emergency_screen_checkbox_state_sync`** (renamed from `FUN_2002ae6c`) runs the *opposite* direction,
  guarded by the same `0x4000` bit under a different alias (`DAT_2002b4e0`): when the Emergency screen opens,
  it reads the LIVE persistent bits (`0x203de175`, bit 3=4630kHz/bit 2=Tuner) and copies them **into**
  `0x2039021d`/`0x2039021e` — i.e. it seeds the checkbox display from the currently-committed mode. This
  confirms those two staging bytes are UI scratch state, not themselves EEPROM-backed, and explains why they
  read as "always in sync" with the persistent bits outside of an active edit.

**Honest gaps — real negatives, not forced**:
- **No EEPROM write found anywhere in this specific commit chain.** `FUN_2002b818` → `FUN_20041dd8` (sets a
  notification/interrupt-guarded flag, not EEPROM) / `FUN_20017390` (clears an unrelated 11-slot pointer
  array) — neither calls `FUN_2001e510`/`FUN_2001e484`. The predicted "menu OK → EEPROM write" half of the
  task 2 hypothesis is **not confirmed** — genuinely not found this session, despite a real, concrete restart
  trigger being found.
- **Could not isolate a Tuner-specific sibling of `emergency_mode_restart_commit`** (something committing
  into `0x2039021e`/`*DAT_2002b470` the same way). Checked the two next candidates in the same OK-handler
  cluster (`FUN_2000de28`→`FUN_20030008`, unrelated feature entirely; `FUN_2000dea4`, mode-1 request, also
  unrelated) — neither fits. Not found; open for a future session.
- **Task 3, the "boot-time EEPROM populate" dead-end closure, is NOT closed.** Checked `notes/kernel-rtos.md`
  directly — contrary to this session's own task brief, it does **not** document an EEPROM-settings-load-at-
  boot mechanism (zero "eeprom" mentions in the file at all — a correction to the brief's assumption, not a
  finding). Checked `notes/eeprom-catalogue.md`'s one documented combined-settings loader
  (`FUN_2006cb84`, base `0x203b31e0`, ~0x1a80 bytes) — ruled out directly by address arithmetic
  (`0x203de174` is ~0x2af94 bytes past that struct's end, nowhere near it). Spot-checked two `PARAM`-tagged
  references to the `0x203de174` struct base (`FUN_200092f0`, `FUN_200278f0`) — neither is an EEPROM call.
  Did not have budget to sweep the remaining ~50 of 73 total `FUN_2001e510` call sites the EEPROM catalogue
  flags as unsampled, nor to read `cold_boot_hw_init` line-by-line for this specific struct. **Net: the
  12th session's "no writer besides system_mode_request_dispatch's own mode-4 branch" dead end for
  `DAT_2002b46c`/`DAT_2002b470` still stands** — what's new this session is a concrete, real UI-side *caller*
  of mode-4 (`emergency_mode_restart_commit`), not the EEPROM round-trip itself.
- **Diode/region gating: real, clean negative, extended.** Every newly-traced function this session
  (`emergency_screen_warning_ok_callback`, `emergency_mode_restart_commit`, `emergency_screen_checkbox_state_sync`,
  `FUN_2002b818`, `FUN_20041dd8`, `FUN_20017390`, `FUN_20042f3c`, `FUN_20041f20`) — zero references to any
  diode-scan-value alias (`DAT_2003c7f8`/`DAT_2003c800`/`DAT_2003ea4c`/`DAT_2003c858`) or to `region_code`.
  Confirms this whole chain is region-independent code, consistent with (not proof of) the user's claim that
  Tuner-mode is all-regions. **Did not find a conditional-visibility check hiding the "4630kHz" checkbox for
  non-JP regions specifically** — checked the one plausible "is this item enabled" gate reachable from the
  generic renderer (`FUN_20041f20`, called for "type 2" items only) — it tests unrelated item codes
  (`0x5d`/`0x5e`/`0x5f`/`0x61`/`0x129`-`0x138`) against a **different** struct (base `0x203de4cc`, not
  `0x203de174`), and the checkboxes render as "type 1" items in `FUN_20042f3c`'s loop anyway, which never
  calls this gate at all — so it's very likely the wrong function regardless. The predicted
  region-conditional-visibility check for 4630kHz specifically remains **unconfirmed, not found**, not
  disconfirmed either.
- **D408/D411/D414/D417/D420**: none of this session's newly-traced functions reference any diode bit
  at all (not just these five) — same real negative shape as every other session, extended to this new
  chain, no new information either way.
- **Orange "E" badge rendering**: not investigated this session — genuinely no time spent, not a negative
  result.
- **CW-mode-force / SWR≥3 threshold**: not conclusively found. One real, new, partial lead: `FUN_200132c4`
  (`if ((*(byte*)(DAT_200134e0+1) & 8) != 0 && param_1 == DAT_200134f4) return 0`, called from
  `FUN_200132f0`, a "is candidate operating-mode value selectable" gate) is a genuine consumer of bit 3
  (4630kHz) of the same `0x203de175` status byte, beyond what any prior session found — but this session
  could not confirm its semantics actually amount to "force CW" (the excluded value's static RAM content,
  `0x46a5f0`, doesn't read as a small mode-enum constant, so either it's populated dynamically at runtime or
  this reading of the gate's direction is incomplete). Recorded as a real lead, not a confirmed closure. No
  SWR~3.0 threshold constant was searched for at all this session.

**Net assessment**: this session upgrades the 4630kHz/Tuner Emergency-Mode chain from "named feature +
disconnected status-bar indicator" (11th/12th sessions) to a real, traced, end-to-end UI action chain for
at least the 4630kHz side (checkbox → warning dialog → restart-to-set button → system-mode-4 request →
already-documented persistent-bit commit + restart), with a firmware-string-level confirmation of the
manual's 50 W claim as a bonus. The EEPROM-persistence half of the mechanism and the Tuner-specific mirror
of the restart-commit function remain genuinely open — flagged honestly rather than forced to fit the
predicted shape.

**Correction, 14th session — the "EEPROM-persistence gap" above doesn't need closing after all.** The
user (who has used this exact feature on real hardware) said the "restart" does not actually power-cycle
the radio — it only appears to. Traced this directly (see the 14th-session section below): the "restart"
never calls `cold_boot_hw_init` or either of this firmware's two hardware-watchdog-reset primitives. It's
pure software task-teardown/reinit within the same continuously-running process. **There is no EEPROM
round-trip to find, because the persistent status bits (`0x203de175`) are never at risk in the first
place** — RAM is simply never cleared or reloaded across this "restart." Treat the "no EEPROM write found"
bullet above as resolved (a real closure), not an open gap.

## 14th session — the "restart" traced end to end: a real, confirmed soft/warm restart, not a hardware reset; a deeper structural search for the 4630kHz visibility gate comes up empty, but finds the real per-item records

Two sharp, concrete follow-ups from the user, both chased directly against the 13th session's remaining
gaps.

### 1. The restart is a genuine software-only soft restart — closed, not open

Traced `emergency_mode_restart_commit` → `FUN_2002b818(4)` → `system_mode_request_dispatch`'s mode-4
branch all the way down through the ~80 unconditional function calls in that function's tail (the "full
subsystem restart" its own 30th-session plate comment already described). **None of them call
`cold_boot_hw_init` (`0x2002afc0`), or either of this firmware's two documented hardware-watchdog-reset
primitives** (`FUN_20029ca4`/`FUN_20052bd0`, both fully decompiled and confirmed in
`notes/firmware-update.md`'s "system restart mechanism" section — the standard RZ/A1H `WRCSR`/`WTCNT`/
`WTCSR` unlock-and-arm sequence at `0xFCFE0000`).

Confirmed this structurally, not just by absence: `cold_boot_hw_init` has **exactly one caller** in the
entire image (`0x2002b1d8`), inside `cold_boot_mode_dispatch` (`0x2002b1c8`, previously `FUN_2002b1c8`) —
which itself has exactly one caller (`0x2002b554`, inside `FUN_2002b29c`, the real top-level cold-
boot/power-state entry dispatcher, itself only reachable via message-dispatch per its own existing plate
comment, i.e. genuinely once per real hardware boot). `cold_boot_mode_dispatch`'s own body is: call
`cold_boot_hw_init()` once, then loop `while (system_mode_request_dispatch(), ...)` forever, picking an
idle-loop variant each iteration from `DAT_2002a4a4`. **`system_mode_request_dispatch` runs entirely inside
this same loop, on every iteration, for every request value including 4 — it never causes the loop (or the
function containing it) to exit or restart.** `FUN_20029ca4` (the function that does hold a real watchdog-
arm sequence) is reached only as `FUN_2002b29c`'s *other* branch, a sibling alternative to
`cold_boot_mode_dispatch`, not something `system_mode_request_dispatch` or any of its callees ever invoke.

**This is a clean, direct, positive confirmation of the user's own hardware observation**: entering
Emergency Mode does not power-cycle the radio, and does not even trigger a CPU/watchdog reset of any kind —
it's pure software task-teardown-and-reinit (task re-activation, SCIF0/SCIF1 reopen, UI/display re-init)
within the exact same continuously-running process image. RAM — including the `0x203de174` struct holding
the persistent status bits — is never cleared or reloaded, so of course it survives; there was never
anything to persist across in the first place. Added a plate-comment addendum on
`system_mode_request_dispatch` documenting this with full addresses.

### 2. The 4630kHz visibility gate — real, deeper structural progress, but genuinely not found

Confirmed the basic premise first: this project's `scratch/unpacked/*/body.bin` directories are all
per-*version* (`111`-`142`), not per-region — one firmware image serves every region, region behavior
selected entirely by the diode matrix at runtime. So if the 4630kHz checkbox is hidden for non-JP builds,
it has to be a runtime gate somewhere in this one image, not a compile-time omission.

**Found the real, concrete list-item records for "4630kHz" and "Tuner"** — a genuinely new, deeper
structural find than the 13th session had (which only reached the two items' *warning-dialog* wiring, not
their list-entry records). They sit in a previously-undocumented **20-byte-stride list-widget table**
starting somewhere before `0x2018ec00` and continuing past `0x2018ee20` (base/driving code not
found — see below), record shape confirmed empirically against two independent, verified anchors
(`all_reset_button_handler` and the two new handlers below, all landing exactly at each record's
offset+8): `{name_EN(4), name_JP(4), tap_handler(4), secondary(4, always 0 in every record sampled),
flags(4)}`.

- **"4630kHz"** row: record at `0x2018edb8` — `name_EN`/`name_JP` both `0x20359ae0` ("4630kHz", a
  language-neutral string used for both fields), `tap_handler` = `0x20041cfc` (renamed
  `emergency_4630khz_item_tap_handler`), `secondary` = `0`, `flags` = `0x1071e`.
- **"Tuner"** row: record at `0x2018edcc` — `name_EN` = `0x20359a3c` ("Tuner"), `name_JP` = `0x2035997c`
  (Shift-JIS, presumably "チューナー"), `tap_handler` = `0x20041d54` (renamed
  `emergency_tuner_item_tap_handler`), `secondary` = `0`, `flags` = `0x10709`.
- For comparison, the already-known `all_reset_button_handler` row sits immediately before these two
  (record at `0x2018eda4`, `flags` = `0x1071e` — same low bits as the 4630kHz row) and a `partial_reset`-
  adjacent row before that (`flags` = `0x10700`, matching several other plain, non-confirm-dialog rows
  sampled). The `flags` field's low byte varies per row (`0x00`/`0x1e`/`0x09`/`0x16` seen) but is a small
  **static, compiled-in integer**, not a pointer — ruled out as a direct region/diode bitmask or as an item
  code for the already-known `is_feature_enabled_for_region` gatekeeper (its item-code range starts at
  `0x24`; codes `0x09`/`0x1e`/`0x16` all fall below that and would hit its unconditional `return 0` path if
  fed in, which can't be right for items that are visible at all — ruled out directly, not just assumed).
- `emergency_4630khz_item_tap_handler` (`0x20041cfc`) is still inside the ARM/Thumb disassembly gap this
  project already queued a fix for last session (`0x20041cfc`, 108 bytes) — confirmed again via the same
  `objdump` ground truth: checks Tuner's own checked-state (`*0x2039021e`) first (an early "uncheck and
  refresh" path), then a selector byte at `0x20390211` to decide between warning dialogs `0x5b`/`0x5a`.
  `emergency_tuner_item_tap_handler` (`0x20041d54`) is a trivial 2-instruction stub —
  `mov r0,#4; b FUN_2002b818` — unconditionally requesting the same system-mode-4 restart, no dialog call
  visible directly from this address (its dialog-showing, if any, must happen through a different path not
  traced this session — flagged honestly rather than assumed).

**Checked directly for a diode/region-code test — real, thorough negative**:
- Both tap-handler bodies (verified via the same `objdump` ground truth used to confirm them) —
  zero references to any diode-scan-value alias or `region_code`.
- The record's own `secondary` field (the position structurally analogous to `notes/ui-menu.md`'s
  `+0x14` "am I available" callback for the *other*, already-documented 72-byte QUICK MENU widget) is
  `0` (null) for every record sampled, including both checkbox rows — no per-item availability callback
  is populated here at all, for any of the ~8 rows checked.
- `references_to` on the `flags` field's own address, for both the 4630kHz and Tuner records
  (`0x2018edc8`/`0x2018eddc`) and on the 4630kHz name field (`0x2018edb8`) — **zero references** in every
  case. No code anywhere reads these fields by a resolvable literal address — the same
  "computed-table-access wall" this project has hit repeatedly for other flat tables (the 216-item table,
  the 326-item defaults table, etc.): the real walker/driver function for this list almost certainly
  computes `base + index*20 + offset` at runtime, which Ghidra's static analysis can't resolve back to
  individual field xrefs.
- Could not locate the table's own base pointer / driving "walker" function (the equivalent of
  `notes/ui-menu.md`'s `DAT_2004f728` for QUICK MENU) within the session budget — the table extends well
  beyond the ~20 records sampled in both directions, with no obvious header/sentinel record found nearby.
  Without the walker, there's no way to check whether *it* applies a region/diode-conditional item count
  or skip-list before ever reaching the generic renderer.

**Net for this question: still genuinely not found, despite a real, deeper, targeted search** — this
session went past the 13th session's "wrong struct, wrong item type" negative and reached the actual,
correct "4630kHz"/"Tuner" list records and their real tap-handlers, and still found no runtime
region/diode gate anywhere in what's reachable. D420 specifically remains completely unconfirmed by this
new chain, same as by every other chain this project has checked it against across 14 sessions now. Not
disconfirmed either — the table's own base/walker function is a concrete, named next step for a future
session (this table is real new territory, not yet in `notes/ui-menu.md`).

**Files touched this session**: `notes/diode-matrix.md` (this section, EEPROM-gap correction above),
`notes/diode-matrix-history.md` (14th-session narrative entry). Ghidra database: 2 new renames
(`emergency_4630khz_item_tap_handler`, `emergency_tuner_item_tap_handler`) + PRE comments on both, plus a
substantial addendum appended to `system_mode_request_dispatch`'s existing plate comment (original text
preserved, not overwritten). No new ARM/Thumb fix queued — the one gap hit (`0x20041cfc`) was already
queued last session and covers this session's findings too. No git commit made.

## 15th session — found the real per-item renderer for "4630kHz"/"Tuner" (`settings_list_item_kind_renderer`); confirms no visibility gate exists in this specific path either, the strongest negative yet

The user pointed at a specific address (`DAT_2018ed50`, 4 bytes before the table-record area the 14th
session mapped) and ran `references_to` on it themselves, finding exactly 2 hits — a genuinely productive,
concrete lead worth the full trace.

**Hit 1 (`0x20042498`, inside `FUN_20042458`) is real and important — the "kind"-based item renderer this
project didn't have before.** Renamed `FUN_20042458` → `settings_list_item_kind_renderer`. It's called
from `FUN_20042f3c` (the "up to 4 items" page renderer, already known) for type-3 items specifically:
`settings_list_item_kind_renderer(uVar5, uVar8)` where `uVar5` = page start index, `uVar8` = slot 0-3.
Resolves an absolute item index `uVar11` via `*(ushort*)(DAT_200426b0 + (uVar5+uVar8)*4 + 2)`, then reads a
"kind" byte via `*(byte*)(DAT_2004196c + uVar11*0x14 + 8)`. `DAT_2004196c` resolves to `0x2018ed48` this
build — **the exact same ROM table** the 13th/14th sessions already explored for the "Others" screen's
per-item name/tap-handler records (`all_reset_button_handler`, `emergency_4630khz_item_tap_handler`,
`emergency_tuner_item_tap_handler` — all independently confirmed via real `DATA` xrefs at their own table
positions, unaffected by anything below). This is a **second, independent field read** over the same
physical 20-byte-stride table, with its own base/offset convention (kind byte at `+8` from a base that's 4
bytes earlier than where the 14th session's name/handler numbering put record boundaries) — two different
consuming functions reading two different fields of the same records, not a contradiction, and a genuine
correction to the 14th session's structural model (which only had the name/handler fields, not this kind
byte).

**Directly confirmed, by reading the raw table bytes at the computed addresses**: kind byte for `uVar11==5`
(at `0x2018edb4`) = `0x1e`; kind byte for `uVar11==6` (at `0x2018edc8`) = `0x1e`. Both dispatch to the same
`case 0x1e` block (`LAB_200429fc`), which switches again on `uVar11` itself: `uVar11==5` sets
`*pbVar13 = 1` (item always considered available) and copies its checkbox state directly from
`*(DAT_20042664+5)` (`0x2039021d`, the already-known 4630kHz target byte); `uVar11==6` does the identical
thing from `*(DAT_20042664+6)` (`0x2039021e`, the Tuner target byte). **This nails down, for the first
time with full confidence, that item index 5 = "4630kHz" and item index 6 = "Tuner,"** and confirms —
directly, not by absence — that **this render path never gates on anything: `*pbVar13` is set to `1`
unconditionally for both items, with no diode, region_code, or any other test in between.**

**Traced `DAT_200426b8` (the 0x1c-stride "is this row available" array used by kind cases `0x16`/`0x18`/
`0x20`) per the user's specific request — real, clean negative, and structurally moot for our two items
anyway** (those cases are for different kind values, never reached by kind `0x1e`). Found and decompiled
all 8 real writers (`FUN_20023d74`, `FUN_20059754`, and 6 siblings in the `0x20059xxx`-`0x2005axxx`
cluster) — they mirror a *different*, unrelated `0x34`-byte source struct (`DAT_20058fb8`) into this 4-slot
cache, with a condition testing `*(DAT_20058fac+0xb)`/`*(DAT_20058f9c+6)` that looks like a generic
"shift/rotate a 4-slot recent-items list" mechanism for some other feature entirely — no diode or
region_code reference anywhere in the writers checked.

**Checked `FUN_2005ea60`/`FUN_2005eab0`/`FUN_2005e8ac`** (the sibling gating calls in cases `0x16`/`0x18`/
`0x20`, alongside the `DAT_200426b8` check) — all three test a single-char mode/status byte at
`*DAT_2005dfb0` against values like `'\0'`/`'\b'`(0x08)/`'K'`/`'R'`/`'S'` — an operating-mode or
hardware-compatibility check (plausibly CI-V/radio-state related given the character codes), confirmed
unrelated to any diode alias or `region_code`. Orthogonal, as the user suspected might be the case.

**Hit 2 (`0x2008a4d4`, inside `FUN_2008cff8`) checked and ruled out as a coincidental false lead**, per the
user's own explicit warning to check rather than assume. Decompiled the full ~10,000-byte function: a
completely unrelated CI-V/service-mode display-synchronization dispatcher (switches on a totally different
selector byte, `9`-`0xf` and `'\t'`/`'\n'`, all `FUN_2008xxxx`/`FUN_200acxxx`/`FUN_200adxxx` calls pushing
diffs to what looks like a front-panel/sub-display sync protocol) with no visible connection to the
settings-list table, its kind bytes, or the 4630kHz/Tuner items at all — the same "coincidental address
match in `references_to`, not a real xref" failure mode this project flagged for a different address in the
13th session.

**Net: the strongest negative yet, and a real structural gain.** This session found the actual, correct
per-item *render* dispatch for both checkboxes (not just their tap-handlers, found last session) and
confirmed it unconditionally shows both — no gate of any kind in this path. Combined with the 14th
session's finding that the tap-handlers themselves have no diode/region test either, **every reachable
piece of code that touches these two specific items (kind-render, tap-handler, checkbox-state read) has
now been checked and is clean.** If a real visibility gate exists at all, it must live one level further
up than anything reached so far: in whatever populates `DAT_200426b0` (the index-resolution array feeding
`uVar11`) or the page's item *count*/bounds for the specific `uVar5` that corresponds to this page — i.e.
still the same "table's own base/walker function" gap the 14th session already flagged, now narrowed
further (it would have to omit index 5 or 6 from the resolved list entirely, not merely disable them, since
disabling isn't wired up for kind `0x1e` at all).

**Files touched this session**: `notes/diode-matrix.md` (this section), `notes/diode-matrix-history.md`
(15th-session narrative entry). Ghidra database: 1 rename (`FUN_20042458` →
`settings_list_item_kind_renderer`) + 1 substantial plate comment. No ARM/Thumb fix newly queued — no new
disassembly gaps were hit this session (both `FUN_20042458` and `FUN_2008cff8` were already fully
disassembled/decompiled by Ghidra). No git commit made.

## Open questions
1. Country/market name correlation to internal region codes 1-7 is
   still not fully pinned: the derived arithmetic mapping (see
   population-data section above) produces `USA`=0 and `EXP`=9, both
   outside the confirmed valid range (1-7) — re-confirmed not a
   parts-list misread, so either the bit-weight formula or the
   region-code-to-name mapping has an unresolved wrinkle. Regions 2/3/4's
   TX/RX tables also not yet pulled (would help pin down more names by
   band-plan fingerprint, as done for regions 0/1/7).
2. Icom's public "Version #" numbering goes at least to 12, but the
   internal 4-bit region code only reaches 7 (9 of 16 diode combinations
   map to 0/invalid) — not reconciled. Possible explanations: this
   firmware's table doesn't cover every variant ever made; there's
   another translation step between the public Version # and the
   internal `region_code`; or the two numbering schemes are simply
   unrelated.
3. ~~What `DAT_2003c85c` (diode 416's second lookup, indexed by
   `DAT_2003c800+2`) actually drives~~ — **resolved, 9th session.**
   `DAT_2003c800+2` is confirmed to be the region_code byte (written by
   `FUN_2003c0ec` via `diode_region_code_lookup`), same 0-7-valid-range
   value used everywhere else in this file. `DAT_2003c85c`'s lookup
   feeds `FUN_2000a5f0(10)`, one branch of a generic ~10/11-item
   menu-cycle "is item N enabled" gate (called from `FUN_2000a6d8`,
   `FUN_2000ba0c`, `FUN_20032f8c`, and 2 more sites — shape matches a
   settings/mode cycling selector, exact feature not pinned down). But
   since the table's only non-zero entries are at indices 13 and 15, and
   region_code never exceeds 7, **this D416-gated path can never fire on
   any real diode combination** — a real consumer, but dead in practice.
4. ~~Where diode 403's raw bit value (`FUN_2003c5c4`'s return) actually
   gets used~~ — **resolved, 9th session.** Its only caller,
   `get_type1_type2_designation` (`0x200134b4`), itself has exactly 3
   callers (`FUN_2000b98c`/`0x2000b9d0`, `FUN_200189e8`/`0x20017db8`,
   `FUN_20065dec`/`0x20065d74`), all decompiled: every one uses the
   `'1'`/`'2'` return purely as the 49-vs-50-tone CTCSS-cycle bound
   passed into the same generic "select item N with wraparound" helpers
   (`FUN_20017c50`/`FUN_20006398`) used elsewhere in the firmware. No
   other distinct consumer exists — D403 doesn't reach band/TX logic
   through this path, full stop.
5. ~~What the `FUN_2003c530` bit-17/18 (D406/D409) classification byte
   (mirrored to `*DAT_2003c858 + 0x29`) is read by~~ — **resolved, 9th
   session.** `*DAT_2003c858` resolves to `0x203def00`, the same live
   "DSP shadow config" struct `dsp_param_table_rebuild_from_settings`
   (`0x200b232c`) already reads from for RTTY/DSP settings (see
   `notes/kernel-rtos-history.md`'s RTTY thread). That function reads
   struct offset `+0x29` (= `0x203def29`, exactly the byte
   `FUN_2003c530` writes), shifts it `<<6`, and ORs it into DSP
   command-table word index 3 (tagged opcode `0x22` via
   `CONCAT13(0x22,...)`) alongside 3 other 2-bit fields from struct
   offsets `+0x2a6`/`+0x2a7`/`+0x2a8`. That word is pushed live to the
   DSP chip over SCIF5 by `dsp_param_sync_tick()` every tick — i.e.
   D406/D409's classification is a real, continuously-synced DSP
   configuration input, not merely local UI/CPU-side state. The other 3
   packed 2-bit fields' own meanings weren't traced (out of scope for
   this question).
6. Which of the ~40+ numeric item codes (`0x24`-`0xf6`) in
   `FUN_2003df34`'s gatekeeper corresponds to which actual named
   feature/menu item, and specifically which item code means Japan's
   "Emergency Mode" (D423's leading hypothesis). Would need the "Set
   Mode" menu string table cross-reference (`~0x2032f000`-`0x20360000`,
   see [[firmware-update]]). **Still open, 9th session** — the real name
   pool is now known to sit at `~0x2035a000`-`0x2035f000` (see
   `notes/kernel-rtos-history.md`'s RTTY thread and `notes/ui-menu.md`),
   but a fresh spot-check this session (`references_to` on two sample
   name strings, `"RTTY Decode USOS"`/`"RTTY Mark Frequency"`) found zero
   static references to either, same "computed-table-access wall" the
   RTTY thread already hit — no static link from the name pool to the
   216-item value table or to `FUN_2003df34`'s item codes found. Not
   exhaustively re-attempted this session beyond that spot-check.
   **New lead, 10th session**: the *326-item* defaults table `FUN_2003dcc0`
   uses for factory reset (`0x20190ecc`, see "Factory reset" section
   above — a different table from the 216-item one referenced here) has,
   at least for item 0, string-pointer fields (`+0x28`/`+0x2c`/`+0x34`/
   `+0x38`) landing directly in the same `0x2035a000`-`0x2035f000` name
   pool. Not confirmed as the missing link (not chased past item 0's
   record this session), but a genuinely new, concrete candidate worth a
   fresh look before assuming another "computed-table-access wall."
   **11th session**: chased this lead further — item `0x73`'s own name
   string (the concrete D423 consumer) resolved cleanly to `"Display
   Language"`, confirming the link is real and readable this way. But
   this closes off, rather than confirms, the "which item code is
   Emergency Mode" question: `0x73` isn't it. Checked `0x22`/`0x32`/
   `0x71`/`0x79`/`0x94`/`0xe5` the same way — none of their name-string
   fields read "Emergency" or anything 4630/tuner-shaped either (`0x94`/
   `0xe5` have no name string at all, just word-typed values). Separately,
   and probably more importantly: found the real "Emergency Communication
   Mode" feature directly as firmware strings (see the new "4630 kHz
   Emergency Communication Mode" section above) — Japan's Emergency Mode
   is now a **firmware-confirmed real feature**, just still not tied to
   any specific item code in either the 216-item or 326-item table. The
   answer to this open question may simply be "it isn't gated through
   either of these two tables at all" — the status-indicator code found
   this session (`FUN_2009060c`) looks like a separate UI-status
   mechanism, not a menu-item gate.
   **12th session**: confirmed `FUN_2009060c`'s mechanism is real and traced it two functions further (see
   the new "12th session" section above) — still not tied to `region_code`/any diode alias, and still not
   tied to an item code in either the 216- or 326-item table; the trigger bottoms out in two read-only
   variables (`DAT_2002b46c`/`DAT_2002b470`) with no writer found anywhere in the image, the same class of
   dead end `notes/kernel-rtos-history.md` already hit for `DAT_2002a158`. Separately, a systematic
   whole-image Shift-JIS sweep (`tools/sjis_string_scan.py`) found that the big `0x2035a000`-`0x2035f000`
   name/message pool this question also concerns is **not** structured as per-item interleaved English/
   Japanese pairs the way the small `0x2032a000` indicator table is — it's either block-separated (a long
   run of all-English fields, then a separate long run of all-Japanese fields in the same relative order,
   confirmed for the dialog/error-message section) or pointer-table-paired (the `0x2032c91c` message table,
   confirmed for at least one record). Worth remembering for any future attempt to pair names in this pool
   by proximity — it will not work the way it does for `0x2032a000`. Also located the separate "Emergency"
   SET-menu-category's home record (a generic 24-byte-stride category table around `0x20190200`) but did not
   trace it to a gating condition — see the new section above.
   **13th session**: the user supplied the real manual text for this screen directly — it's **one shared
   screen** (`MENU > SET > Others > Emergency`, i.e. the same "Emergency" category record found last
   session) holding both the 4630kHz checkbox AND the all-regions Tuner checkbox, not two separate menus.
   Traced the full UI chain for the 4630kHz side end to end (warning dialog → restart-to-set button →
   system-mode-4 request → already-documented persistent-bit commit) — see the new "Emergency Mode / Tuner
   (all-regions)" section above for the complete writeup, renamed functions, and honest remaining gaps
   (no EEPROM write found, no Tuner-specific restart-commit sibling isolated, boot-time EEPROM-populate
   still not found). Diode/region gating remains a clean negative across every newly-traced function.
11. **New, 13th session.** `notes/kernel-rtos.md` does **not** actually document an EEPROM-settings-load-
    at-boot mechanism (zero mentions of "eeprom" in that file) — a correction to an assumption a task brief
    made this session, worth remembering before citing that file for EEPROM-boot claims again. The real
    EEPROM catalogue lives in `notes/eeprom-catalogue.md`, whose one documented combined-settings loader
    (`FUN_2006cb84`) does not cover the `0x203de174` struct region the Emergency-Mode status bits live in
    (ruled out by address arithmetic — see the section above). Where (or whether) `0x203de175` bits 2/3 are
    EEPROM-backed at all remains open.
7. `chunk5_tail.bin` (~1.6-1.7 MB, LZSS-compressed, likely a second
   processor's firmware image) and `base.dat`/font-resource blocks
   remain completely unchecked for diode consumers — low-probability
   locations, but not literally verified for D408/D411/D414/D417/D420.
8. ~~Why P5's direction/mode registers weren't found being set~~ —
   partially resolved, see "Exhausted searches" above; final answer
   needs JTAG (see [[hardware-debug-access]]).
9. **New, 10th session.** `FUN_2002a4c8`'s Partial/All-reset defaults
   pass calls `FUN_20045800()` (wipes all 11 User Band Edge slots)
   **unconditionally, for both reset types** — but the real manual says
   Partial reset preserves User Band Edge. Not reconciled: either
   something else restores/skips this specifically for Partial reset
   (not found), the call is gated on something upstream of what was
   traced, or the manual's wording and the firmware's real behavior
   simply don't match. Worth a fresh, targeted look.
10. **New, 10th session.** The hardware `CLEAR`+`V/M`-held-at-power-on
    forced-reset path the manual documents was not found. The three
    real boot-time button-combo checks in `cold_boot_hw_init`
    (`boot_check_mode1_combo`/`_mode5_combo`/`_challenge_response`) are
    all **service-mode entry** combos, not this one, and none of them
    touch the Partial/All-Reset request variable. Genuinely unfound,
    not just unconfirmed — a real target for a future session or JTAG.

All five remaining unresolved diodes (D408, D411, D414, D417, D420) are
good candidates for live JTAG verification (toggle the position, watch
what changes) rather than further static searching — the same
conclusion earlier sessions reached for D419/D422 before those turned
out to have real consumers, so none of this is necessarily final.

11. **New, 14th session.** Specifically tested D420 against a deeper, more direct target than any prior
    session: the actual "4630kHz"/"Tuner" list-item records in the "Others" screen's 20-byte-stride
    list-widget table (see the new 14th-session section above), including both real tap-handler functions
    and the records' own `secondary`/`flags` fields. Real, thorough negative, same as every prior D420
    check — no diode/region reference found anywhere reachable from this chain either. The table's own
    base pointer / driving walker function was not located this session (a concrete next step, not yet
    attempted the way `notes/ui-menu.md`'s `DAT_2004f728` was found for QUICK MENU) — until that's found,
    D420 can't be fully ruled out as a gate on the *list itself* (e.g. an item-count or skip-list the
    walker applies before the generic renderer ever sees these records), only on everything currently
    reachable from the records and handlers themselves.
    **15th session**: found and traced the real per-item *render* dispatch too (`settings_list_item_kind_renderer`,
    `0x20042458`, previously `FUN_20042458`) — confirmed item index 5 = "4630kHz", index 6 = "Tuner", both
    unconditionally rendered as available (`*pbVar13=1`, no gate) via a shared "kind `0x1e`" checkbox-render
    path. Also traced a candidate "is this row available" array (`DAT_200426b8`) found via a fresh
    `references_to` sweep the user ran directly — confirmed unrelated (feeds a different, unrelated 4-slot
    list-cache feature, and structurally inapplicable to kind `0x1e` items regardless of gating content).
    Every reachable piece of code touching these two specific items (kind-render, tap-handler, checkbox-state
    read) is now checked and clean of any diode/region test. The gate, if real, has to live in the still-
    unfound `DAT_200426b0`-populating walker function — narrower than before, but still not found.
