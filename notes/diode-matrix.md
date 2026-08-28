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

## Living reference: what each diode does (update this as findings change)

Bit numbering per the confirmed scan-result layout: row-bottom bit =
`8-col`, row-middle bit = `16-col`, row-top bit = `24-col`.

| Diode | Position | Bit | Status | Function |
|---|---|---|---|---|
| D401 | bottom, col8 | 0 | ✅ confirmed | Enables the region-restriction check itself (gate for D404/407/410/413's region code taking effect) |
| D402 | middle, col8 | 8 | 🟡 partial | Input to `FUN_2003bd80`'s range-merge algorithm (inverted) — gates a boundary-snapping/clamping behavior (replaces range edges with alternate values from `DAT_2003c820`/`DAT_2003c824` near a threshold). Real, confirmed as an input; exact end-user effect not fully traced |
| D403 | top, col8 | 16 | ✅ confirmed | Selects Type 1 (present) vs Type 2 (absent) market designation; also gates a 51st, non-standard CTCSS tone (150.0 Hz). **Ruled out** for 60m/5MHz via the one consumer traced (`get_type1_type2_designation`) — that path is CTCSS, not band access |
| D404 | bottom, col7 | 1 | ✅ confirmed | Region-code bit, weight 8 (see region table below) |
| D405 | middle, col7 | 9 | ✅ confirmed | **Gates a specific ~5.255 MHz (60m-area) frequency in `FUN_2003bd80`'s range table — D405 present excludes it, D405 absent includes it.** Resolves the D403-vs-D405 conflict from external sources in favor of D405 |
| D406 | top, col7 | 17 | ✅ confirmed (4th session) | Input to `FUN_2003c530`'s post-scan classification (see below) — present → classification byte = 1, takes priority over D409 |
| D407 | bottom, col6 | 2 | ✅ confirmed | Region-code bit, weight 4 |
| D408 | middle, col6 | 10 | ❓ unknown | No consumer found yet |
| D409 | top, col6 | 18 | ✅ confirmed (4th session) | Input to `FUN_2003c530`'s post-scan classification (see below) — present (and D406 absent) → classification byte = 2 |
| D410 | bottom, col5 | 3 | ✅ confirmed | Region-code bit, weight 2 |
| D411 | middle, col5 | 11 | ❓ unknown | No consumer found yet |
| D413 | bottom, col4 | 4 | ✅ confirmed | Region-code bit, weight 1 |
| D414 | middle, col4 | 12 | ❓ unknown | No consumer found yet |
| D416 | bottom, col3 | 5 | ✅ confirmed | Gates the general-coverage RX unlock (0.030–74.8 MHz, 13-segment table), combined with region code 5 or 6. Also gates a separate 2-entry lookup (values 2/3, purpose TBD) |
| D417 | middle, col3 | 13 | ❓ unknown | No consumer found yet. Populated only on EUR/ITR/KOR (`[#03][#05][#06]`) per parts list (2026-08-28 correction — was previously misread as D416's data) |
| D419 | bottom, col2 | 6 | ✅ confirmed (5th session) | **Selects the TX frequency-range table in `FUN_2003bd34`/`FUN_2003be94`, together with D422** — see "D419/D422 resolved" section below. Present (D422 absent) → continuous TX 0.1–74.8 MHz, exactly the mod-guide's "open TX" figure. Populated on all versions per parts list (2026-08-28 correction — the earlier "Japan-only" population read was wrong) |
| D420 | middle, col2 | 14 | ❓ unconfirmed | User hypothesis: language-related. No consumer found yet. **Confirmed Japan-only (`Only [#01]`) per parts list (2026-08-28 correction)** — matches D423, restores the user's original "D420/D423 both JP-only" domain-knowledge lead |
| D422 | bottom, col1 | 7 | ✅ confirmed (5th session) | **Selects the TX frequency-range table in `FUN_2003bd34`/`FUN_2003be94`, together with D419** — see "D419/D422 resolved" section below. Present (D419 absent) → continuous TX 1.6–54 MHz (fills the HF/6m gap only, not the full 0.1–74.8 MHz range the user's external claim attributed to D422 alone) |
| D423 | middle, col1 | 15 | ✅ confirmed (4th session) | Real, direct input (bit 15) to `FUN_2003df34`/`FUN_2003dcc0`, the master feature-gatekeeper — gates item-code overrides including at least `0x22/0x32/0x4b/0x71/0x73/0x79` and the `0x8f-0x93/0x94/0xe5` range. Strong support for the Emergency Mode hypothesis (sits directly in the same gatekeeper as all other regulatory feature checks); exact feature name per item code not yet resolved |

`D412`/`D415`/`D418`/`D421`: pads exist on the physical board (confirmed
by user) but are omitted from published diode-matrix references/photos —
don't assume they're unused; possibly reserved for R&D/factory use. Not
"N/A" in the sense of not existing, just undocumented.

## Official per-version population data from the service manual Parts List (2026-08-27)

User found it: service manual §5 Parts List, Main Unit, page 5-1, has
per-diode `[#NN]` version tags directly against each `D4xx` line —
authoritative population data (which physical variant each diode
position is actually populated on), not something we've had to infer
indirectly before. Read from a user-supplied page screenshot; small
bracket text carries real misread risk on a table this dense, so
confidence is noted per row — **treat anything not marked "high
confidence" as worth a second look before leaning on it hard.**

Version numbers per [[ic7300-signal-chain]]'s official table: `USA #02`,
`EUR #03`, `ITR #05` (Italy), `ESP #06` (Spain), `TPE #07` (Taiwan),
`KOR #08` (Korea), `EXP #12`. This parts list also uses `#01`, not
present in that table — almost certainly `JAP` (Japan domestic), simply
not listed as an export "Version" in that particular table.

| Diode | Population per parts list | Confidence |
|---|---|---|
| D401 | all versions (no tag) | high |
| D403 | all versions (no tag) | high |
| **D404** | **`Only [#12]`** — EXP alone | high (machine-text confirmed) |
| D405 | all versions (no tag) | high (machine-text confirmed — **corrected, see below**) |
| D407 | `[#05]`, `[#06]`, `[#07]`, `[#08]` — ITR/ESP/TPE/KOR | high (machine-text confirmed) |
| D408 | all versions (no tag) | high |
| D410 | `[#03]`, `[#07]`, `[#08]` — EUR/TPE/KOR | high (machine-text confirmed) |
| D411 | all versions (no tag) | high |
| D413 | `[#06]`, `[#08]`, `[#12]` — ESP/KOR/EXP | high (machine-text confirmed) |
| D414 | all versions (no tag) | high |
| D416 | all versions (no tag) | high (machine-text confirmed — **corrected, see below**) |
| D417 | `[#03]`, `[#05]`, `[#06]` — EUR/ITR/KOR | high (machine-text confirmed — **corrected, see below**) |
| D419 | all versions (no tag) | high (machine-text confirmed — **corrected, see below**) |
| **D420** | **`Only [#01]`** — JAP alone | high (machine-text confirmed — **corrected, see below**) |
| D422 | all versions (no tag) | high |
| **D423** | **`Only [#01]`** — JAP alone | high (machine-text confirmed) |

**Correction (2026-08-28), superseding the rows above and the discussion below that relied on them.** The
original reads of this table came from a user-supplied *page screenshot* — real OCR/visual-read risk on a
dense table, which is exactly what happened. Re-extracted the actual parts list this session via
`pdftotext -layout` directly against `IC-7300_Servicio.pdf` (machine text, not an image read — far more
reliable, and the right method to reach for first next time this document needs checking). Comparing
line-for-line:
- **D405**: no tag at all (populated on *every* version) — **not** Italy-only. The "Only `[#05]`" read was
  wrong; most likely visual bleed from `D404`'s real `[#12]` tag sitting directly above it in the table.
- **D416 / D417**: the tags belong to **D417**, not D416 — `D416` has no tag (all versions), `D417` is
  `[#03][#05][#06]` (EUR/ITR/KOR). The previous read had these two adjacent rows' data swapped.
- **D419 / D420**: same swap pattern one row down — `D419` has no tag (all versions), `D420` is
  **`Only [#01]`** (JAP alone). The previous read had D419 carrying D420's real tag and vice versa.

Net effect: of the 5 rows corrected, the errors weren't confined to the "medium confidence, 4-stacked-tags"
rows flagged at the time (D407/D410/D413 — which all check out exactly as originally read, upgraded to
high confidence) — they were rows marked *high* confidence, in unflagged single-tag or no-tag rows. Take
this as a caution against trusting a screenshot read's own confidence self-assessment on this document; a
`pdftotext` extraction is cheap enough to just always do instead going forward.

**`D402`/`D406`/`D409`/`D412`/`D415`/`D418`/`D421` don't appear in this
parts list at all** — not populated on *any* currently-documented
variant. Strong confirmation of the existing "R&D-reserved pads" theory
above for 412/415/418/421, and extends the same conclusion to 402/406/409
(previously just "no consumer found", now also "not populated on any
shipping variant" — consistent, not contradictory, with D402 already
being a confirmed *live* input in `FUN_2003c0ec`: the bit exists and is
read by firmware, it's just apparently never asserted on real hardware
in production, at least across the versions this manual covers).

**Conflict — resolved 2026-08-28, in the opposite direction from the original writeup.** The
"D420 has no tag, contradicting the JP-only claim" conclusion above was itself based on a misread (see the
correction note in the population-data section above): re-extracted via `pdftotext`, **D420 really is
`Only [#01]`** — Japan alone, exactly like D423. The user's original domain-knowledge lead ("D423 and D420
are both JP-model-only") is now **fully confirmed by the official parts list**, not contradicted. Both
halves of the original hypothesis (D423 = Emergency Mode, D420 = Language) are back on equal footing as
population-confirmed Japan-only diodes; D420's code consumer is still not found (see below), but "populated
everywhere so can't be a JP/export differentiator" is no longer a reason to doubt the Language hypothesis.

**D419 Japan-only lead — retracted (2026-08-28).** The "New, sharply-focused lead for D419: it's Japan-only"
conclusion from the previous pass was based on the same misread that swapped D419/D420's rows (see
correction note above). Re-extracted via `pdftotext`: **D419 has no version tag — populated on every
variant**, same as most of the unresolved diodes. This reframing doesn't hold; back to D419 being a genuine
"no consumer found in 4 sessions across every angle tried" diode with no distinguishing population data to
narrow the search by. The user's service-manual caution ("must not be removed") remains the only lead, still
consistent with a hardware-relevant rather than software-feature-gate role.

**Derived region-code hypothesis — NOT verified, arithmetic only, check
before trusting**: combining this table with the already-confirmed
4-bit region-code weights (`D404`=8, `D407`=4, `D410`=2, `D413`=1,
present=1/absent=0) gives an apparent code per version: `USA`→0,
`EUR`→2, `ITR`→4, `ESP`→5, `TPE`→6, `KOR`→7, `EXP`→9. Two of those
(`USA`=0, `EXP`=9) fall **outside** the previously-confirmed valid range
(1–7, from `DAT_2003c7fc`'s 16-entry table).

**Update (2026-08-28): a parts-list misread is no longer a live explanation for this.** All four of
`D404`/`D407`/`D410`/`D413` were independently re-confirmed exactly as originally read, via direct
`pdftotext` extraction of the manual (see the population-data correction note above — the misreads found
this session were all in *other* rows: D405/D416/D417/D419/D420). So the anomaly is real: either the
bit-weight formula itself is wrong (inverted polarity, wrong bit order, or an offset not accounted for), or
the region-code table's mapping to named versions isn't the simple direct one assumed here. **Don't treat
this derived table as fact** — still worth checking directly (e.g. against the user's own board's diode
population and its known market variant), just not via re-reading the parts list again — that's now been
done as carefully as it can be from this document.

## D419/D422 resolved, and D401-absent behavior fully traced (5th session)

Triggered by the user asking what happens when D401 is absent. Traced `FUN_2003c0ec` (the diode-scan
follow-up already documented above) end to end via Ghidra (MCP back up this session): it builds **two**
frequency-range lists — an RX list via `FUN_2003bd80` and a TX list via `FUN_2003be94` — then intersects
them with `FUN_2003bfc8`. Both list-builders share an identical structure and, critically, a previously
unexamined table-selector call.

**D419 (bit 6) and D422 (bit 7) — confirmed, first consumer found in 4 sessions.** `FUN_2003be94`'s TX
table selection calls `FUN_2003bd34(scan_value)`, which extracts exactly these two bits (D422 = bit 7,
D419 = bit 6 — nobody else's bits) and returns a 0-3 selector:

| D419 | D422 | Selector | TX table (decoded from data, both HF ranges merge into one continuous range where adjacent) |
|---|---|---|---|
| absent | absent | 1 | `1.6–30 MHz` + `50–54 MHz` (standard HF ham bands + 6m, gap between them) |
| absent | **present** | 2 | `1.6–54 MHz` continuous (HF/6m gap filled) |
| **present** | absent | 3 | **`0.1–74.8 MHz` continuous** — the exact figure from the mod-guide/service-manual "open TX" claim |
| **present** | **present** | 0 | defers to a per-region-code table array (region 0 decoded: textbook USA plan — 160/80/60(5.255-5.405)/40(7.0-7.3 full)/30/20/17/15/12/10/6m(50-54)) |

**Reassigns which diode the "0.1-74.8MHz open TX" folklore actually describes.** The user's external
lead attributed that figure to D422; the code shows it's actually **D419 present with D422 absent** that
produces it — D422 alone (D419 absent) only fills the HF/6m gap (1.6–54 MHz), a narrower effect. Also
notable: since the (corrected, see above) parts list shows **both D419 and D422 populated on every
shipping version**, every real unmodified radio hits the selector-0 row (per-region table) — the flat
"open" ranges above only apply if a diode has been physically removed, i.e. this is precisely the
mechanism the mod-community folklore is describing. This also gives a concrete, code-level reason for
the service manual's "D419 must not be removed" caution beyond just "it's a factory-set strap": removing
it (with D422 still present) moves the radio onto an unswept/uncharacterized TX range outside the
per-region table's tested band segments.

Region-code table array cross-check: region 5 and 6's TX-table pointers are the *same addresses* as
their RX-table pointers in the already-documented D416 general-coverage array — i.e. the same enumerated
ham-band table (160m through 10m, decoded: 1.8-1.9/3.5-3.5125/5.255-5.405/7.0-7.1/10.1-10.15/14.0-14.35/
18.068-18.168 MHz for region 5) serves both RX and TX gating there, a solid internal-consistency check
that the table parsing here is correct. Region 0's TX table (decoded above) matches the textbook USA
band plan exactly (full 7.0-7.3 MHz 40m, 28-29.7 MHz 10m, no 70 MHz/4m) — first concrete evidence for
what region-code 0 (the arithmetic-derived "USA" guess) actually corresponds to. Regions 1 and 7 were
also spot-checked: region 1 is byte-for-byte identical to region 0 (same plan); region 7 differs only in
160m's lower edge (1.81 MHz instead of 1.80 MHz) — plausibly EXP, since it's the only version whose
diode combination (D404 present, which is EXP-exclusive per the parts list) can reach region-code 7 at
all. Regions 2/3/4's tables not yet pulled.

**D401 absent — traced precisely, not just asserted.** Both `FUN_2003bd80` and `FUN_2003be94` gate each
source-table entry on: `(D401 present) OR (entry's min frequency >= ~74.906 MHz)` (constants
`DAT_2003c814`/`DAT_2003c818` resolve to this threshold). When D401 is present this is always true
(normal operation). When D401 is **absent**, every real table checked — the default HF/6m table, the
region 5/6 ham-band table, and all four D419/D422 TX tables above — has every entry's minimum well below
74.9 MHz, so **no entry survives the filter for either list**. The loop's unconditional tail then writes
a single output range `[0, 0xFFFFFFFF]` regardless — i.e. **D401 absent doesn't select a more permissive
table, it makes the entire RX+TX range-restriction mechanism degenerate into "every frequency is valid."**
Consistent with `is_feature_enabled_for_region`'s other D401-gated menu/feature codes, essentially all of
which resolve to their permissive "enabled" arm when D401 is absent rather than performing the
region-code check at all.

**Caveat**: the general-coverage RX-unlock table (`0x8f`/`0x90-0x93`/`0x95`/`0x96`/`0xdf`-`0xf6` item
codes, documented in the 3rd-session section below) is gated by a wholly separate condition — D416
present + region-code 5-or-6 — that never tests D401. That specific table's behavior is unaffected by
D401 either way.

**Updated tally**: 2 more of the long-unresolved diodes confirmed this session (D419, D422). Remaining
fully unresolved: D408, D411, D414, D417, D420 (D420 now has confirmed Japan-only population data but
still no code consumer found).

## D406/D409/D423 found via raw ARM disassembly, three of twelve unresolved diodes resolved (4th session)

**Methodology note, worth recording**: this session's `ghidra` MCP server connection failed at startup (confirmed the Ghidra process itself was alive and answering HTTP on `127.0.0.1:8080` — `curl` got a normal MCP protocol response — but the session's tool registration was stale/refused and could not be revived without a session restart). Rather than block, did this entire round of digging via **raw disassembly of the extracted `body.bin` with `arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x20005000`**, no Ghidra involved at all. Cross-checked the technique against already-documented functions first (`FUN_2003c530`'s call chain matched the existing notes exactly) before trusting new reads. This worked well for straight-line ARM decoding but has none of Ghidra's xref database — "does anything else reference this address" had to be answered by grepping a full linear disassembly for literal pc-relative loads of the address in question, which only finds direct/simple references, not computed ones. Once Ghidra access is back, these new leads (especially the bit-reversal export function below) are exactly the kind of thing its xref search would finish off in minutes.

**D406 (bit 17) and D409 (bit 18) — confirmed.** `FUN_2003c530` (the master diode-init routine, already documented below) does more than orchestrate the scan: right after the scan and its two follow-up calls (`FUN_2003c0ec`, `FUN_2003c4dc`), it re-reads the live scan value and tests these two bits directly:
```
tst  r0, #0x20000      ; bit 17 = D406
beq  <D406 absent>
mov  r0, #1                    ; D406 present -> classification = 1
b    <store>
<D406 absent>:
tst  r0, #0x40000      ; bit 18 = D409
beq  <neither>
mov  r0, #2                    ; D409 present -> classification = 2
b    <store>
<neither>:
mov  r0, #0                    ; classification = 0
<store>:
strb r0, [r4, #1]              ; r4 = pointer at DAT_2003c800, i.e. the same
                                ; control struct whose +4 is the live scan value
```
The resulting byte is then also mirrored to a second struct (`*DAT_2003c858 + 0x29`). **D406 takes priority if both are present.** Confirmed the bit-17/18 read really is the live diode-scan value: `DAT_2003c800`'s stored word is a pointer (0x20390210 in this firmware build), and `[that pointer + 4]` resolves to the exact same live address that `DAT_2003c7f8` and `DAT_2003ea4c` also point to — three different globally-named pointers all aliasing the one live scan cell. What the resulting 0/1/2 classification actually *drives* isn't traced yet (would need to find readers of `struct+1`/`struct+0x29` — no xref tool available this session to chase that, see methodology note above).

**D423 (bit 15) — confirmed, and it's a strong hit.** Found directly in `FUN_2003df34`, the "is_feature_enabled_for_region" master gatekeeper documented further below, and its sibling `FUN_2003dcc0` (a generic "read a setting's current value, formatted per its type descriptor" function — same value-format-byte scheme as the already-documented menu-item table). Both load the live scan value via yet another alias pointer, `DAT_2003ea4c` (confirmed to resolve to the same live cell as above), and `tst r2, #0x8000` (bit 15) directly gates hard-coded special-case overrides for several item codes (at least `0x22`, `0x32`, `0x4b`, `0x71`, `0x73`, `0x79`, and the `0x8f`-`0x93`/`0x94`/`0xe5` range inside `FUN_2003df34`'s dispatch) — these bypass the generic type-formatted read path entirely, consistent with them being synthetic/computed values rather than plain stored settings. This is D423 sitting directly inside the *same* function that gates every other confirmed regional/regulatory feature in this firmware — about as strong a piece of static evidence as this project has found for "D423 gates a real regulatory feature," matching the user's Emergency Mode hypothesis, though the specific item code that *means* Emergency Mode isn't pinned down yet (would need the Set Mode string table cross-reference, still open per below).

**New lead for the remaining unknowns — a likely full-settings export/clone path.** Found two new, previously-undocumented functions while chasing the above:
- **`FUN_2003c70c`**: takes the full 24-bit live scan value and **reverses the bit order within each of its three row-bytes** (bit 0↔bit 7, 1↔6, etc., independently per byte), writing 3 output bytes — i.e. repacking the internal scan-shift bit order into the "natural" column-reading order (D401 as the high bit, not the low bit). Called from exactly two places: (1) `0x200100a4`, inside a small helper that looks like it's encoding the diode bytes as two BCD-style nibbles; (2) `0x200397d8`, inside a much larger function that first calls **seven** other field-builder functions at fixed offsets (`+1`, `+0x29`, `+0x51`, `+0x79`, `+0x39`, `+0x61`, `+0x89` — evenly spaced, classic fixed-record layout) before appending the reversed diode bytes and passing everything through a generic copy/format routine (`FUN_2003903c`).
- **`FUN_2003ddd4`**: loops item codes `0` through `0x146` (326 total) calling `FUN_2003dcc0` (the value-formatted setting reader that also tests D423 above) for every single one — reads unmistakably like a **bulk "dump every setting" export**, the shape of Icom's clone/backup-to-another-radio or factory-test full-dump mechanism, not a single menu screen.

Neither function's own caller has been traced further up (what triggers the export, where the output buffer ends up — CI-V response, SD card, EEPROM). But a function that touches *all* diode/region bits at once and feeds into what looks like a full clone/settings dump is exactly the kind of place D408/D411/D414/D417/D419/D420/D422 (the remaining fully-unconfirmed diodes) would plausibly surface, if they're read at all outside the region-code/RX-table/D423 paths already found. **Top lead for next session, especially once Ghidra access is back** — xref-searching these two functions' callers and `FUN_2003903c`'s other callers would likely resolve this quickly.

**Also identified**: `FUN_2003c530`'s very first call, to a previously-undocumented tiny function now readable as `FUN_2003bc58` — it tail-calls the generic settings getter for parameter `0x3e44` *into the live scan buffer*, **before** the hardware scan runs, capturing the last-persisted value as `r5`/`[sp]` for the change-detection pass (`FUN_2003c27c`/`FUN_2003c174`) that happens at the end of the function, after the fresh scan has overwritten the buffer. Minor completeness addition to the already-documented call chain, not a new finding in its own right.

**Bits 19-23 (top row's non-existent columns 1-5) — confirmed genuinely untested** anywhere in the ~9KB `0x2003ba00`-`0x2003e000` diode-cluster code region (checked via a full linear ARM disassembly of that range, grepped for every `tst`/`ubfx` instruction). Consistent with those positions being real hardware N/A pads, not just "not found yet."

**Updated tally**: of the 12 diodes that were "no consumer found"/"unconfirmed" going into this session (402 was already partial), **3 are now confirmed** (406, 409, 423). **7 remain fully unresolved**: D408, D411, D414, D417 (never had any hypothesis), D419, D420, D422 (all three have hypotheses, still no code consumer found by any method tried across 4 sessions now).

## D405/D402 found, D419 still not found (3rd session)

Traced the master diode-init routine, `FUN_2003c530` (calls
`scan_diode_matrix_p5` → `sync_diode_matrix_to_eeprom` → two previously
unexplored functions `FUN_2003c0ec`/`FUN_2003c4dc`). `FUN_2003c0ec`
feeds bits 0 (D401), 5 (D416, inverted), 8 (D402, inverted), and 9
(D405, inverted) into `FUN_2003bd80`, an interval-merge algorithm that
builds a list of allowed frequency ranges from a source table (selected
per-region when D416 is present, a shared default table when absent).

**D405 resolved**: its (inverted) bit gates a single-value exclusion
check against `DAT_2003c81c` = **5.255 MHz** — confirmed as a real band
boundary, not a coincidence (`DAT_2003c81c`'s neighboring memory holds
7.000 MHz / 7.300 MHz, the 40m band edges). D405 present → the 5.255 MHz
range is excluded from the allowed list; D405 absent → included. This
directly confirms the "D405, not D403" external claim for 60m/5MHz band
access — first hard evidence resolving that specific conflict.

**D402 resolved (partially)**: also a real input to the same function
(inverted), gating a boundary-snap/clamp behavior — when a range edge is
within a threshold distance of some boundary (`DAT_2003c814`/`DAT_2003c818`),
D402 controls whether that edge gets replaced with an alternate value
from `DAT_2003c820`/`DAT_2003c824`. Confirmed real; the practical
end-user effect (what those alternate values represent) not yet traced.

**D419 (bit 6) still not found** — checked `FUN_2003c530`'s full call
chain (`scan_diode_matrix_p5`, `sync_diode_matrix_to_eeprom`,
`FUN_2003c0ec`, `FUN_2003bd80`) and re-confirmed the same 8 references to
`DAT_2003c7f8` as before (no new xrefs from recent disassembly fixes).
Bit 6 doesn't appear in any consumer found so far, across two separate
digging sessions from different angles.

**Resolved, 5th session** — see "D419/D422 resolved" section above. The consumer wasn't in any of the
functions already traced from this call chain; it's `FUN_2003be94`'s TX-table selector, a sibling function
to `FUN_2003bd80` not fully examined until then.

**Started a broader EEPROM parameter catalogue** (see
[[eeprom-catalogue]]) on the theory that some diode bits might be read
via the EEPROM-persisted value (parameter `0x3e44`) directly rather than
the live RAM copy (`DAT_2003c7f8`) we've been tracing. Found a genuine
third consumer of `0x3e44` this way (`FUN_2006cb84`, part of a versioned
settings-format loader), but traced it to a struct field
(`+0x1a7c`) with no further consumer found either — same wall, different
path. D419 still open.

## D422 dig (2nd session) — still unconfirmed, ruled out two hypotheses

Tried three angles to find D422's ("open TX 0.1–74.8MHz") consumer,
given D416's confirmed RX table as a template:

1. **Sibling functions in the same dispatch table as the RX check**
   (`FUN_2000edb0`/`FUN_2000f330` live in a function-pointer table at
   `0x2018b430` alongside `FUN_2000ef5c`, `FUN_2000ed7c`, `FUN_2000ef1c`
   — checked all three as TX-check candidates). Dead end: `FUN_2000ef5c`
   is a trivial `return 0` stub, `FUN_2000ed7c` is an unrelated small
   config-setter, `FUN_2000ef1c` is also unrelated. None do a
   frequency-range check.
2. **TX-denial user-facing strings** (`"TX Inhibit"`, `"band edge"`,
   `"User Band Edge"`) — found all three as real strings, but none have
   any static reference (same wall as everywhere else), and "TX Inhibit"
   reads as a manual PTT-disable menu item (unrelated to region gating)
   while "User Band Edge" is a *user-configurable inner limit* — a
   different concept from the diode/region system's factory-set outer
   bound, not the mechanism itself.
3. **Structural parallel to the RX table** — checked whether the
   menu-item table's `0x104`/`0x105`/`0x10e`/`0x114`/`0x117` codes
   (initially flagged as a candidate parallel table) share the RX
   table's min/max-pair format (`gate_type=0x0a`). They don't — they use
   `0x01`/`0x02`/`0x0b` (single-value formats), so they're unrelated
   individual settings, not a structurally-parallel TX table.

**Correction from user**: the TX-capable indicator updates continuously
as you tune, *before* PTT is pressed — so it's evaluated in the same
"frequency changed, update display" path as the RX check, not a
separate PTT/key-down function. Retracted the "check the PTT path"
suggestion above accordingly.

**Also retracting part of angle 1 above**: re-examined `0x200105c0`,
`0x2000f314`, `0x20010654`, `0x20010664` (the other addresses I'd read
out of the `0x2018b430` table) directly — they're not function pointers
at all, just data words that happen to *decode* as plausible-looking
single instructions when read as code (e.g. `asrs r0,r4,#0xa`). My
original reading of that table as a `{function, tag}` dispatch was
mistaken.

**Not resolved, and genuinely hit diminishing returns on static tracing
for this specific question** — four distinct angles tried (dispatch-table
siblings, TX-denial strings, structural table matching, the corrected
table re-read above), all dead ends. Good candidate to set aside for
live verification (watchpoint on the TX-indicator state variable, or
single-stepping the display-update routine while tuning across a band
edge) rather than continue speculative static searching.

**Resolved, 5th session** — see "D419/D422 resolved" section above. `FUN_2003bd34`/`FUN_2003be94`,
neither of which existed in this project's notes until then. Turns out the real "0.1-74.8MHz open TX"
effect is D419's, not D422's — D422 alone only extends TX to 1.6-54 MHz.

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

Country/market name correlation (JAP/USA/EUR/ITR/ESP/TPE/KOR/EXP from
the user's photo) to these region codes 1-7 is **not yet confirmed** —
see the "real-world confirmation" section below for the open numbering
mismatch (internal codes only go to 7, Icom's public numbering goes to
at least 12).

## The scan routine: `FUN_2003bb88` (body.bin, RAM)

Uses the RZ/A1H manual's documented Port 5 data register, `P5` at
`0xFCFE3014` (found via `grep` against the manual text — `PM5`/`PMC5`
mode-control registers are at `0xFCFE3314`/`0xFCFE3414`, not directly
referenced by this routine, so pin direction setup for P5 happens
elsewhere, not traced). Located by searching `body.bin` for the register
block's base literal `0xFCFE3000` — found 14 candidate references,
checked each; this was the one touching offset `+0x14`.

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

Worked out from the shift math (each iteration's data shifts right once
per *remaining* iteration, so earlier columns end up at lower final bit
positions): final result packs as **bits `[0-7]` = row-bottom (`P5_8`)
results for columns 8,7,6,5,4,3,2,1** (bit0=col8, bit7=col1), **bits
`[8-15]` = row-middle (`P5_9`)** same column order, **bits `[16-23]` =
row-top (`P5_10`)** same order (only col6/7/8 meaningful — cols 1-5 are
the schematic's N/A positions). Mapped onto the user's diode numbers:

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

## Confirmed consumers — this firmware only actually reads a few of these

- **`FUN_2003bca8(value)`** — extracts **bits 1-4** (weighted 8,4,2,1)
  into a 4-bit index, looks up a byte from a table at `DAT_2003c7fc`.
  Bits 1-4 = **diodes 404, 407, 410, 413** (row-bottom, columns 7-4).
  **Read as: a 4-bit binary-coded selector (16 possible values) into a
  configuration table** — the classic pattern for a factory-set
  region/market variant selector (different frequency allocations per
  country). Table contents at `DAT_2003c7fc` not yet decoded.
- **`FUN_2003c634`/`FUN_2003c6c0`** — both test **bit 0 (diode 401)** to
  gate whether the region-mode value (from `FUN_2003bca8`) is checked at
  all, and separately test **bit 5 (diode 416)** — when the mode value is
  5 or 6 *and* diode 416 is present, a feature gets force-disabled
  (return value flips to 0). Reads as: diode 401 = "region check
  applies", diode 416 = some kind of override/service flag interacting
  with specific region-mode values.
- **`FUN_2003c5d4`** — tests **bit 5 (diode 416)** standalone; if
  present, returns a value from a different table (`DAT_2003c85c`,
  indexed by a byte at `DAT_2003c800+2`) — diode 416 gates a second,
  separate lookup.
- **`FUN_2003c5c4`** — returns **bit 16 (diode 403)** directly as a raw
  value (not just tested as boolean) — used as data somewhere, not yet
  traced further.
- **`FUN_2003bc68`** — ties the whole thing to persistent storage: reads
  a previously-saved 4-byte value under parameter tag **`0x3e44`** (via
  `FUN_2001e510`, a generic settings-store getter), `memcmp`s it against
  the live scan (`DAT_2003c7f8`), and writes the new value back (via
  `FUN_2001e484`, the setter) if it changed. **The diode-matrix state is
  persisted to the EEPROM every boot** — confirms the diode matrix and
  general EEPROM settings storage share the same underlying
  parameter-ID-keyed get/set API, tying together both parts of the
  user's original question.

Not yet confirmed by this firmware *(as of the 2nd session; superseded —
402, 405, 406, 409, and 423 are now all confirmed, see the 3rd- and
4th-session sections below)*: diodes 402, 405, 408, 411, 414, 417,
419, 420, 422, 423, 406, 409 (middle row entirely, most of top row, and
the higher bottom-row columns) — no reference to those specific bit
positions found in `body.bin`. Either genuinely unused in this
build/market, reserved for other hardware variants, or read by code not
yet located (e.g. via the same "3.4.2024" era functions not yet
traced, or possibly base.dat rather than body.bin — not checked).

## Decoded: this is Icom's regulatory/regional feature-restriction system

Traced the lookup tables and callers. `DAT_2003c7fc` (the 4-bit
region-code table diodes 404/407/410/413 index into) contains, at
indices 0-15: `[0,1,2,0,3,4,5,6,0,7,0,0,0,0,0,0]` — only 7 of the 16
possible 4-diode combinations map to a non-zero region code (1-7); the
rest (including "no diodes populated") map to 0/default. `DAT_2003c85c`
(diode 416's separate table) is `[0×13, 2, 0, 3]` — only two non-zero
entries, at indices 13 and 15 (index itself comes from a runtime value,
not statically known).

**Diode 403 is a clean, direct confirmation**: `FUN_200134b4` returns
ASCII `'1'` if diode 403 is present, else `'2'` — **this is Icom's real
"Type 1"/"Type 2" market/regulatory variant designation**, the same one
printed on their spec sheets and manuals.

**`FUN_2003df34`** (called from at least 4 places) is a large
feature/menu-item gatekeeper: takes a numeric item code (~40+ distinct
values seen, ranging `0x24`–`0xf6`) and returns whether that item is
enabled, via a big dispatch checking:
- diode 401 (`uVar2 & 1`) — "does the region restriction apply at all"
- the region code (1-7, from diodes 404/407/410/413) via
  `FUN_2003c634`/`FUN_2003c6c0` — different codes gate different items
  differently (some check the code is in `{2,3,4}`, others treat
  `{5,6}` as special — see `FUN_2003bd04`, reused consistently to mean
  "special region" across the gating system)
- diode 416 (`uVar2 & 0x20`) combined with `FUN_2003bd04`
  (region code 5 or 6) — a secondary override path

This is exactly the shape of Icom's real-world regulatory feature
restriction (extended/out-of-band TX range, band-plan restrictions, etc.
— the kind of thing that legitimately varies by country's amateur radio
licensing rules), gated by a factory-set hardware strap (the diode
matrix) rather than firmware alone — consistent with needing to survive
a firmware *update* without the region designation changing (matches
why it's persisted to EEPROM and re-synced every boot, not just held in
RAM).

**Not yet decoded**: which of the ~40+ numeric item codes (`0x24`-`0xf6`)
corresponds to which actual named feature/menu item — would need
cross-referencing these codes against wherever the menu system's own
item-ID scheme is defined (not yet located). Also not decoded: what
region codes 1-7 correspond to as actual country/market names (no
string table found tied to these small integers directly — would need
tracing further, or may simply not be spelled out anywhere in the
firmware as human-readable strings at all).

## Found the menu-item table — CORRECTED: byte0 is a value-format code, not a "gate category"

`get_next_hidden_menu_item` (`FUN_2000d380`) indexes a table at
`DAT_2000e230` (real address `0x2018a698`) by **menu item number**
(0-based, up to `0xda`-1 = 217 items — so **216 menu items total**),
4 bytes per entry: `[format_type_byte, flag_byte, region_code_lo,
region_code_hi]`. `region_code` (the 2-byte value) is passed directly to
`is_feature_enabled_for_region`. Dumped and decoded all 216 entries —
distribution by the first byte:

- **128 items**: `0x00`
- **52 items** (menu indices 111-215): `0x0a`
- **8 items** (indices 103-171): `0x0b`
- **Remaining ~28 items**: spread across `0x01`-`0x09`, mostly 1-4 items
  each

**Correction, found by tracing further**: `get_next_hidden_menu_item`'s
own caller, `FUN_2000ffb0`, feeds this table's first byte into
`FUN_2000fd48` as `param_2` of a `switch` — and `DAT_20010854`
(referenced there) resolves to the **exact same address**
(`0x2018a698`) as `DAT_2000e230`. **This is the same table, and that
first byte is a value-formatting type code**, not a region-gate
category: `0`=byte, `1`=word, `4`/`5`=BCD-like time/frequency scaling
(note the `%60` — classic minutes/seconds formatting), `8`=raw blob
copy, `0xa`=formats *two* values back-to-back (a min/max pair). The
correlation with region-gating I originally read as "gate category" is
real but indirect: band-edge/frequency-limit settings naturally need
*both* a min/max-pair format *and* region gating — same underlying
setting, two separate properties of it, not one causing the other.

This also reframes what this code path is for: it builds a byte-packed
**value representation** of a setting (not a display string) — plausibly
the serialization for CI-V get/set-setting commands or EEPROM read/write,
not the UI menu label. **Menu item *names* are therefore still a
separate, not-yet-found table** — correlating against the "Set Mode"
menu string table (`~0x2032f000`-`0x20360000`, see [[firmware-update]])
remains open, and this value-formatting table isn't the bridge to it
after all.

## Real-world confirmation: Icom sells/documents named regional "Version" variants

User found a reference image (3rd-party site) showing 8 labeled IC-7300
"Versions" with their diode-population patterns: `JAP #01`, `USA #02`,
`EUR #03`, `ITR #05`, `ESP #06`, `TPE #07`, `KOR #08`, `EXP #12`
(numbers skip 4, 9, 10, 11 — likely more variants exist beyond these 8).
Matrix in that image is rotated 90° clockwise relative to the user's own
PCB-derived layout above, but the diode numbers (401-423) are the same
scheme. **This independently confirms the region/variant mechanism is
real, not just a plausible reading of the disassembly** — Icom really
does ship country-specific hardware-strapped variants of this exact
shape.

**Upgraded to official confirmation, 2026-08-27**: the service manual
itself (§ Introduction, page 1, user-supplied) has an authoritative
`MODEL/VERSION/VERSION NUMBER/OPERATABLE BANDS` table, matching the
third-party photo's numbers exactly (`USA #02`, `EUR #03`, `ITR #05`,
`ESP #06`, `TPE #07`, `KOR #08`, `EXP #12` — no `JAP #01` row in this
particular manual, otherwise identical) — no longer a third-party-photo
claim, this is Icom's own documentation. It adds one new fact the photo
didn't give us: **band access grouped by version** — `EUR`/`ITR`/`ESP`
are `HF/50/70 MHz` (70 MHz/4m band unlocked), while `USA`/`TPE`/`KOR`/
`EXP` are `HF/50 MHz` only (no 70 MHz). This lines up with the confirmed
D405 60m-band finding and [[ic7300-signal-chain]]'s spec-sheet numbers
(`70.000000~70.500000 MHz` transmit, marked "depending on the
transceiver version" — same page also independently confirms the 60m
segment as `5.255000~5.405000 MHz`, exactly matching this file's D405
finding, and general-coverage receive as `0.030000~74.800000 MHz`,
matching D416's finding to within rounding — see [[ic7300-signal-chain]]
for the source).

**Not yet reconciled precisely**: our internally-decoded 4-bit region
code (diodes 404/407/410/413) only produces valid values 1-7 (`DAT_2003c7fc`'s
table, see above) — 9 of 16 possible diode combinations map to 0/invalid.
But Icom's own public "Version #" numbering goes at least to 12. Possible
explanations, not yet checked: this firmware version's table doesn't
cover all variants ever made; the public "Version #" catalog number
differs from the internal `region_code` value and there's another
translation step between them; or the numbering schemes are simply
unrelated (Version # = a service/parts catalog identifier, region_code =
purely internal firmware logic). Worth deliberately reading a couple of
diode patterns pixel-by-pixel from a higher-res version of that image
(or the user's own board) and computing the resulting region_code via
the confirmed formula (bits 1-4 of the diode-matrix scan, weighted
8,4,2,1) to check whether they line up directly.

## User lead: D423/D420 are JP-model-only — likely Language / Emergency Mode

User's domain knowledge: diodes 423 and 420 are populated only on the JP
(Japan) variant, hypothesized as enabling (a) a language-related setting
and (b) Japan's "Emergency Mode" (permits transmitting into a
poorly-matched antenna at reduced power — a real Japanese amateur radio
regulatory allowance for disaster/emergency communication).

Mapped to bits via the confirmed formula (row-middle, bit = `16 -
column`): **D423 (middle, col1) → bit 15**, **D420 (middle, col2) → bit
14**.

Checked all 8 known consumers of the live diode-scan value
(`DAT_2003c7f8`) — none test bits 14/15 *(superseded, 4th session: bit 15/D423
**is** now a confirmed consumer, in `FUN_2003df34`/`FUN_2003dcc0` — see the
4th-session section above; bit 14/D420 is still unconfirmed)*. Independently
confirmed both named features are real and present in this firmware via
string search:
`"SPEECH Language"`/`"Display Language"` (menu items), `"EMERGENCY
MODE"` (banner string) and `"EMERGENCY"` (a top-level Set Mode category
alongside RX/TX/DISPLAY/KEYER MEMORY). The `"EMERGENCY MODE"` banner is
drawn by `FUN_2009060c`, a screen-rendering function — it displays the
banner based on an already-computed flag, but doesn't itself test the
diode bits, so its setter is what would need to be found.

**Not resolved (D420 only) — the setter for that flag isn't among the 8
known diode-scan consumers** *(D423's half of this is now resolved, see
4th-session section above — real, direct bit-15 consumer found in
`FUN_2003df34`/`FUN_2003dcc0`)*. Two explanations not yet distinguished for
D420: (1) it's read via the EEPROM parameter path (`0x3e44` through the
generic `FUN_2001e510` getter) independently of the live RAM copy we've
been tracing — haven't exhaustively checked all callers of that generic
getter; or (2) it's in a code path/task not yet reached by any trace so
far. Good candidate for live verification once JTAG access is available
(watch bit 14, or `0x3e44` EEPROM reads, and see what touches it) rather
than more static tracing.

## Major finding: the band-edge table is the general-coverage RX unlock, confirms D416

User provided several more real-world claims: D416 = "open RX
0.030-74.8MHz", D422 = "open TX 0.1-74.8MHz", D419 = "must not be
removed", and (conflicting external sources) D403 vs D405 both claimed
for 60m/5MHz.

**Checked `is_feature_enabled_for_region`'s 13-row×4-column band-edge
table directly** (`FUN_2000edb0`/`FUN_2000f330`, base `DAT_2000f0c4` =
`0x20190ecc`, row `r` col `0` code = `0x8f + r*8`, min/max stored at
struct offsets `+12`/`+16`). Dumped all 13 rows' min values:

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

**Row 0's min (0.030 MHz) and row 12's max (74.795 MHz) match the user's
quoted D416 range ("0.030-74.8MHz") almost exactly.** This settles what
this table actually is: not separate ham bands, but the **entire
general-coverage receiver spectrum split into 13 contiguous gated
segments** — row 2 (2.0-5.995 MHz) happens to contain the 60m/5.3MHz
allocation, but the table's real purpose is the general-coverage RX
unlock as a whole, not band-specific TX permission.

Traced `is_feature_enabled_for_region`'s dispatch for these row-start
codes: several (`0x8f`, `0x90-0x93`, `0x95-0x96`, `0xdf-0xf6`) require
**both** diode 416 (bit 5) present **and** the region code being exactly
5 or 6 (`FUN_2003bd04`) to return enabled — real, direct confirmation
that diode 416 gates this table, matching the user's claim. Diode 416 is
a necessary but not sufficient condition (needs region 5/6 too, i.e.
diodes 404/407/410/413 landing on that specific code — plausibly the
"EXP"/export variant per the real-world photo evidence).

**Caveat, not glossed over**: tracing one specific row-start code
(`0x97`, row 1) appeared to hit an *unconditional* deny in the dispatch
regardless of any diode. Haven't verified all 13 rows individually — the
core finding (table range matches D416) is solid; whether every row
follows the same diode416+region5/6 rule isn't confirmed.

**D403 vs D405 for 60m — checked D403, doesn't hold up for the
consumer traced.** `get_type1_type2_designation()` (diode 403's only
confirmed consumer) is used elsewhere as a loop bound over a table whose
values (670, 693, 719, 744, 770...) are the **standard 50-tone CTCSS
frequency table** (in 0.1 Hz units) — diode 403 controls whether a 51st,
non-standard tone (150.0 Hz) is available, not band/TX permission. This
doesn't support the "D403 enables 60m" claim for the path traced (though
Type 1/Type 2 could still matter elsewhere, unconfirmed). D405 not yet
checked directly — no consumer found for its bit (9) among the 8 known
`DAT_2003c7f8` references, same situation as D420/D423.

**D422 ("open TX 0.1-74.8MHz") and D419 ("must not be removed") — not
yet confirmed.** Correcting an earlier bit mix-up: bottom row col1=422,
col2=419 (not the reverse), so **D422 = bit 7, D419 = bit 6**. Neither
found among the 8 known consumers. D422's claimed range being so close
to the confirmed
RX table's range (0.1 vs 0.030 MHz low end, same 74.8 MHz high end)
suggests a parallel TX-specific table might exist nearby in memory —
worth checking `DAT_2000f0c4`'s table for a second nearby table, or
searching for a second 13-row structure with min starting at 100,000 Hz
instead of 30,000 Hz, if pursued further.

## Open questions
1. What's actually in the 16-entry table at `DAT_2003c7fc` (region-code
   lookup) — would directly reveal what the diode 404/407/410/413 code
   selects (region name, band plan, etc.).
2. What table `DAT_2003c85c` (diode 416's lookup, indexed by
   `DAT_2003c800+2`) contains.
3. Where diode 403's raw bit value (`FUN_2003c5c4`'s return) actually
   gets used.
4. ~~Why P5's direction/mode registers weren't found being set~~ —
   partially resolved: checked all references to `PM5`/`PMC5`
   (`0xFCFE3300`/`0xFCFE3400` bases) and `PIBC5`/`PBDC5`
   (`0xFCFE7000` base) — none show a clean, simple `+0x14` (P5) offset.
   Found instead: two large, generic bulk port-initialization functions
   (`FUN_200b4320`, `FUN_200b4494`) that configure dozens of ports'
   registers together via bit-masked address computation (not simple
   per-port offsets) — P5's setup is very likely folded into one of
   these as part of a general boot-time GPIO sweep, but verifying the
   *exact* sub-expression reliably by hand isn't tractable (real risk of
   misreading the bitmask arithmetic). Not worth pursuing further
   statically — would resolve trivially by single-stepping through
   either function once JTAG access is available (see
   [[hardware-debug-access]]).
