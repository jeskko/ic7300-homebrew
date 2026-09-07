# Handout: front-panel (`IC501`) protocol and firmware-image thread

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
