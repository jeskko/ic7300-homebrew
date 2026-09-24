# IC-7300 regional band plans / channel lists — session history

Full session-by-session narrative and evidence trail behind [notes/band-plans.md](band-plans.md), which
carries only the current-state summary (confirmed facts, the living region/band tables, and open
questions). Sections below are archived verbatim, in their original order. This thread runs alongside
[notes/diode-matrix.md](diode-matrix.md)'s own investigation and is, like it, currently resting (run to
ground as of 2026-09-24).

## Archived from band-plans.md on 2026-09-24

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

**Not gated by D420 or D423 at the table-selection layer** — re-confirmed directly:

- `FUN_2003c20c` (loads this table) branches purely on `is_region_code_zero()` — no D420/D423 test.
- `FUN_2003c0ec` (builds the "live" TX/RX tables) uses only `region_code` plus D401/D402/D405/D416 — no
  D420/D423 test either.
- The JP table loads into a *separate* scratch buffer (`DAT_2003c840`, `0x203de24c`), identically for USA
  and JAP, whenever `region_code==0`.

### 21st session — the missing link found: real enforcement runs through one master classifier, and a real (if not fully pinned down) consumer of the JP buffer exists

Traced this fully via direct decompile — no subagent this round. Two findings close most of the remaining
gap.

**Finding 1 — the diode/region_code mechanism DOES reach broad, real enforcement**, through a single
function used everywhere: **`classify_frequency_to_band`** (renamed from `FUN_20013218`, `0x20013218`).
This is called from dozens of sites across the whole firmware — the user-band-edge setter
(`FUN_2000e448`), a general frequency-validity checker (`FUN_20040508`, used by whatever calls it to
decide if a proposed frequency/range falls inside one clean, currently-valid ham band), the diode-matrix
self-checks, and many more (21 call sites found via `references_to`). Its default mode (`param_2==0`,
essentially every caller) delegates to `FUN_20013154(freq, DAT_200134ec)`, and `DAT_200134ec` resolves
(confirmed by direct memory read) to `0x203d9ff4` — **the exact same live TX-table buffer**
`FUN_2003c0ec` builds from `region_code`+D401/402/405/416. So `region_code` is not a dead end sitting only
in a display feature — it's the actual, sole basis for "is this frequency in a valid band" everywhere in
the firmware that calls this one classifier. This had not been confirmed before this session.

**Finding 2 — a real, named feature can classify against the JP-narrow-table buffer, though its trigger
condition isn't fully pinned down.** `classify_frequency_to_band`'s *other* mode (`param_2!=0`) walks
`DAT_200134e8` directly, which resolves to `0x203de24c` — `DAT_2003c840`'s target, the JP-table scratch
buffer. Found exactly one real caller using this mode: **`band_edge_beep_check_and_fire`** (renamed from
`FUN_20017830`, called with a hardcoded `param_2=1` at its call site `0x20017864`), itself called from a
per-tuning-tick VFO-frequency-change handler (`FUN_20017a78`). Traced its effect fully: when the
classification of the current frequency changes or goes invalid across two successive calls, it invokes
`FUN_2000a17c(6)`/`FUN_2000a17c(7)` — confirmed, by decompile, to be a **beep-pattern player** (indexes a
beep-sequence table and pushes tones into an audio queue). Two "Band Edge" strings exist in ROM
(`0x2035a6b1`/`0x2035cca8`), confirming this is Icom's real **"Band Edge Beep"** feature (a real IC-7300
menu setting that beeps when tuning crosses into/out of a valid band) — not a cosmetic guess, a named,
findable feature.

**What's still open**: `band_edge_beep_check_and_fire` only uses the JP-buffer mode when a flag byte at a
shared state-struct offset (`*(0x203de4cc + 0x22)`) is greater than 1 (0/1 use the live table; the check
is `1 < flag`). This session could not find what *writes* that flag — `0x203de4cc` is an extremely
widely-shared "current operating state" struct pointer with 50+ separate literal-pool copies scattered
across the firmware (a scale of fan-out this project hasn't hit before), too many to exhaustively check by
hand in one session. So it's confirmed that Band Edge Beep *can* consult the JP table, but not yet
confirmed *when* — whether that's tied to `region_code`/D420/D423 at all, or is simply a generic
user-selectable beep-mode setting (e.g. "beep only at the edge of the currently active band" vs. "beep at
every classified boundary crossing including the JP-narrow segments") unrelated to which hardware variant
is running.

**Best current answer to the original question**: the diode-matrix mechanism (`region_code` +
D401/402/405/416) is now confirmed to drive real, broad frequency validation via
`classify_frequency_to_band` — this is a genuinely new, load-bearing finding, not previously confirmed.
D420/D423 still do not appear anywhere in that specific chain. Japan's real band plan is real, ROM-resident,
loaded into RAM at boot, and *is* reachable by at least one genuine feature (Band Edge Beep) under a
condition not yet fully traced — a meaningfully stronger position than "no consumer found" from the 20th
session, though the exact trigger for real JP hardware remains the one open piece.

### 22nd session — closed: the factory-reset default itself is the trigger, and it applies to USA and JAP alike

Found the flag's writer directly, without needing the "50+ literal-pool copies" brute-force search — asked
a more specific question instead: "Band Edge Beep" is a real, named settings item, so it must have an entry
in the already-documented **326-item factory-reset defaults table** (base `0x20190ecc`, 64-byte-stride
records, the same table this file's diode-matrix companion already fully decoded for six diode/region-
conditional item codes). Searched ROM directly for a second "Band Edge" string fragment
(`"Band Edge Beep"`, found at `0x2035cca8` — a different, more complete string than the truncated one used
elsewhere) and got exactly one hit, landing precisely at record-offset `+0x28` (the confirmed name-field
offset for this table) of the record at `0x2019174c` (item code **`0x22`**, computed directly from
`(0x2019174c - 0x20190ecc) / 0x40 = 34 = 0x22`).

**This is an exact, already-known item code** — `notes/diode-matrix.md`'s "Factory reset / restore-defaults
mechanism" section had documented years ago (10th session) that item `0x22`'s factory-reset default is
force-set to `3` specifically when `region_code == 0`, without ever having identified *which* settings item
`0x22` actually was. It's Band Edge Beep.

Read the record directly and confirmed every piece:

- Offset `+0x00` (the live-value byte pointer): `0x203de4ee` — **exactly** `DAT_200183b8 + 0x22`
  (`0x203de4cc + 0x22`), the flag `band_edge_beep_check_and_fire` reads to pick its classification mode.
- Offset `+0x28` (name): `"Band Edge Beep"`.
- Offset `+0x3c` (option-string table, `0x2019074c`): 4 real option strings —
  `"OFF"` (0), `"ON (Default)"` (1), `"ON (User)"` (2), `"ON (User) & TX Limit"` (3).
- Re-decompiled `reset_apply_item_default` (`0x2003dcc0`) directly and confirmed the literal code:
  `if (item_code == 0x22) { *(byte*)puVar3 = 3; return; }` inside the `is_region_code_zero()` branch,
  where `puVar3` is read straight from this exact record's own live-value pointer field.

**Full, closed chain, entirely confirmed by decompile — no inference left**: on factory reset (and
presumably first-ever EEPROM initialization), Band Edge Beep's value gets forced to `3`
("ON (User) & TX Limit") whenever `region_code == 0`. Since mode `3 > 1`, `band_edge_beep_check_and_fire`
calls `classify_frequency_to_band(freq, 1)`, which classifies against `DAT_2003c840` — the buffer that
holds `tx_band_table_jp_narrow_region0_override` (Japan's real JARL-matching band plan) precisely when
`region_code == 0`. **Both conditions key off the identical `region_code == 0` test — the same bucket both
USA and JAP diode configurations land in.** So: by factory default, Band Edge Beep on a `region_code=0`
unit — USA *or* JAP alike, not distinguished — beeps according to Japan's narrow band segments, not the
plain contiguous USA-shaped table. This is a real, confirmed, slightly surprising consequence of the
firmware's design, not a guess: as with everything else traced in this whole investigation, **D420 and
D423 play no role anywhere in this chain** — the behavior is undifferentiated between USA and JAP hardware.
(The user can change the setting away from mode 3 via the normal settings UI like any other item; this is
a factory default, not a hard-coded permanent state.)

**This closes the JP-band-table mystery this file opened two sessions ago.** Japan's real band plan exists
in ROM, is loaded at boot, and has a real, fully-traced, confirmed consumer — a beep-mode default — that
happens to apply equally to USA-market hardware. No further open link remains in this specific chain; the
only genuinely open items left are the ones listed below (region 5 TPE's external verification, and
region_code 1's unclaimed variant).
