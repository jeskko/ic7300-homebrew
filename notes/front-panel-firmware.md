# Front-panel MCU (`IC501`) firmware — location unknown, open again

See [notes/front-panel-firmware-history.md](front-panel-firmware-history.md) for the full narrative,
including a full RL78-disassembly investigation into a file that turned out to be the wrong one — the
tooling built along the way remains valid and reusable, just not yet pointed at a confirmed target.

**Picking this thread back up? Read
[notes/front-panel-protocol-handout.md](front-panel-protocol-handout.md) first** — a dedicated onboarding
doc (2026-09-07) covering where to start and what to watch for that would point at a real (still unfound)
front-panel firmware image. **Its top-priority question (do physical buttons reach the main CPU over
`SCIF3`?) is now resolved — yes — see the Open questions section below**; the handout also contains a
now-corrected address typo (`0x200301f2` should read `0x203901f2`) flagged there too.

## Current state

- **`IC501` = `R5F104LCAFB`**, Renesas **RL78/G14** (64-pin LQFP), the Display/Front Unit's MCU — see
  `notes/ic7300-hardware.md`. Renesas's own datasheet confirms 32 KB code flash / 4 KB data flash / 4 KB
  RAM.
- **The main-CPU side of the link to this MCU is fully reverse-engineered**: `SCIF3` (base `0xE8008800`,
  pins `P6_0`/`P6_1`), a 33-byte packet-framed UART, `0xFE`/`0xFD` byte framing (same template as
  CI-V/`SCIF0` and the service-mode link/`SCIF1`). `scif3_frame_rx_statemachine`/
  `scif3_frame_dispatch_by_type` parse frames into a shared front-panel status buffer;
  `boot_check_mode1_combo`/`boot_check_mode5_combo` (in `cold_boot_hw_init`) read buffer offset `0xd` bits
  3/4 to detect the MENU+FUNCTION service-mode combo. See `notes/kernel-rtos.md`'s "Factory/service mode"
  section.
- **Confirmed, 2026-09-07: physical button presses do reach the main CPU over this `SCIF3` link.**
  `scif3_key_bitfield_scan_and_resolve` (`0x2002fbc8`, called every tick from `ui_input_poll_tick`) diffs
  live bytes at the `SCIF3` buffer's offset `0xd`-`0x11` against a shadow copy, resolves the changed bit
  through `g_scif3_bit_to_keycode_table` (`0x2018d980`), and feeds the result to
  `key_event_resolve_and_route`'s own key-code struct — verified against the already-known `MENU`/`QUICK`
  key codes. See the Open questions section and `notes/front-panel-firmware-history.md` for the full trace.
- **`front_cpu.bin` is *not* front-panel firmware — retracted.** It was extracted from the v1.42 update
  container's "component0" under a working hypothesis it was `IC501`'s image; two independent checks ruled
  that out: its content is 134,272 bytes, over 4x the RL78/G14's entire 32 KB flash capacity, and the
  transport code that sends this exact chunk during an update is hard-wired to the `SCIF5`/DSP path with no
  `SCIF3` (front-panel) branch anywhere. A follow-up disassembly cross-check (mainline `tic6x-objdump` and
  Capstone `CS_ARCH_TMS320C64X`, agreeing byte-for-byte) positively confirms it's genuine **TMS320C674x
  (DSP)** object code instead. See [[multi-cpu-images]].
- **RL78 tooling stood up and confirmed working, ready for whenever real front-panel firmware turns up**:
  [xyzz/ghidra-rl78](https://github.com/xyzz/ghidra-rl78) (Ghidra processor-module extension, installs
  cleanly into this Ghidra 12.1.2, no compiled Java) and `rl78-objdump`/`rl78-readelf` (mainline GNU
  binutils, built from plain upstream source — recipe in the history file). Neither has been pointed at a
  confirmed real target yet.

## Open questions

- **Where real `IC501` firmware actually lives, if the update container carries it at all.** Not yet
  checked: whether the update file's "Front CPU" version field (`FUN_200a94c8`'s `+0xa4`) is ever compared
  against anything read live from `IC501` itself (e.g. over `SCIF3`) rather than just assumed — this is the
  natural next thread, not more RL78 disassembly of a file already ruled out.
- Whether `IC501` is field-updated at all, or factory-programmed once and never touched by this mechanism.
- **The exact `SCIF3` type→offset→field mapping (which bit is `MENU`, which is `FUNCTION`, etc.) is still
  not fully decoded**, but a real, verified piece of it now exists: see the resolved item below —
  buffer offset `0xd`-`0x11` (bits 0-39) is a physical-key bitfield, and `g_scif3_bit_to_keycode_table`
  gives at least 2 of the 40 bit→key-code mappings directly (bit 16 = `MENU`, bit 23 = `QUICK`); the
  rest of that table (`0x2018d980`, 40 bytes) is straightforward to read off for the remaining bits.
- **Resolved, 2026-09-07 — physical button presses DO reach the main CPU over `SCIF3`.** The previous
  entry here (below, kept for the record) asked whether `key_event_resolve_and_route`'s key-code struct
  connects to the confirmed `SCIF3` status buffer at all, since a same-day `references_to` search on both
  addresses found zero links. Two things had been masking the connection: **(a) a transcription typo** —
  the struct address is `0x203901f2`, not `0x200301f2` as first recorded (no symbol exists at the latter;
  every search run against the wrong address necessarily came back empty), and **(b) genuine indirection**
  — the actual copy happens through `scif3_key_bitfield_scan_and_resolve` (`0x2002fbc8`, called every tick
  from `ui_input_poll_tick` right before `key_event_resolve_and_route`), which diffs 5 live bytes read
  straight from the `SCIF3` buffer (via a pointer confirmed to hold `0x203dcab6`) against a shadow copy,
  resolves the changed bit through a lookup table, and writes the result into the key-code struct.
  Verified against already-known ground truth, not just plausible-looking: the lookup table's byte at
  bit-index 16 is `0x09` (`MENU`) and at bit-index 23 is `0x0c` (`QUICK`), matching `notes/ui-menu.md`'s
  independently-confirmed key codes. Full derivation, renamed functions, and the one still-open loose end
  (a second, not-yet-fully-understood fallback scan in `FUN_2002fa9c`, possibly the encoder/dial) are in
  `notes/front-panel-firmware-history.md`'s "Priority-1 handout question resolved" section.
- *(Superseded by the entry above — kept for the record, not a live question anymore)* New, sharper
  question surfaced 2026-09-07 as a side effect of unrelated UI-menu tracing (`notes/ui-menu.md`): found
  the actual main-CPU-side consumer of physical key/touch events (`key_event_resolve_and_route`,
  `0x2002ef98`) reading a raw key-code byte from a small fixed struct, apparently disconnected from the
  confirmed `SCIF3` status buffer — raised the question of whether button-press data reaches the main CPU
  via `SCIF3` at all. See the resolved entry above for the answer.
