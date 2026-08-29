# Front-panel MCU (`IC501`) firmware — session history

Full narrative and evidence trail behind [notes/front-panel-firmware.md](front-panel-firmware.md),
which carries only the current-state summary and open questions. Kept verbatim, including the RL78
tooling investigation that turned out to be pointed at the wrong file — it remains sound and reusable
groundwork for whenever real front-panel firmware is located.

## Original handoff (written before any real analysis)

Picking up a new thread after the DSP-firmware-location work in [[multi-cpu-images]]. This started as
a **handoff plan**, written before any real analysis of the front-panel firmware itself.

### What was already known going in

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
  `notes/kernel-rtos.md`'s "Factory/service mode" section for the full derivation.
- **The firmware image itself was extracted, decompressed, and file-offset-verified** (2026-08-29):
  `scratch/unpacked/142/front_cpu.bin`, 163,592 bytes, produced by `tools/icom_fw/dsp_chunks.py` from
  the v1.42 update container's "component0" (file offset `0x252c2c`, compressed `0x17e46`→decompressed
  `0x27f08` bytes, LZSS, MD5-trailer-verified against the real container). **"Front CPU" was a
  working-hypothesis label for this component** (component ordering matched the `FUN_200a94c8`
  version-field order, not independently confirmed) — this hedge turned out to matter, see below.

### Toolability, checked concretely — better news than the DSP case (at the time)

Unlike the DSP (TMS320C6745, confirmed **no** disassembler support anywhere — see [[multi-cpu-images]]'s
"DSP firmware precisely located" section), RL78 has real, if unofficial, Ghidra support:

- **[xyzz/ghidra-rl78](https://github.com/xyzz/ghidra-rl78)** — a community RL78 processor module. Per
  its own README: disassembles test binaries to a good degree of accuracy, has known bugs but "mostly
  functions"; the decompiler handles basic control flow but has limitations (notably switch-idiom
  handling), and some less-common instructions are still unimplemented (none of the most common ones,
  per the README).
- **[hedgeberg/RL78_sleigh](https://github.com/hedgeberg/RL78_sleigh)** — a second, independent RL78
  SLEIGH implementation, kept as a fallback, not evaluated.
- Checked and confirmed **absent** on this machine: no `rl78-elf-objdump`/binutils target (at the time —
  see below, this changed), no RL78 support in Capstone.

### Tooling stood up, both paths work; file has a header, not raw code at offset 0 (2026-08-29)

Both toolability questions from the handoff resolved, better than expected:

- **`xyzz/ghidra-rl78` installs and loads cleanly in this Ghidra (12.1.2).** No root needed — it's a
  processor-module directory (`Module.manifest` + `data/languages/{rl78.ldefs,pspec,cspec,slaspec}`, no
  compiled Java at all), so it drops straight into the per-user extensions dir
  (`~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/rl78/`), the same place `GhidraMCP` already lives. On
  headless import (`analyzeHeadless ... -processor RL78:LE:16:default`), Ghidra auto-compiled the 224 KB
  `.slaspec` with only a benign warning ("7 NOP constructors found") and import/analysis succeeded. Real
  disassembly/decompilation testing not done yet — this only confirms the module *loads*, not that its
  instruction semantics are trustworthy.
- **Better find: RL78 needs no community tool at all for disassembly — mainline GNU binutils has had an
  official `rl78-dis.c` in `opcodes/` for years.** No Renesas-specific toolchain, no packaging on this
  machine either, but building it from plain upstream source was trivial: fetched `binutils-2.44` from
  `ftp.gnu.org`, `configure --target=rl78-elf --disable-nls --disable-werror --disable-gdb --disable-sim
  --disable-gprof`, then `make all-libiberty all-bfd all-opcodes all-binutils` — clean build, ~2 minutes,
  no patches needed. Installed at `~/.local/rl78-binutils/bin/{rl78-objdump,rl78-readelf}` (outside the
  repo — reproducible from the recipe above, not committed as a binary blob). Usage:
  `rl78-objdump -m rl78 -b binary -D front_cpu.bin`. This was the primary disassembly tool for this
  thread while it was believed active — official upstream code, not a reverse-engineered guess.
- **Important correction to the handoff's assumption: `front_cpu.bin` is *not* a raw code-at-offset-0
  image.** Linear disassembly from byte 0 with `rl78-objdump` decodes the `"TIPAcYSX"` region as nonsense
  instructions (e.g. `mov e, #73` / `mov x, #65` off literal ASCII bytes) — confirming these are header
  bytes, not code. The first ~0x22 (34) bytes look like a small structured header: magic `"TIPAcYSX"` (8
  bytes) followed by a run of small integers and **three repeats of the 3-byte tag `"YSX"`** at offsets
  0x05, 0x09, and 0x1d — shape never understood (record/section markers? length-prefixed fields? too
  little data to tell, and moot once the file was reclassified — see below).
- **Working hypothesis at the time, never confirmed: real code starts around offset 0x22.** Byte 0x23
  decoded as `subw sp, #123` (reserving stack space) right after a single odd byte at 0x22 — a plausible
  function-prologue shape — but the following stream wasn't unambiguously clean (e.g. `or a, [hl+108]`
  recurring identically at 0x2c and 0x42, which could be a real repeated field access or could mean the
  alignment was still off by one or more bytes). This was explicitly flagged as unconfirmed pending the
  real vector-table location and RL78/G14 datasheet — that check is what overturned the whole premise.

## This file's entire premise overturned, same day (2026-08-29)

Two independent lines of evidence both pointed away from `front_cpu.bin` actually being `IC501`'s
firmware:

1. **The datasheet number checks out, and it's a real contradiction, not a rounding error.** Renesas's own
   part page for `R5F104LCAFB` confirms 32 KB code flash / 4 KB data flash / 4 KB RAM. `front_cpu.bin`'s
   actual content (past the ~34-byte header, before the trailing `0xFF` pad) runs to `0x20c80` —
   **134,272 bytes, over 4x the entire chip's flash capacity.** Byte-entropy profiling and a zoomed 1bpp
   bitmap render (prompted by the user spotting visible periodicity in GIMP) ruled out "it's actually
   graphics/font assets" as the explanation — no recognizable local image content anywhere, just a genuine
   ~16-32 byte statistical periodicity sustained across the whole content region, more consistent with
   dense code/data than a picture. The entropy profile itself sits closer to `dsp_program.bin`'s dense,
   uniform VLIW-code signature than to confirmed-real ARM code in `body.bin`.
2. **The main-CPU update mechanism itself doesn't support the label.** Traced `chunk_transport_send_data`
   (the function that sends this exact chunk, index 0, during a firmware update) all the way down through
   `dsp_page_transfer_verify` and the ring-buffer push helpers — every one of them is hard-wired to the
   `SCIF5`/DSP transport (`scif5_send_and_wait_reply`, `scif5_ring_push_word`), with **no branch anywhere
   that picks the front-panel `SCIF3` link for chunk 0**. See [[multi-cpu-images]]'s "The 'chunk 0 = Front
   CPU' label is now actively doubtful" section for the full trace — this is the same finding from the
   main-firmware side that independently corroborates the forensic finding above.

**Working conclusion, upgraded from "probably not" to "positively looks like" (same day, continued)**:
pointed the DSP thread's newly-built disassemblers (`tic6x-objdump -EL`, Capstone `CS_ARCH_TMS320C64X` —
see [[multi-cpu-images]]'s DSP disassembly section) at `front_cpu.bin` and got a strong, cross-validated
positive result: real C67x+/C674x floating-point instruction sequences
(`spdp`/`dpsp`/`absdp`/`cmpltdp`/`mpydp`/`adddp`/`intspu`, used in coherent order), repeated correct
`mvk`/`mvkh` 32-bit-constant-building pairs, a genuine local backward branch to an in-range address, and an
`addkpc` whose encoded target is exactly `self+4` — the textbook call/return-address idiom. Two independent
disassemblers (binutils and Capstone) agree byte-for-byte on decoded immediates at the same addresses.
**`front_cpu.bin` now looks like genuine TMS320C674x object code**, not RL78 code, not graphics, and not
just a vague "some other DSP blob" — a specific, positively-supported identity. Full detail in
[[multi-cpu-images]]'s "Pointed the new DSP disassemblers at `front_cpu.bin`" section.

The RL78 tooling stood up in the first half of this investigation (Ghidra `xyzz/ghidra-rl78` module,
`rl78-objdump` from stock binutils) is still sound and reusable for whenever real front-panel firmware is
located — but **this specific file is not that**, and shouldn't be pointed at with that expectation
anymore. The plausible-looking RL78 disassembly patterns found earlier (call-target convergence, sane
branch offsets) were real but weak evidence next to this — a sufficiently dense binary blob run through a
permissive variable-length CISC decoder can produce locally-plausible-looking sequences by chance over a
small sample; they don't outweigh the specific, cross-validated C674x floating-point evidence found since.

**Genuinely open, not resolved**: whether real `IC501` firmware exists anywhere in the update container
under a different mechanism, or whether it's provisioned some other way entirely (factory-programmed once,
never updated over this path?). Worth checking whether the update file's "Front CPU" version field
(`FUN_200a94c8`'s `+0xa4`) is ever compared against anything read from `IC501` itself (e.g. over `SCIF3`)
rather than assumed — that would be the natural next thread, not a continuation of the RL78-disassembly
work done above.
