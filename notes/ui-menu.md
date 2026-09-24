# UI menu / touchscreen system

See [notes/ui-menu-history.md](ui-menu-history.md) for the full session-by-session narrative and evidence trail.

## Living reference: what's confirmed so far

| Finding | Address(es) | Status |
|---|---|---|
| **Factory "FRONT CHECK MODE" screen** — a numbered list of 13 real physical front-panel buttons | title at `0x20197153`, list at `0x20196f00`+; selector `FUN_2003a540` (7 states, `0x2c`-`0x32`, from `FUN_20013070()`) | ✅ button names confirmed, screen-select mechanism traced; the actual GPIO/key-matrix scan code itself not yet chased |
| **Generic touchscreen "list menu" widget** — one reusable set of functions drives QUICK MENU, MEMORY MENU, REC/SET, Meter Type, SELECT, and presumably every other list-style menu screen, operating on a global "current list" pointer pair | widget code at `0x2004f0e4`-`0x2004f9xx`; bound, in this static snapshot, to the table at `0x2018f0ec` (72-byte/`0x48` records) | ✅ full selection/navigation/commit flow traced end-to-end through real, named, decompiled code, including three distinct "on commit" action patterns (direct config-byte write, delegated setter call, confirm-dialog-then-cycle-state) — see below |
| **Physical-button-press chain** — traced from a real key press through to a queued screen-open request, for `MENU` and `QUICK` specifically | `ui_input_poll_tick` (`0x2002fca8`) → `key_event_resolve_and_route` (`0x2002ef98`) → `system_command_dispatch` (`0x2002ed9c`) + its 279-entry `g_system_command_table` (`0x2018d9e0`) → `menu_key_command_handler`/`quick_key_command_handler` → `ui_queue_screen_open_request` (`0x2002fd44`) | ✅ chain up to the queued request traced and named through real decompiled code; ❌ the actual final hand-off that switches the visible screen is **not** what was first guessed — checked the real consumer (`FUN_2002fd94`) and it turns out not to be it either (see below) — genuinely unresolved |
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

Reading further into the table (records 13/14) initially looked like a dead end — their `+0x04`/`+0x0c`
function pointers pointed at bytes Ghidra had never disassembled — but fixing the disassembly
(`tools/ghidra_scripts/FixArmThumbMode.java`, itself needed a fix to sweep every mode-aligned address in
the range instead of stopping at the first untraced return) surfaced real code, confirming three
concrete "on commit" action patterns:

| Function | Screen | Action |
|---|---|---|
| `meter_type_commit_direct_value` (`0x2004f864`) | Meter Type | **Direct commit**: copies the selected value straight into a live config byte (`*(byte*)(DAT_2004f730+6) = *(byte*)(DAT_2004f720+3)`), then calls `menu_widget_mark_dirty` |
| `select_screen_commit_via_setter` (`0x2004f898`) | SELECT | **Delegated commit**: calls an external setter function (`FUN_2003fcac`) with the selected value instead of writing a byte directly, then also marks dirty |
| `menu_item_confirm_and_cycle_state` (`0x2004f8f4`) / `menu_cycle_state_confirm_callback` (`0x2004f8b8`) | (unidentified screen, indices beyond what's been read) | **Confirm-then-cycle**: picks one of 4 message-string IDs (`0x4f`-`0x52`) based on the current state and calls `ui_show_message_dialog` (`0x200198bc`, newly named — sets up a real popup-dialog descriptor and a "dialog pending" flag the render loop must check) with a callback; on confirm, the callback advances the state 0→1→2→3→0 and calls an external apply function (`FUN_2003d574`) |

These three patterns (direct write, delegated setter, confirm-and-cycle) answer the original "what do
menu items trigger" question for the screens read so far. `ui_show_message_dialog` in particular looks
like a genuinely reusable finding (a real popup/dialog subsystem entry point) worth checking for at
other call sites throughout the firmware.

## More screens found by reading further into the table

Read records 10-14 (base still `0x2018f0ec`, same `0x48` stride) — the pattern holds further out, and
real new screen names surfaced:

- **Record 11**: container, title `"REC/SET"` (`0x20359bec`) — plausibly the Voice-TX-Record settings
  screen (working hypothesis, not confirmed): the already-documented `voice_tx_memory_control_task`/
  `voice_tx_memory_stream_task` feature (`notes/kernel-rtos.md`) has numbered message slots (`M1`-`M3`),
  which would explain why 3 sampled QUICK MENU item records (2-4) all share the string
  `"VOICE TX RECORD"` — 3 slot shortcuts sharing one category label, distinguished by the per-record
  `+0x44` lookup byte rather than by title.
- **Record 12**: title `"MEMORY MENU"` (`0x20359c18`) — the second screen the original string search
  found, now correctly placed in the table (an item/container immediately after record 11).
- **Records 13, 14**: two more containers, each with their **own distinct** init/activate function
  pointers (`0x2004f858`/`0x2004f864` and `0x2004f888`/`0x2004f898`) — *not* reusing
  `FUN_2004f610`/`FUN_2004f5a0` the way the QUICK MENU container did, so the "generic widget" shares
  the per-item availability convention and record stride across every screen, but a screen's own
  activate/init callback can be screen-specific. Record 13's subtitle reads `"Meter Type"`
  (`0x20359c98`), record 14's reads `"SELECT"` (`0x20359bd0`) — genuinely different content per record,
  confirming the QUICK MENU repetition above really is a coincidence (shared category label), not a
  struct-decoding mistake.

This table encodes a good chunk of the real menu/settings screen tree, with actual title/subtitle
strings recoverable directly from records rather than needing separate correlation to the 216-item
CI-V/EEPROM table or the raw label pool. Both halves of the original question ("what do the different
menu buttons trigger") now have real, code-level answers for at least these screens.

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
   aren't decompiled.

## The physical-button-press chain, traced end to end

**Real `SCIF3` key codes (corrected 2026-09-24 — don't use the FRONT CHECK MODE list-position numbers
below as key codes, they're a different numbering scheme)**: `MENU` = code `0x0f` (bitfield byte 0x0d
bit 3), `QUICK` = code `0x11` (byte 0x0d bit 6); code `9` is actually **A/B**, code `0x0c` is actually
**SPLIT** — so `menu_key_command_handler` (command ID `0x11`) is really the **A/B** handler, and
`quick_key_command_handler` (command ID `0x13`) is really the **SPLIT** handler (toggles bit 1 of
`0x203deaac`). `ui_queue_screen_open_request` turns out to be a **long-press arm**, not a screen-open
call. Full key-code table and the SCIF3 field layout: [notes/front-panel-report.md](front-panel-report.md).
The chain mechanics traced below (poll → resolver → command table → per-command handler) are still
correct; only the MENU/QUICK button-number identification used to label the trace was wrong.

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

**The final hand-off is genuinely unresolved, not just unattempted.** The obvious next consumer
(`FUN_2002fd94`, found via `references_to` on the "request pending" flag `DAT_2002f758`) was checked
and ruled out: the two values `ui_queue_screen_open_request` stores are callback function pointers
(called directly), not the list-widget's table pointers, and for both `MENU`/`QUICK`'s actual call
shape (4th argument `0`) `FUN_2002fd94` takes the branch that just clears the pending flag and posts an
audio-tone acknowledgment — neither callback fires. So this consumer is real but isn't what switches the
visible screen. The rest of the chain above (button → poll → resolver → command table → per-button
handler → queued request) stays solid; only this last step remains open (see the Living-reference table
above).

## MY CALL (SET → DISPLAY → MY CALL) — a separate, previously-undocumented text-entry-field widget

Prompted by `qemu-machine/`'s "who draws the boot splash logo/callsign" investigation — full derivation
(an Opus-model deep static-analysis pass, independently verified against real decompiled code and raw
memory reads) in the history file. Same underlying "generic touchscreen list widget" family as above,
but a **separate table and record format**, dedicated to text-entry (on-screen keyboard) fields
specifically — `MY CALL`, `FILE NAME`, `MEMORY NAME`, `Fixed Edges`, and 7 others.

- **`g_my_call_text` = `0x203de53c`, 10 bytes, plain ASCII, space-padded** — confirmed 3 independent
  ways: `opening_screen_build_frame` (renamed from `FUN_20037c10`, the boot-splash frame builder)
  `memmove`s it straight into the splash frame buffer (`dispbuf` = `DAT_200382e4`/`0x20403f64`) — the
  real, direct link between this setting and the boot screen; the text-entry field descriptor table
  (`g_text_entry_field_table`, `0x201998cc`, 10 records × 44 bytes) has record 3 (MY CALL's field kind)
  pointing both its edit- and source-buffers here, length `10`; the 326-item factory-reset defaults
  table (`0x20190ecc`, 64-byte stride, see `notes/diode-matrix.md`'s "Factory reset" section) has item
  `0x71`'s live-value pointer set to the same address, type byte `0x0a` (=10, length), category byte
  `0x03` (SET ▸ DISPLAY), default `0x2035962c` (ten literal `0x20` space bytes) — cross-checked against
  two other items from the same table (`0x73`="Display Language", `0x22`="Band Edge Beep").
- **Persisted at EEPROM byte offset `0x1a90`, 10 bytes** (`IC351`/`GT24C128B` on RIIC2) — from
  `g_nvram_region_table` (`0x2018bf80`, 4 records × 12 bytes `{ram_base, shadow_base, eeprom_offset,
  length}`), region 2 (`ram_base=0x203de4cc`, `eeprom_offset=0x1a20`) `+0x70` = `0x1a90`. Loaded at boot
  by `nvram_multirecord_load_and_verify` (`0x2001a104`); written back by `nvram_writeback_pump_tick`
  (`0x2001a1ec`, 32 bytes/tick, diffs RAM vs. a shadow copy) — no dedicated per-field EEPROM write call,
  it's part of the generic settings-block diff/writeback. The legacy/older EEPROM loader
  (`FUN_2006cb84`) uses region bases offset by a constant `-0x3e0` from the live loader's — concrete
  evidence the EEPROM layout was rebased between firmware/format versions.
- **Screen-descriptor table, real (corrected) layout**: base `g_screen_descriptor_table` (`0x2018fe24`,
  confirmed via the real index arithmetic at `0x20064348`: `index = screen_id - 0x13`, `base +
  index*24`) — **handler-first**, not handler-last: `{+0x00 u32 render/refresh handler, +0x04 u8 tag,
  +0x05 u8 row-style/icon code, +0x06/+0x07 = 01 01, +0x08 name_en, +0x0c name_en_alt, +0x10 name_jp,
  +0x14 name_jp_alt (Shift-JIS, e.g. コールサイン "Call Sign" for MY CALL)}`. MY CALL's own record sits
  at `0x20190694`, screen id `0x6d`. The `+0x00` handler (`0x2001a88c`, `text_entry_screen_render`) is
  shared across all 10 text-entry screens (ids `0x69`-`0x73`), not a per-field "on select" callback;
  takes no hidden arguments.
- **Screen→field link**: `screen_id_to_text_field_kind` (renamed from `FUN_2003fcf0`) linear-scans a
  25×4-byte table (`g_screen_to_text_field_map`, `0x20199a84`) mapping `{cmd_code, screen_id,
  text_field_kind, sub_index}` — screen `0x6d` (MY CALL) → field kind `3`, indexing
  `g_text_entry_field_table`. `text_entry_open_generic_field`/`text_entry_commit_generic_field`
  (`0x20040de4`/`0x20040e28`) are the generic open/commit routines driven by this table.
- **Cross-reference gotcha worth remembering**: the literal-pool cell `DAT_2001a5d4` (resolves to
  `0x2039e4c0`, the shared on-screen-keyboard edit-session struct) is duplicated across 6 separate
  literal-pool slots (`0x2001a5d4`, `0x2001b8cc`, `0x2001c99c`, `0x200333b4`, `0x20050604`,
  `0x20059450`) — `references_to` on the resolved address only finds code going through one specific
  cell, not sibling cells holding the identical value. A raw pointer-value byte search finds all
  aliases. Same class of gotcha as `notes/memory-map.md`'s SVD-xref caveat.
- **Unresolved discrepancy against the manual**: `reset_apply_item_default` (`0x2003dcc0`) skips item
  `0x71` (MY CALL) — and item codes `0x90`-`0xf6` — when `reset_type != 0`. Per `notes/diode-matrix.md`'s
  confirmed `0`=Partial/`1`=All labelling, this means **All Reset preserves MY CALL, Partial Reset
  clears it** — the opposite of intuition (a full reset wiping identity info, a partial reset leaving it
  alone). Not resolved — either the manual's real behavior really is this counter-intuitive, or the
  Partial/All `reset_type` mapping needs re-checking. Also flagged in `notes/diode-matrix.md`'s own
  Factory Reset section.

Ghidra renames: `text_entry_session_init` (`0x2001acec`), `text_entry_session_open` (`0x2001ad80`),
`text_entry_open_generic_field` (`0x20040de4`), `text_entry_commit_generic_field` (`0x20040e28`),
`text_entry_screen_render` (`0x2001a88c`), `text_entry_restore_default_for_screen` (`0x20041338`),
`screen_id_to_text_field_kind` (`0x2003fcf0`), `opening_screen_build_frame` (`0x20037c10`); data labels
`g_my_call_text` (`0x203de53c`), `g_text_entry_field_table` (`0x201998cc`), `g_screen_to_text_field_map`
(`0x20199a84`), `g_screen_descriptor_table` (`0x2018fe24`), `g_nvram_region_table` (`0x2018bf80`),
`g_settings_live_block_r2` (`0x203de4cc`), `text_entry_field_3_MY_CALL` (`0x20199950`),
`screen_desc_0x6d_MY_CALL` (`0x20190694`), `reset_item_0x71_MY_CALL` (`0x20192b0c`),
`s_my_call_factory_default_10_spaces` (`0x2035962c`); PLATE comments on all of the above.
