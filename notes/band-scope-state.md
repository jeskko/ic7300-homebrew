# Band-scope display state — a newly-found shared "live radio state" structure

Found 2026-08-29, opportunistically, after extending Ghidra's memory map to the RZ/A1H's full
documented 10 MB on-chip RAM extent (`0x20000000`-`0x209fffff`, see [[memory-map]]) — the same
technique that surfaced the `"Fup_AutoEnd_3765"` restart marker in [[firmware-update]]. The user
noticed a dense cluster of cross-references had appeared around `0x2040454d`-`0x204046a3`
(inside the newly-mapped range, previously invisible to Ghidra) and asked for an opportunistic
look. This is that look.

See [band-scope-state-history.md](band-scope-state-history.md) for the full session-by-session
narrative — how each RAM pocket below was located (the memory-map extension, the raw-literal-scan
technique) and characterized. This file carries the confirmed structure and the current pocket map.

## What's confirmed

**A large, shared global structure, `g_radio_ui_state_base` (`0x2040376c`)**, referenced by
multiple, otherwise-unrelated functions using large fixed offsets (`+0xe04`, `+0xe18`, `+0xe1c`,
`+0xe44`, `+0xe45`, ...) — confirmed by reading the actual pointer *values* of several
different literal-pool "DAT_" globals (`DAT_200382b4`, `DAT_200a6d80`) and finding they all
resolve to the exact same address, `0x2040376c`. This is clearly one large "live radio/UI state"
struct, most likely holding VFO frequency/mode/display state for many different screens — of
which only a small, scope-specific sub-region has been examined so far.

**The specific sub-region the user flagged (`+0xe04` onward, i.e. `0x20404570`-ish) is
spectrum/band-scope display state**:

| Offset from `g_radio_ui_state_base` | Absolute address | Renamed to | Contents |
|---|---|---|---|
| `+0xe04` | `0x20404570` | `g_scope_state_mode` | A mode/state byte, values 0-4, selected by several boolean checks in `scope_state_recompute` |
| `+0xe18` | `0x20404584` | `g_scope_freq_low` | Computed low frequency bound = `center - halfspan` |
| `+0xe1c` | `0x20404588` | `g_scope_freq_high` | Computed high frequency bound = `center + halfspan` |

**`scope_state_recompute`** (renamed from `FUN_20038f94`, real function start `0x20038e0c`):
reads live radio state from a separate, not-yet-identified source struct (`DAT_20038304`,
resolves to `0x203fa170` — not yet examined) and derives the scope display state from it:
- `g_scope_state_mode` = 0/1/2/3/4, chosen via a small decision tree checking three flag bytes
  from the source struct (`+0xc`/`+0xd`/`+0xe` relative to it).
- `g_scope_freq_low`/`g_scope_freq_high` = `center ∓ halfspan`, where `center` comes from source
  `+0x1c` (with `0x7fffffff` treated as a "no override" sentinel, reset to 0) and `halfspan` from
  source `+0x10`.
- A span-class byte at `g_scope_state_mode+0x22` is chosen via a `< 50000` threshold check on the
  same half-span value — plausibly a 50 kHz span-mode boundary, consistent with the IC-7300's
  real, user-facing selectable scope span feature.
- Several other fields (`+0x20`/`+0x21`/`+0x24`/`+0x28`/`+0x2c`/`+0x2d`/`+0x30`/`+0x31`/`+0x34`)
  copied or derived from the same source struct — not yet individually identified.

**`scope_freq_to_position`** (renamed from `FUN_200a6a54`): the read/consumer side. Bounds-checks
a candidate frequency against `g_scope_freq_low`/`g_scope_freq_high` (returning `-1`/`-2` if out
of range), with a special-case ±1 nudge when `g_scope_state_mode` is `3` or `4` (the two "wide
span" states `scope_state_recompute` selects via the `50000` threshold) and the candidate lands
exactly on a boundary. On success, calls `FUN_20056e98` — a binary-search-shaped lookup (uses a
midpoint computation, returns `0xfffe`/`0xffff` sentinels on failure) — and returns its result.
**Working read: this looks like the frequency→screen-position mapping function for the band-scope
display**, not yet cross-checked against the actual on-screen scope-drawing code.

**Many other functions reference this same region**, mostly the same kind of generic per-screen
UI "candidate vs. current settings" comparator this project has already identified twice before
(`FUN_2008cff8` in [[multi-cpu-images]], and a very similarly-shaped one at `FUN_2008f75c` found
this session, both dispatching on a screen-type index) — consistent with the band-scope
configuration screen(s) being implemented on the same generic settings-comparison framework as
everything else in this firmware's UI, not a bespoke one-off.

## What's genuinely open

- **`DAT_20038304`'s target struct (`0x203fa170`)** — the actual live-radio-state source
  (VFO frequency, span setting, etc.) that `scope_state_recompute` reads from. Not examined at
  all yet; a natural next step, and probably where the *user-facing* span setting (steps like
  ±5/10/20/50/100 kHz) actually lives before being turned into the derived state documented here.
- **The rest of `g_radio_ui_state_base`'s ~3.7 KB span** (only `+0xe04`-`+0xe45` examined so far)
  — very likely holds equivalent live-state structures for other display features (main VFO
  readout, sub-receiver, memory channels, etc.), following the same pattern. Worth a systematic
  sweep if this thread continues, rather than assuming the scope is the only thing here.
- **`FUN_20056e98`** (the binary-search-shaped position lookup `scope_freq_to_position` calls) —
  not decompiled yet; would confirm or refute the "frequency→pixel" reading directly.
- Exact meaning of `g_scope_state_mode`'s 5 values (0-4) and the span-class byte at `+0x22` —
  plausible guesses (span-width categories) given the `50000`-threshold check, not confirmed
  against the real on-screen span options.

**Related finding, different thread (2026-08-30)**: [[kernel-rtos]]'s `spectrum_scope_fft_task`
(renamed from `status_poll_task_200095d8`) is a real, confirmed 512-point FFT computing dB-scaled
per-bin magnitude bytes — very likely the scope's actual "bar height" data, complementing this
file's frequency-axis/"bar position" state. Its own per-mode threshold table lives at a different
base (`0x203de174`) than `g_radio_ui_state_base` (`0x2040376c`) — confirmed not the same struct —
so the two subsystems' exact relationship (shared producer/trigger?) is still open. See
`notes/kernel-rtos-history.md`'s "the band-scope's real-time FFT engine" section for the full
derivation.
## Current map of confirmed RAM pockets

`g_radio_ui_state_base` is a big, sparse struct — most of it holds no static cross-references
at all. The dense pockets found by the sweeps (see the history file for how each was located and
characterized) are, most to least referenced:

| Address / range | Refs | Status |
|---|---|---|
| `0x20403f6c`-ish (settings-menu flags) | 44 | Characterized — `settings_menu_item_data_builder`, a 13-case (`0`-`0xc`) per-menu-item data builder; case 4 ≈ clock/date screen |
| `0x20404570`-ish (scope state) | 20 | Characterized — band-scope frequency-axis state (`g_scope_state_mode`/`g_scope_freq_low`/`g_scope_freq_high`, see "What's confirmed" above) |
| `0x203ff76c` | 52 (once mapped) | **Resolved** — a generic shared "settings-candidate" scratch buffer reused per screen, not one struct; retires a years-old dead end from [[multi-cpu-images]] |
| `0x203fc57a`-`0x203fc692` | 87 (raw scan) | Likely several unrelated small buffers packed in a pointer table, not one struct — not characterized |
| `0x203fa170` (`DAT_20038304`) | 6-16 | Characterized — the scope's own "live radio state" source struct that `scope_state_recompute` reads |
| `0x203fca1e`-`0x203fcb6a` / `0x203fccbc`-`0x203fcd8e` | 13, 9 | **Resolved** — `sdcard_date_key_lookup` + 3 helpers: builds today's `"\20YYMMDD"` date-coded SD filename from a live RTC shadow (`g_rtc_shadow`, `0x2039027c`, `RX-8803LC`) and binary-searches a table of such date-codes. Open: who calls it (which SD feature) |
| `0x203fabec`-`0x203fac9c` / `0x203fab00`-`0x203fab88` | 19, 14 | Working hypothesis — `FUN_20058c8c`, a 5-field text-entry validator (memory-channel naming / callsign guess, unconfirmed) |
| `0x203fc000` | 6 | Located — `FUN_2005fac4` clears 2 flag bytes (`+0x240`/`+0x241`) of a struct at `0x203fbdc0` (`DAT_20060700`); not deeply characterized |
| `0x20403fec` | 3 | **Resolved as a non-issue** — just another field of the settings-menu-item struct (`DAT_2003a538` = `0x20403f66`, 6 bytes from the menu flags) |
| `0x2040466c` | 3 | Unresolved — most likely a data-dependent computed table-index inside `FUN_200a94c8`, not a fixed struct; low priority |

The real "hot window" is a ~80 KB stretch, roughly `0x203f0000`-`0x20404770` (very plausibly the
firmware's application-level BSS), bounded on both sides by memory confirmed empty of static
cross-references — see the history file's broad-sweep sections. The remaining uncharacterized
pockets above, plus ~40 smaller 1-4-ref clusters, are the lowest-priority remaining leads; the
pattern so far is that most turn out to be more instances of the same generic per-screen
"candidate vs. current settings" comparator framework.

## Methodological note

This is the **second** substantial finding in one session that was invisible until Ghidra's
memory map was extended to the RZ/A1H's real, datasheet-confirmed RAM extent (see
[[firmware-update]]'s `"Fup_AutoEnd_3765"` section for the first). Unlike that one — a marker at
a fixed *edge* of RAM — this is a large, ordinary heap/static-data structure that just happens to
sit past where the plain `body.bin` image ends. Worth treating memory-map extension as a
standing, low-cost technique for this project: anything the running firmware allocates beyond its
own static image size was, until now, structurally invisible to every tool used here.

**Correction/caveat, 2026-09-25**: this file's own "confirmed empty of static cross-references"
language (here and in the history file's broad-sweep sections) was later cited by `sdk/
app-loader-design.md` as evidence that RAM past `body.bin`'s static image was safe to place
appended custom code in — and that placement was live-tested and found **wrong**: the region gets
progressively overwritten by a runtime memory pool as boot proceeds (see `notes/kernel-rtos.md`'s
"`kernel_start`'s bring-up initializes a runtime memory pool" section). The "empty of xrefs"
finding itself isn't retracted — it's still true that no compile-time literal references this
range — but a zero-xref result over this class of region only rules out *statically-addressed*
content, never genuine runtime allocator content, which is invisible to `references_to` by
construction. Don't read "confirmed empty" in this file (or its history) as "confirmed safe to
write persistent data/code into" without a live marker-write-then-reboot check.
