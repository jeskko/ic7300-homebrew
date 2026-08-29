# Front-panel MCU (`IC501`) firmware — handoff, not yet started

Picking up a new thread after the DSP-firmware-location work in [[multi-cpu-images]]. This file starts
empty of findings on purpose — it's a **handoff plan**, written before any real analysis of the front-panel
firmware itself. Read this section first when starting that work; append real findings below it as a new
dated section, same convention as every other note file in this project.

## What's already known, don't re-derive

- **`IC501` = `R5F104LCAFB`**, a Renesas **RL78/G14** MCU (64-pin LQFP), the Display/Front Unit's MCU —
  the "SX3765" mystery from an early session, resolved via the service manual's parts list and
  independently visible on the Front Unit schematic. See [[icom-ic7300-re-project]]'s memory-file history
  and `notes/ic7300-hardware.md`.
- **Full physical pinout already supplied by the user**: 8 buttons on direct individual GPIOs `P70`-`P77`
  (including `MENUK`=`P73`=MENU, confirmed elsewhere `P74`/`SCPEK`=FUNCTION per a schematic-silkscreen
  correction), a 16-button resistor-multiplexed matrix on 4 pins, and two quadrature dial-encoder pairs. See
  `notes/kernel-rtos.md`'s "Factory/service mode" section for the exact citation.
- **The main-CPU side of the link to this MCU is fully reverse-engineered already**: `SCIF3` (base
  `0xE8008800`, pins `P6_0`/`P6_1`), a 33-byte packet-framed UART, `0xFE`/`0xFD` byte framing (same shared
  template as CI-V/`SCIF0` and the service-mode link/`SCIF1`). Key functions already named in Ghidra:
  `scif3_frame_rx_statemachine`, `scif3_frame_dispatch_by_type` (dispatches complete frames by type byte
  into a shared front-panel status buffer). `boot_check_mode1_combo`/`boot_check_mode5_combo` (in
  `cold_boot_hw_init`) read specific bits of that status buffer to detect the documented MENU+FUNCTION
  service-mode combo — buffer offset `0xd` bit 3 = MENU, bit 4 = FUNCTION, confirmed exactly. See
  `notes/kernel-rtos.md`'s "Factory/service mode" section for the full derivation — this is the natural
  anchor point for orienting inside the RL78 disassembly (find the code building/parsing the *same* framed
  protocol from the MCU's own side).
- **The firmware image itself is now extracted, decompressed, and file-offset-verified** (2026-08-29,
  same session as this handoff): `scratch/unpacked/142/front_cpu.bin`, 163,592 bytes, produced by
  `tools/icom_fw/dsp_chunks.py` from the v1.42 update container's "component0" (file offset `0x252c2c`,
  compressed `0x17e46`→decompressed `0x27f08` bytes, LZSS, MD5-trailer-verified against the real
  container). **"Front CPU" is a working-hypothesis label for this component** (component ordering matches
  the `FUN_200a94c8` version-field order, not yet independently confirmed) — worth keeping a small amount
  of skepticism about until the RL78 disassembly itself confirms it (e.g. by finding recognizable
  SCIF3-protocol-mirroring code).
- **Not yet examined at all**: no byte of `front_cpu.bin` has been disassembled or even manually decoded.
  Everything below is preparation, not findings.

## Toolability — checked concretely, better news than the DSP case

Unlike the DSP (TMS320C6745, confirmed **no** disassembler support anywhere — see [[multi-cpu-images]]'s
"DSP firmware precisely located" section), RL78 has real, if unofficial, Ghidra support:

- **[xyzz/ghidra-rl78](https://github.com/xyzz/ghidra-rl78)** — a community RL78 processor module.
  Per its own README: disassembles test binaries to a good degree of accuracy, has known bugs but "mostly
  functions"; the decompiler handles basic control flow but has limitations (notably switch-idiom
  handling), and some less-common instructions are still unimplemented (none of the most common ones,
  per the README). **Not yet installed or tried in this environment.**
- **[hedgeberg/RL78_sleigh](https://github.com/hedgeberg/RL78_sleigh)** — a second, independent RL78
  SLEIGH implementation. Not evaluated at all; worth trying if `ghidra-rl78` turns out to be too rough.
- Checked and confirmed **absent** on this machine: no `rl78-elf-objdump`/binutils target, no RL78 support
  in Capstone (not even installed). So a Ghidra extension (one of the two above) is the realistic path —
  there's no simpler CLI-tool fallback like there sometimes is for other architectures.

## Concrete next steps, in priority order

1. **Install `xyzz/ghidra-rl78`** into this Ghidra instance (`/opt/ghidra/Ghidra/Processors/`) and confirm
   it actually loads (appears as a language choice on import, no plugin-registration failure — this
   project has already hit one dead community-extension install once before, for a different tool, see
   [[icom-ic7300-re-project]]'s "Dead end, already tried" entry; don't assume success, verify it directly).
   If it fails to register, try `hedgeberg/RL78_sleigh` instead before giving up on Ghidra entirely.
2. **Import `scratch/unpacked/142/front_cpu.bin`** as a new Ghidra program with the RL78 language selected.
   Run auto-analysis and do the same empirical sanity check already used for the DSP chunks: does it find
   real functions/instructions, or nothing (which would mean either the module doesn't work well enough, or
   this isn't a plain flash-image-at-offset-0 layout)?
3. **Nail down the image's real base address before trusting anything else.** RL78 typically has its
   reset/interrupt vector table at fixed low flash addresses (conventionally starting at `0x00000`) — check
   whether `front_cpu.bin`'s first ~0x80 bytes look like a plausible RL78 vector table (a sequence of
   2-byte addresses, mostly pointing into a sensible code range) before assuming byte 0 of this file ==
   RL78 address 0. The file's exact size (163,592 B) doesn't match a round RL78/G14 flash-size number
   (checked: 128 KB/192 KB don't match either) — this is very plausibly a "used code only" extract from a
   larger flash part, not a full memory-size dump, so don't assume simple padding conventions without
   checking. Pull the real RL78/G14 datasheet (`R01DS0053...`, found via web search this session, not yet
   read) for `R5F104LC`'s exact flash size and vector-table layout before guessing further.
4. **Cross-reference against the already-solved `SCIF3` protocol as the fastest orientation anchor.** Once
   disassembly produces *anything* readable, search for: the same `0xFE`/`0xFD` frame-start bytes as
   literal constants; a UART/serial peripheral init sequence; and — most valuable — code that reads the
   MENU (`P73`) and FUNCTION (`P74`) GPIO pins and packs them into a status byte matching the exact bit
   positions (`0xd`, bits 3/4) `boot_check_mode1_combo` already expects on the main-CPU side. Finding that
   handshake from both ends would be strong, fast confirmation the RL78 side is being read correctly.
5. **Look for the two ASCII fragments already spotted** in `front_cpu.bin` during the DSP-chunk forensic
   pass: `"TIPAcYSX"` near the very start (meaning unclear, not investigated), and a trailing plain-ASCII
   tag `"31101070"` right before EOF (padded with `0xFF` before it — shaped like a build/version stamp,
   the same convention `dsp_program.bin` shows with its own trailing `"20001000"`). Once real disassembly
   exists, check what (if anything) references these regions.
6. **Once oriented, natural first real targets**: the reset handler and interrupt vector table; the
   button/encoder scan routine (should correlate directly with the user-supplied `P70`-`P77` direct-GPIO +
   16-button matrix + 2 encoder pairs pinout); whatever builds outgoing `SCIF3` frames to send key/encoder
   events to the main CPU.

## Expectations going in

Treat this like the DSP thread, not like a quick lookup: a brand-new architecture, a community tool of
unknown real-world quality, and zero bytes examined so far. The first session on this thread should expect
to spend real effort just on "does the tool work at all" and "where does byte 0 of the file map to in the
real address space" before any actual firmware logic gets read — don't expect the SCIF3 cross-reference
payoff (step 4) on the very first pass.
