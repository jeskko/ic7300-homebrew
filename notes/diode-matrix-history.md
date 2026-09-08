# Diode matrix — full session history

Full session-by-session narrative and evidence trail for the base-configuration
diode matrix (P5 GPIO) investigation. See
[notes/diode-matrix.md](diode-matrix.md) for the current living-reference
summary — what's confirmed, the up-to-date tables, and open questions.

This file is the permanent record: every session's findings, false starts,
and corrections, in chronological order. Nothing here is cut or summarized;
`diode-matrix.md` is the condensed, current-state view built from this.

## Initial investigation

The foundational scan-mechanism writeup, initial bit-layout derivation, first
confirmed consumers, and the first round of decoded tables/hypotheses,
before any of the numbered follow-up sessions below. (The scan routine,
confirmed bit layout, and living-reference tables from this investigation
are kept up to date in `diode-matrix.md`; this section preserves the
original narrative as first written.)

### Living reference: region code from diodes 404/407/410/413

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

### The scan routine: `FUN_2003bb88` (body.bin, RAM)

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

### Confirmed bit layout of the scan result (`DAT_2003c7f8`)

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

### Confirmed consumers — this firmware only actually reads a few of these

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

### Decoded: this is Icom's regulatory/regional feature-restriction system

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

### Found the menu-item table — CORRECTED: byte0 is a value-format code, not a "gate category"

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

### Real-world confirmation: Icom sells/documents named regional "Version" variants

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

### User lead: D423/D420 are JP-model-only — likely Language / Emergency Mode

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

### Major finding: the band-edge table is the general-coverage RX unlock, confirms D416

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

**Resolved, 5th session** — see "D419/D422 resolved" section below. `FUN_2003bd34`/`FUN_2003be94`,
neither of which existed in this project's notes until then. Turns out the real "0.1-74.8MHz open TX"
effect is D419's, not D422's — D422 alone only extends TX to 1.6-54 MHz.

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

**D402 resolved (partially this session, fully in the 6th — see that section below for the concrete
effect)**: also a real input to the same function (inverted), gating a boundary-snap/clamp behavior — when
a range edge is within a threshold distance of some boundary, D402 controls whether that edge gets
replaced with an alternate value from `DAT_2003c820`/`DAT_2003c824`. Confirmed real; the practical
end-user effect (what those alternate values represent) not yet traced. **Correction, 6th session**: the
threshold constants are `DAT_2003c80c`/`DAT_2003c810` (not `DAT_2003c814`/`DAT_2003c818` as first
written here — those are a *different* pair, used by the unrelated D401 absent-fallback check found in
the 6th session).

**D419 (bit 6) still not found** — checked `FUN_2003c530`'s full call
chain (`scan_diode_matrix_p5`, `sync_diode_matrix_to_eeprom`,
`FUN_2003c0ec`, `FUN_2003bd80`) and re-confirmed the same 8 references to
`DAT_2003c7f8` as before (no new xrefs from recent disassembly fixes).
Bit 6 doesn't appear in any consumer found so far, across two separate
digging sessions from different angles.

**Resolved, 5th session** — see "D419/D422 resolved" section below. The consumer wasn't in any of the
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

**Bits 19-23 (top row's non-existent columns 1-5, i.e. diodes 424/421/418/415/412) — re-checked 5th session across the *entire* firmware, not just the diode cluster.** Grepped the complete ~902K-line `objdump` disassembly of `body.bin` for every immediate bit-mask (`tst`/`and`/`bic` with `#0x80000`/`#0x100000`/`#0x200000`/`#0x400000`/`#0x800000`) and every `ubfx`/`lsr`/`asr` extracting bit positions 19-23: zero genuine hits. Two `lsr`-by-19/22 instructions turned up outside the cluster (`0x2006378c`, `0x2002ba48`) but both are unrelated — a compiler multiply-by-constant division idiom and an RTC-style byte-packing routine, neither touching the scan-value aliases. The **only** place any of these 5 bits are read at all is `FUN_2003c70c` (the bit-reversal/repack helper documented above) — but it mechanically reverses *all 24* scan bits into natural column order for its two callers (a BCD-nibble encoder and the likely clone/settings-export builder); it doesn't branch or gate on any bit's value, it just echoes whatever these floating/pad GPIO lines happen to read into the output blob. So: technically touched by one generic repack routine, but never tested/acted on by any real feature check anywhere in the firmware — this reinforces rather than overturns the "genuinely unused hardware N/A pads" conclusion.

**Updated tally**: of the 12 diodes that were "no consumer found"/"unconfirmed" going into this session (402 was already partial), **3 are now confirmed** (406, 409, 423). **7 remain fully unresolved**: D408, D411, D414, D417 (never had any hypothesis), D419, D420, D422 (all three have hypotheses, still no code consumer found by any method tried across 4 sessions now).

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
notable: since the (corrected, see population-data section below) parts list shows **both D419 and D422 populated on every
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
codes, documented in the 3rd-session section above) is gated by a wholly separate condition — D416
present + region-code 5-or-6 — that never tests D401. That specific table's behavior is unaffected by
D401 either way.

**Updated tally**: 2 more of the long-unresolved diodes confirmed this session (D419, D422). Remaining
fully unresolved: D408, D411, D414, D417, D420 (D420 now has confirmed Japan-only population data but
still no code consumer found).

## D402 fully resolved — the practical effect, precisely (6th session)

Follow-up to the 3rd session's partial finding (real input, effect untraced) and the 5th session's D401
work, which required re-reading `FUN_2003bd80`'s clamp logic closely enough to nail this down. Verified
by direct computation (brute-forced the C unsigned-arithmetic semantics, not hand arithmetic — worth
noting since a first pass at this by hand got the window wrong before checking it programmatically) rather
than asserted.

**The clamp window, precisely**: `DAT_2003c80c` (signed) = -7,000,001; `DAT_2003c810` = 299,999. The
guarded condition `(uVar6 + DAT_2003c80c) < DAT_2003c810`, evaluated with C's unsigned-arithmetic
promotion rules (both are cast to `uint` before the add), is true exactly when
**7,000,000 < uVar6 < 7,300,000** — i.e. any range edge that falls *strictly inside* the 7.000-7.300 MHz
window (not "anywhere below 7.3 MHz", which is what a naive signed-subtraction reading suggests). When
true, a min-edge gets replaced with `DAT_2003c820` = 7,000,000 Hz exactly, a max-edge with
`DAT_2003c824` = 7,300,000 Hz exactly. This clamp is shared, byte-identical, between `FUN_2003bd80` (RX)
and `FUN_2003be94` (TX) — same constants, same effect on both.

**What actually falls in that window, in the real per-region tables**: pulled the raw bytes for the
region 5 and region 6 tables (`0x20198cb4`/`0x20198d20`, the two per-region tables that differ from the
plain default — see the "Official per-version population data" / region-code sections above/below). Both
store an unusually **narrow 40m allocation**: region 5 = `7.000-7.100 MHz`, region 6 = `7.000-7.200 MHz`
(everything else in both tables — 160m/80m/60m/30m/20m/17m — is outside the clamp window and unaffected).
Both max edges (7.1 MHz, 7.2 MHz) sit inside the (7.0, 7.3) window and so get snapped up to 7.300000 MHz
exactly by the clamp; the min edges (both exactly 7.000000 MHz) sit right at the window's excluded lower
bound and are never touched.

**The answer**: **D402 gates whether the region 5/6 (export/general-coverage) variant's 40m band gets the
full 7.0-7.3 MHz allocation or a narrower stored one.** D402 **absent** → clamp active → both regions'
40m upper edge forced to 7.300 MHz (full standard allocation) for both RX and TX. D402 **present** →
clamp disabled → the narrower raw value (7.1 MHz for region 5, 7.2 MHz for region 6) applies instead,
restricting 40m TX/RX to that slice.

Per the parts list, **D402 is never populated on any of the 8 documented shipping variants** (same
never-populated status D406/D409 had before those were resolved in the 4th session) — meaning on every
real radio that lands on region-code 5 or 6, this clamp is always active and 40m always gets the full
7.0-7.3 MHz allocation via this mechanism. The narrower stored values, and D402's "populated" state that
would enforce them, are apparently unused by any currently-documented market — plausibly a service/factory
option for some narrower-40m-allocation market not covered by the 8 named variants, or a vestigial
fallback value.

## D408 exhaustive search — genuinely no consumer found anywhere (7th session)

Requested follow-up chase. D408 = bit 10 (middle row, col6). Searched the complete ~902K-line firmware
disassembly from multiple angles, not just the diode cluster (learned from the 5th session that a real
consumer, D419/D422's, turned out to live entirely outside the cluster):

1. **Direct immediate mask** (`tst`/`and`/`bic`/`orr`/`eor`/`cmp` with `#0x400`) — zero hits anywhere in
   the firmware.
2. **Single-bit `ubfx` at start-bit 10** — 5 hits total. One (`0x2003c778`) is inside the already-known
   `FUN_2003c70c` bit-reversal/repack function (same mechanical echo that touches the N/A bits 19-23,
   confirmed by its position — the instruction immediately before the already-documented top-row-byte
   block). The other 4 (`0x200f59d8`, `0x20109660`, `0x2013745c`, plus a byte-level check) all operate on
   registers loaded from completely unrelated struct fields (a `ldrh` from an unrelated offset, and two
   near-identical UI/menu-building routines with large stack frames) — none trace back to any of the four
   known scan-value alias pointers (`DAT_2003c7f8`/`DAT_2003c800`/`DAT_2003ea4c`/`DAT_2003c858`).
3. **Wider `ubfx` fields starting at bit 10** (`#10,#21` and `#10,#5`) — both ruled out: the width-21 one
   is on a value computed as `n*5` right after a `-19` offset (reads as a date/calendar/BCD calculation),
   and the width-5 one is a textbook RGB555/565 colour-channel unpack (paired with a second `#5,#5`
   extract and an `and #31`, classic R/G/B split). Neither is diode-related.
4. **Register-mediated mask** (load `#1024` into a register, then `and`/`tst` against it) — 1024 is an
   extremely common generic constant in this firmware (buffer sizes, offsets: 42+ hits, almost all
   `cmp`/`add`/`rsb` against buffer-length-shaped values). Not checked instance-by-instance because every
   confirmed diode consumer found across this whole project uses a **direct** immediate mask or `ubfx` in
   the same instruction — never a register preloaded elsewhere — so a register-mediated bit test would be
   inconsistent with this codebase's established compiler pattern for these checks.
5. **EEPROM-persisted path** (`notes/eeprom-catalogue.md`) — already checked in the 3rd session: the
   diode value's EEPROM-persisted copy (parameter `0x3e44`, landing at struct offset `+0x1a7c` /
   `0x203b4c5c` in `FUN_2006cb84`'s loaded struct) has **no reader at all**, for any bit — dead end, not
   specific to D408.

**Conclusion**: unlike the confirmed-N/A top-row pads, **D408 is populated on every shipping version**
per the parts list — a real, soldered component on every radio anyone will ever see — yet no code
anywhere branches on its value. Only genuinely new information: it's touched (mechanically, not
meaningfully) by the same bit-reversal repack that echoes all 24 scan bits into the likely clone/
settings-export blob (see the 4th-session section above), so its raw state does end up *somewhere* in
whatever consumes that blob's output — just not tested by any conditional logic found so far. Good
candidate for live verification once JTAG access exists (toggle the position, watch what changes) rather
than further static searching — the same conclusion the 2nd/3rd sessions reached for D422 before it
turned out to have a real (if surprising) consumer, so this isn't necessarily final, just exhausted for
now.

## Cross-version exhaustive search — D408/D411/D414/D417/D420 checked against all 10 known firmware versions (8th session)

User asked whether the `objdump`/raw-binary-grep technique could reach firmware content Ghidra hasn't
disassembled — a good prompt, since every search up to this point (this file's 5th-7th sessions) only
ever covered **one** firmware version. Worth recording precisely what "not currently disassembled in
Ghidra" turns out to mean here:

- **The live Ghidra project (`icom1`) already has more loaded than just `body.bin`** — checked via
  `memory.list_blocks`: it also has `base.dat` (66000 B boot loader, at `0x17ffffd4`) and both update
  slots' `chunk1.ttf`/`chunk2.ttf`/`chunk3.dat` (at `0x18210000`/`0x18600000` and neighbors) loaded as
  memory blocks. Not checked for diode consumers this session (implausible location — boot loader and
  what look like font/resource blobs, not application settings logic — but not literally verified).
- **`chunk5_tail.bin` (~1.6-1.7 MB per version) is not loaded in Ghidra at all**, and was never covered
  by the `objdump` sweeps either — it's LZSS-compressed raw tail data, not a flat binary, and
  `notes/bitmaps.md`/`container-format.md` flag it as the leading candidate for a **second processor's**
  firmware image (a different architecture entirely, most likely) — decompressing and correctly
  identifying it is its own undertaking, not attempted here.
- **`body.bin`'s loaded size (3,738,392 bytes) matches firmware version `142` exactly** — the newest of
  10 versions sitting unpacked in `scratch/unpacked/` (`111`/`112`/`113`/`114`/`120`/`121`/`130`/`140`/
  `141`/`142`). Every `objdump`-based search in this file through the 7th session, and every Ghidra
  query, only ever checked this one version. **This is the part of the question actually worth chasing.**

**Method note — raw byte-diffing across versions is a trap, don't reach for it first.** Tried a direct
`sha256`/byte diff of the diode-cluster region across all 10 versions first: every single version differs
(different hash), and a full-file diff between adjacent versions of *identical size* (e.g. 111 vs 112)
shows **millions of differing bytes** starting almost immediately (offset `0x4a`). This looks alarming
but is a known false signal for ARM binaries: a single unrelated size change anywhere upstream shifts the
PC-relative encoding of every `bl`/literal-pool load downstream, even when the actual logic is completely
unchanged — so raw-byte diffs are dominated by address-encoding noise, not real edits. **The fix**:
`objdump` each version and diff the disassembly text with the address column and raw opcode bytes
stripped, comparing only mnemonics + operands.

**Result, using the correct method**: ran the same bit-mask/`ubfx` sweep from the 7th session (masks
`#0x400`/`#0x800`/`#0x1000`/`#0x2000`/`#0x4000` for bits 10-14, plus every `ubfx` at start-bits 10-14)
against all 10 versions' `body.bin`. Every version returns exactly the same 20 hits, and — after
stripping addresses — **19 of the 20 are textually identical across all 10 versions, start to finish**;
the one exception is an unrelated 7-bit field (`ubfx r0,r0,#13or14,#7` on a value loaded via `ldr r0,[r4,#4]`,
nothing to do with the diode scan) that shifts by one bit position between versions — clearly an unrelated
struct-layout change, not width-1 so never a diode candidate regardless. Checked the previously-unexamined
single-bit hits for bits 11/13/14 too (`0x200b3578`, `0x200b4f24`, `0x200b4f34` in v142): all three load
their value from a literal pool address resolving to **`0xFCFE3200`** — a different GPIO port's data
register entirely (P5, the diode-scan port, is `0xFCFE3014`) — confirming they're unrelated hardware
inputs, not diode tests. The remaining multi-bit-width candidates (`ubfx` widths 4/5/8 at these start
bits) were spot-checked and are the same RGB555 color-unpack and jump-table dispatch-code patterns
already ruled out for D408.

**Conclusion**: D408, D411, D414, D417, and D420 all have **zero** consumers, checked identically across
every firmware version from the earliest available (`111`) to the newest (`142`) — this isn't a gap that
got fixed or introduced at some point in the update history; the code testing these bits (or rather, not
testing them) has been stable across the entire known version range. `chunk5_tail.bin` and `base.dat`
remain the only genuinely unchecked firmware content, and both are low-probability locations for
application-level region/settings logic. Live JTAG verification remains the most promising next step for
any of these five diodes.

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
population-confirmed Japan-only diodes; D420's code consumer is still not found (see the 3rd/5th/8th
session sections above), but "populated everywhere so can't be a JP/export differentiator" is no longer a
reason to doubt the Language hypothesis.

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

## 9th session — three long-open "consumer not traced" questions resolved (Open Questions 3/4/5); D408/411/414/417/420 re-confirmed unresolved via fresh fallback tooling and cross-thread checks

Picked up specifically to re-attack the 5 fully-unknown diodes plus 4 open questions, prompted by a large
amount of unrelated RE work (menu-system tracing, RTTY/SSTV thread, front-panel-protocol work, CI-V dispatch
work, UI touchscreen widget work) having happened since this file was last touched — on the theory that some
of that work may have incidentally disassembled or documented a missing link back to this investigation.
Read `notes/ui-menu.md`, `notes/kernel-rtos.md`'s RTTY thread, `notes/front-panel-firmware.md`, and the
README Status section first for full context on what's new.

**Method, before touching any specific diode**: re-ran `references_to` fresh (live Ghidra xrefs) on all 5
known alias/table addresses (`DAT_2003c7f8`, `DAT_2003c800`, `DAT_2003ea4c`, `DAT_2003c858`, `DAT_2003c85c`)
— returned the exact same closed set of consumer addresses as every previous session, confirming Ghidra's own
analysis hasn't picked up anything new since the last sweep. Cross-checked this independently against
`scratch/superset_142.sqlite` (the full ARM+Thumb superset disassembly built by `tools/superset_disasm.py`,
regenerated 2026-09-07 per `tools/README.md` — confirmed current) via direct SQL queries for literal-pool
loads (`ldr Rd,[pc,#imm]`) targeting these 5 addresses: identical closed set, zero new hits — this
independently confirms Ghidra isn't missing anything the superset tool would catch (per this project's own
methodology lesson about code that exists in the binary but was never disassembled by Ghidra). Went one step
further than any previous session: swept the whole image for `movw`/`movt` immediate-pair loads of these
same 5 addresses' 16-bit halves (in case some function forms the pointer without ever using a PC-relative
literal load) — zero hits. Also directly searched for hardcoded literal loads of the *live* runtime addresses
these pointers resolve to this build (`0x20390214`/`0x20390210`/`0x20390212`/`0x203def29`) — zero hits, ruling
out any function bypassing the alias-pointer indirection entirely.

### Open Question 5 resolved: the D406/D409 classification byte feeds a live DSP command word

`*DAT_2003c858` resolves (read directly, this build) to `0x203def00` — recognized this address immediately:
it's the exact same "live DSP shadow config" struct base `notes/kernel-rtos-history.md`'s RTTY thread already
mapped as `DAT_200b1ca0`, read by `dsp_param_table_rebuild_from_settings` (`0x200b232c`, real entry
`0x200b18c0`). Decompiling that function again with this specific offset in mind: it reads
`*(char*)(iVar4+0x29)` (`iVar4` = the same `0x203def00` base) — exactly the byte `FUN_2003c530` writes the
D406/D409 classification result to (`*(undefined1*)(DAT_2003c858+0x29)` in `FUN_2003c530`'s own decompile,
confirmed address-identical). The read value is shifted `<<6` and OR'd together with 3 other 2-bit fields
(struct offsets `+0x2a6`, `+0x2a7`, `+0x2a8`, each masked `&3`) into one packed word, tagged with opcode byte
`0x22` (`CONCAT13(0x22, ...)`) and stored to `DAT_200b1cb8[3]` — slot 3 of the same 24-word DSP command table
`dsp_cmd_table_init`/`dsp_param_sync_tick` already fully mapped by the multi-cpu-images/RTTY threads. The
function's last statement is an unconditional call to `dsp_param_sync_tick()`, which diffs this table against
its last-synced shadow and pushes any changed word to the DSP chip over `SCIF5`.

**What this means**: D406/D409's post-scan classification (0=neither, 1=D406 present, 2=D409-present-only) is
not just local CPU-side bookkeeping — it's packed as a real 2-bit sub-field of a live DSP configuration word
that gets continuously synced to the DSP chip, the same mechanism this project's RTTY thread already
confirmed drives things like passband edges and mode-specific parameters. The other 3 packed 2-bit fields
(`+0x2a6`/`+0x2a7`/`+0x2a8`) weren't identified — out of scope for this specific question, but a plausible
next lead if this word's DSP-side meaning (opcode `0x22`) is ever recovered (e.g. from the DSP's own object
code, `dsp_program.bin`/`dsp_data.bin`, referenced in `notes/multi-cpu-images-history.md`).

### Open Question 4 resolved: D403's raw bit value only ever bounds a CTCSS-tone cycle

`FUN_2003c5c4`'s only caller was already known (`get_type1_type2_designation`/`FUN_200134b4`). Chased one
level further this session: that function itself has exactly 3 real callers, all decompiled in full —
`FUN_2000b98c` (`0x2000b9d0`), `FUN_200189e8` (`0x20017db8`), `FUN_20065dec` (`0x20065d74`). All 3 use the
`'1'`/`'2'` (`0x31`/`0x32`) return value purely as the upper bound of a linear search / cycling loop over a
CTCSS-tone table, passed straight into the same generic "select item N with wraparound" helpers
(`FUN_20017c50`, `FUN_20006398`) already seen gating the meter-cycle function in the next section below.
`FUN_2000b98c` in particular is a clean confirmation: it parses a 3-character command string, looks up a tone
frequency in a table (`DAT_2000c288`) bounded by `get_type1_type2_designation()`'s 49-vs-50 count, and returns
the matched index — textbook CI-V/menu "resolve CTCSS tone name to table index" code. `FUN_20065dec` does the
same bounding for what looks like an encoder/dial-driven tone-selection UI handler. **No consumer beyond the
already-documented CTCSS-tone-count mechanism exists anywhere** — this closes the question definitively rather
than just re-asserting the existing finding.

### Open Question 3 resolved: `DAT_2003c85c`'s consumer found, but it's unreachable dead code

`FUN_2003c5d4`'s only caller was already known to be inside `FUN_2000a5f0` (renamed nowhere yet) — but tracing
one level further: `FUN_2000a5f0(param_1)` is a boolean gate checked against a numeric `param_1`, with two
special cases — `param_1==9` (gated on an unrelated flag, `*DAT_2000a7ec & 1`) and `param_1==10` (gated on
`FUN_2003c5d4() != 0`, i.e. D416 present AND the `DAT_2003c85c` lookup is non-zero). `FUN_2000a5f0` itself has
5 real callers (`FUN_2000a774`→`FUN_2000a6d8`, `FUN_2000a794`, `FUN_2000ba54`→`FUN_2000ba0c`,
`FUN_20032fb0`→`FUN_20032f8c`, `FUN_20033078`→`FUN_20033054`), several of which visibly cycle through an
11-item list (`param_1` / loop bound `0`-`0xa`/`10`) skipping any item for which `FUN_2000a5f0` returns
nonzero — the shape of a "select next enabled item, wrapping" UI selector, structurally similar to (but not
confirmed to be) a settings/mode cycling widget. Exact feature identity of items 9/10 not pinned down.

**The key finding, though**: `DAT_2003c85c`'s index (`DAT_2003c800+2`) was independently confirmed this
session to be the region_code byte, not some separate raw diode index — `FUN_2003c0ec` (already documented,
re-read closely this session) contains `puVar1[2] = (char)diode_region_code_lookup(*(undefined4*)(DAT_2003c800+4))`,
i.e. it writes the *resolved* region_code (the same 0-7-valid-range value from `DAT_2003c7fc`'s table used
throughout this whole file) into that exact offset, confirmed by a second independent reader
(`FUN_2003c4a4`, which indexes a per-region table array, `DAT_2003c808`, with the identical
`*(byte*)(DAT_2003c800+2)` expression). Since `DAT_2003c85c`'s only non-zero entries sit at indices 13 and 15,
and region_code's confirmed valid range is 0-7 (`DAT_2003c7fc` = `[0,1,2,0,3,4,5,6,0,7,0,0,0,0,0,0]`), **this
specific D416-gated path can never evaluate non-zero on any diode combination this firmware's own tables can
produce** — a real, traced consumer, but one that is dead code in practice on any real hardware. Worth
flagging as a genuinely new *kind* of finding for this project: not "no consumer found" and not "confirmed
live effect," but "consumer found, and it's provably unreachable" — a third bucket this file hadn't needed
before.

### D408/D411/D414/D417/D420 — re-confirmed unresolved, including targeted checks of every new cross-thread lead the task specifically flagged

With the alias-address reference set reconfirmed closed (see Method above), specifically checked each
lead the orchestrating session's brief called out as a possible missing link, rather than relying on the
closed-reference-set argument alone:

- **The touchscreen list-menu widget's per-item availability callback**, `FUN_2004f250` (`notes/ui-menu.md`)
  — decompiled: it only tests the widget's own record table (`DAT_2004f728`-based) byte at offset `+0x1c`,
  bit 0. No reference to any diode-scan alias, no plausible indirect path either (the value it tests comes
  from the widget's own static per-item data, not a runtime diode read). Ruled out cleanly.
- **`g_system_command_table`** (`0x2018d9e0`, 279 entries, `notes/ui-menu.md`) — dumped and parsed all 279
  `{command_id, handler}` pairs directly from memory. Zero handler addresses fall inside the diode-matrix
  code cluster (`0x2003b000`-`0x2003f000`), and none match any of the already-enumerated diode-consumer
  function addresses. Doesn't rule out a handler calling *into* the already-known consumer chain (e.g.
  `FUN_2003df34` is already known to be called from "at least 4 places" per the existing notes), but no
  *direct* new consumer sits in this table.
- **CI-V `1A 05` settings bridge** (`dsp_param_table_rebuild_from_settings`'s sibling, `civ_cmd_1a05_handler`
  / `0x2000dcb4`, per `notes/kernel-rtos-history.md`'s RTTY thread) — re-read the existing trace: it feeds the
  same 216-item value table (`DAT_2000e230`) and passes the item's own code straight through to
  `menu_item_value_set_by_format_type`/`menu_item_value_set_simple`. Dumped the full 216-entry table this
  session (see below) and confirmed the field that trace called `region_code` is actually just the item's own
  numeric code (`0x24`-`0xf6` range, matching `FUN_2003df34`'s dispatch codes exactly) passed through
  verbatim — no additional masking or gating logic beyond what's already documented; not a new bridge into
  the diode bits.
- **`ui_show_message_dialog`** (`0x200198bc`) — has ~38 real callers; not walked individually (would be
  redundant given the closed alias-reference-set result above — none can test a diode bit without going
  through one of the 5 now-conclusively-enumerated alias addresses). Flagging this explicitly as a
  **shortcut taken**, not an exhaustive per-caller check, in case a future session wants to verify it
  directly rather than trust the inference.
- **The menu-item name string table** (`~0x2035a000`-`0x2035f000`) vs. the 216-item value table
  (`DAT_2000e230`/`0x2018a698`) — dumped and parsed the full 216-entry table directly from memory (4
  bytes/entry: format_type_byte, flag_byte, item_code 16-bit). Spot-checked `references_to` on two sample name
  strings already found by the RTTY thread (`"RTTY Decode USOS"` @ `0x2035c390`, `"RTTY Mark Frequency"` @
  `0x2035cfd1`): **zero static references to either**, matching the RTTY thread's own "computed-table-access
  wall" finding exactly. No static bridge from the name pool to the value table found this session either —
  Open Question 6 remains open (see diode-matrix.md's Open Questions for the precise wording).

**Conclusion**: D408, D411, D414, D417, D420 remain genuinely unresolved. This is now the *third* session
(7th, 8th, 9th) to reach this conclusion via a different set of methods each time, most recently including a
whole-image superset-disassembly cross-check and a dedicated sweep of every specific new lead this session's
brief called out — a strong, well-supported negative result, not a gap in searching. `chunk5_tail.bin` and
`base.dat` remain the only literally-unchecked firmware content (not attempted this session either — out of
scope given the time spent on the resolved questions above). Live JTAG verification remains the most
promising next step for any of these five diodes, same conclusion as every prior session.

**Not reached this session**: Open Questions 1 and 2 (region-code-to-country-name reconciliation, the
0-7-vs-public-Version-#-up-to-12 mismatch) and Open Question 7 (`chunk5_tail.bin`/`base.dat` never checked)
— no new work done on these, time was spent entirely on Questions 3/4/5 and the D408/411/414/417/420
re-confirmation above.

## 10th session — the factory-reset/defaults mechanism found and decompiled end to end; D423 gets a new, concrete consumer; D408/411/414/417/420 checked against this genuinely new code path, still nothing

Prompted by the observation that this project had never actually identified the Partial-reset/All-reset
functions the real manual documents (`IC-7300_ENG_FM_12b.pdf` pp. 14-3/14-4), nor the hardware
`CLEAR`+`V/M`-at-power-on forced-reset path — a new thread, not a continuation of the diode-hunt proper, but
one whose defaults-selection logic was worth checking against the diode matrix per the task brief. Found and
traced the whole chain; the hardware boot-time combo was **not** found (see below).

### Finding the two confirm-dialog strings and the message-ID table that owns them

Raw string search turned up all four of the manual's quoted UI strings in one small cluster:
`"Reset All Edges?"` (`0x2035ec24`), `"Reset to the default settings?"` (`0x2035ec58`),
`"Partial Reset?"` (`0x2035ec8c`), `"All Reset?"` (`0x2035ec9c`) — right alongside the SD-card file-manager's
own confirm strings (`"Delete all memo pads?"` etc.), confirming these all share one generic confirm-dialog
message-ID table. Walking each string's single `DATA` xref back one level (the string pointer sits inside a
record, not referenced directly by code) and cross-checking against `ui_status_message_render_rows`
(`0x200aa750`, already named from the firmware-update thread) — which indexes
`DAT_200ab2d0 + message_id*0x4c` (a **76-byte-stride table**, base resolves to `0x2032c91c` this build) —
pins down the message IDs precisely: **`0x52`** = "Reset to the default settings?" (a generic string, reused
by an unrelated per-item confirm-and-cycle feature elsewhere — not reset-specific despite the name), **`0x54`**
= "Reset All Edges?", **`0x57`** = "Partial Reset?", **`0x59`** = "All Reset?". Confirmed `ui_show_message_dialog`
(`0x200198bc`)'s first argument really is this same message-ID space by checking `FUN_200199d0` (a second,
narrower entry point that just stages `{id, callback}` into the dialog struct for a widget's generic
"activate" handler to pick up and forward) — only 12 real callers project-wide, making this a far more
tractable search surface than every `ui_show_message_dialog` call site directly.

### "Reset All Edges" (User Band Edge screen) — fully traced, confirms the widget-table hypothesis

`FUN_2003331c` (a previously-undefined function — Ghidra had never disassembled these bytes; created it fresh
the same way `notes/ui-menu.md`'s "empty function list doesn't mean no code" lesson describes) is the
User-Band-Edge screen's "Reset All Edges" button/list-item, referenced only from a table entry
(`0x20033408`, `PARAM` reference) exactly matching the generic touchscreen list-widget infrastructure
`notes/ui-menu.md` documents. It unconditionally calls `FUN_20045e94()`, which shows the `"Reset All Edges?"`
(`0x54`) confirm dialog with callback `FUN_20045e68`. On confirm, that callback calls `FUN_20045800()`, which
loops all 11 user band-edge slots (`FUN_200457ec(i)`: `*(u32*)(DAT_20045ea8 + i*8 + 0xde4) = 0xFFFFFFFF` — the
same sentinel this project already knows means "blank/unused" for these slots) and clears a handful of
trailer/header bytes, then calls `FUN_2004577c` to recompute which slot index is "current" after the wipe.
This is a small, self-contained, fully-confirmed reset function in its own right — not diode-gated at all,
no scan-value reference anywhere in this specific chain.

### Partial Reset / All Reset — the real trigger chain, end to end

**Partial Reset** (`0x57`): `FUN_20041c38` unconditionally calls `FUN_200199d0(0x57, PTR_FUN_20042670)`.
The callback (`PTR_FUN_20042670` → `FUN_20041c0c`) calls `FUN_2000a17c(1)` then `FUN_2002b818(3)`.
`FUN_2002b818(param_1)` does `*DAT_2002b4fc = param_1` (`= 3` here) then two generic UI-transition calls.

**All Reset** (`0x59`): its trigger, `FUN_20041c9c`, was **another undisassembled gap** — Ghidra had left the
whole function (52 bytes, right after `FUN_20041c70`) as raw undefined bytes; the project's own fallback
(`arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x20005000` on the real `body.bin`, per
`tools/README.md`) confirmed it as real ARM code and gave ground truth before creating the function in Ghidra.
It checks a flag byte (`[0x20390218+5]`): if already set, tail-calls `FUN_2003e5f0(0)` directly (a
skip-the-dialog fast path); otherwise it tail-calls `ui_show_message_dialog(0x59, 0x20041c70, 0, 0)` directly
— explaining why this specific call didn't show up in the initial `references_to(ui_show_message_dialog)`
sweep (38 hits, all direct `bl`s) at all: it's a `b` inside code Ghidra hadn't disassembled yet, invisible to
any static reference search until the gap is fixed. The confirm callback, `FUN_20041c70`, sets that same flag
to 1 and calls `FUN_2003e5f0(0)`.

**The key link**: `*DAT_2002b4fc` (resolves to `0x20390310`) turned out to be the *exact same* address as
`DAT_2002a158`, the "system mode request" byte `system_mode_request_dispatch` (`0x2002a6b8`, already fully
documented in `notes/kernel-rtos-history.md`'s 30th session) switches on — just two different literal-pool
copies of one pointer, this project's standard pattern. So writing `*DAT_2002b4fc = 3` is a real, if indirect
(polled, not called), trigger: the next time `sys_monitor_task`'s poll loop runs `system_mode_request_dispatch`,
its `case 3` fires `FUN_2002a698()` (Partial), and (independently, via whatever sets the same variable to `5`
for All Reset — not traced back further than "some path reaches value 5 too, given `case 5` exists and does
the parallel All-Reset-shaped work"; the All-Reset button's own path to writing `5` specifically into this
variable, as opposed to Partial's directly-observed `3`, wasn't nailed down to a single instruction this
session — a real gap, flagged honestly rather than assumed) `FUN_2002a678()` runs instead. Both call the same
EEPROM write (`FUN_2001e484`, tag `16000`) with different source buffers/lengths, then both call
**`FUN_2002a4c8(param_1)`** — 0 for Partial, 1 for All — which is the real bulk-reset function.

### `FUN_2002a4c8` — the actual bulk-reset function, and the real defaults table

`FUN_2002a4c8(reset_type)` clears a large number of live-state struct fields directly (channel/mode/squelch
working state, DSP-adjacent flags, etc.) and, critically, calls `FUN_2003ddd4(reset_type)`, which loops item
codes `0`–`0x145` (326 items) calling `FUN_2003dcc0(item_code, reset_type)` for each. **This is the literal
default-value table the task asked about.**

`FUN_2003dcc0` indexes `DAT_2003ea50 + item_code*0x40` — a **64-byte-stride, 326-entry table** whose base
(`0x20190ecc` this build) is the *same address* `notes/diode-matrix.md`'s already-documented 13-row band-edge
min/max table uses (there as `DAT_2000f0c4`, read by `is_feature_enabled_for_region`/`FUN_2000edb0`) — i.e.
this is one canonical per-item-code record table serving double duty: min/max sub-fields for band-edge-shaped
items, and (freshly read this session, item 0's record) a live-value pointer (`+0`), a type byte (`+4`:
1=byte/2=short/4=word/else=blob-copy via `FUN_2017c710`), a literal default value (`+8`), and — unexpectedly —
string pointers at `+0x28`/`+0x2c`/`+0x34`/`+0x38` landing squarely in the `0x2035axxx`–`0x2035fxxx` name-string
pool this file's Open Question 6 has been chasing. **Not confirmed as the missing name-table link** (didn't
chase further this session — out of scope for this thread — but flagged here since it's a real, concrete new
lead for that question, worth a fresh look).

For each item, `FUN_2003dcc0` normally just writes the table's own literal default (`+8`) into the live
pointer (`+0`) per its type byte. **Six item codes get special-cased instead of the generic table default** —
and this is where the diode matrix comes back in:

- **Item `0x73`: `if ((*DAT_2003ea4c & 0x8000) == 0 || item != 0x73) { normal path } else { *live = 1; }`**
  — `DAT_2003ea4c` is one of the diode-scan-value's confirmed aliases (`DAT_2003c7f8`/`DAT_2003c800`/
  `DAT_2003ea4c`/`DAT_2003c858`), and bit `0x8000` is bit 15 = **D423**. So: **when D423 is present, item
  `0x73`'s default on reset (either Partial or All — this branch doesn't check `reset_type`) is forced to `1`
  instead of whatever the generic table says.** This is a real, new, concrete consumer of D423 — not just
  "gates feature-code checks" (the existing, vaguer characterization) but specifically "picks item `0x73`'s
  factory-reset default value." Item `0x73`'s actual menu-item identity is still unresolved (same open
  problem as the rest of `is_feature_enabled_for_region`'s item codes), so this doesn't yet name the Emergency
  Mode feature, but it's a materially stronger, more specific piece of evidence for the same hypothesis.
- **Items `0x22`/`0x32`/`0x79`: gated on `FUN_2003bcdc()`**, which is `diode_region_code_lookup(...) == 0` —
  i.e. **region_code == 0** (the diode-matrix.md region table's "no diode combination maps here" default/
  unmapped code). When true, these three items' reset defaults are forced to `3`, `1`, and `0x6c` (108)
  respectively, overriding the generic table value. **A second, independent, real consumer of the diode-derived
  region_code inside the reset/defaults path** — not just band/TX tables and the menu-enable gatekeeper, but
  the factory-reset default-value selection itself.
- **Item `0x4b`: on Partial reset only (`reset_type == 0`)**, its default comes from a live-mirror byte
  (`*(byte*)(0x203dee04 + 0x91)`, a single, un-traced-further reference — `references_to` on its own
  literal-pool address found exactly one hit, inside this same function) instead of the table value; on All
  reset it uses the generic table default like everything else. Not chased further (single reference, low
  value for the time spent) — flagged as a loose end, not a diode/region consumer as far as traced.
- Checked `is_feature_enabled_for_region` (`0x2003df34`, the sibling function `notes/diode-matrix.md` had
  previously described together with `FUN_2003dcc0` as one "master gatekeeper" pair) fresh, specifically to
  see whether it also participates in the *defaults* path — it doesn't: it only ever tests bit 0 (D401) and
  bit 5 (D416) for its own, disjoint set of item codes (`0x24`–`0xf6`, matching its existing documentation
  exactly), never bit 15. **Correction to the existing note's phrasing**: "`FUN_2003df34`/`FUN_2003dcc0`...
  real, direct input (bit 15)" reads as if both functions test bit 15 — only `FUN_2003dcc0` does; `df34`'s own
  gating is bits 0/5 only, for entirely different item codes. Not a factual reversal of anything already
  concluded (D423 is still real, still bit 15, still gates item-code overrides), just a precision fix on
  which of the two functions does which part.

**A real discrepancy worth flagging, not resolved this session**: `FUN_2002a4c8` calls `FUN_20045800()` (the
same "Reset All Edges" wipe documented above) **unconditionally**, regardless of `reset_type` — i.e. by this
reading, **both Partial and All Reset wipe every user band edge**, even though the real manual explicitly
lists "User Band Edge" among the settings **Partial reset preserves**. Possible explanations not
distinguished here: the manual is right and something else (not reached this session) restores/skips this for
Partial reset specifically; the call is conditioned on something upstream of what was traced; or this is a
genuine firmware behavior the manual's wording doesn't quite match. Flagging honestly as unresolved rather
than picking one.

Also confirmed, more positively, that `FUN_2002a4c8` **does** implement an asymmetry matching the manual's
framing: three calls (`FUN_2003d5e4`/`FUN_2004bf24`/`FUN_2001f400`) run only when `reset_type == 0` (Partial)
and are skipped for All Reset — not decompiled further this session, but structurally consistent with
"Partial reset does less than All reset," the manual's core distinction.

### D408/D411/D414/D417/D420 against this new code path — still nothing

Checked every function in this newly-traced chain (`FUN_2003dcc0`, `FUN_2003df34`/`is_feature_enabled_for_region`,
`FUN_2002a4c8`, `FUN_2003ddd4`, `system_mode_request_dispatch`, both button-trigger functions and both confirm
callbacks) for any test of bits 10/11/12/13/14 (D408/D411/D414/D417/D420) against any of the diode-scan-value
aliases. Found none — the only bits tested anywhere in this whole reset/defaults path are bit 0 (D401), bit 5
(D416), and bit 15 (D423), plus the derived region_code (weight bits 1/2/4/8 = D404/D407/D410/D413). This is a
genuinely new code path (never previously examined by this project, since the reset mechanism itself had never
been located) reaching the same negative result as every prior session's whole-firmware sweep — a real,
independent reconfirmation, not a repeat of old ground.

### The hardware `CLEAR`+`V/M`-at-power-on forced-reset path — not found this session

Went looking for this specifically (the task's third target) but did not find it. `cold_boot_hw_init`
(`0x2002afc0`) does contain a cluster of real, already-named-from-a-prior-session boot-time button-combo
checks (`boot_check_mode1_combo`, `boot_check_mode5_combo`, `boot_check_challenge_response` — all three fully
documented in `notes/kernel-rtos.md`'s "Factory/service mode" section, 30th session) that run in the same
place `*DAT_2002b4fc` gets zero-initialized at cold boot — but all three are **service-mode entry combos**
(`MENU`+`FUNCTION`+shorted REMOTE jack, and two others), not the manual's `CLEAR`+`V/M` all-reset combo, and
none of them touch `*DAT_2002b4fc`/`*DAT_2002a158` at all (they write a different variable, `DAT_2002a4a4`,
the idle-loop-selector). No fourth, `CLEAR`+`V/M`-shaped combo check was found nearby or elsewhere this
session — genuinely not located, not just unconfirmed. This remains open for a future session (or JTAG): the
manual's boot-time forced-reset path may poll physical key-matrix state through a completely different
mechanism than the SCIF3 front-panel-status-buffer bits `boot_check_mode1_combo`'s cluster reads (e.g. a
dedicated GPIO read of `CLEAR`/`V-M` specifically, not yet located), or may not exist as a distinct boot-time
code path in `body.bin` at all (possibly handled entirely by the front-panel's own separate firmware/MCU
before the main CPU is even up — consistent with this project's existing multi-CPU findings).

**Files touched this session**: `notes/diode-matrix.md` (new "Factory reset / restore-defaults mechanism"
section, D423's living-table entry updated, Open Questions updated) and this history file. No git commit made
— left for the user's own review, per this project's standing instruction. Two previously-undefined code
regions were disassembled and named in the live Ghidra project as part of this work (`FUN_2003331c`,
`FUN_20041c9c` — the latter recovered via `arm-none-eabi-objdump` ground truth before being defined in Ghidra,
per this project's established ARM/Thumb-gap workflow); no `.bin`/binary files were modified, only the Ghidra
project's own analysis database.

## 11th session — testing a new user lead: Japan's 4630 kHz/CW-only Emergency behavior. Confirmed real via firmware strings; exact gating code not found

Prompted by a specific, concrete piece of the user's own domain knowledge (real amateur radio operator): Japan's
IC-7300 variant reportedly has a special emergency-frequency function tied to **4630 kHz, CW-only**, layered on
top of an all-regions Emergency-Mode-shaped behavior (**reduced TX power, tuner accepting worse SWR/match**).
4630 kHz is Japan's real disaster-communication frequency (JARL's network) — a concrete, testable claim, not
vague. Read this file's own D423 row and the 9th/10th session entries first per the task brief.

### Confirming the frequency-encoding convention before searching

Checked how this firmware actually stores frequency literals before guessing at 4630's representation:
decompiled `FUN_2003bd80` (RX band-edge clamp, already in this file) and confirmed via `mcp__ghidra__memory`
decimal search that known band edges (`7300000`, `7000000`, `5255000` — 7.3 MHz/7.0 MHz/5.255 MHz) all hit
cleanly as **plain 32-bit little-endian Hz integers** at the expected `0x20193xxx`/`0x20198xxx` band-table
addresses. This is the confirmed convention this firmware uses for frequency comparisons — searched for 4630
kHz in this exact form (`4630000` / `0x46A6D0`) first, then kHz-scale (`4630`), then float/double
(`4.630`/`4630000.0`). **All came back with zero hits** — a real negative, not a search-method failure (the
sanity-check literals above prove the method works on this exact kind of value).

### Found the feature anyway — as real UI strings, not a numeric literal

Broadened to string search (`"4630"`, `"Emergency"`, `"EMG"`) across the whole image and got immediate, strong
hits, all inside the already-known `~0x2035a000`-`0x2035f000` UI name/message pool (the same pool this file's
Open Question 6 has been chasing) plus one more small cluster at `0x2032a000`:

- `"You can transmit on 4630 kHz for "` (`0x2035e518`) directly followed in memory by `"Emergencies."`
  (`0x2035ea7c`) — these are two separate null-terminated strings concatenated at render time into **"You can
  transmit on 4630 kHz for Emergencies."**, confirmed by reading the raw bytes directly (not an OCR/inference
  read — this is a literal ASCII string in `body.bin`).
- Three more standalone `"4630kHz"` strings (`0x20359ae0`, `0x2035fac0`, `0x2032a036`).
- `"EMERGENCY"` (`0x2035a5c4`) and `"Emergency"` (`0x2035a81c`) as standalone strings.
- Reading wider context around `0x2035a5c4` turned up something unexpected and useful: `"EMERGENCY"` sits
  **inside a real menu-category name list** — `...RX..TX..TX DELAY....DISPLAY.EMERGENCY...KEYER
  MEMORY....RTTY MEMORY.RTTY...` — i.e. it reads as a genuine SET-mode category name sitting in sequence with
  other real IC-7300 category names, not just a one-off status message.
- The `0x2032a000` cluster is a small, self-contained **bilingual status-indicator string table**: Shift-JIS
  `"非常通信モード"` (literally "Emergency Communication Mode" — 非常 hijō/emergency, 通信 tsūshin/
  communication, モード mōdo/mode) with **no English translation** (a blank padded field, where the table's
  other rows, e.g. `"TUNER"`/`"チューナー"`, have both languages populated), plus `"4630kHz"`, `"TUNER"`, and
  two pre-combined display strings `"4630kHz / TUNER"` and `"4630kHz / チューナー"`. The missing English
  string is itself real, if circumstantial, evidence this is JP-only — consistent with D423/D420 being the two
  confirmed JP-only diodes.

Decoded the Shift-JIS by hand from the raw hex bytes (`94 f1 8f ed 92 ca 90 4d 83 82 81 5b 83 68` →
非常通信モード) — cross-checked against the standard Shift-JIS code table, not guessed from vibes.

### Tracing the status-indicator code — real, but hit a wall past one layer

`references_to` on the `0x2032a09c`/`0x2032a058` string addresses found a real code consumer:
`FUN_2009060c`, a status-bar/icon-text renderer. It reads a byte from a global UI-status struct
(`DAT_20090a18` → `0x2040376c` this build; `+0x7f8` = icon "kind" selector, `+0x7f9` = a separate
English/Japanese display-language byte) and switches on it — `case 6` is confirmed, by walking the real ARM
listing at `0x20090790`-`0x200907cc` (`ldr r2,[0x20090a40]` etc.), to be the "4630 kHz / Tuner" indicator,
picking between the Japanese-only string cluster based on the language byte. This confirms the string cluster
is live, referenced UI code, not orphaned data — a real, if partial, win.

Tried to go one level further — find what sets that `+0x7f8` byte to `6` (i.e. what actually *triggers* this
indicator) — and hit a genuine dead end: `references_to` on the computed absolute address (`0x20403f64`)
returned a long list of "WRITE" hits, but decompiling several of them showed completely unrelated code (a
meter/frequency-display string-building switch statement on an unrelated variable, `DAT_2003a538`/
`DAT_2003a4a8`, nothing resembling the expected struct or offset). Concluded this is very likely a coincidental
address match rather than a real trace (possibly a mislabeled switch-table data reference, per this project's
existing "Ghidra reference type on computed addresses can be misleading" experience) — flagged honestly as an
unresolved trace rather than forced into a story. **D423's own known consumers were checked directly instead**:
item `0x73` (the one concrete, already-traced D423 consumer) resolved via its own name-string field
(`+0x28` in the 326-item defaults table, same table/mechanism this file's Open Question 6 flagged as a new lead
last session) to **`"Display Language"`** — not Emergency Mode, not a frequency, ruling it out cleanly. Checked
the other D423-gated item codes the same way (`0x22`, `0x32`, `0x71`, `0x79`, and the `0x94`/`0xe5` pair from
the `0x8f-0x93/0x94/0xe5` range) — none have name strings or record shapes matching this feature either;
`0x94`/`0xe5` are word-typed records with plausible-looking large numeric values but no name string at all, and
`0xe5`'s own gating in `is_feature_enabled_for_region` (re-read fresh) tests bits 0/5 (D401/D416) only, matching
that function's already-documented general-coverage-style gating — not D423.

### What this session did and didn't establish

Also searched directly for `"CW mode"` / `"only in CW"` near the 4630 kHz strings and read ~700 bytes of the
surrounding string-pool context by hand — no CW-restriction wording found anywhere nearby. The user's CW-only
recollection is **not disconfirmed**, just not independently corroborated by any firmware string this session
found — worth being precise about, since the frequency and tuner/emergency wording *did* get directly
confirmed while the CW-specific detail didn't.

**Not attempted this session**: any search for the broader, all-regions "reduced power + relaxed tuner
matching" Emergency-Mode-shaped behavior the user described as separate from the JP-specific 4630 kHz/CW case.
Time went entirely into confirming/tracing the JP-specific lead. This remains genuinely open — a real next
step would start from `notes/kernel-rtos.md`'s `tuner_engage_gpio_toggle`/`tuner_freq_and_txstate_precheck`
section looking for a TX-power-scaling table or a tuner SWR/matching-tolerance constant, neither of which was
searched for this session.

**Bottom line**: the user's lead is **confirmed real** — this is not a coincidence or a misremembered detail.
The firmware contains a genuinely named, JP-flavored "Emergency Communication Mode" (非常通信モード) feature
concretely tied to 4630 kHz and to the tuner, referenced from live status-bar-rendering code. What remains
unresolved: the precise trigger/gating logic (attempted and hit a real wall, not skipped), whether it's
D423-specific as opposed to something else, the CW-only detail specifically, and the broader all-regions
reduced-power behavior (not investigated this session, not the same thing as a negative result).

**Files touched this session**: `notes/diode-matrix.md` (D423 living-table entry corrected — item `0x73` is
"Display Language", not Emergency Mode; new "4630 kHz Emergency Communication Mode" section added; Open
Question 6 updated) and this history file. No git commit made — left for the user's own review. No ARM/Thumb
disassembly gaps were hit this session (all code inspected was already disassembled by Ghidra), so nothing was
queued in `scratch/armthumb_fix_requests.txt`. No `.bin` files or the Ghidra project's binary contents were
modified — only comments/notes in this repo.

## 12th session — systematic Shift-JIS sweep, per the "turn the 4630kHz technique into a tool" task brief

Task: take the 11th session's by-hand discovery method (a Shift-JIS status string with no adjacent English
translation, unlike its sibling rows, was the real signal that led to the 4630 kHz "Emergency Communication
Mode" find) and turn it into a systematic, repeatable, whole-image sweep, then chase whatever it flags.

### The tool

Built `tools/sjis_string_scan.py`. Confirmed the real firmware path first (`tools/README.md` + existing
`scratch/unpacked/` contents) rather than re-deriving it — `scratch/unpacked/142/body.bin`, base `0x20005000`,
matching `superset_disasm.py`'s own default/example usage.

Single left-to-right tokenizer walks the whole 3,738,392-byte image once, greedily consuming runs of printable
ASCII (`0x20`-`0x7E`) and/or Shift-JIS lead/trail pairs (lead `0x81`-`0x9F`/`0xE0`-`0xFC`, trail `0x40`-`0x7E`/
`0x80`-`0xFC` excluding `0x7F`) that actually decode under Python's `shift_jis` codec — precomputed once into a
~30k-entry lookup table so the hot loop is dict lookups, not repeated try/except decodes. Runs under 3 real
characters are discarded (`--min-chars 3`, the project's own "adjust as needed" threshold — chosen because an
earlier `--min-chars=2` trial was dominated by stray 2-byte coincidences, while 3 still keeps real short labels).
Every kept run is classified `jp` (contains at least one real kanji/kana codepoint — Hiragana/Katakana, half-
width kana, CJK ideographs, or fullwidth forms — the plausibility filter the task asked for, since plenty of
byte pairs decode "validly" as Shift-JIS without ever landing on a real character), `ascii` (pure printable
ASCII), or `sjis_other` (valid Shift-JIS pairs, no real kanji/kana — kept for audit, not treated as a candidate).

Result: **126,603 total runs** — 107,785 `ascii`, **18,474 `jp`**, 344 `sjis_other`.

### Sanity-checking against the known-good calibration hits

`--check 0x2032a014 0x2032a07a` (非常通信モード / チューナー) confirmed both addresses show up as `jp`
candidates before trusting anything else the tool produced, per the task's explicit instruction. Both did.

### A real bug caught during calibration, not after

The pairing check ("does the preceding NUL-delimited field contain plausible English") initially computed the
span between the single nearest preceding NUL and the run's own start — which is trivially empty by
construction in these back-to-back tables, since a field's text begins immediately after its own opening NUL.
Manually re-verifying the known 非常通信モード/チューナー calibration pair against a byte-exact hex dump (not
just trusting the tool's own report) caught this: `find_preceding_field` needed to look at the field bounded by
the *second*-nearest NUL, not the nearest one. Fixed before trusting the sweep's "unpaired" output at all — see
the tool's own docstring for the full before/after. This also **overturned the 11th session's own headline
finding**: 非常通信モード does have a real English pair, `"EMERGENCY MODE"`, sitting exactly one 0x22-byte slot
back — the 11th session's by-hand read had made the identical off-by-one mistake, stopping at the (trivially
empty) span immediately before the Japanese text rather than checking one slot further back. Full narrative,
and the live-code confirmation via `FUN_2009060c`'s cases 1/2/4/5/6 all sharing the same `+lang_byte*0x22`
indexing convention, is in `notes/diode-matrix.md`'s new 12th-session section — not duplicated here.

### Characterizing the noise honestly

Of the 18,474 `jp` candidates: **7,110 + 1,372 = 8,482 (46%) fall inside the two large, already-confirmed pure-
ARM-code blocks** (`0x20005000`-`0x200ca000`, `0x200d1000`-`0x200fd000`) — almost certainly coincidental decodes
of instruction bytes, not real text, given those blocks contain no known string data. Only **4,920 (27%)** fall
inside the known real string-pool/data range (`0x2018e000`-`0x2035d000`); the rest scatter across font/glyph
data and other non-text blocks. Within the string-pool range, 3,492 of 4,920 (71%) are flagged `paired=0`
("no adjacent English found").

Ranking the longest unpaired candidates (a natural first instinct for "most interesting") turned out to
surface almost entirely a **Shift-JIS character/glyph enumeration table** around `0x20336000`-`0x20341000` —
long, dense runs of what look like most of the common-use kanji set in near-sequential JIS code order,
clearly font/glyph metadata, not UI strings. Recognized this from the output itself (not assumed) by hex-
dumping a sample and recognizing it as an enumeration, not prose — a real, correctly-identified noise source,
worth calling out explicitly since naive "longest string" ranking would otherwise waste an entire session
chasing font tables.

Narrowing to the three known real-text ranges (`0x20329000`-`0x2032c000`, `0x2032c000`-`0x2032e000`,
`0x20359000`-`0x20360000`) and reading the unpaired hits directly surfaced two more candidate clusters, both
chased and both resolved as **false positives of the pairing heuristic, not new features**:

1. A run of ~13 consecutive JP-only SET-menu category names (`0x2035a880`-`0x2035a9b8`: "トーンコントロール/
   送信帯域幅" [Tone Control/TX Bandwidth], "RTTYデコードログ表示" [RTTY Decode Log Display], "交信録音/再生"
   [QSO Recording/Playback], "VOICE送信録音", "ディスプレイ設定" [Display Settings], "VOICE送信設定",
   "RTTYデコードログ設定", "CW-KEY設定", "機能設定" [Function Settings, appears twice], "RTTYデコード設定",
   "オーディオスコープ設定" [Audio Scope Settings], "スコープ設定", "スキャン設定") — all ordinary, already-
   public IC-7300 SET-menu items, not hidden features. Hex-dumping the wider region (`0x2035a600`-`0x2035a9c0`)
   showed why: this part of the pool is **not** per-item English/Japanese pairs at all — it's a flat list of
   distinct menu/category names where each entry is monolingual (some entries English-only, like `"User Band
   Edge"`/`"Preset Name"`/`"Date/Time"`, others Japanese-only, like this cluster), evidently because each name
   is simply stored in whichever language it was authored in for this particular breadcrumb level, not a
   bilingual-pair table like `0x2032a000`.
2. `0x2035fac0` (`"4630kHz非常通信用周波数での送信ができ..."`) — already covered above/in diode-matrix.md:
   genuinely paired with the known English 4630 kHz message, just via the `0x2032c91c` pointer table rather
   than adjacency. Confirmed by hex-dumping the *English* message-table region (`0x2035e480`-`0x2035eb00`)
   directly and finding it is itself a long, separate, contiguous **all-English** run (RTTY Decode Log, QSO
   Recorder, Voice TX, "You can transmit on 4630 kHz for ", "Emergencies.", dozens more dialog/error strings)
   sitting well before the corresponding **all-Japanese** run this sweep flagged (`0x2035ed9c`-`0x2035fd04`,
   same messages, same relative order, confirmed by eye) — i.e. this whole message pool is genuinely
   block-separated by language for long stretches, not interleaved, so a proximity check will systematically
   misfire across most of it. `references_to` on `0x2035fac0` resolving to record 90/field `+0x28` of the
   `0x2032c91c` table (whose `+0x04`/`+0x08` fields are the already-known English message) confirmed the real
   pairing mechanism directly, not just inferred it.

**Net characterization, stated plainly per the task's ask for an honest negative**: outside the one small,
genuinely interleaved table this whole thread started from (`0x2032a000`), proximity-based pairing is
unreliable across this image — both large clusters checked here turned out to be tool false positives, not
new findings. The sweep's real value this session was (a) catching and correcting the 11th session's own
off-by-one error inside the one table where proximity pairing *does* apply, and (b) a clean, well-characterized
negative for the broader "spray the whole image and expect more hidden features to fall out" hope — nothing
else flagged panned out as new.

### Chasing the corrected finding with real Ghidra analysis

Full chain, condensed in `notes/diode-matrix.md`'s 12th-session section (not repeated in full here):
`FUN_2009060c` cases 4/5/6 all read the same field (`DAT_20090a20` → `"EMERGENCY MODE"`/`0x22`-offset
"非常通信モード", by language byte) that cases 1/2 use for the already-trusted ALL RESET/PARTIAL RESET
pairs — confirming the pairing structurally and by shared code convention. The case-selector byte itself
(icon "kind", `0x20403f64` this build) is set by `FUN_20037c10` from bits 2/3 of a status byte at
`0x203de175` (aliased `DAT_2002b45c[1]`/`*(DAT_20038290+1)`). That byte is cleared by the already-documented
`factory_reset_apply_defaults` (`0x2002a4c8`) and set by the already-documented `system_mode_request_dispatch`
(`0x2002a6b8`)'s previously-uncommented mode-request-value-4 branch, which in turn depends on two variables
(`DAT_2002b46c`/`DAT_2002b470`) with **no writer found anywhere in the image** — the same "populated via an
untraced message/event mechanism, or needs JTAG" dead end `notes/kernel-rtos-history.md`'s 30th session already
hit one variable earlier in this same chain (`DAT_2002a158`). Not a new dead end, just one level further into
an already-known one.

**D408/D411/D414/D417/D420 checked against this entire chain — a real, independent negative.** Read all four
functions' full decompiled bodies end to end (not a keyword grep): zero references to any diode-scan-value
alias or `region_code` anywhere in `FUN_2009060c`, `FUN_20037c10`, `system_mode_request_dispatch`, or
`factory_reset_apply_defaults`. This specific Emergency-Mode-indicator toggle mechanism is region-independent
as far as this chain reaches — consistent with, though not proof of, the user's own point (made mid-session)
that the broader reduced-power/relaxed-tuner-matching behavior might be an all-regions feature rather than
diode-gated at all.

### The separate "Emergency" SET-menu category — bounded check only, per the user's mid-session correction

The user corrected an earlier conflation risk mid-session: the standalone `"EMERGENCY"`/`"Emergency"` SET-
menu-category strings (`0x2035a5c4`/`0x2035a81c`) are a different feature from the JP-only 4630 kHz cluster,
despite sharing the English word "Emergency," and shouldn't be assumed diode-gated. Did a bounded (not
exhaustive) follow-up: both strings resolve via `references_to` to one record in a 24-byte-stride generic
SET-category table starting near `0x20190200` (`{type-tag byte (mostly 0x09), 4 name-pointer fields, 1
callback pointer}`); the Emergency record's callback (`0x20042f3c`) is a generic "render up to 4 sub-items"
page handler shared by several neighboring categories, parameterized by an external page-type byte, not by
anything specific to Emergency. Did not trace further (would need the specific item-list data this generic
renderer walks for the Emergency page, out of scope for remaining session time) — a real, partial structural
placement, not a gating answer. Still genuinely open.

**Files touched this session**: `tools/sjis_string_scan.py` (new tool), `tools/README.md` (new documentation
section), `notes/diode-matrix.md` (new 12th-session section, Open Question 6 addendum, correcting the 11th
session's "no English counterpart" claim), this history file. No git commit made — left for the user's own
review, per this project's standing instruction. No ARM/Thumb disassembly gaps were hit this session (all
code inspected was already disassembled correctly by Ghidra), so nothing was queued in
`scratch/armthumb_fix_requests.txt`. No `.bin` files or the Ghidra project's binary contents were modified —
only comments/notes in this repo and the two new/edited files under `tools/`.

## 13th session — Emergency Mode / Tuner traced end to end for the 4630kHz side; real user manual text sharpens and confirms the whole feature; EEPROM-write and boot-populate steps still not found

Task brief asked to pick up the 12th session's dead end (`FUN_2009060c`'s trigger bottoming out in two
read-only variables, `DAT_2002b46c`/`DAT_2002b470`, no writer found) using a brand-new, concrete lead: the
user described the real menu flow from actual hardware use — `MENU > SET > Others > Emergency > Tuner > OK`,
device restarts, an orange "E" badge appears top-left. Partway through the session the user additionally
pasted the real Japanese manual page for "非常通信モードの運用" (Emergency Communication Mode operation),
which sharpened the task significantly — see below.

### The manual text, and what it changed

The pasted manual confirmed: `MENU > SET > Others > Emergency` (非常通信) is **one shared screen** with two
independent checkboxes, `"4630kHz"` and `"チューナー"` (Tuner) — not two separate menu categories, correcting
an ambiguity the 11th/12th session entries left open (they treated "Emergency" the SET-menu category and
"4630kHz Emergency Communication Mode" as two separate, only loosely related threads; they're the same
screen, two checkboxes on it). The manual explicitly states: 4630kHz mode **forcibly switches the operating
mode to CW**, and Tuner mode **expands the antenna tuner's matching range to start tuning even at SWR ≥ 3**
(normally requires ≤3) and, **specifically for the IC-7300, limits max TX output to 50 W**. Both directly
confirm the user's original domain-knowledge claim (predating this whole project thread) about the "reduced
power + relaxed tuner matching" all-regions Emergency Mode. The flow for either checkbox: tap it, tap `OK`
(dismisses a warning dialog), tap a *separate* `"≪再起動してセット≫"` ("Restart to Set") button, the radio
reboots, mode is now active; un-checking both and repeating cancels it.

### Locating the Emergency category's actual sub-items (task 1)

Picked up from the 12th session's placement of the Emergency category's home record (`0x2019024c`, inside
the 24-byte-stride category table starting `~0x20190200`) and its generic renderer, `FUN_20042f3c` — a
"render up to 4 sub-items" handler parameterized by a byte at `DAT_20042658+0xb`. Traced `DAT_20042658`
itself: it's a **fixed pointer constant** (`0x203de174`, the *same* physical struct base already known as
`DAT_2002b45c` from the 12th session's Emergency-indicator chain) — not a per-category cursor as the 12th
session's phrasing implied. So `DAT_20042658+0xb` = `0x203de17f`, a single fixed byte.

Traced who writes `0x203de17f`: only one direct writer besides `factory_reset_apply_defaults` (which zeros
it) — `emergency_screen_checkbox_state_sync`'s neighbor `FUN_2005815c` (called from
`system_mode_request_dispatch`'s tail, right before `operating_mode_change_dispatch`), which syncs this byte
from a companion field at offset+0x13 (`0x203de187`) whenever the current value exceeds `0x12` (18) — i.e.
this byte doubles as "current operating mode" (values 0-18) and "current SET-menu page ID" (values ≥19, the
renderer subtracts `0x13` to get a 0-based page index) in the same physical byte. This reconciles cleanly
with `FUN_20042f3c`'s own `- 0x13` arithmetic, but tracing *which* literal page-ID value corresponds to
Emergency specifically, and how the outer category-list navigation writes offset+0x13 per-category, was not
completed (the one writer found for offset+0x13, `FUN_2003dcac`, only zeroes it as part of factory reset).

Rather than keep chasing this generic page-index plumbing, pivoted to the newly-supplied manual text and
worked from the two checkboxes' own warning-dialog strings instead — a much more direct and productive path
(see below). **Net for task 1**: did not cleanly nail the item-list data structure the generic renderer walks
for the Emergency page specifically (that remains open, same as the 12th session left it), but *did* locate
and confirm both real sub-item names ("4630kHz" and "Tuner") via their warning-dialog text, and traced both
items' actual UI-action code — arguably a more useful resolution to the spirit of the question than the
literal item-list table would have been.

### The Tuner warning dialog — a new firmware string, confirms the manual's "50 W" claim directly

The existing 12th-session message-ID table work (`0x2032c91c`, 76-byte stride, already pinned `0x54`/`0x57`/
`0x59` for the Reset dialogs and `0x5a` for the 4630kHz warning, "You can transmit on 4630 kHz for
Emergencies.") was extended one message ID further: **`0x5b`** reads, assembled from 4 fragments at
`0x2035f7dc`/`0x2035f8a0`/`0x2035f7b8`/`0x2035e6f8`: *"Expands max. matching ratio for Emergencies. Output
power is limited to max. 50W. Danger! Never get close to the antenna during TX."* This is the Tuner-mode
warning dialog, and its text is a **direct firmware-string confirmation** of the manual's 50 W claim (and
close paraphrase of the SWR-matching-range claim) — stronger evidence than a numeric constant would have
been, since it's literally the on-screen warning text.

### The confirm-callback chain — hit and resolved a real ARM/Thumb disassembly gap

Searched for what calls `ui_show_message_dialog` with these two message IDs (`0x5a`/`0x5b`) via a hex search
for the ARM `mov r0,#imm` encodings (`5a 00 a0 e3`/`5b 00 a0 e3`) — found both at `0x20041d4c`/`0x20041d38`.
Ghidra's own `inspect.listing`/`inspect.decompile` at this address range (`0x20041cfc`-`0x20041d68`) returns
**raw undefined bytes**, not instructions — a real ARM/Thumb-adjacent disassembly gap, this project's
established pattern. Followed the documented fallback: ran `arm-none-eabi-objdump -D -b binary -m arm
--adjust-vma=0x20005000` against `scratch/unpacked/142/body.bin`, which disassembles this region cleanly as
plain ARM32 (`ldrb`/`cmp`/`beq`/`ldr`/`b` — a checkbox-state dispatch that decides whether to show `0x5b` or
`0x5a` based on a separate context byte at `0x20390211`, then tail-calls `ui_show_message_dialog` with the
callback loaded from literal-pool slot `0x20042680` in both branches). Queued
`0x20041cfc 108 arm` in `scratch/armthumb_fix_requests.txt` for the user to apply via the Ghidra GUI script.

Confirmed via this ground truth: **both** the `0x5a` and `0x5b` dialogs use the *same* confirm-callback,
`0x20041cd0` (renamed `emergency_screen_warning_ok_callback`) — it just sets a generic "confirmed" flag
(`*(DAT_20042664+6)` = `*0x2039021e` = 1), not distinguishing which checkbox. Its sibling
`all_reset_confirm_callback` (already named, 10th session) is structurally identical but writes
`*(DAT_20042664+5)` = `*0x2039021d` instead — confirming this pair of bytes (`0x2039021d`/`0x2039021e`,
offsets +5/+6 of the shared page-context struct `0x20390218`) is reused as a generic "OK confirmed for
current item slot" scratch flag across *several* different confirm dialogs (Partial Reset, All Reset,
4630kHz, Tuner), not a dedicated pair of "is 4630kHz checked"/"is Tuner checked" bits as first hypothesized.

### The real bit-specific commit — found, for the 4630kHz side

Since the shared OK-callback doesn't set the bit-specific target, looked instead at the *separate*
"restart to set" button, per the manual's distinct step. Found a small cluster of generic "Others"-list
OK-handlers referenced from a literal-pool/table region around `0x2018b1a8` (the same infrastructure
already used for Partial/All Reset): `FUN_2000de50` unconditionally calls `FUN_2002b818(3)` — confirming
this cluster really is the shared "Others" action list (`FUN_2002b818(3)` is the already-documented Partial
Reset trigger). Its immediate neighbor, `0x2000de64` (renamed `emergency_mode_restart_commit`): guarded by
`*DAT_2000e238 & 0x4000`, reads a pending 0/1 value from `*(DAT_2000d23c+3)`, and if valid, writes it via
`*DAT_2000e24c` — confirmed (by reading the literal pool value directly) to be the **exact same physical
address** as `*DAT_2002b46c` (`0x2039021d`), the 4630kHz-target byte `system_mode_request_dispatch`'s
already-documented `uVar11==4` branch reads — then calls `FUN_2002b818(4)`, writing mode-request value 4
into `DAT_2002a158`/`DAT_2002b4fc` and running the shared restart machinery.

This is a real, concrete, ground-truth confirmation of the task brief's predicted shape for the 4630kHz
checkbox: **checkbox tap → warning dialog OK → restart-to-set button → stages pending value → requests
system-mode-4 → (already-documented) `system_mode_request_dispatch`'s mode-4 branch commits the value into
the persistent status bits (`0x203de175` bit 3) → full subsystem restart.** Decompiled `FUN_2002b818` itself
(`*DAT_2002b4fc = param_1; FUN_20041dd8(); FUN_20017390();`) and both of its callees — **neither is an EEPROM
call**: `FUN_20041dd8` sets an interrupt-guarded notification bit, `FUN_20017390` clears an unrelated
11-slot pointer array. **No EEPROM write was found anywhere in this specific commit chain** — a real,
honest negative for half of the task 2 hypothesis.

Also found and renamed `emergency_screen_checkbox_state_sync` (`0x2002ae6c`) — runs in the *opposite*
direction (guarded by the same `0x4000` bit under a different alias, `DAT_2002b4e0`, matching this project's
established "one physical flag, several DAT_ names" pattern): when the Emergency screen opens, it reads the
live persistent bits (`0x203de175` bits 2/3) and copies them **into** the two staging bytes, seeding the
checkbox display from the currently-committed mode. This confirms `0x2039021d`/`0x2039021e` are UI scratch
state, refreshed from the live bits on screen-open, not themselves EEPROM-backed — consistent with (and
explaining) why no direct EEPROM consumer was ever found for them.

**Could not isolate a Tuner-specific sibling of `emergency_mode_restart_commit`** (something writing
`0x2039021e`/`*DAT_2002b470` the same way, from a pending checkbox value). Checked the next two candidates in
the same OK-handler cluster — `FUN_2000de28` (calls `FUN_20030008`, an unrelated bit-3 set in a totally
different `DAT_200306c4` struct) and `FUN_2000dea4` (calls `FUN_2002b818(1)`, mode-request value 1, also a
different feature) — neither fits. Genuinely not found this session, open for a future one.

### Boot-time EEPROM populate (task 3) — still not found; one assumption in the brief corrected

Checked `notes/kernel-rtos.md` directly for the "general EEPROM-settings-load-at-boot mechanism" the task
brief said this project had already mapped there — **it hasn't**: a direct grep for "eeprom" in that file
returns zero hits. This is a correction to the brief's assumption, not a finding about the firmware.
Checked `notes/eeprom-catalogue.md` instead — its one documented combined-settings loader (`FUN_2006cb84`,
loading into a struct based at `0x203b31e0`, roughly `0x1a80` bytes total) does **not** reach `0x203de174`
(the offset would be ~`0x2af94` bytes, far past that struct's end) — ruled out by simple address arithmetic,
not exhaustive search. Spot-checked two `PARAM`-tagged references to the `0x203de174` struct base
(`FUN_200092f0`, `FUN_200278f0`) that looked superficially promising — neither turned out to be an EEPROM
call; both are small, unrelated field-setters. Did not have session budget to sweep the ~50 of 73 total
`FUN_2001e510` call sites `notes/eeprom-catalogue.md` itself flags as still unsampled, nor to read
`cold_boot_hw_init` line-by-line looking for this specific struct. **Net: the 12th session's dead end for
`DAT_2002b46c`/`DAT_2002b470` (no writer besides `system_mode_request_dispatch`'s own mode-4 branch) is
NOT closed.** What's new is a concrete, real UI-side *caller* of that mode-4 request
(`emergency_mode_restart_commit`) — genuine forward progress — but the EEPROM round-trip (write at menu-OK
time, read back at a real cold boot) that would explain how this survives an actual power cycle remains
unconfirmed, flagged honestly rather than forced to fit.

### Diode/region gating (task 5) — checked, real clean negative, same as always

Read the full decompiled bodies of every function newly traced this session
(`emergency_screen_warning_ok_callback`, `emergency_mode_restart_commit`, `emergency_screen_checkbox_state_sync`,
`FUN_2002b818`, `FUN_20041dd8`, `FUN_20017390`, `FUN_20042f3c`, `FUN_20041f20`) — **zero references** to any
diode-scan-value alias (`DAT_2003c7f8`/`DAT_2003c800`/`DAT_2003ea4c`/`DAT_2003c858`) or to `region_code` in
any of them. D408/D411/D414/D417/D420 specifically: none of these functions touch any diode bit at all
(not just these five), so this new chain neither confirms nor further narrows those five diodes — same
shape as every prior session's check. Looked for, but did not find, a region-conditional visibility check
specifically hiding the "4630kHz" checkbox for non-JP regions (the structurally natural prediction, since
4630kHz is JP-only per D420/D423 while Tuner should be universal) — the one plausible "is this item enabled"
gate reachable from the generic renderer, `FUN_20041f20`, tests unrelated item codes (`0x5d`/`0x5e`/`0x5f`/
`0x61`/`0x129`-`0x138`) against a **different** struct (base `0x203de4cc`, not `0x203de174`), and is only
called for "type 2" items in `FUN_20042f3c`'s render loop — the checkboxes render as "type 1" items, which
never reach this gate at all. So `FUN_20041f20` is very likely the wrong function regardless of its own
content, and no better candidate was found in the time available. Region-conditional visibility for the
4630kHz checkbox remains unconfirmed — not found, not disconfirmed.

### Orange "E" badge (task 4) — not investigated

No time was spent on this; genuinely open, not a negative result.

### Bonus — functional confirmation (task 6)

**50 W power limit: confirmed directly via firmware string** (message `0x5b`'s text, above) — stronger
evidence than a numeric constant would have been. **SWR≥3 threshold**: not searched for at all this session.
**CW-mode force**: not conclusively found, but one real, new, partial lead: `FUN_200132c4` —
`if ((*(byte*)(DAT_200134e0+1) & 8) != 0 && param_1 == DAT_200134f4) return 0` (called from `FUN_200132f0`,
an "is candidate operating-mode value selectable" gate) — is a genuine consumer of bit 3 (4630kHz) of the
same `0x203de175` status byte, not found by any prior session. Could not confirm this actually implements
"force CW" specifically: the excluded value's static RAM content this build (`0x46a5f0`) doesn't read as a
small mode-enum constant, so either `DAT_200134f4` is populated dynamically at runtime with a real mode
value, or this reading of the gate's purpose/direction is incomplete. Recorded as a real, partial lead, not
a confirmed closure — worth a fresh look in a future session (start by finding who writes `DAT_200134f4`).

### Renames and comments (Ghidra database)

- `FUN_2000de64` → `emergency_mode_restart_commit` (+ plate comment with the full chain).
- `FUN_20041cd0` → `emergency_screen_warning_ok_callback` (+ plate comment, including the objdump-verified
  dual dialog-ID wiring and the ARM/Thumb gap).
- `FUN_2002ae6c` → `emergency_screen_checkbox_state_sync` (+ plate comment).

**Files touched this session**: `scratch/armthumb_fix_requests.txt` (queued one fix line, `0x20041cfc 108
arm`), `notes/diode-matrix.md` (new "Emergency Mode / Tuner (all-regions)" section, Open Question 6/11
updates), `notes/diode-matrix-history.md` (this entry). Ghidra database: 3 renames + 3 plate comments (listed
above), no destructive changes. No git commit made — left for the user's own review, per this project's
standing instruction.

## 14th session — the "restart" traced end to end and confirmed as a genuine software-only soft restart; a deeper structural search for the 4630kHz visibility gate finds the real per-item records but still no gate

Two sharp, concrete corrections/refinements from the user, both aimed at the 13th session's two biggest
remaining gaps. No re-derivation needed — picked up exactly where that session left off.

### Direction 1: "restart" is very likely soft/warm, not a power-cycle — traced and confirmed

The user, who has actually used this feature on real hardware, said the radio does not really power-cycle
when entering Emergency Mode — it only appears to restart. Traced the already-found call chain
(`emergency_mode_restart_commit` → `FUN_2002b818(4)` → `system_mode_request_dispatch`'s mode-4 branch) all
the way down through the ~80 unconditional function calls in that function's own tail (already described,
30th kernel-rtos session, as "a full subsystem restart"). Read every one of those callees' names/addresses
against two already-fully-documented hardware-reset primitives this project separately mapped in
`notes/firmware-update.md`'s "system restart mechanism" section: `FUN_20052bd0` (the `P1_6`-gated brownout
watchdog-reset in `main_idle_loop`) and `FUN_20029ca4` (the second, independent watchdog-reset path found
via the `Fup_AutoEnd_3765` marker thread). **Neither appears anywhere in `system_mode_request_dispatch`'s
call tree**, and neither does `cold_boot_hw_init` (`0x2002afc0`) itself.

Confirmed this structurally, not just by absence, by walking the caller graph directly:
- `cold_boot_hw_init` has **exactly one caller** in the whole image: `0x2002b1d8`, inside
  `cold_boot_mode_dispatch` (`0x2002b1c8`, previously unnamed `FUN_2002b1c8` — renamed this session).
  `cold_boot_mode_dispatch`'s body: `*DAT_2002b500=1; cold_boot_hw_init(); ... while (system_mode_request_
  dispatch(), *pcVar3=='\0') { pick an idle loop from DAT_2002a4a4; main_operating_loop(0,1); }` —
  i.e. `cold_boot_hw_init` runs exactly once, then the function loops forever calling
  `system_mode_request_dispatch` on every iteration.
- `cold_boot_mode_dispatch` itself has **exactly one caller**: `0x2002b554`, inside `FUN_2002b29c` — the
  real top-level cold-boot/power-state entry dispatcher (already has its own detailed plate comment from an
  earlier session: "references_to finds ZERO direct callers of this function anywhere in body.bin... most
  likely reached via first_task_entry's message-dispatch loop" — i.e. genuinely runs once per real hardware
  boot/reset). Decompiled `FUN_2002b29c` fully: it picks between `cold_boot_mode_dispatch()` and
  `FUN_20029ca4()` as two **mutually exclusive, sibling** branches based on a wake/mode-source byte chain
  — `FUN_20029ca4` (the watchdog-reset path) is a completely separate top-level choice, never reachable
  from inside `cold_boot_mode_dispatch`'s own loop.

**Conclusion: this is a real, well-evidenced, positive closure, not an open gap.** Emergency Mode's
"restart" (and every other `system_mode_request_dispatch` request value, by the same evidence) is pure
software task-teardown-and-reinit within the *same continuously-running process* — no CPU reset, no
watchdog trigger, no re-entry to `cold_boot_hw_init` at all. This directly confirms the user's own hardware
observation. It also means the 13th session's "no EEPROM write found, no boot-time EEPROM-populate found"
finding doesn't describe an open dead end after all: there's no EEPROM round-trip to look for, because
`0x203de174`'s persistent status bits are never at risk of being cleared or reloaded in the first place —
RAM simply isn't touched by this kind of "restart." Added a substantial addendum to
`system_mode_request_dispatch`'s existing plate comment documenting this (original text preserved intact,
not overwritten, per this project's correction-handling convention).

### Direction 2: hunting the 4630kHz visibility gate, specifically testing D420 — deeper search, still not found

The user's own hypothesis: the 4630kHz checkbox (JP-only per the earlier-documented D420/D423 population
data) must be hidden from the menu in non-JP builds by *something* — possibly region_code, possibly D420
directly, since D420 has never had a confirmed consumer across 13 prior sessions. Confirmed the basic
premise first: `scratch/unpacked/*/body.bin` is organized per firmware *version* (`111`-`142`), not per
region — this project's whole diode-matrix premise is one image, region selected by physical diodes at
runtime — so a real gate, if it exists, must be runtime code somewhere in this same image.

**New structural find**: traced past the 13th session's dead-end ("`FUN_20041f20` — wrong struct, unrelated
item codes, and type-1 items never call it anyway") to the *actual* list-item records for "4630kHz" and
"Tuner." These live in a previously undocumented **20-byte-stride list-widget table** — a different, larger
mechanism from the "up to 4 sub-items" generic renderer (`FUN_20042f3c`/`DAT_200426b0`) the 12th/13th
sessions had been exploring; that 4-byte-per-entry mechanism appears to serve the top-level SET-menu
*category* grid (RX/TX/Display/Emergency/etc., shown 4 tiles at a time), while this new 20-byte-record table
is the "Others" screen's *own* scrollable item list (Firmware Update, Load Setting, Touch Screen
Calibration, an unrelated "Tuner" calibration item, Play Files, CI-V Address, Format, Partial Reset, All
Reset, Unmount, REF Adjust, ..., and — found this session — "4630kHz" and a second "Tuner" row, distinct
from the earlier unrelated one). Confirmed the record shape empirically against known-good anchors
(`all_reset_button_handler` lands exactly at offset+8 of its 20-byte-aligned record, and the two new
handlers below land at the identical offset in theirs): `{name_EN, name_JP, tap_handler, secondary(always 0
in every record sampled), flags}`.

- **"4630kHz"** record (`0x2018edb8`): `name_EN`/`name_JP` both `0x20359ae0` ("4630kHz" itself — a
  language-neutral string reused for both fields), `tap_handler` = `0x20041cfc` (renamed
  `emergency_4630khz_item_tap_handler`), `secondary` = 0, `flags` = `0x1071e`.
- **"Tuner"** record (`0x2018edcc`): `name_EN` = `0x20359a3c` ("Tuner"), `name_JP` = `0x2035997c`
  (Shift-JIS), `tap_handler` = `0x20041d54` (renamed `emergency_tuner_item_tap_handler`), `secondary` = 0,
  `flags` = `0x10709`.
- Confirmed `emergency_4630khz_item_tap_handler` is the same function already partly reverse-engineered
  last session (still inside the queued ARM/Thumb gap at `0x20041cfc`, ground truth via `objdump`): checks
  Tuner's own checked-state (`*0x2039021e`) first (an early uncheck-and-refresh path), then a selector byte
  at `*0x20390211` to pick warning dialog `0x5b` or fall through to `0x5a`.
  `emergency_tuner_item_tap_handler` is a trivial 2-instruction stub, `mov r0,#4; b FUN_2002b818` —
  unconditionally requesting system-mode-4, no dialog call visible from this address directly (an honest
  loose end — its own warning-dialog trigger, if separate, wasn't traced this session).

**Checked directly for a diode/region-code test, real thorough negative**:
- Both tap-handlers' bodies (`objdump`-verified) — zero diode/region references in either.
- The record's `secondary` field — structurally the position analogous to `notes/ui-menu.md`'s QUICK MENU
  `+0x14` "am I available" callback — is null (`0`) in every one of the ~8 records sampled, including both
  checkbox rows. No per-item availability callback is populated here at all.
- `references_to` on the `flags` field address and the 4630kHz name field address, individually
  (`0x2018edb8`/`0x2018edc8`/`0x2018eddc`) — zero hits on all three. No code anywhere reads these fields via
  a resolvable literal address — the same "computed-table-access wall" this project has hit for the
  216-item table, the 326-item defaults table, and others: the real per-row walker almost certainly computes
  `base + index*20 + field_offset` at runtime, invisible to static xref analysis.
- Ruled out the `flags` field's low byte (`0x00`/`0x1e`/`0x09`/`0x16` across the rows sampled) as a
  disguised item code for the already-known `is_feature_enabled_for_region` gatekeeper — that function's
  item-code range starts at `0x24`; feeding it any of these smaller values would hit its unconditional
  `return 0` path, which can't be right for items that render at all.
- **Could not locate the table's own base pointer / driving walker function** (the equivalent of
  `notes/ui-menu.md`'s `DAT_2004f728` for QUICK MENU) within the session's budget — sampled roughly 20
  records in both directions from the two known handlers with no header/sentinel record found. Without the
  walker, a region/diode-conditional item count or skip-list applied *before* the generic renderer reaches
  these records can't be ruled out — only everything currently reachable from the records/handlers
  themselves.

**Net**: real, meaningfully deeper progress (concrete records + real tap-handlers, a materially better
target than the 13th session's wrong-struct dead end), but the visibility gate itself — and D420
specifically — remains genuinely not found, not disconfirmed. Flagged as a concrete next step (find the
table's base/walker) rather than left as a vague "still open."

### Renames and comments (Ghidra database)

- `FUN_20041cfc` → `emergency_4630khz_item_tap_handler` (+ PRE comment with the record/field addresses).
- `FUN_20041d54` → `emergency_tuner_item_tap_handler` (+ PRE comment).
- `system_mode_request_dispatch`'s existing plate comment: appended a new paragraph (original text kept
  intact) documenting the soft-restart closure with full addresses/evidence.
- No new symbols created as `FUN_2002b1c8`/`cold_boot_mode_dispatch`/`FUN_2002b29c` were already named or
  already carried a sufficient plate comment from earlier sessions — read, not re-annotated.

**Files touched this session**: `notes/diode-matrix.md` (new 14th-session section, EEPROM-gap correction,
Open Question 11), `notes/diode-matrix-history.md` (this entry). Ghidra database: 2 renames + 2 PRE
comments + 1 plate-comment addendum, listed above. No destructive changes. No ARM/Thumb fix newly queued —
the one gap touched this session (`0x20041cfc`) was already queued last session and covers it. No git
commit made, per the user's standing instruction (they handle commits themselves).

## 15th session — found the real per-item render dispatch (`settings_list_item_kind_renderer`) for "4630kHz"/"Tuner"; strongest negative yet on the visibility-gate question, one concrete "false lead" ruled out

The user pointed at a specific address, `DAT_2018ed50` — 4 bytes before the 14th session's mapped
table-record area, i.e. that record's own field the 14th session had (mis-)labeled "flags" — and ran
`references_to` on it directly, finding exactly 2 hits. Asked for both to be traced fully.

### Hit 1 (`0x20042498`, inside `FUN_20042458`) — real, and the missing piece of the render chain

Decompiled `FUN_20042458` fully and renamed it `settings_list_item_kind_renderer`. Confirmed it's called
from `FUN_20042f3c` (the already-known "up to 4 items" page renderer) for type-3 items specifically —
`settings_list_item_kind_renderer(uVar5, uVar8)`, `uVar5` = page start index, `uVar8` = slot 0-3. It
resolves an absolute item index via `uVar11 = *(ushort*)(DAT_200426b0 + (uVar5+uVar8)*4 + 2)` (the same
`DAT_200426b0` index-resolution array already known from the 13th session), then reads a "kind" byte via
`*(byte*)(DAT_2004196c + uVar11*0x14 + 8)`.

`DAT_2004196c` resolves to `0x2018ed48` this build — confirmed via direct memory read. This is **the exact
same physical ROM table** the 13th/14th sessions already explored for the "Others" screen's per-item
name/tap-handler records — a genuine, useful reconciliation, not two separate tables. The 14th session's
"record" model (name_EN/name_JP/tap_handler/secondary/flags at offsets 0/4/8/12/16, base found by aligning
`all_reset_button_handler` etc. to offset+8) and this function's own "kind byte at base+8" read are simply
**two different consumers reading two different fields of the same 20-byte-stride records**, with base
pointers 8 bytes apart (`DAT_2004196c` = `0x2018ed48`; the 14th session's inferred name/handler record base
for the same logical row = `0x2018ed48 + 8` further in). Not a contradiction — the 14th session's
handler/name field identifications (independently confirmed via real `DATA` xrefs, e.g.
`all_reset_button_handler` at `0x2018edac`) remain solid; this session just adds the "kind" field on top,
read via its own, separately-confirmed base.

**Read the actual kind bytes directly from ROM** at the computed addresses: `uVar11==5` → kind byte at
`0x2018edb4` = `0x1e`; `uVar11==6` → kind byte at `0x2018edc8` = `0x1e`. Both hit the function's
`case 0x1e: goto LAB_200429fc;` block, which switches again on `uVar11` itself:
```c
if (uVar11 == 5) { *pbVar13 = 1; *(byte*)(iVar7+0xad) = *(byte*)(DAT_20042664 + 5); return; }
if (uVar11 == 6) { *pbVar13 = 1; *(byte*)(iVar7+0xad) = *(byte*)(DAT_20042664 + 6); return; }
```
`DAT_20042664+5`/`+6` are `0x2039021d`/`0x2039021e` — the already-known 4630kHz/Tuner checked-state target
bytes. **This is the first fully confident confirmation that item index 5 = "4630kHz" and index 6 =
"Tuner"** (previously inferred only from name-string proximity to tap-handlers), and it directly shows —
by reading the actual code, not by absence — that `*pbVar13` (the "is this item available/shown" output)
is set to `1` **unconditionally** for both. No diode, region_code, or any other test appears anywhere in
this dispatch path.

### `DAT_200426b8` — traced per the user's specific request, real negative, and moot for these two items anyway

The user flagged `case 0x16`/`0x18`/`0x20`'s shared pattern (`*(char*)(iVar10 + param_2*0x1c) != '\0'`,
`iVar10 = DAT_200426b8`) as a strong candidate visibility-gate array. Read `DAT_200426b8`'s resolved value
(`0x203a2f88`) and found its 8 real writers via `references_to` filtered to `WRITE`: `FUN_20023d74` (a
simple 4-slot zero-initializer) and `FUN_20059754` plus 5 siblings in the `0x20059xxx`-`0x2005axxx`
cluster. Decompiled both representative ones fully: they copy fields from a **different**, unrelated
0x34-byte source struct (`DAT_20058fb8`) into this 0x1c-stride cache, 4 slots at a time, gated on a
condition (`*(DAT_20058fac+0xb)=='0'` and `*(DAT_20058f9c+6)==0`) that shifts/rotates the 4 cached slots —
the shape of a generic "recent items" or similar rolling list cache for some other feature entirely. No
diode or `region_code` reference in any writer checked. **Also structurally moot regardless**: cases
`0x16`/`0x18`/`0x20` are different kind values from the `0x1e` our two checkbox items actually use — this
array is never even consulted for "4630kHz"/"Tuner" specifically.

Checked the three sibling gate calls in those same cases too (`FUN_2005ea60`/`FUN_2005eab0`/`FUN_2005e8ac`,
per the user's request to understand what they test): all three read a single-char mode/status byte at
`*DAT_2005dfb0` against values `'\0'`/`'\b'`(0x08)/`'K'`/`'R'`/`'S'` — plausibly a CI-V/operating-mode
compatibility check, confirmed to have nothing to do with diodes or `region_code`. Orthogonal, as the user
suspected might be the case.

### Hit 2 (`0x2008a4d4`, inside `FUN_2008cff8`) — checked and ruled out as a coincidental false lead

Per the user's explicit warning not to assume this one's real just because `references_to` flagged it,
decompiled the full ~10,000-byte function. It's a completely unrelated CI-V/service-mode
display-synchronization dispatcher (switches on an entirely different selector byte space — `9`-`0xf` and
`'\t'`/`'\n'` — full of `FUN_2008xxxx`/`FUN_200acxxx`/`FUN_200adxxx` calls that read like a front-panel or
sub-display diff/sync protocol) with no visible relationship whatsoever to the settings-list table, its
kind bytes, or the two Emergency-mode items. Confirmed as a coincidental address match, the same "false
lead" failure mode the 13th session flagged for a different address.

### Net assessment

This session reaches the strongest negative yet on the "who hides 4630kHz for non-JP regions" question:
every reachable function that touches these two specific items by name, index, or state — the kind-based
render dispatch (this session), the tap-handlers (14th session), and the checkbox-state bytes themselves
(13th/14th sessions) — has now been read in full and is clean of any diode or `region_code` reference. If a
real gate exists, it has to live in whatever populates `DAT_200426b0` (the index-resolution array) for the
relevant page, or in the page's own item count/bounds — the same "table's own base/walker function is not
yet found" gap the 14th session already flagged, now narrowed considerably (a hide-by-omission from the
resolved index list, not a per-item disable, since disabling isn't wired up for kind `0x1e` at all).

### Renames and comments (Ghidra database)

- `FUN_20042458` → `settings_list_item_kind_renderer` (+ a substantial plate comment covering the whole
  trace: the kind-table reconciliation, the `uVar11==5/6` confirmation, `DAT_200426b8`'s real writers and
  why it's moot here, the three mode-byte gate functions, and the ruled-out false lead).

**Files touched this session**: `notes/diode-matrix.md` (new 15th-session section, Open Question 11
addendum), `notes/diode-matrix-history.md` (this entry). Ghidra database: 1 rename + 1 plate comment, listed
above. No destructive changes. No new ARM/Thumb disassembly gaps were hit this session (both functions
inspected were already fully disassembled/decompiled by Ghidra), so nothing new was queued in
`scratch/armthumb_fix_requests.txt`. No git commit made, per the user's standing instruction.

## 16th session — D420 confirmed: found the settings-list "walker," and it's a direct diode-bit test, not a region-code test

The user asked, directly, the exact question this whole 13th-16th-session thread had been circling without
answering: for the "4630kHz" visibility gate, is the enabler a diode, or is it the region code resolved
*from* the diode matrix? Delegated the remaining trace (finding whatever populates `DAT_200426b0`/item
count, the one piece sessions 13-15 never reached) to a subagent, then independently re-verified every
material claim by direct decompile/memory read before writing anything up — this session's finding is a
positive, not another negative, and the bar for accepting it needed to be high.

### Why the walker was invisible to 9 sessions of `references_to` sweeps

Two separate, compounding blind spots, both confirmed directly rather than assumed:

1. `DAT_200426b0`/`DAT_20042664`/`DAT_200426ac`/`DAT_20042658` (the symbols the 13th-15th sessions' xref
   sweeps were built around) are never write targets *under those names* anywhere in the program — all-READ,
   confirmed by a fresh `references_to` sweep on all four. They're compile-time-fixed pointers into a
   shared, reusable "current settings screen" singleton; the real writer holds its own private literal-pool
   copies of the identical pointer values under different symbol names (`DAT_2003ea44`/`DAT_2003ea48`/
   `DAT_2003ea64`/`DAT_2003ea60`/`68`/`6c`/`70`) roughly `0x4400` bytes away, so Ghidra's xref database never
   linked the two call sites even though they touch the same RAM. Confirmed identical by direct memory read:
   `DAT_20042664`(`0x20390218`)==`DAT_2003ea48`, `DAT_20042658`(`0x203de174`)==`DAT_2003ea44`,
   `DAT_200426b0`(`0x203da12e`)==`DAT_2003ea64`.
2. Separately — and this is the more consequential one — the diode-bit consumer itself reads
   `DAT_2003ea4c`, an address that *is* one of the 5 diode-scan aliases every prior session's exhaustive
   sweep explicitly targeted, and **`references_to` on it still doesn't surface the read**. Directly
   re-verified this session: `references_to(0x2003ea4c)` → 4 hits (`0x2003dc84`/`0x2003dce8`/`0x2003df3c`/
   `0x2003e23c`); the real consumer at `0x2003e108`, whose decompile plainly shows `*DAT_2003ea4c & 0x4000`,
   is not among them. A genuine, confirmed gap in Ghidra's own reference analysis for this load site — not
   a methodology mistake by sessions 4-15, whose "no consumer found for D420" conclusions were accurate
   given the tooling available. Worth remembering going forward: an exhaustive `references_to` sweep is
   only as complete as Ghidra's own analysis of that address's readers.

### The chain: `settings_list_builder` → `settings_item_visibility_filter` → `settings_item_diode_region_gate`

`settings_list_builder` (`FUN_2003e5f0`, `0x2003e5f0` — Ghidra's auto-detected function boundary for this
entry point is corrupted, running implausibly to `0x2005ea37`; the real logic lives in
`~0x2003e690`-`0x2003e858`, everything past that is a mis-merged jump-table region and was not trusted)
clears the 152-slot `DAT_2003ea64` scratch array, then loops over the current category's items (source:
`DAT_2003ea70[category_id]`, a ROM registry of `{item_count; quick_flags_table_ptr; reserved}` triples,
base `0x201993e0` — read directly, confirmed) calling `settings_item_visibility_filter(item_index)` per
item. A failing item is never appended to the output array — genuinely removed, not disabled — matching the
15th session's structural prediction exactly. Passing items get their raw `{kind:u16, val:u16}` record
copied verbatim; `val` becomes the catalog index (`uVar11`) `settings_list_item_kind_renderer` later reads
against the `0x2018ed48` table, tying this directly back to the already-confirmed catalog indices 5/6 =
"4630kHz"/"Tuner".

`settings_item_visibility_filter` (`FUN_2003e29c`, `0x2003e29c`) has a few category-specific special cases
(all confirmed operating-mode/hardware checks, unrelated to diodes), but always falls through to one
universal call: `settings_item_diode_region_gate(&quick_flags_table[item_index])`.

`settings_item_diode_region_gate` (`FUN_2003e108`, `0x2003e108`) is the actual gate. Re-decompiled directly
to confirm (not taken from the subagent's report alone):

```c
else if (cVar1 == '\x03') {
    if (*(short *)(param_1 + 2) == 5) {
        if ((*DAT_2003ea4c & 0x4000) == 0) { uVar4 = 1; }   // D420 absent -> EXCLUDE
        else                                { uVar4 = 0; } // D420 present -> include
    }
    else if (*(short *)(param_1 + 2) == 7) {
        // unrelated hardware/model nibble-compare, DAT_2003ea44+1 vs DAT_2003ea48+5/+6
    }
}
```

`0x4000` = bit 14 = D420 per this project's own confirmed scan-bit layout (row-middle, column 2). Read with
the established "1 = diode present" convention: **D420 absent → item excluded from the built list entirely
(never inserted) → checkbox doesn't appear; D420 present → included.** This is a raw diode-bit test, sitting
in the same function as (but structurally distinct from) `is_feature_enabled_for_region()` calls used for
neighboring `kind 1`/`kind 2` items — so the firmware author had `is_feature_enabled_for_region()` available
right there and deliberately used a direct bit test instead for this specific item.

### Independently re-verified, byte-for-byte, this session

`DAT_2003ea70` → `0x201993e0`. Category `0x22`'s registry record at `0x201993e0 + 0x22*0xc = 0x20199578` →
`item_count=3, table_ptr=0x20199160`. Table at `0x20199160`: `{kind=3,val=5}`, `{kind=3,val=6}`,
`{kind=3,val=7}` — `val=5` is the diode-gated record (matches the already-confirmed "4630kHz" catalog index
5), `val=6` matches no case in the gate → always included (matches the already-confirmed-clean "Tuner",
catalog index 6), `val=7` hits the separate hardware/model nibble-compare (an unidentified third item in
this category, not investigated further this session).

### The one remaining inferential step

Category `0x22` → "the Emergency screen" isn't closed by a literal string/label xref: the 13th-session
"Emergency" category record (`0x20190248`, `0x18`-byte stride, holding the `"EMERGENCY"`/`"Emergency"`
string pointers) doesn't itself store a numeric category id in any of its 6 fields — re-checked directly,
they're padding, string pointers, or the generic page-renderer address (`0x20042f3c`). Instead this rests
on two independent, convergent, non-inferential matches: `settings_list_builder(0)` is called directly from
`emergency_screen_warning_ok_callback` (`0x20041cd0`, the already-confirmed OK-handler for both the 4630kHz
and Tuner warning dialogs), and the category's 3-item shape (one diode-gated, one clean, one hardware-gated)
lines up exactly with the independently-confirmed catalog indices 5/6. Strongly evidenced, not
string-xref-closed — the one honest gap in an otherwise fully byte-verified chain.

### Net assessment

The 4630kHz-visibility question this thread opened with is answered: **it's the diode (D420), not the
resolved region code, and not `is_feature_enabled_for_region()`.** D420 moves from "❓ unconfirmed, no
consumer found across 15 sessions" to "✅ confirmed" in the living-reference table — the first real
consumer ever found for it, reached via a route (the settings-list walker/filter machinery) that no prior
session's search had tried, one level upstream of every path those sessions correctly found clean
(tap-handlers, render dispatch, checkbox-state reads, the name/kind catalog itself).

### Renames and comments (Ghidra database)

- `FUN_2003e5f0` → `settings_list_builder` (+ plate comment: the walker's role, why it was invisible to
  prior xref sweeps, the item-copy mechanism, the corrupted auto-detected function-boundary warning).
- `FUN_2003e29c` → `settings_item_visibility_filter` (+ plate comment on the category special-cases and the
  universal fallthrough call).
- `FUN_2003e108` → `settings_item_diode_region_gate` (+ plate comment with the full kind-1/2/3 breakdown,
  the D420 bit-14 identification, and the registry-table cross-check).
- `EOL` comment at `0x20199160` documenting the 3-record quick-flags table and its category/registry
  linkage.

**Files touched this session**: `notes/diode-matrix.md` (D420 living-reference row updated to confirmed,
"No consumer found" list corrected with an explanation of the xref-analysis gap, Open Question 11
addendum, new 16th-session section), `notes/diode-matrix-history.md` (this entry). Ghidra database: 3
renames + 4 plate/EOL comments, listed above, all independently re-verified by direct decompile/memory
reads rather than accepted from the tracing subagent's report alone. No new ARM/Thumb disassembly gaps
queued. No git commit made, per the user's standing instruction.

## 17th session — D417 ruled out as the 70 MHz/4m gate: it's purely `region_code`-driven, confirmed with real ROM band-table bytes

Straight follow-up question after D420's resolution: D417 is populated only on `EUR`/`ITR`/`ESP`
(`[#03][#05][#06]` per the parts list), exactly Icom's official "HF/50/70 MHz" (4m-unlocked) country group
— first caught and fixed a stale error in this file's own living table, which had misread `#06` as `KOR`
instead of `ESP`. Does D417 gate 70 MHz directly, the way D420 gates 4630kHz, or is 70 MHz access just a
consequence of the already-resolved `region_code`?

Delegated the trace to a subagent with the fresh D420-session lesson built into the brief (Ghidra's
`references_to` can miss a real diode-bit read — don't trust it alone), then independently re-verified the
central claim by hand: read the raw bytes of `tx_band_table_ptrs_by_region_code` (`0x20198a20`, an 8-entry
pointer array indexed directly by `region_code`) and every table it points to for region codes 0, 2, 3, and
4, and manually decoded each `{min_hz, max_hz}` row (little-endian u32 pairs, `0xffffffff` sentinel) by
hand rather than trusting any tool's summary.

**Confirmed by direct byte decode**: region 0's table (`0x20198ad0`, region 1 shares it — its data starts
immediately after region 0's own sentinel) runs 1.8-2.0/3.5-4.0/5.255-5.405/7.0-7.3/10.1-10.15/14.0-14.35/
18.068-18.168/21.0-21.45/24.89-24.99/28.0-29.7/50-54 MHz then sentinel — **no 70 MHz row at all**. Regions
2 (`0x20198b88`) and 3 (`0x20198bec`) both carry the identical band plan except a narrower 160m/80m/40m/6m
(1.81 or 1.83-2.0/3.5-3.8/7.0-7.2/50-52) **plus a `70.000-70.500 MHz` row** right before their sentinel.
Region 4 (`0x20198c50`) matches 2/3's narrowed HF/6m shape but gets a **visibly different, narrower**
70 MHz slice: `70.150-70.250 MHz`. Regions 5/6 (spot-checked structurally, not fully hand-decoded this
session) are the already-known heavily channelized/restrictive tables — no 70 MHz row.

**Exactly 3 region codes (2, 3, 4) carry a 70 MHz row — matching the exactly-3-country 70MHz-unlocked group
from the parts list, one-to-one.** `FUN_2003be94` (TX)/`FUN_2003bd80` (RX) select these tables purely by
indexing `tx_band_table_ptrs_by_region_code`/`rx_band_table_ptrs_by_region_code` with `region_code` (itself
computed only from D404/407/410/413, confirmed no D417 involvement anywhere in that formula); the
functions' other diode-bit inputs (D401/D416/D402/D405) only adjust clamping/merging of already-selected
rows. **No bit-13 (`0x2000`, D417) test exists anywhere in this selection chain** — checked by full
decompile plus a targeted raw-listing sweep of the neighborhood where D420's gate turned up, not by
`references_to` alone.

### Net assessment

**It's `region_code`, not D417.** `region_code` alone fully determines 70 MHz access; D417 plays no role
anywhere in this mechanism. The D417-population/70MHz-country correlation looks coincidental in the causal
sense — Icom populates D417 on the same 3 country PCBs that get region codes 2/3/4, but nothing found so
far shows D417 itself driving any software behavior. This also **corrects** this file's own previously-
derived, never-verified `EUR`=2/`ITR`=4/`ESP`=5 guess: region 5's table is the heavily-restricted one
grouped with region 6, not CEPT-shaped, so `ESP` can't be region 5 — the real 70MHz-unlocked set is
`{2,3,4}` as a set, matching `{EUR,ITR,ESP}` as a set, with region 4's distinctly narrower 70MHz slice
(and tighter 1.81-1.85 MHz 160m allocation) as a real fingerprint difference between the three, still not
pinned to a specific country name.

D417 itself remains genuinely unconfirmed — this session narrows what it *isn't* (definitively not the
4m-band gate, checked with real ROM data rather than another `references_to` negative) without finding
what it *is*. New leads for a future session: the raw-listing bit-13 sweep only covered the
`0x2003e0xx`-`0x2003ecxx` neighborhood and the already-known diode/region-code function list, not the full
~110KB of the still-corrupted `settings_list_builder` auto-detected function range
(`0x2003e5f0`-`0x2005ea37`); a non-frequency EUR/ITR/ESP-specific behavior (compliance string, duty-cycle/
power table, regulatory label) is a more promising direction than more band-edge searching, since
`region_code` already fully explains the band-access angle; live JTAG toggling of bit 13 remains the most
direct fallback if static analysis keeps coming up empty.

### Renames and comments (Ghidra database)

- `DAT_2003c82c`/`0x20198a20` and its RX counterpart `0x20198a00` → `tx_band_table_ptrs_by_region_code` /
  `rx_band_table_ptrs_by_region_code`, with plate comments documenting the per-region-code table layout.
- Per-region table addresses labeled: `0x20198ad0` (region 0/1, no 70MHz), `0x20198b88` (region 2, full
  70MHz), `0x20198bec` (region 3, full 70MHz), `0x20198c50` (region 4, narrow 70MHz), `0x20198cb4`/
  `0x20198d20` (regions 5/6, no 70MHz, channelized).
- Plate comment on `FUN_2003be94` documenting the region-code-only selection logic and the confirmed
  absence of any D417/bit-13 test.

**Files touched this session**: `notes/diode-matrix.md` (D417 living-reference row, Open Question 1
correction, the `#06`=ESP not KOR fix in two places, new 17th-session section), `notes/diode-matrix-history.md`
(this entry). Ghidra database: 2 renames + 6 plate/label comments, listed above, all independently
re-verified this session by direct memory reads and hand-decoded table bytes, not accepted from the tracing
subagent's report alone. No new ARM/Thumb disassembly gaps queued. Committed (`81e2eb2`).

## 18th session — new consolidated `notes/band-plans.md`; region_code-to-country mapping finally resolved; region 5/6 identified as TPE/KOR

User asked for a single file gathering every region's band plan/channel list, and whether region 5/6 could
be identified now that their tables were on the table (so to speak) from the 17th session. Did all the
remaining hand-decoding directly this session (no subagent delegation needed — this was arithmetic and
memory reads, not a broad search) and it resolved two of this file's oldest open questions as a side
effect.

### The region_code mapping bug, found and fixed

`diode_region_code_lookup` (`FUN_2003bca8`) computes a raw 4-bit weighted index
(`8*D404+4*D407+2*D410+1*D413`) and looks it up in a real table — its own decompile shows
`*(byte*)(DAT_2003c7fc + index)` plainly. The "derived arithmetic hypothesis" that had sat unresolved in
this file since an early session was computing that raw index and calling it `region_code` directly,
**skipping the lookup step entirely** — that's the whole source of the years-old "USA=0 and EXP=9 fall
outside the valid 1-7 range" mystery (Open Questions 1 and 2). Found the real table this session: read
`DAT_2003c7fc`'s own stored value directly (it's a pointer variable, not the table itself) — it holds
`0x20198a40`, sitting right after the already-known TX band-table pointer array. Read that address
directly: `00 01 02 00 03 04 05 06 00 07 00 00 00 00 00 00` — index→region_code
`[0,1,2,0,3,4,5,6,0,7,0,0,0,0,0,0]`, exactly matching what this file's own "Living reference: region code"
section had already documented structurally (just never connected to the country-mapping question).

Redone through the *real* lookup, cross-referencing each of the 8 named variants' own D404/D407/D410/D413
population tags (already sitting in this file's own parts-list table, no new data needed) against this
table:

```
USA(#02): none            -> index 0 -> region_code 0
JAP(#01): none (no tag)   -> index 0 -> region_code 0  (shares USA's table)
EUR(#03): D410            -> index 2 -> region_code 2
ITR(#05): D407            -> index 4 -> region_code 3
ESP(#06): D407,D413       -> index 5 -> region_code 4
TPE(#07): D407,D410       -> index 6 -> region_code 5
KOR(#08): D407,D410,D413  -> index 7 -> region_code 6
EXP(#12): D404,D413       -> index 9 -> region_code 7
```

**Every one of the 8 documented variants resolves to a clean, valid, unique region_code 0-7 — the
mystery evaporates once the lookup step is included.** Each variant's diode-presence tuple is distinct from
every other's, so this isn't a coincidental fit — it's the only self-consistent assignment. Only
`region_code` 1 is unclaimed by any of the 8 — genuinely open (an undocumented 9th variant, given gaps in
Icom's public numbering at `#04`/`#09`/`#10`/`#11`, or simply unused).

This also directly answers the 17th session's leftover "which of {2,3,4} is which of {EUR,ITR,ESP}"
question: `EUR`=2, `ITR`=3, `ESP`=4 (not yet cross-checked against real national band plans, but no longer
an open *derivation* question — just an unconfirmed final check).

### Region 5 = TPE, region 6 = KOR, with their band tables fully hand-decoded

Same derivation identifies region 5 (TPE, the only variant with D407+D410 but not D413) and region 6 (KOR,
the only variant with all of D407/D410/D413). Read both tables directly, byte by byte (the 17th session had
only structurally spot-checked their shape via the tracing subagent, not fully decoded them):

- **Region 5 (TPE)**, `0x20198cb4`: 160m 1.800-1.900, 80m split into two ~12.5kHz slices
  (3.500-3.5125 & 3.550-3.5625), 40m 7.000-7.100 (pre-D402-clamp), 30m trimmed to 10.130-10.150 (its top
  20kHz only), 6m split into two ~12.5kHz slices (50.000-50.0125 & 50.110-50.1225) — the more heavily
  channelized of the two, restricting almost every band.
- **Region 6 (KOR)**, `0x20198d20`: 160m trimmed to a 25kHz sliver (1.800-1.825), 80m in two segments
  (3.500-3.550 & 3.790-3.800), 40m 7.000-7.200 (pre-clamp) — but 30m/20m/17m/15m/12m/10m/6m are all
  completely unrestricted, a clearly different and less-restrictive shape than region 5's, a solid
  independent fingerprint distinguishing the two beyond just the diode-presence derivation.

Confidence is high on the derivation (clean, exhaustive, no ambiguity); not yet independently cross-checked
against real Taiwanese/Korean national band plans, flagged as the natural next step rather than treated as
fully closed.

### New file: `notes/band-plans.md`

Consolidates: the 8-variant table (version #, country, manual's stated band access); the region_code
derivation above with full reasoning; a single table with every region's complete band-by-band frequencies
(all 8 regions, hand-decoded and cross-checked across this session and the 17th); the general-coverage RX
table; and a summary of the diode overlays that turn the raw tables into as-shipped reality
(D401/D402/D405/D416/D419/D422), cross-referenced from `notes/diode-matrix.md` rather than re-derived. Also
notes, from already-known facts newly read together: D419 and D422 are **both present on every real unit**
(both "all versions, no tag" in the parts list), so neither one's documented "continuous coverage" TX
override (0.1-74.8 MHz or 1.6-54 MHz) ever fires on stock hardware — very likely the real mechanism behind
the well-known "open TX" hardware mod (physically removing D422 to leave only D419 present).

### Renames and comments (Ghidra database)

- `DAT_2003c7fc`'s target, `0x20198a40` → `region_code_lookup_table`, with a plate comment giving the full
  derivation table above.
- `0x20198ad0` → `tx_band_table_region0_usa_jap`, `0x20198b2c` → `tx_band_table_region1_unclaimed`,
  `0x20198c50` → `tx_band_table_region4_esp_narrow_70mhz`, `0x20198cb4` →
  `rxtx_band_table_region5_tpe_channelized`, `0x20198d20` → `rxtx_band_table_region6_kor`, `0x20198d84` →
  `tx_band_table_region7_exp`.

**Files touched this session**: `notes/band-plans.md` (new), `notes/diode-matrix.md` (Open Questions 1/2
resolved, new 18th-session section), `notes/diode-matrix-history.md` (this entry). Ghidra database: 6
renames + 1 plate comment, listed above, all backed by direct memory reads performed this session, not
taken from any subagent report (no subagent was used this session). No new ARM/Thumb disassembly gaps
queued. Committed (`fd5113a`).

## 19th session — externally verified ITR/ESP/KOR against real national band plans; caught an unsourced "ITR=Italy" claim

Two sharp follow-up questions from the user: what's the actual source for "`ITR`=Italy," and why does the
EUR/ITR difference show up only in the 160m edge rather than somewhere else? Worth taking seriously rather
than defending the prior write-up — went back to the primary source and then checked externally.

**The manual itself never says "Italy."** Re-extracted the real service-manual PDF page directly
(`pdftotext -layout` on `IC-7300_Servicio.pdf`'s actual `MODEL/VERSION/VERSION NUMBER/OPERATABLE BANDS`
table) rather than trusting the prior session's paraphrase — confirmed the table prints only the bare
3-letter codes (`USA`/`EUR`/`ITR`/`ESP`/`TPE`/`KOR`/`EXP`), never a spelled-out country name anywhere. The
"(Italy)"/"(Spain)"/"(Taiwan)"/"(Korea)" parentheticals this file has carried since an early session were
always this project's own inference (`ESP`→Spain and `KOR`→Korea are essentially unambiguous; `ITR`→Italy
was the shakiest of the four, never independently confirmed until now). Corrected `notes/band-plans.md`'s
variant table to flag this plainly instead of stating it as manual-sourced fact.

**Then checked it — and it holds up, with the 160m difference being exactly the right explanation.** Web
search against real national amateur-radio band plans, three exact matches:

- **Spain (region 4)**: real allocation extended to exactly `70.150-70.250 MHz` on 27 October 2017 (ARRL
  news) — an exact match to region 4's ROM table, not just "narrower."
- **Italy (region 3)**: real 160m allocation starts at `1.830 MHz`, not the general European/CEPT
  `1.810 MHz` edge — an exact match to region 3's 160m row. Directly answers the user's second question:
  re-checked region 2 vs region 3's full ROM tables byte-by-byte (already done in the 17th/18th sessions,
  re-confirmed here), and the 160m edge is the *only* difference between them — every other band is
  byte-identical. That's not a weirdly narrow way to distinguish EUR from Italy; it's the actual, correct,
  single real-world distinguishing feature between the general-European and Italian national allocations
  on this specific band. `region_code` 2 = EUR follows by elimination, no longer just a derivation.
- **Korea (region 6)**: real band plan (Korea Amateur Radio League, `karl.or.kr`) gives 160m
  `1.800-1.825 MHz` and 80m `3.500-3.550 kHz` — both exact matches to region 6's table.

**Taiwan (region 5) remains unconfirmed** — no citable Taiwanese national allocation table turned up in
search to check its channelized 80m/30m/6m slices against. The diode-presence derivation itself stays
exhaustive and unambiguous either way (TPE is the only variant with D407+D410 but not D413).

These are `WebSearch` results, not primary regulatory PDFs pulled and read directly — flagged in the notes
as strong corroborating evidence (three independent exact numeric matches), not the same evidentiary tier
as the ROM reads or the service-manual PDF text itself.

**Files touched this session**: `notes/band-plans.md` (country-name sourcing caveat, new "External
verification" section, Open Questions 2/3 updated), `notes/diode-matrix.md` (Open Question 1 addendum),
`notes/diode-matrix-history.md` (this entry). No Ghidra database changes this session (no new RE, just
external verification of an existing finding). No new ARM/Thumb disassembly gaps queued. No git commit made
yet this session.
