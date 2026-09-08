# IC-7300 regional band plans / channel lists — consolidated reference

Single place for the actual RX/TX frequency tables the firmware selects per hardware variant, and how the
diode matrix ([notes/diode-matrix.md](diode-matrix.md), the mechanism-level reference — read that first for
*how* any of this is computed) picks between them. Everything in the tables below is a direct, hand-decoded
byte dump from ROM (`body.bin`), independently cross-checked address-by-address — not inference. Frequencies
are in MHz unless noted; ROM stores them as plain 32-bit Hz integers, little-endian, in `{min, max}` pairs
per band, terminated by an `0xffffffff` sentinel.

## The 8 official Icom variants

Per the service manual's own `MODEL/VERSION/VERSION NUMBER/OPERATABLE BANDS` table (§ Introduction, page 1
— re-extracted directly from the source PDF this session, `pdftotext -layout` on
`IC-7300_Servicio.pdf`) plus the §5 Parts List diode-population tags — see `notes/diode-matrix.md`'s
"Official per-version population data" section for the parts-list sourcing.

**Important caveat on the country names**: the manual's own table prints **only the 3-letter codes**
(`USA`/`EUR`/`ITR`/`ESP`/`TPE`/`KOR`/`EXP`) — it never spells out full country names anywhere. The full
names below (Italy, Spain, Taiwan, Korea) are this project's own inference, not manual text. `ESP`=Spain
and `KOR`=Korea are essentially unambiguous (standard abbreviations). `TPE`=Taiwan (Taipei) is very likely
right but unconfirmed. `ITR`=Italy was the least certain of the set — **now strongly corroborated
externally, see "External verification" below**, but still technically an inference, not a manual-stated
fact.

| Version # | Country/market | Manual's stated band access |
|---|---|---|
| `#01` | JAP (Japan) | not in this manual's table (JP-specific diodes D420/D423 layer JP-only features separately — see below) |
| `#02` | USA | HF/50 MHz |
| `#03` | EUR | **HF/50/70 MHz** |
| `#05` | ITR (Italy — inferred, see caveat above) | **HF/50/70 MHz** |
| `#06` | ESP (Spain) | **HF/50/70 MHz** |
| `#07` | TPE (Taiwan — inferred) | HF/50 MHz |
| `#08` | KOR (Korea) | HF/50 MHz |
| `#12` | EXP (export/general coverage) | HF/50 MHz |

## The region_code mapping — fully resolved this session

`region_code` (0-7) is computed by `diode_region_code_lookup` (`FUN_2003bca8`/`FUN_2003c0ec`, entry
`0x2003bca8`) from 4 diodes only — **D404** (weight 8), **D407** (weight 4), **D410** (weight 2), **D413**
(weight 1), 1 = diode present:

```c
index = 8*D404 + 4*D407 + 2*D410 + 1*D413;           // 0-15
region_code = *(byte*)(DAT_2003c7fc_pointer + index); // table at 0x20198a40, confirmed by direct read
```

The lookup table at **`0x20198a40`** (confirmed by direct memory read this session: bytes
`00 01 02 00 03 04 05 06 00 07 00 00 00 00 00 00`):

| index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10-15 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| region_code | 0 | 1 | 2 | 0 | 3 | 4 | 5 | 6 | 0 | 7 | 0 |

**This resolves the two oldest open questions in `notes/diode-matrix.md`** (Open Questions 1 and 2, open
since early sessions). The old "derived region-code hypothesis" was computing the **raw 4-bit `index`**
(the weighted sum, 0-15) and mislabeling it as `region_code` directly, skipping the lookup-table step
entirely — that's why it produced out-of-range values (`USA`=0 valid but `EXP`=9 invalid) and never
reconciled. Redone correctly, cross-referencing each of the 8 variants' actual D404/D407/D410/D413
population tags (from `notes/diode-matrix.md`'s parts-list table) against this exact lookup, **every one of
the 8 named variants resolves cleanly to a valid `region_code` 0-7, with zero leftover wrinkle**:

| Variant | D404 | D407 | D410 | D413 | index | **region_code** |
|---|---|---|---|---|---|---|
| USA (#02) | – | – | – | – | 0 | **0** |
| JAP (#01) | – | – | – | – | 0 | **0** (shares USA's table/code — see note below) |
| EUR (#03) | – | – | ✓ | – | 2 | **2** |
| ITR (#05) | – | ✓ | – | – | 4 | **3** |
| ESP (#06) | – | ✓ | – | ✓ | 5 | **4** |
| TPE (#07) | – | ✓ | ✓ | – | 6 | **5** |
| KOR (#08) | – | ✓ | ✓ | ✓ | 7 | **6** |
| EXP (#12) | ✓ | – | – | ✓ | 9 | **7** |

(✓ = populated per the parts-list tags: D404 only-EXP; D407 on ITR/ESP/TPE/KOR; D410 on EUR/TPE/KOR; D413
on ESP/KOR/EXP — see `notes/diode-matrix.md`.) **`region_code` 1 is not claimed by any of the 8 documented
variants** — no combination of these 4 diodes' known population tags produces index 1 (D413-only, all
others absent). Either an undocumented 9th variant exists (Icom's public numbering goes to at least `#12`
with gaps at `#04`/`#09`/`#10`/`#11`), or region_code 1 is simply unused in practice. Open.

**JAP note**: Japan (`#01`) isn't in the manual's HF/50/70MHz table and has no D404/D407/D410/D413 tag at
all in the parts list, so by this formula it lands on `region_code` 0 — the same code/table as USA. That's
a real, structural finding, not a guess: Japan's actual distinguishing features (the 4630kHz Emergency mode,
factory-reset display-language default, etc.) are handled entirely by the separate JP-only diodes **D420**
and **D423** layered on top of this same base region_code=0 table, not by a unique region_code of their own
— consistent with everything found tracing D420/D423 in `notes/diode-matrix.md`.

## Per-region_code band tables (raw ROM, before diode overlays)

TX table selector: `tx_band_table_ptrs_by_region_code` (`0x20198a20`, 8 pointers, one per `region_code`).
RX table selector: `rx_band_table_ptrs_by_region_code` (`0x20198a00`). Selection logic: `FUN_2003be94`
(TX)/`FUN_2003bd80` (RX), confirmed to index purely by `region_code` — see `notes/diode-matrix.md`'s
17th-session section for the full trace. All values below hand-decoded directly from ROM bytes, independently
cross-checked (not taken from any single tool's summary).

| Band | region 0 (USA/JAP) | region 1 (unclaimed) | region 2 (EUR) | region 3 (ITR) | region 4 (ESP) | region 5 (**TPE**) | region 6 (**KOR**) | region 7 (EXP) |
|---|---|---|---|---|---|---|---|---|
| 160m | 1.800-2.000 | 1.800-2.000 | 1.810-2.000 | 1.830-2.000 | 1.810-1.850 | 1.800-1.900 | 1.800-1.825 | 1.810-2.000 |
| 80m | 3.500-4.000 | 3.500-4.000 | 3.500-3.800 | 3.500-3.800 | 3.500-3.800 | 3.500-3.5125 & 3.550-3.5625 | 3.500-3.550 & 3.790-3.800 | 3.500-4.000 |
| 60m | 5.255-5.405 | 5.255-5.405 | 5.255-5.405 | 5.255-5.405 | 5.255-5.405 | 5.255-5.405 | 5.255-5.405 | 5.255-5.405 |
| 40m | 7.000-7.300 | 7.000-7.300 | 7.000-7.200 | 7.000-7.200 | 7.000-7.200 | 7.000-7.100 | 7.000-7.200 | 7.000-7.300 |
| 30m | 10.100-10.150 | 10.100-10.150 | 10.100-10.150 | 10.100-10.150 | 10.100-10.150 | 10.130-10.150 | 10.100-10.150 | 10.100-10.150 |
| 20m | 14.000-14.350 | 14.000-14.350 | 14.000-14.350 | 14.000-14.350 | 14.000-14.350 | 14.000-14.350 | 14.000-14.350 | 14.000-14.350 |
| 17m | 18.068-18.168 | 18.068-18.168 | 18.068-18.168 | 18.068-18.168 | 18.068-18.168 | 18.068-18.168 | 18.068-18.168 | 18.068-18.168 |
| 15m | 21.000-21.450 | 21.000-21.450 | 21.000-21.450 | 21.000-21.450 | 21.000-21.450 | 21.000-21.450 | 21.000-21.450 | 21.000-21.450 |
| 12m | 24.890-24.990 | 24.890-24.990 | 24.890-24.990 | 24.890-24.990 | 24.890-24.990 | 24.890-24.990 | 24.890-24.990 | 24.890-24.990 |
| 10m | 28.000-29.700 | 28.000-29.700 | 28.000-29.700 | 28.000-29.700 | 28.000-29.700 | 28.000-29.700 | 28.000-29.700 | 28.000-29.700 |
| 6m | 50.000-54.000 | 50.000-54.000 | 50.000-52.000 | 50.000-52.000 | 50.000-52.000 | 50.000-50.0125 & 50.110-50.1225 | 50.000-54.000 | 50.000-54.000 |
| **4m (70MHz)** | none | none | **70.000-70.500** | **70.000-70.500** | **70.150-70.250** | none | none | none |

Table addresses (TX): region0 `0x20198ad0`, region1 `0x20198b2c` (byte-identical duplicate of region0's —
not a shared pointer, a separate copy), region2 `0x20198b88`, region3 `0x20198bec`, region4 `0x20198c50`,
region5 `0x20198cb4`, region6 `0x20198d20`, region7 `0x20198d84` (byte-identical to region0 except the
160m lower edge, 1.81 vs 1.80 — the one previously-known region7 quirk, reconfirmed this session). RX table
addresses are identical to TX for regions 5/6 only (`0x20198cb4`/`0x20198d20` again — same restrictive table
used for both directions); every other region's RX is the unrestricted general-coverage table below,
regardless of TX restrictions.

**General-coverage RX table** (`0x2019892c`, used by region_code 0/1/2/3/4/7's RX, and by 5/6's RX too once
D416 unlocks it — see overlays below): `0.030000-29.999999 MHz` + `30.000000-74.800000 MHz` (split into two
segments at the 30MHz boundary, presumably to avoid an exact-equality edge case — otherwise one continuous
0.03-74.8MHz range).

## Diode overlays that turn the raw tables above into as-shipped reality

The raw tables are only half the picture — several diodes adjust or override them at runtime. Full
derivations live in `notes/diode-matrix.md`; summarized here for a single reference:

| Diode | Real-hardware state | Effect |
|---|---|---|
| **D401** | present, all variants | Master switch for the whole region-restriction mechanism. If ever absent, every frequency becomes valid (no restriction at all) |
| **D402** | **absent**, all documented variants | With D402 absent (the real shipping state), any 40m upper edge strictly between 7.000000-7.300000 MHz gets clamped up to the full 7.300000 MHz — so **region 5 (TPE)'s raw 7.0-7.1 and region 6 (KOR)'s raw 7.0-7.2 both actually ship as 7.0-7.3 MHz**, same as everyone else. The raw-table 7.1/7.2 values above are the pre-clamp numbers, not what real units transmit |
| **D405** | present, all documented variants | Gates a specific ~5.255 MHz point in the RX range table (`FUN_2003bd80`) — a narrow RX-side edge-inclusion nuance, not a whole-band exclusion (60m 5.255-5.405 MHz TX is confirmed present in every region's raw table above). Exact real-hardware effect on the single 5.255 MHz point not fully pinned down; see `notes/diode-matrix.md` |
| **D416** | present, all documented variants | With D416 present, RX for region_code 5/6 (TPE/KOR) switches from their own restrictive/channelized table to the same unrestricted 0.03-74.8 MHz general-coverage table everyone else gets — **so real TPE/KOR units get full general-coverage RX despite their narrower, channelized TX allocations shown above** |
| **D419** / **D422** | **both present**, all documented variants | `FUN_2003bd34` selects a *third*, separate "continuous coverage" TX range depending on which of these two is present: D419-only → continuous TX 0.1-74.8 MHz; D422-only → continuous TX 1.6-54 MHz. **On real hardware both are always present together**, so neither override fires and the normal per-region_code table above applies — this pair is very likely the mechanism behind the well-known "open TX" hardware mod (physically removing D422 to leave only D419, unlocking continuous 0.1-74.8 MHz TX) |
| **D420** | JP-only (`#01`) | Gates visibility of the JP-exclusive "4630kHz" Emergency-mode checkbox (forces CW-only TX on 4630 kHz) — checked directly (20th session) against the band-edge/table-selection chain below: no test of this diode exists anywhere in it |
| **D423** | JP-only (`#01`) | Master gatekeeper for a large set of region-conditional menu items/features (`is_feature_enabled_for_region`) plus the factory-reset display-language default — also checked directly (20th session) against the band-edge chain: no test of this diode there either |
| **D417** | EUR/ITR/ESP only (`#03`/`#05`/`#06`) | **Checked directly this session and ruled out as a band-table gate of any kind** (see below) — its actual function is still unknown |

## Region 5/6 identity: TPE (Taiwan) and KOR (Korea)

Derived purely from the diode-presence intersection above (region_code 5 = TPE, region_code 6 = KOR — see
the mapping table), independently supported by the two tables' real content being clearly distinguishable
and each internally consistent:

- **Region 5 (TPE)** is the more restrictive of the two: narrow ~12.5 kHz slices on 80m (two of them) and
  6m (two of them), and 30m trimmed to just its top 20 kHz (10.130-10.150 instead of 10.100-10.150). This
  is a "channelized" shape, not a simple truncated continuous band — the kind of allocation pattern seen
  where amateur use shares spectrum with other licensed services in narrow permitted windows.
- **Region 6 (KOR)** restricts only 160m/80m/40m (160m to a 25 kHz sliver, 80m to two segments — one 50 kHz,
  one 10 kHz — 40m to 7.0-7.2 pre-clamp) but leaves 30m, 6m, and everything above fully open — a
  meaningfully different, less-restrictive shape than region 5's.

**KOR externally confirmed** (see "External verification" below) — Korea's real, published amateur band
plan is 160m `1.800-1.825 MHz` and 80m `3.500-3.550 kHz`, an **exact** match to region 6's 160m row and to
the first of its two 80m segments. **TPE still unconfirmed** — web search turned up no specific, citable
Taiwanese national allocation table for 80m/40m/6m to check the channelized slices against; the diode-
presence derivation (TPE is the only variant with D407+D410 but not D413) is exhaustive and unambiguous, so
confidence stays high, but this one specific piece is not independently verified the way KOR/ITR/ESP now are.

## D417 and the 70 MHz question — ruled out, still unexplained

D417 (populated only on EUR/ITR/ESP — exactly the three 70MHz-unlocked variants) does **not** gate the
4m band, or any other band-table row found so far. `region_code` alone (computed only from D404/407/410/413,
with zero D417 involvement in that computation) fully determines the 70MHz row's presence and content —
confirmed directly against the ROM tables above. The D417/70MHz-country correlation looks coincidental:
Icom populates D417 on the same 3 PCBs that happen to get region_code 2/3/4, not because D417 itself drives
that behavior. D417's real function remains unknown — see `notes/diode-matrix.md`'s 17th session for the
full negative trace and open leads.

## Japan's real band plan exists in ROM — but D420/D423 don't gate it, and neither does anything else found so far (20th session)

Direct user question: real JARL (Japan) band plans are known to differ substantially from the US's —
Japan's 80m allocation in particular is a set of narrow, non-contiguous segments, not a simple continuous
band. Since **JAP and USA both resolve to `region_code` 0** (neither has any of D404/D407/D410/D413
populated — see the mapping table above), and share the literal same ROM table
(`tx_band_table_region0_usa_jap`), does populating the JP-only diodes **D420**/**D423** change the enforced
band edges at all?

**A real, deliberately-authored JP-specific band table does exist in ROM**, found this session by
searching directly for JARL's published segment edges as raw bytes: **`tx_band_table_jp_narrow_region0_override`**
(`0x20198940`, a separate table, not part of the region_code-indexed array):

| Band | This table (JP) | region0 (USA/JAP shared table) |
|---|---|---|
| 160m | 1.800-1.875 & 1.9075-1.9125 | 1.800-2.000 |
| 80m | 3.500-3.580, 3.599-3.612, 3.662-3.687, 3.702-3.716, 3.745-3.770, 3.791-3.805 (6 segments) | 3.500-4.000 |
| 60m | *(no entry at all)* | 5.255-5.405 |
| 40m | 7.000-7.200 | 7.000-7.300 |
| 30m–6m | identical to region0 | identical to region0 |

Cross-checked against JARL's own published band plan: 4 of 5 checked 80m segment edges match exactly
(`3.599-3.612`/`3.702-3.716`/`3.745-3.770`/`3.791-3.805`); one (`3.662-3.687` here vs. JARL's currently
published `3.680-3.687`) differs on the low edge only, possibly an older JARL revision or an Icom-internal
margin — not independently dated. The missing 60m band and the 7.0-7.2 MHz 40m cap both match Japan's real
allocations too. This is unambiguous: Icom's engineers did encode Japan's actual regulatory band plan
somewhere in this firmware image.

**But it isn't gated by D420 or D423.** Fully traced (direct decompile, independently re-verified in this
session, not taken from a subagent's word):

- The function that loads this table, `FUN_2003c20c`, branches purely on `is_region_code_zero()` (i.e.
  `region_code == 0`) — no test of D420 (`0x4000`) or D423 (`0x8000`) anywhere in it.
- `FUN_2003c0ec` — the function that builds the actual "live" TX/RX tables (`DAT_2003c83c`/`DAT_2003c838`)
  that real operating code is presumed to consult — uses only `region_code` plus D401/D402/D405/D416.
  Re-decompiled and re-checked directly: no D420/D423 test anywhere in it either.
- The table this session found gets copied not into the live table, but into a *separate* scratch buffer
  (`DAT_2003c840`, `0x203de24c`) whenever `region_code==0` — **for both USA and JAP alike**, since neither
  diode distinguishes them at this point. A raw whole-ROM hex search for that buffer's own address (not
  just Ghidra's `references_to`, given this project's prior experience with that missing real reads) found
  only 2 hits total: the buffer's own storage slot and one self-consistency check
  (`FUN_2003c27c`/`FUN_2003c320`, which re-validates that the buffer's segments still classify cleanly
  against a universal ham-band table and, if not, just reloads it — a self-healing/sanity mechanism, not an
  enforcement swap). **No real-time TX-permission consumer of this buffer was found.**

**Current best answer**: no — as far as every reachable piece of this specific mechanism goes, populating
D420 and/or D423 on top of a `region_code=0` configuration does **not** change the enforced TX/RX band
tables. The band-edge selection logic only ever looks at `region_code` (from D404/407/410/413) plus
D401/D402/D405/D416; USA and JAP get identical treatment there. Japan's real, correct band plan is
genuinely present in ROM, and is loaded into RAM under the right condition (`region_code==0`), but this
investigation could not find where — or whether — it actually reaches the real go/no-go transmit decision.
That's either a real design quirk (unlikely for a shipping product that must meet Japanese regulations) or,
more likely, **an open gap in this trace**: the actual VFO/TX-frequency-validation function itself (as
opposed to the table-selection and table-refresh functions checked this session) hasn't been identified and
checked for whether it reads `DAT_2003c840` under some condition not yet found, or for a D420/D423 test of
its own.

**Concrete next steps for a future session**: (1) find the real TX-permission/frequency-validation function
(what actually runs when the user tunes or presses PTT) and check what buffer *it* reads —
`DAT_2003c83c`/`DAT_2003c838`, `DAT_2003c840`, or something else entirely; (2) if it turns out to read
`DAT_2003c83c`/`DAT_2003c838` only, check whether `FUN_2003c0ec`'s build step is ever passed a different
region_code specifically for D420/D423-populated hardware (i.e. whether `region_code` itself, not just the
table lookup, could be conditionally altered upstream — not checked this session); (3) as a fallback, live
JTAG verification (populate D420/D423 on a `region_code=0` test board, watch whether `DAT_2003c83c`/`838`
or `DAT_2003c840` actually changes, or whether TX genuinely gets blocked on JARL-restricted frequencies).

## External verification (19th session)

Checked the diode-derived region_code↔country assignments against real, independently-published national
amateur band plans via web search. Three separate, exact matches found — strong, independent evidence the
identification is correct, not just a clean-looking coincidence of the diode arithmetic:

- **ESP (region 4) — confirmed.** Spain's real amateur 70 MHz allocation was extended to exactly
  **70.150-70.250 MHz** effective 27 October 2017 (ARRL/IARU reporting). This is an **exact** match to
  region 4's ROM table, down to the kHz — not just "narrower," the identical numbers.
- **ITR (region 3) — confirmed, and this directly answers the "why does EUR/ITR only differ in 160m"
  question below.** Italy's real amateur 160m allocation is `1.830-2.000 MHz`/`1.830-1.850 MHz` (per
  Italian regulatory sourcing found via search), not the general European/CEPT `1.810-2.000 MHz` edge —
  an **exact** match to region 3's 160m row, and the *only* thing that differs between region 2 and region
  3's ROM tables (every other band is byte-identical between them — re-checked directly, not assumed). So
  the 160m-only difference this project found is not a weirdly narrow way to distinguish EUR from ITR —
  it's the actual, correct, single real-world distinguishing feature between the general European and
  Italian national allocations on this particular band. `region_code` 2 = EUR follows by elimination.
- **KOR (region 6) — confirmed** on two independent bands: Korea's real published band plan (Korea Amateur
  Radio League, `karl.or.kr`) gives 160m `1.800-1.825 MHz` and 80m `3.500-3.550 kHz` — both an **exact**
  match to region 6's ROM table (160m row, and the first of its two 80m segments).
- **TPE (region 5)** — no specific, citable Taiwanese national allocation table found this session to check
  against; remains derived-but-unconfirmed (see above).

These are all `WebSearch` results, not primary regulatory documents pulled directly — treat as corroborating
evidence (three independent exact numeric matches is strong), not the same evidentiary tier as the ROM
reads or the service-manual PDF text itself. Worth pulling the actual Italian (ARI)/Korean (KARL)/Spanish
(URE) primary source documents directly if this ever needs to be airtight.

Sources: [ARRL: Spain Extends Amateur Privileges on 70 MHz](https://www.arrl.org/news/spain-extends-amateur-privileges-on-70-mhz), [Tourist Ham Radio in Italy — IW5EDI](https://www.iw5edi.com/ham-radio-reference/tourist-ham-radio-in-italy), [KARL Band Plan 1.8 MHz (160M)](https://myerseng.com/Publico/Korea-Band-Plan-2004.pdf), [Band Plan in Korea — karl.or.kr](https://www.karl.or.kr/bbs/board.php?bo_table=info&wr_id=15).

## Open questions

1. **Region_code 1's owner is unknown** — no documented variant's diode tags produce it. Undocumented 9th
   variant, or genuinely unused? The gap in Icom's public Version# numbering (`#04`/`#09`/`#10`/`#11` never
   seen) is suggestive but unconfirmed.
2. ~~Region 4 (ESP)'s exact band-plan fingerprint hasn't been cross-checked~~ — **resolved, 19th session**,
   see "External verification" above: exact match to Spain's real, current 70.150-70.250 MHz allocation.
   `region2`=EUR/`region3`=ITR is also now resolved the same way (Italy's real 1.830 MHz 160m edge is an
   exact match to region 3, and is the *only* difference between region2/region3's tables).
3. **Region 5 (TPE) identification** — solid by diode-presence derivation, region 6 (KOR) now externally
   confirmed on two bands (see above), but TPE itself still has no independent national-band-plan check —
   the one piece of this file's identifications still resting on derivation alone.
4. **D405's exact real-hardware effect** on the single ~5.255 MHz RX edge point is still a narrow,
   unresolved nuance (see `notes/diode-matrix.md`) — doesn't affect anything in this file's tables, which
   are all TX-side or the unrestricted general-coverage RX range, but worth closing for completeness.
