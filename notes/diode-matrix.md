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
| D403 | top, col8 | 16 | ✅ confirmed | Selects Type 1 (present) vs Type 2 (absent) market designation (`FUN_200134b4` returns ASCII `'1'`/`'2'`); also gates a 51st, non-standard CTCSS tone (150.0 Hz). **Ruled out** for 60m/5MHz via the one consumer traced — that path is CTCSS, not band access |
| D404 | bottom, col7 | 1 | ✅ confirmed | Region-code bit, weight 8 (see region table below) |
| D405 | middle, col7 | 9 | ✅ confirmed | **Gates a specific ~5.255 MHz (60m-area) frequency in `FUN_2003bd80`'s range table** — D405 present excludes it, D405 absent includes it. Resolves the D403-vs-D405 conflict from external sources in favor of D405 |
| D406 | top, col7 | 17 | ✅ confirmed | Input to `FUN_2003c530`'s post-scan classification — present → classification byte = 1, takes priority over D409 |
| D407 | bottom, col6 | 2 | ✅ confirmed | Region-code bit, weight 4 |
| D408 | middle, col6 | 10 | ❓ unknown | **Exhaustively searched** — no feature-gate consumer found anywhere in the full firmware. Only touched by the mechanical bit-reversal echo (`FUN_2003c70c`). Genuinely populated on every shipping version (unlike the confirmed-N/A pads), so its lack of any software consumer is a real open question, not just "not found yet" |
| D409 | top, col6 | 18 | ✅ confirmed | Input to `FUN_2003c530`'s post-scan classification — present (and D406 absent) → classification byte = 2 |
| D410 | bottom, col5 | 3 | ✅ confirmed | Region-code bit, weight 2 |
| D411 | middle, col5 | 11 | ❓ unknown | **Exhaustively searched across all 10 known firmware versions** — no consumer found |
| D413 | bottom, col4 | 4 | ✅ confirmed | Region-code bit, weight 1 |
| D414 | middle, col4 | 12 | ❓ unknown | **Exhaustively searched across all 10 known firmware versions** — no consumer found |
| D416 | bottom, col3 | 5 | ✅ confirmed | Gates the general-coverage RX unlock (0.030–74.8 MHz, 13-segment table), combined with region code 5 or 6. Also gates a separate 2-entry lookup (`DAT_2003c85c`, values 2/3, purpose TBD) |
| D417 | middle, col3 | 13 | ❓ unknown | **Exhaustively searched across all 10 known firmware versions** — no consumer found. Populated only on EUR/ITR/KOR (`[#03][#05][#06]`) per parts list |
| D419 | bottom, col2 | 6 | ✅ confirmed | **Selects the TX frequency-range table in `FUN_2003bd34`/`FUN_2003be94`, together with D422.** Present (D422 absent) → continuous TX 0.1–74.8 MHz, exactly the mod-guide's "open TX" figure. Populated on all versions per parts list |
| D420 | middle, col2 | 14 | ❓ unconfirmed | User hypothesis: language-related. **Exhaustively searched across all 10 known firmware versions** — no consumer found. **Confirmed Japan-only (`Only [#01]`) per parts list** — matches D423, supporting the user's original "D420/D423 both JP-only" domain-knowledge lead |
| D422 | bottom, col1 | 7 | ✅ confirmed | **Selects the TX frequency-range table in `FUN_2003bd34`/`FUN_2003be94`, together with D419.** Present (D419 absent) → continuous TX 1.6–54 MHz (fills the HF/6m gap only, not the full 0.1–74.8 MHz range the user's external claim attributed to D422 alone) |
| D423 | middle, col1 | 15 | ✅ confirmed | Real, direct input (bit 15) to `FUN_2003df34`/`FUN_2003dcc0`, the master feature-gatekeeper — gates item-code overrides including at least `0x22/0x32/0x4b/0x71/0x73/0x79` and the `0x8f-0x93/0x94/0xe5` range. Strong support for the Emergency Mode hypothesis (sits directly in the same gatekeeper as all other regulatory feature checks); exact feature name per item code not yet resolved |

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
  lookup table `DAT_2003c85c` = `[0×13, 2, 0, 3]`.
- **`FUN_2003c5c4`** — returns bit 16 (D403) as a raw value (see
  `FUN_200134b4` Type1/Type2 designation above); further consumer not
  traced.
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
  byte, mirrored to `*DAT_2003c858 + 0x29`; consumer of that byte not
  traced.
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
through `142`). Bits 19-23 (the non-existent top-row N/A pads) are
likewise only ever touched by the `FUN_2003c70c` mechanical echo, never
gated on.

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
3. What `DAT_2003c85c` (diode 416's second lookup, indexed by
   `DAT_2003c800+2`) actually drives, beyond its raw contents
   (`[0×13, 2, 0, 3]`).
4. Where diode 403's raw bit value (`FUN_2003c5c4`'s return) actually
   gets used.
5. What the `FUN_2003c530` bit-17/18 (D406/D409) classification byte
   (mirrored to `*DAT_2003c858 + 0x29`) is read by — not traced.
6. Which of the ~40+ numeric item codes (`0x24`-`0xf6`) in
   `FUN_2003df34`'s gatekeeper corresponds to which actual named
   feature/menu item, and specifically which item code means Japan's
   "Emergency Mode" (D423's leading hypothesis). Would need the "Set
   Mode" menu string table cross-reference (`~0x2032f000`-`0x20360000`,
   see [[firmware-update]]).
7. `chunk5_tail.bin` (~1.6-1.7 MB, LZSS-compressed, likely a second
   processor's firmware image) and `base.dat`/font-resource blocks
   remain completely unchecked for diode consumers — low-probability
   locations, but not literally verified for D408/D411/D414/D417/D420.
8. ~~Why P5's direction/mode registers weren't found being set~~ —
   partially resolved, see "Exhausted searches" above; final answer
   needs JTAG (see [[hardware-debug-access]]).

All five remaining unresolved diodes (D408, D411, D414, D417, D420) are
good candidates for live JTAG verification (toggle the position, watch
what changes) rather than further static searching — the same
conclusion earlier sessions reached for D419/D422 before those turned
out to have real consumers, so none of this is necessarily final.
