# Front-panel MCU (`IC501`) firmware — location unknown, open again

See [notes/front-panel-firmware-history.md](front-panel-firmware-history.md) for the full narrative,
including a full RL78-disassembly investigation into a file that turned out to be the wrong one — the
tooling built along the way remains valid and reusable, just not yet pointed at a confirmed target.

**Picking this thread back up? Read
[notes/front-panel-protocol-handout.md](front-panel-protocol-handout.md) first** — a dedicated onboarding
doc (2026-09-07) covering exactly where to start, a real open question about whether physical buttons
even reach the main CPU over `SCIF3`, and what to watch for that would point at a real (still unfound)
front-panel firmware image.

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
  not decoded** — flagged as the concrete next step back in the 30th session (see `scif3_frame_dispatch_by_type`'s
  own header comment) and still true.
- **New, sharper question surfaced 2026-09-07 as a side effect of unrelated UI-menu tracing
  (`notes/ui-menu.md`)**: found the actual main-CPU-side consumer of physical key/touch events
  (`key_event_resolve_and_route`, `0x2002ef98`) — it reads a raw key-code byte from a small fixed
  struct at `0x200301f2` (offset `+4`), completely separate from the confirmed `SCIF3` front-panel
  status buffer at `0x203dcab6`. Checked directly with `references_to` on the *raw addresses* (not
  just symbol names, learning from this session's own earlier alias mistakes): zero static references
  connect the two. Confirmed `MENU`=key-code `9`→command `0x11` and `QUICK`=key-code `12`→command
  `0x13` via a lookup array (`g_key_code_to_command_id`, `0x2018d9a8`) feeding a 279-entry system
  command table — a real, working, *independent* path to "which command did this button trigger,"
  useful in its own right, but it does **not** confirm or use the `SCIF3` byte-level protocol at all.
  This raises a real, previously-unasked question: **does button-press data reach the main CPU via
  `SCIF3` at all, or is `0x200301f2`'s raw-code byte populated some other way** (a local GPIO key-matrix
  scan on the main board, a different serial link, or an indirect/computed copy from the `SCIF3` buffer
  that a simple address xref wouldn't catch)? The writer of `0x200301f2+4` itself wasn't found either
  (no direct references — likely written via a computed/indexed address, same class of gap this
  project has hit before). Not resolved — a real next step if this thread is picked up, and higher
  leverage than the type→bit mapping question above, since it would settle whether that mapping work
  is even the right place to look for physical-button semantics.
