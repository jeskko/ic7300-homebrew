# UI menu / touchscreen system

Started 2026-09-07, prompted by wanting to know what the different on-screen menu buttons actually
trigger. First real look at this subsystem — a genuinely new thread, not a continuation of an existing
one. Two separate, real structures found; the second one's record layout was mis-aligned in an earlier
pass this same session and has now been corrected against the actual pointer variables the code reads
(see "Correction" below) — don't reuse the first-pass offsets, they were wrong.

## Living reference: what's confirmed so far

| Finding | Address(es) | Status |
|---|---|---|
| **Factory "FRONT CHECK MODE" screen** — a numbered list of 13 real physical front-panel buttons | title at `0x20197153`, list at `0x20196f00`+; selector `FUN_2003a540` (7 states, `0x2c`-`0x32`, from `FUN_20013070()`) | ✅ button names confirmed, screen-select mechanism traced; the actual GPIO/key-matrix scan code itself not yet chased |
| **Generic touchscreen "list menu" widget** — one reusable set of functions drives QUICK MENU, MEMORY MENU, and presumably every other list-style menu screen, operating on a global "current list" pointer pair | widget code at `0x2004f0e4`-`0x2004f7xx`; bound, in this static snapshot, to the table at `0x2018f0ec` (72-byte/`0x48` records) | 🟡 full selection/navigation flow traced end-to-end (see below); the actual **per-item unique action** the flow ultimately triggers sits in fields that are blank (zeroed) in the static image — likely runtime-populated, the same "can't go further without live hardware" signature this project has hit elsewhere |

## The "FRONT CHECK MODE" factory button list

Unchanged from the first pass — a raw string `"FRONT CHECK MODE"` at `0x20197153`, immediately preceded
by a clean numbered list of real physical front-panel button names (read directly from memory,
`0x20196f00`+):

```
1. TRANSMIT      2. TUNER         3. VOX/BK-IN     4. PBT-CLR
5. P.AMP/ATT     6. NOTCH         7. NB            8. NR
9. MENU          10. FUNCTION     11. M.SCOPE      12. QUICK
13. EXIT
```

A factory/service self-test screen (walks the operator through pressing each physical button in turn,
presumably checking the key matrix isn't stuck) — confirms button #9 is the physical `MENU` key and
#12 is `QUICK` (the dedicated Quick Menu key). `FUN_2003a540` is the screen-selector: it reads a state
value from `FUN_20013070()` (values seen: `0x2c`-`0x32`, 7 consecutive states) and switches between
several factory-mode screens, of which "FRONT CHECK MODE" (state `0x31`) is one — the other 6 states
are other factory screens, not yet identified. The key-matrix/GPIO scan logic itself isn't traced yet.

## The generic touchscreen list-menu widget

Found via a raw string search for `"QUICK MENU"` / `"MEMORY MENU"` (11 and 1 hits respectively, all in
`0x2018f1xx`-`0x2018f5xx`) — not a QUICK-MENU-specific table as first assumed, but the data this session
traced to a **generic, reusable list-widget** (the same code almost certainly drives every list-style
menu screen — MEMORY MENU's single string hit inside the same address range is exactly what you'd
expect if its own item list sits immediately after QUICK MENU's in the same table format).

**Correction, same session**: the first pass hand-decoded this table's record layout starting from
`0x2018f118` (where a `00000101` marker word first appears) and got self-contradictory results — one
field looked like a string pointer in the raw dump but the widget code plainly dereferences the same
field as a function pointer. Root cause: `0x2018f118` is **not** the real record 0 base. The widget's
own code (`FUN_2004f610` etc.) reads its list pointer from a fixed global, `DAT_2004f728`, which in
this static image currently holds `0x2018f0ec` — **44 bytes (`0x2c`) earlier** than the `00000101`
landmark. Re-decoding from the real base fixed every contradiction. Lesson worth keeping: don't infer a
struct's base address from where a recognizable bit-pattern *starts* — check what the code that reads
it actually uses as the base pointer first.

**Confirmed record layout** (72 bytes/`0x48`, base = `DAT_2004f728`'s current value, `0x2018f0ec` in
this snapshot):

| Record | Role | Key fields (all confirmed against real decompiled code, not just pattern-matched) |
|---|---|---|
| index 0 (`0x2018f0ec`) | unused/sentinel | entirely zero |
| index 1 (`0x2018f134`) | **the "QUICK MENU" screen container** | `+0x04` screen init/setup callback (`0x2004f5a0`); `+0x0c` = `FUN_2004f610` ("activate the currently-selected item"); `+0x10` = `FUN_2004f688` ("scan for up to 4 visible/enabled item positions" — matches the touchscreen's real 4-tiles-at-a-time Quick Menu layout); `+0x14` = this record's own "am I available" test callback; `+0x20` = pointer to a **runtime** current-focus-position byte (blank/unreadable statically); `+0x30` = screen title string `"QUICK MENU"`; `+0x44` = pointer to a small position→item-index lookup table (`0x2018f054`+, bytes `0x0d, 0x14, 0x16, 0x2e, ...` — i.e. absolute item indices `13, 20, 22, 46, ...`, confirming there are at least ~46+ total entries in this whole table, far more than the 3-4 sampled) |
| indices 2, 3, 4 (`0x2018f17c`, `0x2018f1c4`, `0x2018f20c`) | individual QUICK MENU items | `+0x0c`/`+0x10` same generic function pointers as the container (shared code, not per-item); `+0x14` = **this item's own** "am I available" callback — each is a real 4-byte Thumb thunk (`adds r0,rN,#0x7; b <shared target>`, `rN` and the target both incrementing per item) that tail-calls one shared routine, `FUN_2004f250`, which reads an enabled/checked-state bit — real code, not guessed; `+0x30`/`+0x34` = same `"QUICK MENU"`/`"VOICE TX RECORD"` string pointers as the container and each other — **still not understood why an item-specific-sounding name like "VOICE TX RECORD" is identical across 3 sequential item records**; either it's a stale/default compile-time value never meant to be read this way, or these 3 particular sampled indices are literally 3 sibling sub-options of one feature (e.g. on/off/some third state for the same Voice-TX-Record item) rather than 3 different features — not resolved |

**The full selection/activation flow, traced end-to-end through real decompiled code** (not guessed):
1. `FUN_2004f688` walks item positions 0..N, calling each one's `+0x14` availability thunk, and
   builds a list of up to 4 visible/enabled item indices (matching the touchscreen's real 4-tile Quick
   Menu row) into a small buffer.
2. `FUN_2004f610` resolves "which absolute item index is currently focused" via the container's
   `+0x44` lookup table indexed by a **runtime** focus-position byte (`+0x20`, blank in the static
   image — this is a live "which of the 4 visible tiles has the cursor" value, can't be read
   statically), then calls that item's own `+0x08` handler if present, and — if that returned 0 *and*
   the item's enabled-bit is set — calls `FUN_2004f0f0(item_index)`.
3. `FUN_2004f0f0` calls the *container's* `+0x18` callback if set (null/no-op in this snapshot for
   QUICK MENU), stores the target item index into a "pending selection" state field, and calls
   `FUN_2004f0e4`, which just marks the widget dirty for redraw.

**Where this dead-ends statically**: the fields that would carry each item's actual *unique action*
(the container's `+0x18` "on commit" callback, and items' own `+0x08` handler) are all **zero in this
static image** for every record sampled so far. This is the same signature as several other genuinely
runtime-populated structures already documented elsewhere in this project (`kernel_start`'s mystery
task descriptor, the multi-display attach bitmask) — plausibly filled in during screen construction at
runtime rather than being compile-time constants, which would mean the real per-item action dispatch
needs live hardware (JTAG) to observe, not more static reading. Not fully certain yet — worth checking
a few more of the ~46+ total item records first (only indices 1-4 have been read) in case a later one
in the table *does* carry a non-null value statically.

## More screens found by reading further into the table

Read records 10-14 (base still `0x2018f0ec`, same `0x48` stride) to see whether the pattern holds
further out, and it does — plus real new screen names surfaced:

- **Record 11**: a container with title `"REC/SET"` (`0x20359bec`) — very plausibly the Voice-TX-Record
  settings screen, which would also explain the earlier mystery of `"VOICE TX RECORD"` appearing
  identically across 3 sampled QUICK MENU items (records 2-4): the already-documented
  `voice_tx_memory_control_task`/`voice_tx_memory_stream_task` feature (`notes/kernel-rtos.md`) has
  multiple numbered message slots (`M1`/`M2`/`M3`) — a good working hypothesis is that those 3 records
  are "Voice TX Record slot 1/2/3" quick-menu shortcuts sharing one feature-category label, distinguished
  by the per-record `+0x44` lookup byte rather than by their title string. Not confirmed, but it fits
  cleanly and stops the repetition from being an unexplained oddity.
- **Record 12**: title `"MEMORY MENU"` (`0x20359c18`) — the second screen the original string search
  found, now correctly placed in the table structure (an item/container immediately after record 11,
  same table).
- **Records 13, 14**: two more containers, each with their **own distinct** init/activate function
  pointers (`0x2004f858`/`0x2004f864` and `0x2004f888`/`0x2004f898` respectively) — *not* reusing
  `FUN_2004f610`/`FUN_2004f5a0` the way the QUICK MENU container did. So the "generic widget" isn't
  fully uniform: some fields (the per-item availability-test convention, the record shape/stride) are
  shared across every screen, but a screen's own activate/init callback can be screen-specific.
  Record 13's subtitle field reads `"Meter Type"` (`0x20359c98`) — a real IC-7300 multi-function-meter
  setting — and record 14's reads `"SELECT"` (`0x20359bd0`), a generic UI prompt. Both are genuinely
  different content per record, unlike the QUICK MENU cluster — reinforcing that the field really is
  per-record, and the QUICK MENU repetition above is a real coincidence (shared category label) rather
  than a struct-decoding mistake.

This table clearly encodes a good chunk of the real menu/settings screen tree, with actual title/
subtitle strings recoverable directly from records rather than needing separate correlation to the
216-item CI-V/EEPROM table or the raw label pool. That's real, if partial, progress toward the original
question ("what do the different menu buttons trigger") — screen *names* are falling out cleanly; the
per-item *action taken on press* is still the missing piece (see "where this dead-ends statically"
above).

## Open questions / next steps
1. **Read more of the table** — only records 0-14 read so far (of at least ~46+ real entries, per the
   position-lookup table's largest observed index). Check whether any record anywhere has a non-null
   `+0x08`/`+0x18` action-callback — would settle whether the per-item action dispatch is genuinely
   runtime-only or just null for the 12 or so items sampled so far. ~~Figure out why title strings were
   identical across the QUICK MENU items~~ — resolved well enough by records 11-14: real per-record
   subtitles do vary (`"Meter Type"`, `"SELECT"`), so the QUICK MENU repetition looks like a genuine
   shared-category-label coincidence (see hypothesis above), not a struct-decoding error.
2. Whether this table is the "walker" that was missing to connect `notes/diode-matrix.md`'s 216-item
   CI-V/EEPROM value table to the separate menu-name string pool (open question 6 there) — still not
   confirmed; the string pool this table points into (`~0x20359xxx`-`0x2035dxxx`) may or may not be the
   same pool diode-matrix.md found (`~0x2035a000`-`0x2035f000`) — worth checking directly.
3. The "FRONT CHECK MODE" factory screen's other 6 sibling states (`0x2c`-`0x30`, `0x32`) are other
   factory/service screens, not yet identified at all.
4. `FUN_2004f5a0` (the QUICK MENU container's own init/setup callback), records 13/14's own distinct
   init callbacks (`0x2004f858`/`0x2004f888`), and `FUN_2004f0f0`'s caller chain (how does a real touch
   event on the touchscreen actually reach `FUN_2004f610`?) aren't traced — that's the piece that would
   connect this whole widget to an actual physical touch coordinate.
