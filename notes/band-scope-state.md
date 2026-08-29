# Band-scope display state — a newly-found shared "live radio state" structure

Found 2026-08-29, opportunistically, after extending Ghidra's memory map to the RZ/A1H's full
documented 10 MB on-chip RAM extent (`0x20000000`-`0x209fffff`, see [[memory-map]]) — the same
technique that surfaced the `"Fup_AutoEnd_3765"` restart marker in [[firmware-update]]. The user
noticed a dense cluster of cross-references had appeared around `0x2040454d`-`0x204046a3`
(inside the newly-mapped range, previously invisible to Ghidra) and asked for an opportunistic
look. This is that look.

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

## Swept the rest of `g_radio_ui_state_base` for other hotspots — found a much bigger subsystem (2026-08-29, same day)

Sampled xref density at regular offsets across `g_radio_ui_state_base`'s span (every ~0x40-0x100
bytes from `+0x0` to `+0x1200`) the same way the scope region was found. Almost everywhere came
back with zero references — the scope sub-region (`+0xe04`-`+0xe45`) is a real, dense pocket, not
typical of the whole struct. One more genuine hotspot found: **`+0x800`
(`0x20403f6c`), 44 references** — more than double the scope region, spanning a huge diversity of
unrelated-looking callers (`0x20037xxx`-`0x200a8xxx`).

**Traced to a general settings-menu framework, bigger in scope than the band-scope find**:
- **`g_settings_menu_active_flag`/`g_settings_menu_item_id`** (`+0x7a0`/`+0x7a1`, i.e.
  `0x20403f0c`/`0x20403f0d` — close to but distinct from `+0x800`, and from the band-scope
  sub-region) track which settings-menu item is currently active.
- **`settings_menu_item_data_builder`** (renamed from `FUN_20038d60`): a 13-case dispatcher
  (cases `0`-`0xc`) that builds per-menu-item working/display data into a shared buffer and
  records the active item via the two flags above. **Case `4` is very likely a clock/date
  screen** — decodes two 2-digit ASCII fields into a year (`digit - '0'` arithmetic, `+2000`) and
  calls a function that looks like a date-validity or day-of-week calculation, consistent with
  this radio's RTC (`IC351`, see [[ic7300-hardware]]). The other 11 cases (`0`,`1`,`2`,`3`,`5`-`c`)
  do substantially different, non-trivial work each and aren't yet individually characterized —
  this reads as roughly one case per distinct settings-menu screen (keyer/CW settings, memory
  channels, etc. are plausible guesses, not confirmed).
- The `0x20403f6c` hotspot's 44 references are mostly yet another instance of the generic
  per-screen "candidate vs. current settings" comparator framework already identified twice
  elsewhere in this project (see [[multi-cpu-images]] and this file's earlier section) — this
  time serving an unrelated menu screen with `×7`-indexed lookup-table reads (plausibly a table of
  fixed-width strings — mode names, day names, or similar; not confirmed).
- **Minor tooling caveat, not chased further**: Ghidra currently reports
  `settings_menu_item_data_builder`'s function bounds as `0x2001ab40`-`0x2004f54b` — an
  implausibly large ~210 KB single function, almost certainly a merged/mis-bounded function
  artifact (the same general class of issue as this project's known ARM/Thumb disassembly-context
  bug, though not confirmed to be that specific bug). Renaming still applied cleanly at the real
  entry point (`0x20038d60`); worth a GUI fix if this function is revisited in depth.

**Net assessment**: `g_radio_ui_state_base` is a big, sparse struct — most of it is empty of
static cross-references (either genuinely unused by any comparator-style code, or accessed only
through the generic frameworks in ways that don't produce simple literal-pool xrefs), with a
handful of real dense pockets scattered through it (the two found so far: band-scope state at
`+0xe04`, menu-item-tracking at `+0x7a0`-ish). Systematically walking the *rest* of the
`0x0`-`0x1200`+ span would very likely turn up more individual settings screens' state one at a
time, but each one looks like a similarly-sized side quest to the two found so far — worth
scoping deliberately (which specific radio feature to chase next) rather than continuing a blind
sweep.

## Methodological note

This is the **second** substantial finding in one session that was invisible until Ghidra's
memory map was extended to the RZ/A1H's real, datasheet-confirmed RAM extent (see
[[firmware-update]]'s `"Fup_AutoEnd_3765"` section for the first). Unlike that one — a marker at
a fixed *edge* of RAM — this is a large, ordinary heap/static-data structure that just happens to
sit past where the plain `body.bin` image ends. Worth treating memory-map extension as a
standing, low-cost technique for this project: anything the running firmware allocates beyond its
own static image size was, until now, structurally invisible to every tool used here.
