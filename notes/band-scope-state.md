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

## Methodological note

This is the **second** substantial finding in one session that was invisible until Ghidra's
memory map was extended to the RZ/A1H's real, datasheet-confirmed RAM extent (see
[[firmware-update]]'s `"Fup_AutoEnd_3765"` section for the first). Unlike that one — a marker at
a fixed *edge* of RAM — this is a large, ordinary heap/static-data structure that just happens to
sit past where the plain `body.bin` image ends. Worth treating memory-map extension as a
standing, low-cost technique for this project: anything the running firmware allocates beyond its
own static image size was, until now, structurally invisible to every tool used here.
