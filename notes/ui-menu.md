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
| **Generic touchscreen "list menu" widget** — one reusable set of functions drives QUICK MENU, MEMORY MENU, REC/SET, Meter Type, SELECT, and presumably every other list-style menu screen, operating on a global "current list" pointer pair | widget code at `0x2004f0e4`-`0x2004f9xx`; bound, in this static snapshot, to the table at `0x2018f0ec` (72-byte/`0x48` records) | ✅ full selection/navigation/commit flow traced end-to-end through real, named, decompiled code, including three distinct "on commit" action patterns (direct config-byte write, delegated setter call, confirm-dialog-then-cycle-state) — see below |
| **Physical-button-press chain** — traced from a real key press through to a queued screen-open request, for `MENU` and `QUICK` specifically | `ui_input_poll_tick` (`0x2002fca8`) → `key_event_resolve_and_route` (`0x2002ef98`) → `system_command_dispatch` (`0x2002ed9c`) + its 279-entry `g_system_command_table` (`0x2018d9e0`) → `menu_key_command_handler`/`quick_key_command_handler` → `ui_queue_screen_open_request` (`0x2002fd44`) | ✅ chain up to the queued request traced and named through real decompiled code; ❌ the actual final hand-off that switches the visible screen is **not** what was first guessed — checked the real consumer (`FUN_2002fd94`) and it turns out not to be it either (see "Correction" below) — genuinely unresolved |
| **"Opening Message" (SET → DISPLAY) gates the whole boot splash** — factory-reset item `0x70`, live byte `0x203de53b` (right before MY CALL), `0`=OFF/`1`=ON, factory default ON | record `0x20192acc` (default at `+0x08`); gate in `system_mode_request_dispatch` at `0x2002abe0` → `FUN_2002a2a4` fade driver → `opening_screen_build_frame`; EEPROM `0x1a8f` (region 2 base `0x1a20` + `0x6f`) | ✅ confirmed live 2026-09-23 (emulator: OFF → no splash at all; ON → full fade-in/hold/fade-out renders). See `qemu-machine/README.md` 2026-09-23 Status |

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

**Update, same session — the "dead end" was a disassembly gap, not a runtime-only field.** Records
13/14's own `+0x04`/`+0x0c` function pointers (`0x2004f858`, `0x2004f864`, `0x2004f888`, `0x2004f898`)
pointed into bytes Ghidra had simply never disassembled — the classic "empty function list doesn't mean
no code" gap, invisible until something actually reads the function-pointer table (which Ghidra's
static analyzer has no way to follow on its own). Fixed via `tools/ghidra_scripts/FixArmThumbMode.java`
(which needed its own fix first — `disassemble(start)` alone only follows control flow and stops at
the first return with no traced successor, leaving the later independent stub functions in the range
undefined; the script now sweeps every mode-aligned address in the range instead of one call). With
real code now disassembled, decompiled, and named, **three concrete "on commit" action patterns are
now confirmed**:

| Function | Screen | Action |
|---|---|---|
| `meter_type_commit_direct_value` (`0x2004f864`) | Meter Type | **Direct commit**: copies the selected value straight into a live config byte (`*(byte*)(DAT_2004f730+6) = *(byte*)(DAT_2004f720+3)`), then calls `menu_widget_mark_dirty` |
| `select_screen_commit_via_setter` (`0x2004f898`) | SELECT | **Delegated commit**: calls an external setter function (`FUN_2003fcac`) with the selected value instead of writing a byte directly, then also marks dirty |
| `menu_item_confirm_and_cycle_state` (`0x2004f8f4`) / `menu_cycle_state_confirm_callback` (`0x2004f8b8`) | (unidentified screen, indices beyond what's been read) | **Confirm-then-cycle**: picks one of 4 message-string IDs (`0x4f`-`0x52`) based on the current state and calls `ui_show_message_dialog` (`0x200198bc`, newly named — sets up a real popup-dialog descriptor and a "dialog pending" flag the render loop must check) with a callback; on confirm, the callback advances the state 0→1→2→3→0 and calls an external apply function (`FUN_2003d574`) |

This directly answers the original question for several real cases: pressing a settings-menu item can
either write straight to a live config byte, delegate to a setter function, or pop a confirmation
dialog before cycling to the next state — three different, now-named, code-level patterns rather than
one guessed-at mechanism. `ui_show_message_dialog` in particular looks like a genuinely reusable finding
(a real popup/dialog subsystem entry point) worth checking for at other call sites throughout the
firmware. The earlier concern that these fields might be runtime-populated (like `kernel_start`'s
mystery task descriptor or the multi-display attach bitmask) turned out not to apply here — but that
pattern is real elsewhere in this project, so still worth keeping in mind if a *different* record's
action field turns out to be genuinely zero after disassembly is fixed there too.

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
216-item CI-V/EEPROM table or the raw label pool. Combined with the three confirmed "on commit" action
patterns above, both halves of the original question ("what do the different menu buttons trigger") now
have real, code-level answers for at least these screens — screen *names* and their *on-press actions*
are both falling out of the same table.

## Open questions / next steps
1. **Read more of the table** — only records 0-14 read so far (of at least ~46+ real entries, per the
   position-lookup table's largest observed index) — now worth doing precisely *because* records 13/14
   turned out to hide real, previously-undisassembled code once actually read; later records likely do
   too. Each new record read is now a two-step process: read the raw bytes to find its function
   pointers, then (if they land on undefined bytes) queue a `FixArmThumbMode.java` fix before they can
   be decompiled — same pattern that paid off for records 13/14.
2. Whether this table is the "walker" that was missing to connect `notes/diode-matrix.md`'s 216-item
   CI-V/EEPROM value table to the separate menu-name string pool (open question 6 there) — still not
   confirmed; the string pool this table points into (`~0x20359xxx`-`0x2035dxxx`) may or may not be the
   same pool diode-matrix.md found (`~0x2035a000`-`0x2035f000`) — worth checking directly.
3. The "FRONT CHECK MODE" factory screen's other 6 sibling states (`0x2c`-`0x30`, `0x32`) are other
   factory/service screens, not yet identified at all.
4. Which screen `menu_item_confirm_and_cycle_state` (`0x2004f8f4`) actually belongs to isn't identified
   yet — it wasn't reached via any record read so far, only via `menu_widget_mark_dirty`'s call site at
   `0x2004f940` (itself not yet tied to a specific record). `FUN_2004f5a0` (the QUICK MENU container's
   own init/setup callback) and records 13/14's own distinct init callbacks (`0x2004f858`/`0x2004f888`)
   aren't decompiled. ~~`menu_widget_commit_selection`'s caller chain (how does a real touch event on
   the touchscreen actually reach `menu_widget_activate_focused_item`?)~~ — **traced, see below.**

## The physical-button-press chain, traced end to end

**Correction, 2026-09-24 — the "MENU=9, QUICK=12" identification below was wrong, don't reuse
it.** Those are FRONT CHECK MODE's *list positions* (a display sequence for the factory
self-test screen), not the actual `SCIF3` key codes, and treating them as if they were the key
codes fed into `g_key_code_to_command_id` mixed up two different button-numbering schemes. Real
`SCIF3` key codes, from static analysis of the front-panel report buffer: **MENU = code 0x0f**
(bitfield byte 0x0d bit 3), **QUICK = code 0x11** (byte 0x0d bit 6). Code 9 (0x09) is actually
**A/B**, and code 0x0c is actually **SPLIT** — so `menu_key_command_handler` (0x200327dc,
command ID 0x11) is really the **A/B** handler, and `quick_key_command_handler` (0x20032900,
command ID 0x13) is really the **SPLIT** handler (it toggles bit 1 of 0x203deaac).
`ui_queue_screen_open_request` (0x2002fd44) turns out to be a **long-press arm**, not a
screen-open call. Full key-code table and the SCIF3 field layout:
[notes/front-panel-report.md](front-panel-report.md). The chain mechanics below (poll → resolver
→ command table → per-command handler) are still correct; only the MENU/QUICK identification is
wrong.

Picked this up specifically to answer "how does a real key/touch event reach the menu system at all."
Traced the complete chain from a physical button press through to a queued screen-open request, for two
concrete buttons (`MENU` and `QUICK`, both confirmed against the FRONT CHECK MODE numbering above):

```
physical button press (MENU=9, QUICK=12 per FRONT CHECK MODE)
  -> ui_input_poll_tick (0x2002fca8) -- called from the main idle loop, confirmed via its own two
     callers sitting right next to the already-known idle-loop-variant functions (FUN_20053154 etc.,
     notes/kernel-rtos-history.md's system_mode_request_dispatch section)
  -> key_event_resolve_and_route (0x2002ef98, renamed from FUN_2002ef98) -- a genuinely massive
     (2500+ byte) raw-input processor: reads a raw key/touch code byte, runs extensive radio-state
     guards (TX state, band, split, tuner...), and resolves the raw code to a numeric "command ID"
  -> for raw codes 1-0x1e, resolution goes through g_key_code_to_command_id (0x2018d9a8, a plain
     ushort[] indexed by code-1) -- read directly: index 8 (key 9, MENU) -> command ID 0x11; index 11
     (key 12, QUICK) -> command ID 0x13
  -> system_command_dispatch (0x2002ed9c, renamed from FUN_2002ed9c) -- a genuine, previously-
     undocumented **279-entry system-wide command table** (g_system_command_table, 0x2018d9e0; each
     entry is 8 bytes: u32 command_id + u32 handler function pointer; ids run sequentially 0-0x116;
     linear-scans for a matching id, falls back to a default label if none matches, then calls the
     resolved handler). This table is a major structural find in its own right -- worth checking
     later whether it's also what CI-V or other subsystems dispatch through, or is UI-input-specific.
  -> per-command handler: menu_key_command_handler (0x200327dc, command 0x11) for MENU;
     quick_key_command_handler (0x20032900, command 0x13) for QUICK. Both decompiled in full:
     MENU's handler runs three pre-checks (FUN_20061f88/ffc/2006210c) and, if they pass, queues a
     screen-open request. QUICK's handler is a real **toggle** -- if a state bit is already set it
     closes (calls FUN_20062790(0)) instead of opening, confirming QUICK MENU really does open/close
     on repeated presses rather than only opening.
  -> ui_queue_screen_open_request (0x2002fd44, renamed from FUN_2002fd44) -- both handlers converge on
     the exact same call shape, `(descriptor_ptr, list_ptr, 0x54, 0)`: fills in a small request struct
     (screen size/type, the two pointers, a "new request" flag) and sets a "request pending" bit
     (`*DAT_2002f758 |= 0x80`) for something else to notice.
```

**Correction, same session — the hand-off hypothesis above was wrong, checked and retracted.** Went
looking for the consumer of the "request pending" flag (`references_to` on `DAT_2002f758`) and found a
real one, `FUN_2002fd94`, sitting right after `ui_queue_screen_open_request`. Decompiling it settled the
question, just not the way expected:

- The two values `ui_queue_screen_open_request` stores are **not** data-table pointers shaped like
  `DAT_2004f728`/`DAT_2004f720` — they're **callback function pointers**, called directly
  (`(**(code**)(request+4))()`). Reading what `menu_key_command_handler` actually passes
  (`DAT_200333a0` → `0x200327bc`, `DAT_2003339c` → `0x20032780`) and decompiling both targets shows
  small precondition-style helper functions (bit-flag checks, status codes 0/2/3) — nothing that
  resembles setting up the list-widget's table pointer.
- Worse for the hypothesis: `FUN_2002fd94`'s branch logic only calls **either** callback when a stored
  flag byte (the request's `+0xe` field, set from `ui_queue_screen_open_request`'s 4th argument) equals
  `2`. Both `menu_key_command_handler` and `quick_key_command_handler` pass that argument as `0`, which
  unconditionally lands on a branch that just clears the pending flag and posts what looks like an
  audio-tone acknowledgment (`FUN_2001cbac`/`FUN_2001cbe4`) — **neither callback fires** for these two
  buttons' actual requests.

So this specific consumer is real and does something, but it is not the mechanism that switches the
visible screen. **The genuine final hand-off to the list-widget system (or whatever actually redraws
the screen after MENU/QUICK is pressed) remains unresolved** — an honest open question, not a confirmed
link. The rest of the chain above (button → poll → resolver → command table → per-button handler →
queued request) stays solid; only this last step is now correctly marked as still open rather than
"probably this."

## MY CALL (SET → DISPLAY → MY CALL) fully resolved — a separate, previously-undocumented
## "text-entry field" widget instance, distinct from the QUICK-MENU-style widget above

Prompted by `qemu-machine/`'s own "who draws the boot splash logo/callsign" investigation (see
`qemu-machine/README.md`'s Status section) — an Opus-model deep static-analysis pass, independently
verified against real decompiled code and raw memory reads before being recorded here. Same underlying
project-wide "generic touchscreen list widget" family this file already documents for QUICK
MENU/MEMORY MENU, but a **separate table and record format**, dedicated to text-entry (on-screen
keyboard) fields specifically — `MY CALL`, `FILE NAME`, `MEMORY NAME`, `Fixed Edges`, and 7 others.

**`g_my_call_text` = `0x203de53c`, 10 bytes, plain ASCII, space-padded.** Confirmed three independent
ways:
1. `opening_screen_build_frame` (renamed from `FUN_20037c10`, the power-on splash-screen frame
   builder — 7 call sites, all inside the boot fade-in/hold/fade-out driver at `0x2002a2a4`, ramping
   brightness 0→0x64 in steps of 0x14) does `memmove(dispbuf+8, g_my_call_text, 10)` directly into the
   splash frame buffer (`dispbuf` = `DAT_200382e4`/`0x20403f64`) — **this is the real, direct link
   between this setting and the boot screen** `icom-openvg-rendering`'s own thread was looking for.
2. The text-entry field descriptor table (`g_text_entry_field_table`, `0x201998cc`, 10 records × 44
   bytes) — record 3, MY CALL's field kind — has both its edit-buffer and source-buffer pointers set
   to `0x203de53c`, length `10` (edited in place, unlike MEMORY NAME/FILE NAME which stage through a
   separate scratch buffer at `0x203da390`).
3. The 326-item factory-reset defaults table (`0x20190ecc`, 64-byte stride — the same table this
   file's own "Factory reset" section documents, see `notes/diode-matrix.md`) has item `0x71`'s live-
   value pointer set to the same `0x203de53c`, type byte `0x0a` (=10, length), category byte `0x03`
   (SET ▸ DISPLAY), default value `0x2035962c` (ten literal `0x20` space bytes) — independently
   cross-checked against two other items read from the same table (`0x73`="Display Language",
   `0x22`="Band Edge Beep") to confirm the record layout, both landing on already-known real names.

**Persisted at EEPROM byte offset `0x1a90`, 10 bytes** (`IC351`/`GT24C128B` on RIIC2). The live NVRAM
region table (`g_nvram_region_table`, `0x2018bf80`, 4 records × 12 bytes: `{ram_base, shadow_base,
eeprom_offset, length}`) places `0x203de53c` at `+0x70` inside region 2 (`ram_base=0x203de4cc`,
`eeprom_offset=0x1a20`, `length=0x5e0`) → `0x1a20+0x70=0x1a90`. Read directly off the compiled image,
byte for byte — not inferred. Loaded at boot (all 4 regions, chunked ≤0x20 bytes/read) by
`nvram_multirecord_load_and_verify` (`0x2001a104`, already named from prior work); written back by
`nvram_writeback_pump_tick` (`0x2001a1ec`, 32 bytes/tick, diffs RAM vs. a shadow copy, dirty-bit
`*DAT_2003faac |= 0x40`) — no dedicated per-field EEPROM write call exists for MY CALL specifically,
it's just part of the generic settings-block diff/writeback.

**Bonus, worth recording separately**: the legacy/older EEPROM loader (`FUN_2006cb84`, this project's
own long-documented "dense scan" — see `qemu-machine/README.md`'s Status history) uses region bases
`0x40`/`0x12e0`/`0x1620` for the *same* lengths (`0x129c`/`0x338`/`0x468`) the live loader uses at
`0x420`/`0x16c0`/`0x1a20` — **the EEPROM layout was rebased by a constant `+0x3e0` between firmware/
format versions**, real concrete evidence (not just a hypothesis) for the "versioned settings format"
question already flagged elsewhere in this project's EEPROM notes.

**Real, corrected understanding of the record format** (the earlier working notes in this thread had
the field order backwards): the screen-descriptor table's real base is `g_screen_descriptor_table`
(`0x2018fe24`, confirmed via the real index arithmetic at `0x20064348`: `index = screen_id - 0x13`,
`base + index*24`) — **handler-first**, not handler-last: `{+0x00 u32 render/refresh handler, +0x04
u8 tag, +0x05 u8 row-style/icon code (NOT a max-length, an earlier misreading), +0x06/+0x07 = 01 01,
+0x08 name_en, +0x0c name_en_alt, +0x10 name_jp, +0x14 name_jp_alt (Shift-JIS — the "non-ASCII blob"
in an earlier reading of this table was just the Japanese label, e.g. コールサイン "Call Sign" for
MY CALL)}`. MY CALL's own record sits at `0x20190694`, screen id `0x6d`. The `+0x00` handler
(`0x2001a88c` for MY CALL and 10 sibling text-entry screens, ids `0x69`-`0x73`) is
`text_entry_screen_render` — a shared render/refresh routine for every text-entry-type screen, **not**
a per-field "on select" callback as first guessed; genuinely takes no hidden arguments (confirmed —
not another instance of this codebase's usual "hidden register argument" gotcha this time).

**The real screen→field link**: `screen_id_to_text_field_kind` (renamed from `FUN_2003fcf0`) linear-
scans a 25×4-byte table (`g_screen_to_text_field_map`, `0x20199a84`) mapping `{cmd_code, screen_id,
text_field_kind, sub_index}` — screen `0x6d` (MY CALL) → field kind `3`, which indexes
`g_text_entry_field_table` above. `text_entry_open_generic_field`/`text_entry_commit_generic_field`
(renamed from `FUN_20040de4`/`FUN_20040e28`) are the real generic open/commit routines driven by this
table.

**A real gotcha worth keeping on record for future sessions**: the "no static write to `DAT_2001a5d4`
anywhere" dead end this thread hit turned out to be a false negative from address-based `references_to`
— that literal-pool cell (resolves to `0x2039e4c0`, the shared on-screen-keyboard edit-session struct,
not a per-field pointer) is duplicated across **6 separate literal-pool slots** (`0x2001a5d4`,
`0x2001b8cc`, `0x2001c99c`, `0x200333b4`, `0x20050604`, `0x20059450`), each an independent compiled
copy of the same pointer value — Ghidra's `references_to` on the *resolved target address* only finds
code that goes through the *specific* literal-pool cell it's tied to, not sibling cells holding the
identical value. A raw memory/byte-pattern search for the little-endian pointer value itself (not an
address-based xref) is what actually finds all the aliases. Worth remembering alongside this project's
other known cross-reference blind spots (e.g. `notes/memory-map.md`'s SVD-xref caveat) the next time a
"no writer found anywhere" result looks suspicious.

**A real, unresolved discrepancy against the operating manual, found along the way** (see
`notes/diode-matrix.md`'s own "Factory reset" section for the base mechanism): `reset_apply_item_default`
(`0x2003dcc0`) skips item `0x71` (MY CALL) — along with a whole range of item codes `0x90`-`0xf6` — when
`reset_type != 0`. Per that section's own confirmed `0`=Partial/`1`=All labelling, this means **All
Reset preserves MY CALL, and Partial Reset clears it** — the opposite of what you'd normally expect
(a full/all reset wiping personal identity info, a partial reset leaving it alone). Not resolved here;
flagged in `notes/diode-matrix.md`'s own Factory Reset section too. Either the manual's actual real
behavior really is this counter-intuitive, or the Partial/All `reset_type` value mapping documented
there needs re-checking.

**Ghidra renames made this pass** (project saved): `text_entry_session_init` (`0x2001acec`),
`text_entry_session_open` (`0x2001ad80`), `text_entry_open_generic_field` (`0x20040de4`),
`text_entry_commit_generic_field` (`0x20040e28`), `text_entry_screen_render` (`0x2001a88c`),
`text_entry_restore_default_for_screen` (`0x20041338`), `screen_id_to_text_field_kind` (`0x2003fcf0`),
`opening_screen_build_frame` (`0x20037c10`); new data labels `g_my_call_text` (`0x203de53c`),
`g_text_entry_field_table` (`0x201998cc`), `g_screen_to_text_field_map` (`0x20199a84`),
`g_screen_descriptor_table` (`0x2018fe24`), `g_nvram_region_table` (`0x2018bf80`),
`g_settings_live_block_r2` (`0x203de4cc`), `text_entry_field_3_MY_CALL` (`0x20199950`),
`screen_desc_0x6d_MY_CALL` (`0x20190694`), `reset_item_0x71_MY_CALL` (`0x20192b0c`),
`s_my_call_factory_default_10_spaces` (`0x2035962c`); PLATE comments on all of the above with the full
derivation.
