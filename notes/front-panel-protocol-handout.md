# Handout: front-panel (`IC501`) protocol and firmware-image thread

**Correction, same day (2026-09-07), after this handout was picked up again**: goal 1 below is now
**resolved** — see `notes/front-panel-firmware.md`'s "Open questions" section and
`notes/front-panel-firmware-history.md`'s "Priority-1 handout question resolved" section for the full
answer. Also, **every `0x200301f2` address in this document is a typo for `0x203901f2`** (no symbol
exists at the former; the latter is `key_event_resolve_and_route`'s real key-code struct) — that typo is
exactly what made the "zero references found" cross-check below come back empty, on top of the real
indirection it also correctly anticipated. **Goal 2 has real progress too, same day, follow-up session**:
traced a confirmed one-shot boot-time `SCIF3` handshake that's how the main CPU gets the front-panel
version for the version-info screen, and checked the entire outbound-`SCIF3` driver it uses for any
chunked-firmware-write shape — found nothing (a real negative result, not a whole-image sweep). See
`notes/front-panel-firmware-history.md`'s "How the main CPU gets the front-panel version" section. Left
unedited below for the record; read the current-state notes files for the up-to-date picture rather than
treating this document's own "not yet decoded"/"still open" framing as current.

Written 2026-09-07 as an onboarding doc for whoever (a fresh session) picks this thread up next. Two
goals, in priority order:

1. **Settle whether physical button presses even reach the main CPU over the known `SCIF3` link** — a
   real, sharp, previously-unasked question that fell out of unrelated UI-menu tracing the same day (see
   "How this thread came to exist" below). This is higher-leverage than the older "which bit is MENU"
   question, since it would confirm whether that question is even aimed at the right structure.
2. **While digging through the front-panel-related main-CPU code, stay alert for anything suggesting
   `IC501` (the RL78 front-panel MCU) can be firmware-updated at all.** If real update code exists — an
   erase/program sequence, a "send firmware chunk" packet type, a bootloader-entry command — that's
   strong evidence a real `IC501` firmware image exists somewhere this project hasn't found yet. See
   "The firmware-image angle" below for exactly what to compare against and where to look.

Read `notes/front-panel-firmware.md` (current state) and `notes/front-panel-firmware-history.md` (full
narrative) first if you haven't — this handout summarizes and points, it doesn't replace them.

## Hardware identity (settled, don't re-derive)

- `IC501` = **`R5F104LCAFB`**, Renesas **RL78/G14** (64-pin LQFP), marked `SX-3765C-1` on the board —
  the Display/Front Unit's own MCU. 32 KB code flash / 4 KB data flash / 4 KB RAM (Renesas datasheet).
  Every `"SX3765 Vx.xx-xxx"` string in the main firmware is a compatibility/version check against *this*
  chip, not an embedded image of it — a long-settled identity question, see `multi-cpu-images.md`.
- The main firmware's update container (`7300_1XX.dat`) is now **fully accounted for**: the main body
  (`chunk1`/`chunk2` fonts + `chunk3`) plus exactly 3 separately-identified sub-components, confirmed
  by correlating against Icom's own officially-published per-release version fields: `DSP Program`,
  `DSP Data`, `FPGA` (`IC1351`, Altera Cyclone IV E). **None of these four pieces is front-panel
  firmware** — `front_cpu.bin` (component 0's internal extraction name) was specifically checked and
  ruled out: it's 134,272/163,592 bytes (compressed/decompressed), over 4x the RL78/G14's entire 32 KB
  flash capacity, and disassembles as genuine TMS320C674x (DSP) object code via three independent
  disassemblers agreeing byte-for-byte. See `multi-cpu-images.md`'s component table for exact offsets/
  sizes if you want to re-verify any of this.
- RL78 Ghidra/binutils tooling is **already installed and confirmed working**, just never pointed at a
  real target: [xyzz/ghidra-rl78](https://github.com/xyzz/ghidra-rl78) (Ghidra processor module) and
  mainline `rl78-objdump`/`rl78-readelf`. If you find a real `IC501` image, this tooling is ready.

## What's already solid about the `SCIF3` wire protocol (main-CPU side only)

- Physical link: **`SCIF3`**, register base `0xE8008800`, pins `P6_0`/`P6_1`.
- Framing: 33-byte packets, `0xFE`/`0xFD` preamble/terminator — **the same convention** used by CI-V
  (`SCIF0`) and the service-mode link (`SCIF1`); one shared driver template across all three UARTs in
  this firmware, confirmed by direct comparison.
- Two named, fully decompiled functions handle receive:
  - `scif3_frame_rx_statemachine` (`0x20036c68`) — assembles raw bytes into a complete frame.
  - `scif3_frame_dispatch_by_type` (`0x20036bb8`) — once a frame is complete, reads its first content
    byte (right after the `0xFE` marker) as a **"type"** (range `0x00`-`0x1F`; `0xF0`/`0xF1` are special-
    cased ACK/NAK-style types handled inline). For ordinary types, the frame body gets copied via
    `FUN_2017c710` into offset `type` of **one shared front-panel status struct at `0x203dcab6`**
    (reached via pointer variable `DAT_20037590`, itself holding `0x203dcab6` in this v1.42 image —
    **always read the pointer's stored value, don't assume a `DAT_` symbol name is the buffer's own
    address**, a mistake made and caught more than once this project).
- The **one place we know reads specific bits back out** of that struct: `boot_check_mode1_combo` /
  `boot_check_mode5_combo` (called from `cold_boot_hw_init`) read buffer offset `0xd`, bits 3/4, to
  detect the MENU+FUNCTION power-on combo that enters factory/service mode. See
  `notes/kernel-rtos.md`'s "Factory/service mode" section for that flow.
- **Not yet decoded**: the rest of the type→offset→bit mapping. Which bits correspond to which physical
  key, which to encoder deltas, etc. `scif3_frame_dispatch_by_type`'s own header comment (left by an
  earlier session) already flags this as the natural next step — still true, still open.

## How this thread came to exist: a real cross-check, not a guess

Completely separate UI-menu tracing the same day (see `notes/ui-menu.md` for the full story, not needed
to act on this handout) found the actual main-CPU consumer of physical key/touch events for the menu
system: `key_event_resolve_and_route` (`0x2002ef98`, a genuinely massive ~2500-byte raw-input processor
called every tick from the main idle loop via `ui_input_poll_tick`). It reads a raw key-code byte from a
small fixed struct at `0x200301f2` (the code lives at offset `+4`), then resolves codes `1`-`0x1e`
through a lookup array (`g_key_code_to_command_id`, `0x2018d9a8`) into a numeric command ID, ultimately
dispatched through a genuinely major, previously-undocumented **279-entry system command table**
(`g_system_command_table`, `0x2018d9e0`) to a per-command handler. Concrete, verified facts from this:
key-code `9` (physical `MENU` button, per the "FRONT CHECK MODE" factory screen's numbered list — see
`notes/ui-menu.md`) resolves to command `0x11`; key-code `12` (`QUICK`) resolves to command `0x13`.

**The cross-check**: does `0x200301f2` (the struct `key_event_resolve_and_route` reads) have anything to
do with `0x203dcab6` (the confirmed `SCIF3` status buffer)? Checked directly with `references_to` on
both **raw addresses** (not symbol names — critical, since a `DAT_` name is often a pointer *variable*
whose stored value is the real address, as noted above). Result: **zero static references connect
them.** Also checked who writes `0x200301f2+4` (the raw key-code byte itself) — **no direct references
found either**, meaning it's very likely written via a computed/indexed address rather than a fixed
literal, the same class of gap this project has repeatedly hit (an address xref search alone won't find
it; either wider/manual searching or a live hardware capture would).

**So the real open question**: does button-press data reach the main CPU over `SCIF3` at all? Three real
possibilities, not yet distinguished:
1. `0x200301f2` gets populated by a **local GPIO key-matrix scan on the main board itself** — meaning
   physical buttons never touch `IC501`/`SCIF3` at all, and the RL78 handles something else entirely
   (the touchscreen digitizer chip, a sub-LCD, encoder debounce only, etc.).
2. There **is** a copy from the `SCIF3` buffer into `0x200301f2`, but through a computed/indirect path
   (e.g. a loop with a runtime-computed destination offset) that a simple address `references_to` can't
   surface — would need either a wider search (grep the whole disassembly for literal-pool references
   to `0x203dcab6`/`0x20037590`/`0x200301f2` beyond what Ghidra's own analysis caught, e.g. via
   `arm-none-eabi-objdump` against the raw `body.bin`, or `tools/superset_disasm.py`'s sqlite DB) or
   tracing every caller of `scif3_frame_rx_statemachine`/`_dispatch_by_type` forward more carefully.
3. The touchscreen and physical keys use genuinely **different** paths into `0x200301f2` (touch via one
   route, physical keys via `SCIF3` via another) — plausible given `key_event_resolve_and_route` clearly
   handles a wide code range (up to `0x1e`+ direct codes, plus separate touch-coordinate-shaped handling
   further in its body for codes ≥ `0x23`, per its full decompile in `notes/ui-menu.md`'s history).

**Concrete next steps, roughly in order of expected payoff**:
- Find the actual writer of `0x200301f2+4`. Try: (a) `tools/superset_disasm.py`'s existing
  `scratch/superset_142.sqlite` — query for `ldr`/`str` instructions whose PC-relative literal resolves
  near `0x200301f2` (the `target` column already resolves literal-pool loads); (b) a raw string/pattern
  search of `body.bin` for the literal word `0x200301f2` itself (objdump or the sqlite DB) to find every
  place it's loaded, not just what Ghidra's own xref analysis caught.
- Once found, check whether that writer's own inputs trace back to `scif3_frame_rx_statemachine`'s
  buffer, a GPIO port register (check `ic7300-hardware.md`/the RZ/A1H manual for candidate key-matrix
  pins), or something else.
- If it turns out to be local GPIO scanning: that's a real, valuable, load-bearing finding on its own —
  document it and reconsider whether `SCIF3`'s undecoded bits are worth pursuing further at all (they'd
  then only matter for whatever the RL78 *does* still own — display, touchscreen, etc., not basic keys).

## The firmware-image angle: watch for update-mechanism code

While reading through whatever front-panel-related code you find, keep an eye out for anything shaped
like the main CPU's **own** firmware-update mechanism, since finding an analogous one for `IC501` would
be strong, concrete evidence a real (currently unfound) front-panel firmware image exists somewhere.
`notes/firmware-update.md` documents the main-CPU pattern in full; the shapes worth pattern-matching
against are:

- **A chunked write loop**: main CPU's own `FUN_20024db8` reads up to 64 KB at a time from a file,
  memcmp's it against current flash content (skips unchanged blocks), erases, then programs. A front-
  panel analogue would look different in scale (RL78 flash is only 32 KB total, so likely one shot or a
  couple of chunks, not 64 KB blocks) but the same *shape*: read a chunk, compare/erase/program, repeat.
- **An orchestrator that reads size/offset header fields** and drives the above — main CPU's is
  `FUN_20025ae4`, reading `size1`..`size7` from the update container header. Look for anything in the
  update-container-reading code (`firmware_update_main` and its callees, `notes/firmware-update.md`) that
  branches on a "Front CPU" version-field mismatch (the update file's `+0xa4` field, per
  `FUN_200a94c8` — **already flagged as unchecked** in `notes/front-panel-firmware.md`'s open questions:
  is this field ever compared against something read *live* from `IC501` over `SCIF3`, or is it just
  cosmetic/unused?). That comparison, if it exists and actually triggers something, is likely the
  smoking gun.
- **A bootloader-entry or erase/program command sent *over* `SCIF3`** — i.e. one of the currently-
  undecoded packet "types" (`0x00`-`0x1F`) in `scif3_frame_dispatch_by_type` might itself be a
  "here's a firmware chunk, program it" command rather than a status/input report. Worth checking
  whether any outbound (main→front-panel) frame construction exists at all — everything traced so far
  is *inbound* (front-panel→main); an outbound direction with a chunked-write shape would be very
  telling.
- **If any of this turns up positive**, the next question is *where the image itself lives* — re-check
  the three container components already ruled out as DSP/FPGA (they're solidly identified by version-
  field correlation, unlikely to secretly also hold a 4th image), and reconsider files not yet fully
  accounted for: `base.dat` and the main body's own `chunk3` (already flagged in `multi-cpu-images.md` as
  having "zero found code references anywhere in the version checked" — worth a fresh look specifically
  for RL78 machine code shape, now that RL78 tooling is confirmed working, rather than assuming it's
  inert padding).

## Tooling reminders

- `ghidra` MCP server (themixednuts/GhidraMCP v0.8.0, see README.md) — standard `functions`/`symbols`/
  `inspect`/`memory` tools, no script-execution or context-register tool (checked and confirmed absent,
  don't re-check).
- `tools/arm_thumb_scan.py` + `tools/superset_disasm.py` (+ its `scratch/superset_142.sqlite` output) —
  for any ARM/Thumb disassembly-mode issues on the *main-CPU* side encountered while chasing this. If a
  real never-disassembled gap or wrong-mode region turns up, `tools/ghidra_scripts/FixArmThumbMode.java`
  applies the fix (reads `scratch/armthumb_fix_requests.txt`, one `<address> <length> <arm|thumb>` line
  per fix, applied by running the script in Ghidra's Script Manager — see `tools/README.md`).
- RL78 tooling (see "Hardware identity" above) — sitting ready, only useful once/if a real `IC501` image
  turns up.

## 2026-09-20 update: a real live traffic capture tool now exists, and it already caught new,
## undecoded outbound content — a strong, concrete next entry point into this handout's own
## still-open "type→offset→bit mapping" question (see above).

**New tool, built and working**: `qemu-machine/src/scif.c` now has a per-channel-selectable serial
bus logger (`RZA1H_DEBUG=scif<N>`, e.g. `scif3` for exactly this front-panel bus — `RZA1H_DEBUG=scif`
still means every channel, unchanged). It assembles TX bytes from `0xFE` to `0xFD` and logs one
readable line per complete outbound frame (`scif%u: TX frame type=0x.. [hex bytes] (N bytes)`), on
top of the existing per-byte log — no GDB, no perturbation risk, just the device model's own
`fprintf`. **Deliberately TX-only for now**: the channel-3/5 virtual responders (this file's own
`rza1h_scif3_frontpanel_ack`/`rza1h_scif5_...`) precompute their RX replies directly into guest RAM
rather than feeding them through `FRDR` byte-by-byte (real, hard-won timing the logger has no
business touching — see those functions' own comments), so a generic RX accumulator can't see full
reply frames the same way TX can; building that is a real, separate next step if the RX side ever
matters here.

**A first live capture already caught 4 distinct outbound frames during a plain auto-boot run**:
```
scif3: TX frame type=0xf0 [fe f0 fd] (3 bytes)      -- the already-known boot identify handshake
scif3: TX frame type=0xf1 [fe f1 fd] (3 bytes)      -- the already-known keepalive ping
scif3: TX frame type=0x00 [fe 00 00 00 00 00 67 80 90 a7 af c3 ca dc 86 aa 20 08 00 00 00 00 00
                            00 00 00 00 00 00 00 00 00 00 00 00 00 fd] (36 bytes)
scif3: TX frame type=0x01 [fe 01 01 fd] (4 bytes)
```
The last two are **new, real, undecoded content** — genuine payload bytes this project hasn't
traced the meaning of yet, sitting squarely inside this handout's own still-open "rest of the
type→offset→bit mapping" question from 2026-09-07.

**Where this data comes from, already narrowed down this same session (2026-09-20) via
`scif3_driver_pump_tick`'s own decompile — the complete, exhaustively-enumerated outbound driver,
only 3 real call sites into `scif3_send_frame` exist anywhere in the whole binary, all inside this
one function**:
```c
void scif3_driver_pump_tick(void)  // 0x200373ac
{
    ...
    if (bit 0x20 set) { scif3_send_frame(0, DAT_200375b4, 0x21); }       // <-- matches the type=0x00, 33-byte frame exactly (0x21=33)
    else if (bit 0x10 set) { /* dynamic type via FUN_20006308, a queue */ scif3_send_frame(computed_type, ...); }
    else if (bit 0x08 set) { scif3_send_frame(0x21, &local_18, 0x13); }  // already fully decoded: XOR-0x55 "ICOM INC. (C)2016"
    else { /* 0xF0/0xF1 handshake, already known */ }
}
```
The captured `type=0x00`/33-byte frame is almost certainly the `bit 0x20` case — the byte count
matches exactly (`0x21` = 33, plus `fe`/type/`fd` = 36 total, exactly what was captured). The
`type=0x01` 1-byte-payload frame doesn't obviously match any of the three literal cases above at a
glance — worth re-checking against the `bit 0x10` (dynamic-type) case specifically, since that one's
own type value is *computed*, not a fixed literal, and could plausibly resolve to `0x01` under some
condition.

**Concrete next-session plan, in order of expected payoff**:
1. **Resolve `DAT_200375b4`/`DAT_200375b8`'s live pointer values** (same technique used throughout
   this project: `xp /1xw <pool addr>` against a running boot) to find the *real* RAM address of the
   `bit 0x20` frame's 33-byte payload buffer, then find **who writes into that buffer** before the
   send (a `references_to` on the resolved live address, or on the pool addresses of any other
   symbol that shares the same underlying target — this project has hit "the same address reached
   via a different literal-pool slot, or via register-relative offset from a different base" more
   than once, so check both). That writer is very likely the actual "status/state we're telling the
   front panel about" producer — a strong, direct answer to "what does this frame mean" once found.
2. **Trace `FUN_2003754c`'s own `FUN_20006308`-driven queue** (the `bit 0x10` dynamic-type path) —
   what gets enqueued, by whom, and under what type values. This is also the path
   `power_state_pwrk_wait_and_bringup` itself uses (confirmed this session: called when entered with
   `civ_state==1`), so tracing it doubles as still-open groundwork for that thread too.
2b. Recall separately that `power_state_pwrk_wait_and_bringup` sends this frame then does a genuine
   RTOS wait-for-flag (`FUN_20062c1c` → `FUN_20187010(1, 0xffffffff)`, not a raw spin) on a byte at
   `DAT_2002a0dc+3` before continuing — if a fresh session wants to check whether this is a real,
   satisfiable wait or a latent hang risk under different conditions than this project's own test
   coverage so far, that flag's real setter is a related, not-yet-traced thread.
3. **Capture more of the picture live**: rerun `RZA1H_DEBUG=scif3` (or `scif3,scif0` etc. for a
   wider bus view) over a longer boot/idle window (including a `--hold-pwrk`-style long run, if the
   PWRK-wait branch's own frame differs from auto-boot's) to see whether new frame types appear once
   the system is further along (e.g. once real UI drawing might start — see the separate,
   still-open `qemu-machine/` VDC5-framebuffer thread from this same session) — more real captured
   traffic is cheap and directly narrows which of the undecoded `0x00`-`0x1F` types actually get
   used in practice, rather than needing to reason about all 32 in the abstract.
4. Cross-reference any newly-decoded type against `scif3_frame_dispatch_by_type`'s own **inbound**
   side (`DAT_20037590 + type` writes into the shared front-panel status struct at `0x203dcab6`) —
   if the main CPU ever sends a type it can also *receive*, that's a strong hint the same frame
   shape is used bidirectionally for that specific status, e.g. echoed/acknowledged state.

## 2026-09-20 follow-up: step 1 done — the `bit 0x20`/type=0x00 frame's real payload buffer is
## found, live, and two of its fields have real, named writers tied to SVC/service-mode state,
## not general key/encoder telemetry.

**Method**: booted a plain-auto-boot QEMU instance with `-qmp unix:/tmp/qemu_fp.sock,server,nowait`
(no GDB, zero perturbation risk, this project's established preference) and
`RZA1H_DEBUG=scif3`, captured the same four frames as before within the first ~10s, then read
`DAT_200375b4`/`DAT_200375b8`'s live stored pointer values directly via `xp /1xw` over QMP (see
the plain word-read snippet added this session, close cousin of `tools/qmp_read_mem.py` which
only does bytes).

**Result — three independent literal-pool symbols resolve to the exact same live address**,
`0x203dca54`, confirming this project's own standing "same address via a different `DAT_` slot"
warning applies here too:
- `DAT_200375b8` (`scif3_driver_pump_tick`'s memcpy source for the bit-0x20/type=0x00 frame)
- `DAT_20042688` (target of the newly-named `scif3_status_svcmode5_flag_set`/`_clear`)
- `DAT_20013090` (`iVar3` inside `scif1_svc_status_field_switch`)

A live 48-byte read at `0x203dca54` matched the earlier captured frame's payload byte-for-byte
at every offset except `+1`, which had advanced from `0x00` (at capture time, ~10s earlier in
the same boot) to `0x01` — direct, live confirmation this buffer is genuinely dynamic status,
not a static/config blob.

**Two fields now have real, named producers** (both renamed in Ghidra, plate comment added at
`0x203dca54` itself):
- **Offset+0** = an SVC-mode-5 (factory/service mode) active flag. Set to `1` by
  `scif3_status_svcmode5_flag_set` (`0x20041d8c`, was `FUN_20041d8c`), cleared by
  `scif3_status_svcmode5_flag_clear` (`0x20041dd8`). Callers: `svc_mode5_idle_loop` (name
  already existed from an earlier session — the "mode 5" naming lines up with the already-
  documented `boot_check_mode5_combo` MENU+FUNCTION factory-mode entry, see
  `notes/kernel-rtos.md`) and `scif1_svc_status_field_switch`'s own `case '2'`.
- **Offset+1** = written directly inside `scif1_svc_status_field_switch` (`0x20012ddc`, an
  **SCIF1** — the service-mode link, not SCIF3 — inbound command-byte dispatch switch): chars
  `'('`→`1`, `')'`→`4`, `'*'`→`2`, anything else resets it to `0`. **This is the exact field
  caught changing live during a plain, no-service-mode-interaction auto-boot** — meaning this
  SCIF1-side dispatcher's default/reset path runs during ordinary boot, not only under
  deliberate factory-mode entry.

**New, concrete, previously-unknown cross-link**: at least one `SCIF3` outbound status field is
populated from **`SCIF1`** (service-mode link) command handling, not from key/encoder/touch
input at all. This reframes part of the original "does button data reach the main CPU over
`SCIF3`" question from earlier in this handout — some of `SCIF3`'s outbound traffic is clearly
about *service-mode/status echo*, a separate concern from physical-input reporting, and the two
should probably be tracked as separate sub-questions from here on.

**A second, related producer path found but not fully traced**: `scif3_dynqueue_post_and_flush`
(`0x2002aed8`, was `FUN_2002aed8`) sets a bit in a *third*, still-unidentified struct
(`DAT_2002a10c+1 |= 1`) then calls `scif3_dynqueue_flush_sync` (`0x2003754c`, was
`FUN_2003754c` — the function this handout's own prior section flagged as "`FUN_2003754c`'s
`FUN_20006308`-driven queue"). Its real body is simpler than assumed: it just sets the
pump's `bit 0x10` flag and synchronously loops `scif3_driver_pump_tick()` + a queue-non-empty
check (`FUN_20037514`) until drained — a "post one item, then flush now" helper, not the
enqueue itself. **Not yet found**: what `DAT_2002a10c+1` actually is, or where the real
`FUN_20006308` enqueue call (the one matching pump_tick's own dequeue) lives — `scif3_dynqueue_
post_and_flush` doesn't call `FUN_20006308` directly, so something else must populate whatever
queue slot it's implicitly referring to. Worth a fresh look specifically for `FUN_20006308`'s
other call sites (only its dequeue use inside `scif3_driver_pump_tick` has been traced so far).

**Next-session plan, in order**:
1. Find `FUN_20006308`'s other callers (the actual enqueue side) — `references_to` on
   `0x20006308` directly, something this session didn't get to.
2. Resolve `DAT_2002a10c`'s live target and check whether it's a fourth alias for `0x203dca54`
   or a genuinely separate struct — same "resolve the pointer live" technique used here.
3. Trace `scif1_svc_status_field_switch`'s own 3 callers (`0x2001302c`/`0x20013048`/`0x2001306c`)
   to find where the inbound SCIF1 command byte (`*DAT_20013080`) actually originates — likely
   `scif1`'s own frame-dispatch-by-type analogue, not yet named/traced this session.
4. The original bytes-2-through-32 of the 33-byte buffer (the `67 80 90 a7 af c3 ca dc 86 aa 20
   08` block, then trailing zeros) are still completely unaccounted for — same live-pointer-then-
   `references_to` technique against `0x203dca54+2` etc. once the above threads are closed.

## 2026-09-20 follow-up #2, same day — the `bit 0x10` "dynamic-type queue" was never a queue: it's
## a delta/diff encoder, and this single finding fully explains the mystery `type=0x01` frame.

**What `references_to` on `0x20006308` turned up**: 4 call sites total. One is the already-known
`scif3_driver_pump_tick` use; the other 3 (`FUN_2000790c`, `FUN_2001a1ec`, `FUN_2001f7b4`) are in
a completely unrelated part of the binary and all feed into `FUN_2001e484`→`FUN_2001dcc4`, a
32-byte-chunked request/wait RPC that looks like NVRAM/EEPROM settings write-back (header fields
are literally offset+length+data, command code `0x14`, a real `FUN_20062c1c` RTOS wait for
completion) — **not traced further, flagged only so a future session doesn't re-confuse the two**.
`0x20006308` is a genuinely generic, widely-shared primitive, not a SCIF3-specific queue.

**Decompiling `0x20006308` itself settles what it actually does**, and it isn't a dequeue at all
— it's **memcmp-shaped**: walks two buffers forward while bytes match (trimming the common
*prefix*), then, if more than one differing byte remains, walks backward from the end trimming
the common *suffix* too. Returns the length of the genuinely-different middle range, with both
input pointers left pointing at where that range starts in each buffer. **Renamed to
`diffbuf_find_changed_range`.** (`0x2017c710`, used throughout this project's notes under that
raw name, is confirmed plain `memmove` — dest,param_1/src,param_2/len,param_3, handles overlap
both directions — renamed to `memmove_generic`.)

**What this means for `scif3_driver_pump_tick`'s bit-0x10 case, precisely**: `DAT_200375b8`
(`0x203dca54`, this handout's confirmed live status buffer) is the *current* value; `DAT_200375b4`
(`0x203dca75`) is a **last-sent shadow copy**, not a second producer. Each pump tick,
`diffbuf_find_changed_range` finds the smallest byte range that changed since the last send,
`memmove_generic` copies just that range from the live buffer into the shadow (bringing the
shadow up to date), and `scif3_send_frame` transmits it with **wire "type" = that range's byte
OFFSET into the buffer** — not a semantic command code as originally assumed.

**This directly and fully explains the originally-mysterious `type=0x01`, 1-byte-payload frame**
from the very first capture: it means "the byte at offset 1 changed, new value 0x01" — and offset
+1 is exactly the field this handout already traced to `scif1_svc_status_field_switch` (SCIF1
service-command dispatch), caught live going `0x00`→`0x01` in the same boot window. Every piece
now agrees: the SCIF1 dispatcher wrote `0x01` at offset+1, the very next pump tick diffed it,
and sent a targeted 1-byte `type=0x01` frame — captured, traced, and mechanistically closed in
one sitting.

**Ghidra state**: `diffbuf_find_changed_range` and `memmove_generic` renamed; plate comments on
both plus a refreshed one at `0x203dca54` itself (now correctly describing the shadow-buffer
relationship instead of calling `0x203dca75` a second producer). Saved.

**What's still open, roughly in order**:
1. Bytes 2-32 of the buffer (the `67 80 90 a7 af c3 ca dc 86 aa 20 08` block + trailing zeros) —
   unchanged from follow-up #1's own open item, now with a precise tool to chase it: **force or
   wait for one of those specific offsets to change, capture the resulting `type=<offset>` frame
   live, and cross-reference `references_to` on `0x203dca54+<offset>` the same way this session
   did for offsets 0 and 1.** Since the mechanism is now fully understood, decoding the rest of
   this buffer is a matter of repeating this exact technique per offset, not further reverse
   engineering of the send path itself.
2. `scif1_svc_status_field_switch`'s own 3 callers (`0x2001302c`/`0x20013048`/`0x2001306c`) —
   still not traced, would show where the inbound SCIF1 command byte itself originates.
3. The bit-0x20 case (always sends the full 33 bytes, no diffing) and bit-0x08 case (the
   already-fully-decoded `"ICOM INC. (C)2016"` XOR string) are both already understood and don't
   need further work.

## 2026-09-20 follow-up #3, same day — item 2 above was already solved, by an entirely separate,
## much older thread this handout had never cross-linked to. No new tracing needed, just a link.

Decompiling the 3 callers turned out to be unnecessary: `0x2001302c`/`0x20013048`/`0x2001306c` are
all inside `scif1_svc_command_dispatch` (`0x20012f5c`) itself — its own literal pool (dumped via a
plain `listing` call) resolves `DAT_20013080`→`0x20390050` (the real command-byte address),
`DAT_20013088`→`0x203dcab6` (**the already-confirmed SCIF3 inbound RX status struct** — a fourth
independent alias for it, on top of the three already known), and `DAT_20013090`→`0x203dca54`
(this handout's own outbound buffer, static-literal-confirmed, matching the earlier live-QMP
read exactly). So `scif1_svc_command_dispatch` **reads** from the SCIF3 RX struct and its
"finalize" step (`scif1_svc_status_field_switch`) **writes** into the SCIF3 TX/status buffer — a
real, concrete, bidirectional SCIF1↔SCIF3 link, not just the one outbound field found earlier.

**Who calls `scif1_svc_command_dispatch`, and where the real command byte comes from, turns out to
already be fully documented** in `notes/kernel-rtos-history.md`'s **"SCIF1 service-mode protocol"**
section (2026-08-29, 30th session, well before this handout existed) — a whole separate
investigation this front-panel thread simply hadn't been cross-referenced against until now.
Summary of what that section already established (see it directly for full derivation, not
repeated here): `scif1_svc_command_dispatch` is called only from `svc_mode_idle_loop`
(`0x20053270`, the *general* service-mode idle loop — a sibling of `svc_mode5_idle_loop`, not the
same function); `SCIF1` is a real, physically-confirmed **second CI-V-shaped calibration/self-test
link** (`P6_13`+`P7_12` RX, `P6_12` TX, reachable over the same USB `CP2102` bridge as CI-V, on
different pins); it's gated behind a system-wide "service mode" driven by `system_mode_request_
dispatch`, itself gated on `DAT_2002a158` (values `1`-`9`,`0xb`); and modes `6`/`7`/`8` specifically
inject a *synthetic* starting command via `scif1_svc_post_synthetic_command` at exactly this
handout's own three call sites — already found and named as a group by that 2026-08-29 session,
before this handout's own SCIF1 involvement was known. **Who ultimately writes `DAT_2002a158`
itself remains that section's own flagged "genuine dead end"** (real effort already spent: a
`references_to` sweep and a full-image literal-pool `objdump` grep, both zero hits — most likely
an RTOS message/event mechanism, not a direct write; that section's own next step (d) is "live
JTAG, once available, would likely resolve this quickly").

**The one genuinely new fact this session adds to that older thread**: it was flagged there as
"not proven connected to any documented feature" — this handout's own `scif3_status_svcmode5_flag_
set`/`scif1_svc_status_field_switch` finding (follow-up #1, above) now gives it exactly that: a
real, observable, external side-effect (an outbound status bit echoed to the front-panel MCU over
`SCIF3`). Cross-linked in both directions — see `kernel-rtos-history.md`'s own SCIF1 section for
the pointer back here.

**Net effect on this handout's own open-items list**: item 2 is closed (answer: solved by an older
thread, now linked, with one remaining sub-question — `DAT_2002a158`'s writer — inherited as a
shared open item with that section, not specific to the front panel). Items 1 (buffer offsets
2-32) and the `DAT_2002a10c`/dynqueue loose end from follow-up #1 remain the real next steps.

## 2026-09-20 follow-up #4, same day — item 1 (buffer offsets 2-32) genuinely doesn't yield to more
## live capture; a promising-looking lead was checked and ruled out; recording an honest negative
## result rather than leaving stale "just capture more traffic" framing in place.

**Two full live-capture windows, ~150s of real execution combined, zero new frame types**:
- Auto-boot branch, `RZA1H_DEBUG=scif3`, run to its own ~50s ring-overflow crash point (this
  project's own known, unrelated frontier — see `qemu-machine/README.md`'s Status section): still
  only the same 4 frames (`0xf0`/`0xf1` handshake, `type=0x00`, `type=0x01`) as the very first
  capture. No new offsets fired.
- **PWRK-wait branch, held for a full 100s of genuine post-boot idle** (`idle_loop_wfe_spin`, per
  [[icom-pwrk-handler-located]] — this branch doesn't hit the ring-overflow trap at all, so it's a
  much longer real window): same script pattern as `tools/vdc5_framebuffer_peek.py --hold-pwrk`
  (wait for PC `0x20029B18`, `qom-set pwrk-pressed=true` over QMP, no GDB). **Still exactly the
  same 4 frames, nothing more, across the entire 100s of idle.** This is a real, meaningful
  negative result, not just "wasn't captured yet" — during passive idle with no user interaction,
  bytes 2-32 of the status buffer genuinely never change.

**A promising-looking lead, checked and ruled out**: `scif3_frontpanel_init_and_latch_version`
(`0x2002af80`, an existing name from an earlier session) copies 12 bytes from `DAT_2002b4d8`
into `DAT_2002b4e4`, and the name alone looked like a strong candidate for "the thing that fills
in the static identity-shaped bytes 4-15." **Live-resolved and ruled out**: `DAT_2002b4d8` →
`0x203dcab6` (this project's own confirmed SCIF3 **inbound RX** struct, yet another independent
alias for it) and `DAT_2002b4e4` → `0x203dca96` — a *different* address, `0x42` bytes past this
handout's own `0x203dca54` TX buffer, entirely outside its tracked 33-byte span. So this function
does copy real identify-handshake data around at boot, but into a separate staging area, not into
the bytes this handout is trying to explain. Genuinely ruled out, not just unlikely.

**Working hypothesis for why no writer has been found, static or live** (not confirmed, offered
honestly as the most likely explanation rather than left unstated): bytes 2-32 may simply be this
global buffer's **compiled-in initial `.data` value** — content baked into the flash image and
bulk-copied to RAM once by generic C-runtime startup code (a ROM-to-RAM `.data` copy loop with a
runtime-computed destination, the same class of "computed address, not a literal `references_to`
can find" blind spot this project has hit repeatedly, e.g. `0x200301f2`'s key-code writer in this
handout's own original section) — rather than a discrete, semantically-named "producer" at all.
If true, these bytes may only ever change on **real hardware**, in response to a real front-panel
reply this emulation's own RX responder (which precomputes replies directly into guest RAM rather
than modeling genuine byte-by-byte `FRDR` traffic, per this handout's own tooling notes) doesn't
faithfully reproduce — meaning no amount of further live QEMU capture would find it either.

**Next steps if this specific thread continues, in order of expected payoff**:
1. **A GDB hardware watchpoint on `0x203dca54`-`0x203dca75` (write access) during a full boot**,
   via `tools/gdbrsp.py`'s existing breakpoint primitives — genuinely not yet tried for this
   buffer. A watchpoint catches indirect/computed writes a static `references_to` sweep can't, so
   this is a real, different technique from everything tried so far, not a repeat.
2. ~~Check `flash.bin` directly for whether `0x203dca54`'s known static content appears as literal
   bytes in the image's own `.data` initializer region~~ — **done, inconclusive, not a real
   answer**: a raw byte-string search of `flash.bin` for `67 80 90 a7 af c3 ca dc 86 aa 20 08`
   found nothing. Weak evidence at best either way — the very first capture this whole thread ever
   took was already several hundred ms into boot (`t=90835.84` vs. QEMU's own `t=0`), so plenty of
   early init code could already have written this value before any capture window existed; a
   `.data`-copy loop can also legitimately not appear as one contiguous literal run (compiler-
   chosen field order, word-at-a-time copies, etc.). **Does not disprove the compiled-in-default
   hypothesis**, just failed to cheaply confirm it — item 1 (a real GDB watchpoint from the very
   first instruction) is the only technique here that could actually settle this.
3. If a watchpoint from true `t=0` still finds no write, this specific sub-question is genuinely
   gated on real hardware (JTAG or a live capture against the real `IC501` link) rather than
   anything further this emulation-based approach can resolve — worth saying so plainly rather
   than re-attempting more live QEMU captures, which this session's own two full runs already show
   don't help.
