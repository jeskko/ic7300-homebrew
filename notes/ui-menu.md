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

## The SET-style settings-list engine (SD CARD menu et al.) — category registry + item catalog (2026-09-25)

A second, separate list engine (not the `0x2018f0ec` widget above) drives every SET-tree list screen whose
screen-descriptor render callback is `0x20042f3c` (now `settings_list_page_fill`): SD CARD, the
Emergency/Others screens, LOAD OPTION, the firmware-update file list, and so on. Everything below was
checked this session against decompiles, listings and byte reads of the 1.42 image.

**Data, three levels:**
1. **Operating-mode table** `0x2019add4` (`operating_mode_table_entry_lookup`, 24-byte records, index
   `screen-0x13`). This is a different table from `g_screen_descriptor_table` `0x2018fe24`, which holds the
   names and the render callback. `+4` u16 = **category id**, `+8` = screen-enter callback.
   `operating_mode_change_dispatch` copies `+4` into `*(u16*)0x20390366`. Screens 0x13–0x67 map to
   categories 0x00–0x49 (0xff = not a list screen). **SD CARD = screen `0x2f` → category `0x18`**. Its
   enter callback `0x20057010` only calls `settings_list_builder(0)`.
2. **`g_settings_category_registry`** `0x201993e0`: 0x4a × `{u32 count; ptr list; u32 flags}`. List
   entries are `{u8 type; u8 pad; u16 val}`:
   - type 1 = go to screen `val`
   - type 2 = settings value item `val` (0x40-stride table `0x20190ecc`, 326 entries, edited on screen 0x68)
   - type 3 = catalog item `val`

   **Correction, 2026-09-25**: `+8` is not reserved/padding as first read — it's real flags, bit 0 wraps
   the cursor and bit 1 wraps pages (consumed by `FUN_2003ece8`/`FUN_2003ee80`, the paging logic). SD CARD's
   own entry leaves it `0`, which is correct (no wrap) and was left unchanged by `sdk/examples/
   homebrew-apps-menu/`'s own patch.
3. **`g_settings_item_catalog`** `0x2018ed48`: 39 × 20-byte records `{action, query, flags, en, jp}`. The
   flags low byte is the "kind" used by `settings_list_item_kind_renderer` (0 = plain label); byte 1 (bits
   for command IDs `0x14`-`0x16`, likely softkey enables) is also read by `FUN_20041448`, one confirmed real
   consumer beyond the renderer — bytes 2-3 have no confirmed consumer. This is the table earlier notes
   placed "around `0x2018eebc`". **The earlier `{en, jp, cb_action, cb_query, flags}` framing was off by one
   field group**: each name goes with the action/query *before* it. Every consumer uses base `0x2018ed48`
   (literal pools `0x2003fab0`, `0x2004196c`, `0x20086674`, `0x2008abec`).

   **Correction, 2026-09-25**: indices are **not** cleanly u16 everywhere — one consumer,
   `FUN_2003fd6c`, truncates a type-3 `val` to a **u8** before using it (`cVar8 = pcVar4[2]`, the low byte
   only). For `sdk/examples/homebrew-apps-menu/`'s own index `0x881`, the truncated value is `0x81`,
   confirmed to land on a genuinely blank catalog record in the 1.42 image (no collision with any other
   slot's own low byte in the `0x81`-`0x92` neighborhood) — harmless there, but version-dependent, and worth
   checking again before reusing an index above `0xff` against a different firmware release.

**SD CARD menu (category 0x18, count 8, list `0x201990bc`)**: Load Setting (3,9) · Save Setting (3,8) ·
Save Form (2,325) · SD Card Info (1,0x34) · Screen Capture View (3,20) · Firmware Update (3,18) · Format
(3,22) · Unmount (3,23). REC Start/Stop, Play Files, CI-V Address and similar are catalog entries of
*other* categories (0x3c voice recorder, 0x1c LOAD OPTION). They are not rows of the SD menu.

**Build**: `settings_list_builder(0)` (`0x2003e5f0`) copies each registry-list entry that passes
`settings_item_visibility_filter` into `0x203da12e` (152 slots). It then sets `+0xe` = n-1 and `+0x10` = n
in the list-state struct `0x20390218` (`+0xa` = cursor, `+0xc` = page start = cursor & ~3). **The item
count is just `registry[cat].count` minus the filtered items.** There is no terminator and no hardcoded
per-screen count. Lists that aren't a multiple of 4 are normal: category 0 has 7 items, and empty trailing
slots have type 0 and render blank.

**Render**: `settings_list_page_fill` fills the 4 slot records of the current page (stride 0x54 at
`0x203ff76c+0xa4`: type, val, enabled, …). Per type:
- type 1 → `FUN_20042c34`
- type 2 → `FUN_20042d74` (value text) plus `FUN_20041f20` (grey-out test for a few specific value ids)
- type 3 → `settings_list_item_kind_renderer`

Labels come from `settings_list_row_label_ptr` (`0x20086000`). For type 3 that is `catalog[val]+0xc+lang*4`.

**Tap → action** (the question of how a tap reaches `cb_action` is now closed):
- **Touch**: system command `0xca` → `settings_list_touch_row_cmd_handler` (`0x20035dd0`, slot from
  `*(u8*)0x203901f2`). It checks the row is within the count (`FUN_2003f004`), then calls
  `settings_list_query_row`. If that returns 0, it arms `key_longpress_arm(release=0x20035db8,
  hold=0x20035dac, 200)`; otherwise the release callback is the no-op `0x2002ed94`. The release callback
  `0x20035db8` (raw ARM) calls `settings_list_activate_row(*(u8*)0x203901f2)`.
- **MULTI push**: command `0x40` → `multi_push_cmd_handler` (`0x20032a44`) → `settings_list_query_row` →
  `settings_list_activate_row(cursor - page)`.
- **`settings_list_activate_row`** (`0x2003f184`), per type:
  - type 1 → `operating_mode_change_dispatch(val)`
  - type 2 → `*(u16*)0x20390220 = val`, then dispatch(0x68)
  - **type 3 → `bx` to `catalog[val].action`**, with no arguments (actions read the cursor from
    `*(u16*)0x20390222`)
- **Query** (`settings_catalog_call_query`, `0x2003f07c`): a NULL query sets `*out = 1` and the row is
  selectable. A return of 0 means selectable. Placeholder rows use query `0x20041b4c` (`*out=2; return 1`)
  with the no-op action `0x20041b48`.

**Firmware Update, concretely**: catalog idx 18 has action `0x2005dc3c` → `sd_firmware_update_row_impl`
(`0x2005dba0`) → SD checks → `operating_mode_change_dispatch(0x38)`. The file row on the file-list screen
(screen `0x37`, category `0x1e`, catalog idx 19, action `0x2005dd1c`) goes to
`sd_firmware_update_file_row_impl` (`0x2005dc4c`), which sets SD-UI state `0x37`. So the update takes two
taps, not one. This corrects the earlier tap-chain trace in `notes/kernel-rtos.md`; the downstream part of
that trace still holds.

**Adding a row (for the SDK "Homebrew Apps" button).** There is no dead slot to reuse:
- All 8 SD-menu entries are real, visible items, and the list sits directly against category 0x19's list
  at `0x201990dc`.
- The only catalog records in no registry list are idx 28/29 (REC Start/Stop, swapped in by
  `settings_list_page_fill` on screen 0x5e) and 35 (`-- Blank --`). All three are non-selectable
  placeholders.

Extending the list is cheap, though, because the count and the list pointer are plain data:
- Patch `registry[0x18]` (`0x20199500`): count 8→9, and point the list at a new 9-entry copy with one
  extra `(3, N)` entry. The list can live anywhere, for example next to the SDK hook at `0x20600000`.
- The new catalog record must sit at `0x2018ed48 + N*20` with N ≤ 0xffff (u16). The zero padding after the
  registry, `0x20199758`–`0x201998cc`, is unreferenced: registry categories ≥ 0x4a are never used and no
  pointer points into the gap. It has stride-aligned slots from `0x2019975c` (N = 0x881) to `0x201998b0`
  (N = 0x892).
- The record is `{action = SDK entry (ARM or Thumb, reached via bx), query = 0, flags = 0x00010700 (same
  as Format), en = jp = "Homebrew Apps"}`. N is neither 5 nor 7, so `settings_item_diode_region_gate`
  always includes it.

**Live-tested, 2026-09-25** — see `sdk/examples/homebrew-apps-menu/`: this exact patch, built and
booted in `qemu-machine`, driven through the real touchscreen UI. The SD CARD menu genuinely shows
3 pages (stock is 2), page 3 shows one correctly-labeled "Homebrew Apps" row with no rendering
glitches, and tapping it runs the action with no crash, no navigation side effect, and the radio
fully responsive to CI-V afterward. Confirms every claim in this section (the count/list-pointer
patch, the new catalog record, the padding-gap placement, the `bx`-with-no-arguments action
convention) against real behavior, not just static reading.

**New finding, 2026-09-25 (adversarial review pass): this state is EEPROM-backed.** Region 2 of
`g_nvram_region_table` (`notes/eeprom-catalogue.md`-adjacent finding, not yet cross-referenced
there) covers `0x203de4cc` for `0x5e0` bytes — the SD CARD menu's own saved cursor position
(`0x203de4cc + cat*4 + 0x288`) and a snapshot of the currently-selected row's own list-entry bytes
(`+0x438`) both live inside that range, so both persist across a real reflash back to stock
firmware. Confirmed benign against stock: stock clamps the cursor to the category's own real item
count (7 for the un-patched SD CARD list), so a cursor value left pointing at the removed 9th slot
doesn't go out of bounds — but it's a real, persistent side effect of using this menu, not just an
in-RAM one, worth remembering if this technique is applied somewhere the "safe to clamp" property
hasn't been separately checked.

## Popup message dialogs — item table, message records, live state (2026-09-25)

Traced for `sdk/examples/hello-gui/` and confirmed live in the emulator: a custom app shows a real
firmware dialog with its own text and OK button, the button dismisses it, the callback fires.
Every popup the radio shows ("Format OK?", "SD Card error." [CLOSE], the firmware-update
progress messages...) goes through this one mechanism.

- **`g_dialog_item_table`** (`0x2018b8f0`, `0x69` × 16-byte items, index 0 unused):
  `+0` u8 **type** — `0` no buttons (progress: "LOADING Please wait..."), `1` one button
  (CLOSE/OK), `2` two buttons (YES/NO, OK/CANCEL, CANCEL/NEXT), `8` timed toast (with `+2`/`+4`
  u16 timeouts, e.g. `0x64`); `+6` u8 **message record index**; `+0xc` optional on-activate
  function, called by `ui_activate_menu_item`. Items `0x2d`/`0x31`/`0x3a` are unused (type 1,
  record `0xff`). Stock one-button OK dialog: item **`0x66`** → record `0x53` "The USB
  SEND/Keying settings were corrected." [OK], shown with no callbacks at all
  (`ui_show_message_dialog(0x66, 0, 0)` in `FUN_2004d1a4`, a settings-load routine).
- **`g_status_message_table`** (`0x2032c91c`, `0x4c`-byte records): `+0` u32 flags (0 plain,
  1 on destructive YES/NO questions, 2 on errors); `+0x04..+0x24` 9 English string pointers;
  `+0x28..+0x48` 9 Japanese (Shift-JIS) pointers. In each language, **slots 0-5 are text lines,
  6/7 the left/right button labels** (a one-button dialog uses slot 7 only), slot 8 always
  empty; unused slots point at a shared `""` (`0x20359e5c`), never NULL. Past record `0x64` the
  table runs into string data — there is no free record slot. (This corrects the record layout
  in `firmware-update.md`'s dialog section, whose "japanese_ptr(+0x24)/suffix(+0x28)" is off by
  one slot: `+0x28`/`+0x2c` are simply Japanese lines 0/1.)
- **`g_dialog_state`** (`0x2039c584`) — the one live dialog: `+0` u8 active item (0 = none),
  `+1` u8 last button tapped, `+2` u16 timeout, `+5` u8 flags, `+8` primary-button callback,
  `+0xc` secondary callback, `+0x10`/`+0x14` a pending "next" item + callback, `+0x18` an on-close
  hook.
- **API**: `ui_show_message_dialog(item, primary_cb, secondary_cb, flag)` (`0x200198bc`) activates
  the item and installs the callbacks; `ui_show_message_dialog_with_timeout(item, use_timeout,
  cb)` (`0x20019920`) is the no-button/toast variant. A tap goes `FUN_20035890` →
  **`ui_dialog_button_tap(button, &out)`** (`0x20019bf8`), which calls the button's callback as
  `int cb(u8 *result)`; with no callback it yields 2 with `*result` left at 1. A callback that
  returns 2 and leaves `*result` alone closes the dialog (confirmed live with the SDK's). Real callbacks seen: return 2 with
  `*result = 3` (`menu_cycle_state_confirm_callback`); call **`ui_dialog_close`** (`0x200199ec`,
  clears `+0`/`+8`/`+0xc`) themselves and return 3; or chain to a follow-up dialog via
  **`ui_dialog_set_pending_next(item, cb)`** (`0x200199d0`) and return 4 (the "All Reset?" →
  NEXT → "Clears all settings..." pair, callback `0x20041c38`), which
  **`ui_dialog_show_pending_next`** (`0x20019cec`) then opens.
- The whole image runs from RAM, so both tables are writable at runtime. `sdk/runtime/ui_dialog.c`
  borrows item `0x66` by swapping record `0x53`'s string pointers for the duration of one dialog,
  then restores them (verified restored, live).

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
