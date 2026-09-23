# Handoff: DSP (TMS320C6745) code analysis — tooling check + CPU↔DSP communication

**For a fresh session starting the DSP thread.** Written 2026-09-24 at the end of the session that
got the main CPU emulator (`qemu-machine/`) to boot to a fully drawn main screen. Goal of the next
session: (1) take stock of the DSP analysis tools we already have and decide whether they are
enough or something must be acquired; (2) use them to understand at least part of the CPU↔DSP
communication from the DSP side, so `qemu-machine/src/scif.c`'s DSP mock can become more
faithful. Emulating the DSP itself is explicitly **not** the goal (see "Why not emulate" below).

## Where things stand (don't re-derive)

- The emulator boots v1.42 to the main screen: splash, then **14.100.00 USB FIL2**, S/Po meter
  scales, band-pass filter latch B7S. Screenshot: `qemu-machine/screenshots/2026-09-24-main-screen-14100.png`.
  Live window: `python3 qemu-machine/tools/run_gui.py` (GTK). Full status: `qemu-machine/README.md`.
- The DSP is modelled only as a **protocol mock** in `qemu-machine/src/scif.c`
  (`rza1h_scif5_dsp_ack`). It acks every SCIF5 command instantly (class 2), except for the identity
  query, which gets per-command version replies (`scif5_dsp_identity_reply[]`). Nothing is ever sent
  unsolicited, and the SSIF audio/IQ stream to the CPU is never fed, so there is no spectrum scope.

## The chips and images (names are misleading — read this)

| Chip | Part | Role |
|---|---|---|
| IC901 | TI **TMS320C6745** (C674x, floating point VLIW) | "IF-DSP": demod, AGC, filters, audio |
| IC902 | SPI flash | the DSP's own boot flash (the CPU rewrites it during firmware updates) |
| IC1351 | Altera Cyclone IV EP4CE55 | ADC/DAC front end, feeds the DSP |

The extracted images are in `scratch/unpacked/142/`. **The file names come from an early guess and
are wrong**; the true identities were confirmed by correlating against Icom's per-release version
fields (`notes/multi-cpu-images.md`):

| File | Really is | Notes |
|---|---|---|
| `front_cpu.bin` (163,592 B) | **DSP Program** | starts with AIS magic `0x41504954` ("TIPA") + AIS commands (`0x585359xx`); ends with ASCII `31101070` |
| `dsp_program.bin` (720,648 B) | **DSP Data** | also genuine C674x code; not AIS at offset 0 |
| `dsp_data.bin` (859,412 B) | **FPGA** bitstream | not code |

**AIS** (Application Image Script) is the C674x boot ROM's load format: a sequence of commands
such as Section Load (address + size + data), Set (register write), Jump & Close (entry point).
Parsing it gives every section's load address and the entry point. That is the first concrete step,
since everything else needs correct addresses. TI documents the format in SPRAB41 ("Using the
C674x/OMAP-L1x Bootloader"); verify the opcode list against it rather than from memory.

## Tools already installed (verified 2026-09-24)

- **binutils tic6x**: `~/.local/tic6x-binutils/bin/tic6x-objdump`, `tic6x-readelf`. Raw dumps
  need `-EL` plus ELF wrapping; the recipe is in `notes/multi-cpu-images-history.md`.
- **Capstone 5.0.7** (Python) with `CS_ARCH_TMS320C64X`. Good for scripted scans; C674x FP
  instructions may be partially covered, so check against `dis6x`.
- **TI Code Generation Tools 8.3.1**: `~/.local/ti-cgt-c6000/ti-cgt-c6000_8.3.1/bin/` —
  `dis6x` (TI's own disassembler, needs a hand-built `.c6xabi.attributes`, recipe in the history
  file), `ofd6x`, `nm6x`, `hex6x`, the compiler `cl6x`, and the linker. The compiler is useful to
  learn compiler idioms by compiling small C snippets.
- **The three disassemblers agree byte for byte** on the samples checked so far.
- **Ghidra** has **no** TMS320C6000 SLEIGH module (an open upstream request), so there's no
  decompiler for DSP code. Main-CPU-side analysis stays in Ghidra (`body.bin`).

**Candidates to evaluate, only if the above proves too painful:**
- a community Ghidra C6x processor module (search first; quality unknown);
- rizin/radare2 (Capstone-backed TMS320 support: gives cross-references and functions, but no decompiler);
- IDA Pro (commercial, has C6x);
- a small in-house Python "disassemble + CFG + xref" layer over Capstone/dis6x output.

The decision the next session should make: is linear disassembly plus our own xref scripts enough
for protocol work, or do we need function/CFG tooling?

## What the main CPU side already tells us about the protocol (confirmed)

All of this is in `notes/multi-cpu-images-history.md` ("SCIF5 command API", "DSP command API")
and `qemu-machine/src/scif.c`:

- **Transport:** SCIF5 UART, 4-byte words, bit-reversed on the wire (`scif5_bitrev_transmit_word`;
  RX `rbit`). A job ring (`shared_job_ring_dispatch` `0x200b0f68`, 0x57 slots) carries the
  following job types:
  - 1 = SCIF5 command + reply
  - 2 = band shift-register DMA
  - 3 = RSPI2 (FPGA) transfer
  - 4 = other
- **Reply classes:** top nibble of the reply word. `scif5_classify_reply` (`0x200b0dc4`) resolves on
  classes 1/2/8. The identity query needs class 0xF, and `dsp_page_transfer_verify` needs class 0xE.
- **Identity query:** commands `0xE0000000`–`0xE0000005` (`dsp_identity_query_cmd0..5`), formatted
  by `dsp_identity_format_reply` (`0x200b23e0`) into records at `0x203def00` (+0x00/+0x0d/+0x1a).
  `dsp_fpga_identity_version_check` (`0x2002a3c0`) requires "3.11" / "2.00" / "3.16" and otherwise
  shows the "DSP/FPGA firmware is wrong version" dialog. The odd commands (version screen only) are
  **guessed** in the mock. The DSP image should reveal what it really answers.
- **Live parameter sync:** `dsp_cmd_table_init` sets up a 24-slot table, and `dsp_param_sync_tick`
  pushes changed slots with `scif5_ring_push_word`. Slots are rebuilt from settings by
  `dsp_param_table_rebuild_from_settings` (`0x200b232c`). The ~22 slots aren't individually named.
- **Frequency:** `dsp_sync_rx_freq_words` (`0x200b56b0`) sends f + 36 kHz IF as two words
  (seen at boot: `0x100000D7`, `0x1080B2C0` for 14.1 MHz). It's fire-and-forget.
- **Firmware update:** the page-by-page transfer is checked by `dsp_page_transfer_verify`, which
  uses a checksum plus an echo of the page address.
- **Other lines:** HSK1 (`P8_9`) is the DSP-ready handshake; DRESD (`P2_6`) is the DSP reset. DRESD
  *is* released at boot, via `PSR2 = 0x00400040` at `0x2002b078` (an old note claiming otherwise
  was corrected). SSIF0/1 plus DMA carry audio/IQ into the ring at `0x203fbdc0` that
  `spectrum_scope_fft_task` reads; the emulator never fills it.

## Suggested plan for the DSP session

1. **Tool check (short):** disassemble a known region of each code image with all three tools
   and confirm they still agree. Note what's missing, such as function boundaries and
   cross-references.
2. **Parse the AIS script** in `front_cpu.bin` (DSP Program): list sections (load address, size)
   and find the entry point. Check whether `dsp_program.bin` (DSP Data) is a raw section image
   loaded to a fixed address, and whether it is referenced from the AIS script, e.g. by a Set or
   Section Load command.
3. **Find the DSP's UART (SCIF5 peer) driver:** find the C6745 UART register block (from the
   C6745 datasheet or TRM), search the code for its base address, then follow RX into the command
   dispatcher.
4. **Command dispatcher:** map how command words (top byte = class/opcode) are decoded. First
   targets:
   - `0xE0000000`–`05`: confirm "3.11"/"2.00"/"3.16" and get the real odd-half values;
   - the frequency words `0x10……`;
   - the param-sync slot words.
   The output is a table to drive `scif.c`.
5. **Any unsolicited DSP → CPU traffic** (meter/S-meter values? status?) and what the CPU
   expects over SSIF (sample format, rate) — the next emulator feature (spectrum scope, S-meter).

## Why not emulate the DSP (decided 2026-09-24, revisit later)

- No usable C674x emulator exists; writing one is a real ISA-simulator project (VLIW, delay
  slots, SPLOOP, compact instructions, FP), plus the C6745's peripherals.
- The FPGA front end can't realistically be emulated from its bitstream.
- Protocol-level knowledge gets most of the value: a correct mock, plus synthetic SSIF data for
  the scope.

## Gotchas

- **Ghidra loads `body.bin` at `0x20005000`.** Raw-file scans must add that, not `0x20000000`.
- The QEMU gdbstub perturbs timing (memory `icom-gdb-perturbation-resolved`). Prefer QMP / device
  debug logs (`RZA1H_DEBUG=...`) for the emulator side.
- `-icount shift=1` is the emulator default now; a boot to the main screen takes about 2 minutes
  of wall time.
