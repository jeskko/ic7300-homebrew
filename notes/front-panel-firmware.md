# Front-panel MCU (`IC501`) firmware — location unknown, open again

See [notes/front-panel-firmware-history.md](front-panel-firmware-history.md) for the full narrative,
including a full RL78-disassembly investigation into a file that turned out to be the wrong one — the
tooling built along the way remains valid and reusable, just not yet pointed at a confirmed target.

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
