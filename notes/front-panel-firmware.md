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

## 2026-08-29: tooling stood up, both paths work; file has a header, not raw code at offset 0

Both toolability questions from the handoff are resolved, and better than expected on both counts.

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
  no patches needed. Installed at `~/.local/rl78-binutils/bin/{rl78-objdump,rl78-readelf}` (outside the repo
  — reproducible from the recipe above, not committed as a binary blob). Usage:
  `rl78-objdump -m rl78 -b binary -D front_cpu.bin`. This is now the primary disassembly tool for this
  thread — it's official upstream code, not a reverse-engineered guess, so its instruction decoding can be
  trusted the way Ghidra's community module can't yet.
- **Important correction to the handoff's assumption: `front_cpu.bin` is *not* a raw code-at-offset-0
  image.** Linear disassembly from byte 0 with `rl78-objdump` decodes the `"TIPAcYSX"` region as nonsense
  instructions (e.g. `mov e, #73` / `mov x, #65` off literal ASCII bytes) — confirming these are header
  bytes, not code, consistent with the ASCII-fragment observation in the original handoff (step 5). The
  first ~0x22 (34) bytes look like a small structured header: magic `"TIPAcYSX"` (8 bytes) followed by a
  run of small integers and **three repeats of the 3-byte tag `"YSX"`** at offsets 0x05, 0x09, and 0x1d —
  shape not understood yet (record/section markers? length-prefixed fields? too little data to tell).
  **Not yet checked**: whether this header shape matches anything already seen elsewhere in this project
  (the DSP chunks, `dsp_chunks.py`'s own container parsing) — worth a quick diff/grep before treating it as
  front-panel-specific.
- **Working hypothesis, not confirmed: real code starts around offset 0x22.** Byte 0x23 decodes as `subw
  sp, #123` (reserving stack space) right after a single odd byte at 0x22 — a plausible function-prologue
  shape — but the following stream isn't unambiguously clean (e.g. `or a, [hl+108]` recurring identically
  at 0x2c and 0x42, which could be a real repeated field access or could mean the alignment is still off by
  one or more bytes somewhere in between). **Do not treat 0x22 as confirmed** — this needs the real vector
  table location and the RL78/G14 datasheet (step 3 of the original plan, still not done) before trusting
  any specific offset as the code start.

### Immediate next steps (supersedes old steps 1-2, which are now done)

1. Read the `R5F104LC` datasheet section on flash memory layout/vector table (already found via web search
   last session, not yet actually read) and use it to sanity-check the 0x22 hypothesis and figure out
   whether the ~34-byte header has a recognizable Renesas or Icom-specific shape.
2. Try `rl78-objdump` with a few different candidate start offsets/alignments around 0x20-0x24 and compare
   against what a genuine RL78 vector table / reset handler should look like once the datasheet is read.
3. Only then resume the original steps 4-6 (SCIF3 cross-reference, the two ASCII-fragment follow-ups, reset
   handler / button-scan hunting) — those all still stand as written above.

## 2026-08-29, same day — this file's entire premise is now in doubt, don't build further on it yet

Two independent lines of evidence from the same day both point away from `front_cpu.bin` actually being
`IC501`'s firmware:

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

**Working conclusion**: `front_cpu.bin` is very likely **not** `IC501`'s firmware. It's more plausibly a
third piece of DSP-side data (perhaps a second partition of the DSP's own external SPI boot flash,
`IC902`). The RL78 tooling stood up in the first half of this file (Ghidra `xyzz/ghidra-rl78` module,
`rl78-objdump` from stock binutils) is still sound and reusable — but **don't keep pointing it at
`front_cpu.bin` expecting front-panel firmware** until this is resolved. The plausible-looking RL78
disassembly patterns found earlier in this file (call-target convergence, sane branch offsets) are real but
weak evidence next to this — a sufficiently dense binary blob run through a permissive variable-length CISC
decoder can produce locally-plausible-looking sequences by chance over an 8 KB sample; they don't outweigh
two independent, code-level/datasheet-level findings.

**Genuinely open, not resolved**: whether real `IC501` firmware exists anywhere in the update container
under a different mechanism, or whether it's provisioned some other way entirely (factory-programmed once,
never updated over this path?). Worth checking whether the update file's "Front CPU" version field
(`FUN_200a94c8`'s `+0xa4`) is ever compared against anything read from `IC501` itself (e.g. over `SCIF3`)
rather than assumed — that would be the natural next thread, not a continuation of the RL78-disassembly
work done above.
