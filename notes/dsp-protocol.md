# CPU ↔ DSP link protocol, from the DSP side (TMS320C6745, v1.42 "DSP Program" 3.11)

Started 2026-09-24. Source: `scratch/unpacked/142/front_cpu.bin` (file name is historical: it is
the **DSP Program**, see [[multi-cpu-images]]). Everything below is read from the DSP's own code
unless marked otherwise. CPU-side counterparts: `qemu-machine/src/scif.c`, [[multi-cpu-images-history]]
("SCIF5 command API").

## Tooling (all in `tools/dsp/`)

| Tool | What it does |
|---|---|
| `ais2elf.py` | parses the AIS boot script, writes an ELF with one section per Section Load at its real address + a `.c6xabi.attributes` (Tag_ISA=C674x) so `dis6x` decodes FP and compact opcodes |
| `c6x_consts.py` | recovers 32-bit constants from MVK/MVKL+MVKH pairs in `dis6x` output |
| `c6x_xref.py` | call graph / function owner / DP-relative (`*+B14[n]`, scaled by access size) xrefs; extra function starts from `<dis>.starts` |
| `cinit.py` | walks the `.cinit` table (initialised globals, incl. the command dispatch table) |

Recipe:
```
python3 tools/dsp/ais2elf.py scratch/unpacked/142/front_cpu.bin scratch/dsp/dspprog.elf
~/.local/ti-cgt-c6000/ti-cgt-c6000_8.3.1/bin/dis6x scratch/dsp/dspprog.elf scratch/dsp/dspprog.dis
```
**Why the old "~30 % undefined words" figure was wrong:** earlier disassembly ran on the raw file.
Section 0's data starts at file offset 0x28, so the 32-byte fetch packets were misaligned and the
C674x compact-instruction headers (`.fphead`) were misread. Disassembled at the real addresses,
`.word` lines drop to about 5 %, and those are mostly the data sections. The linear disassembly
plus these scripts is enough for protocol work; no CFG or decompiler was needed.

## Image layout (AIS)

`0x58535963` (Sequential Read Enable, per TI SPRAB04/SPRAAT2), `FunctionExec idx=5 (0x0e000204,
0x60239, 5)` (a ROM clock/PLL-style config; index meaning not confirmed), then 7 Section Loads, all
in L2 RAM, then `Jump&Close 0x118177a0`.

| Load addr | Size | Contents |
|---|---|---|
| 0x11800000 | 0x17b20 | `.text` |
| 0x1181ee30 | 0x64c8 | constants (incl. version strings at 0x11825280 / 0x118252c0) |
| 0x1182f2f8 | 0x281c | `.cinit` |
| 0x11832158… | small | vectors / misc |

`_c_int00` = 0x118177a0: SP = 0x1181f2f4, **DP (B14) = 0x11831b18**, then calls 0x11816f60, the
auto-init at 0x118179a0 (cinit table 0x1181f2f8), and 0x11817120 (main). `.bss`/`.far` live at
0x11817b20–0x1181ee30 and 0x11831b18+. DSP Data (`dsp_program.bin`) isn't in the AIS script; the
DSP reads it from its flash at runtime (its version tag lands in RAM at 0x1181ec58).

Peripheral base pointers are globals initialised by `.cinit`, not inline constants. That's why a
constant scan of the code finds none. DP-relative ones: DP+0x5e0 EDMA3CC 0x01C00000, DP+0x5e4
McASP0 0x01D00000, **DP+0x5e8 McASP1 0x01D04000**, DP+0x5ec McASP2, DP+0x5f0 SPI0 0x01C41000 (its boot flash), DP+0x5f4 SPI1 0x01E12000 (probably the FPGA),
DP+0x604/0x608 McASP0 DATA 0x01D02000, DP+0x60c/0x610 McASP1 DATA 0x01D06000.

## Physical link

The CPU's SCIF5 pins go to **McASP1** on the DSP (DSPCK→ACLKX1, DSPR→AXR1[2], DSPX→AXR1[1]; pin
table in [[ic7300-signal-chain]]). The DSP **receives on serializer 2 (RBUF2, McASP1+0x288)**.
It **transmits via EDMA**: `0x11814ae8` is called from the EDMA setup at 0x11814664 with src =
0x1183213c (the "TX word" global, DP+0x624), dst = McASP1 DATA, channel 3. The code never reads
the TX word; only the EDMA does. McASP transmits MSB-first and the CPU bit-reverses in software,
so both ends see the same 32-bit word value (e.g. the DSP sees `0xE0000000` as `0xE0000000`).

**Every word slot carries one word in each direction.** The DSP always sends *something*: the ISR
picks the next TX word on every frame (see below). So a CPU "reply" is simply the next DSP word
whose class nibble it is waiting for.

## McASP1 ISR (0x11811e78–0x1181231c, the only `B IRP` function)

- **RX** (RSTAT bit 5 RDATA): `w = RBUF2`, saved to DP+0x620 (0x11832138).
  - `w>>24 == 0x43` is handled **inline**: the previous P[0x43] goes to 0x11818538 and `w` is stored
    in P[0x43] (0x11817b58). Then floats: 0x11817da8 = (byte1 ? 0.0625 : 0), 0x11817dac = −1.0,
    and `f(10·byte2, 8000.0)` → 0x11817da0, with a ±0.1 constant chosen by the result's magnitude.
    This looks like a fast-path tone/frequency parameter (not yet named).
  - `w>>24 == 0x48` sets byte flag `*(DP[0x61c]+0x30) = 1`, then falls through.
  - every non-0x43 word is pushed into the **RX ring** at 0x11819300: {buf*, size, wr_count,
    rd_count, widx…, overflow flags at +0x18/+0x19}. A 0xDEADDEAD sentinel is written after the
    newest entry. The main loop drains it through the dispatch table.
- **TX** (otherwise; XSTAT bit 6 is written to clear it): choose the TX word from 7 slots
  `C[k]`, each a pair (word, stamp) at 0x11817ba8 + 8k, against "last sent" copies
  `L[k]` at 0x11818588 + 8k:
  1. `C0 != L0` (both words compared) → send C0.word
  2. `C3.stamp != L3.stamp` → send C3.word (slot 3 = 0x11817bc0, class 7)
  3. `C6.stamp != L6.stamp` → send C6.word (identity reply)
  4. otherwise, round-robin fallback:
     - byte DP+0x1eb == 0 (the flash-update mode flag, cleared by E2) → send **C5** (0xE… page verify)
     - else if counter `L[18]` (0x118185d0) != 0 → send **C4** (class 8); reset it when it reaches 8
     - else bit 23 of P[0x00] (0x11817b20) clear → **C1** (class 1), set → **C2** (class 2)

### TX slots (initialised by 0x118131f8)

| Slot | Addr (word) | Initial word | Class | What it carries |
|---|---|---|---|---|
| C0 | 0x11817ba8 | 0x00000000 | 0 | top priority (sent whenever anything in it changes). byte1 = 7-bit slewed magnitude + bit7 "still moving" (same idiom at ≥4 sites, incl. the 0x00 handler's neighbours and 0x1180d638/0x1180fdd8); byte0 bit6 = \|x\| < ~1.5e-6 test (0x1180fa60); a flag bit in byte3's low nibble. A live meter-like value (medium) |
| C1 | 0x11817bb0 | 0x10000000 | 1 (9) | byte0 = int·2.2 (0x11803968, 0x118071cc, 0x11807488, 0x118076a8) or two 0..1 floats ·255 → byte0/byte1 (0x11802afc, in 0x11801108); byte3 bits 26/27 = 2-bit status (≈12 sites in the 0x22 mode code); **bit31** set from the audio-block code at 0x118125b0 (flag \| DP+616), turning class 1 into **9**, which the CPU aliases to class 1 ✔. CPU: "step towards target using byte 2" (medium-high for "meter reading") |
| C2 | 0x11817bb8 | 0x20000000 | 2 | byte2 = a value saturated to 0..255 (0x11802204, same pass as C1 in 0x11801108). Not just an ack: C1 or C2 goes out by P[0x00] bit 23 (medium) |
| C3 | 0x11817bc0 | 0x70000000 | 7 | no writer found yet (sent when its stamp changes) |
| C4 | 0x11817bc8 | 0x80000000 | 8 | byte0 = computed byte (0x11803cd4, in 0x11803b08); byte1 bits 1..0 = stage field (set to 2 at 0x11803bb4/dcc, 0x11804028/210, cleared at 0x1180416c, bit0 set by 0x11804e18/0x11805008). Sent while L[18] ≠ 0, which the ISR resets at 8; **L[18]'s setter not found** (no absolute reference) |
| C5 | 0x11817bd0 | 0xE0000000 | E | firmware page verify: page counter + −checksum (B0–BF handler) |
| C6 | 0x11817bd8 | 0xF0000000 | F | identity reply (E0 handler), stamp at 0x11817bdc |

Slot words are little-endian in DSP RAM (byte N = address + N). C0–C4 are written from the
audio/mode code, never from command handlers, so they are DSP → CPU telemetry. The writer list
comes from a Sonnet subagent; attribution to functions is approximate because `c6x_xref.py`
function boundaries over-merge.

The handlers rewrite only the low bits of each slot, so the class nibble is fixed per slot.

## Command dispatch

The main loop (0x11812668, never returns) alternates between audio-block work (gated on EDMA3CC
IPR bits 1/2, then a call to 0x11812324) and draining the RX ring, calling `table[w>>24](w)` for
each word (0x11812644 is the one-shot dispatch helper). Opcodes **0x20/0x21 and 0x25 are
coalesced**: the loop only parks them in DP+0x26c / DP+0x274, so only the newest one is applied,
at a fixed point in the loop. The table has 256 entries, initialised by `.cinit` to RAM 0x11818100. Default handler 0x1180a008 is just `return`, so unknown
opcodes are silently ignored.
Almost every handler starts with `prev[op] = P[op]; P[op] = w;`: P = "current word per opcode"
at 0x11817b20…, prev = P + 0x9e0. Initial P values are the bare opcode (`0xNN000000`), except
0x20/0x21 = 0xFFFFFFFF, 0x22 = 0x22FFFFFF, 0x40 = 0x40005555.

Per-opcode semantics were read by subagents (Sonnet) from the disassembly. Mechanics are
address-verified; the "meaning" column is inference unless marked. Rows marked ✔ I re-checked
myself. P slots are 0x11817b20 + 4·k in table order, not 4·opcode.

| Opcode | Handler | P slot | Decode → effect | Meaning (confidence) |
|---|---|---|---|---|
| 0x00 | 0x1180a030 | b20 | bit9: one-shot trigger (DP+0 byte = 1, bit cleared back in P); bit5: DP+1 halfword = 0x30; bits13/12 → 2-bit code, on change `0x11806cac` (full reset / partial reset / idle, which installs callback 0x11804350 at DP+187); **bit23 selects TX fallback C1 vs C2** ✔ | mode/reset control word (low) |
| 0x01 | 0x1180a00c | b24 | store only; bit2 is read by 0x1180de18 (±1.0 direction from 0.3-scaled compares vs DP+118) | slew enable/direction (low) |
| 0x10 | 0x11809fb8 | b28 raw, **b2c freq** | bit23=0: bits 10..0 → freq bits 26..16; bit23=1: low 16 bits → freq low half ✔ (0x100000D7 + 0x1080B2C0 → 0xD7B2C0 = 14 136 000) | RX frequency incl. 36 kHz IF (high). No static reader of b2c in the DSP Program |
| 0x20 | 0x11809f40 → 0x11808bb0 | b30 | change-gated; two 9-bit fields (bits 17..9, 8..0), difference bounded 180/−140, bit9 sign; trig polynomial + table MAC → DP+142/145/146/148; resets P[0x21] | filter coefficient generator, PBT/notch-like (medium) |
| 0x21 | 0x11808ac4 → 0x11807ea0 | b34 | 6-bit preset index (bits 5..0, ranges <10/<24/<30), 12-bit signed field bits 23..12; copies 11-float presets from 0x11825290; resets P[0x20] | filter-shape preset, pairs with 0x20 (medium) |
| 0x22 | 0x11807378 | b38 | gated on byte1 change; byte1 indexes a jump table at 0x11832270 (about 17 valid) that installs per-mode callbacks (DP+51/121/187), resets state, 480-sample (10 ms at 48 kHz) settle counter, clears C1 low bytes | **operating mode / demodulator select** (high for role) |
| 0x23 | 0x11806b48 → 0x11806a30 | b3c | byte1 → (255−b)/256 → DP+48/50, ×0.65 → DP+10; nibble → 16-entry double table 0x11824088; helper 0x11806a30 also called from the 0x22 mode branches (≥14 sites) | shared gain/reference context (low) |
| 0x24 | 0x11806918 | b40 | byte0 → 3.0 + (255−b)·4/255 (3.0..7.0) → DP+161; byte1 → tables 0x11819468/0x11819868; nibble bits 19..16 → table 0x11824d30 | 3–7 range suggests kHz, e.g. a filter width (medium-low) |
| 0x25 | 0x118067e8 → 0x11806004 | b44 | coalesced; bit23 sign, bits 9..0 magnitude clamped 1020, bits 17..16 select 48- or 60-long coefficient sets, SPLOOP FIR/biquad regeneration | IF filter width/shape (medium) |
| 0x27 | 0x11805fd8 | b48 | store only | ? |
| 0x40 | 0x11805fb0 | b4c | store only (initial 0x40005555) | ? |
| 0x41 | 0x11805f40 | b50 | bits 19..16 index a 16-float geometric table 0x11824580 (0.0058→0.647) → DP+120 | smoothing/decay rate, AGC-like (medium) |
| 0x42 | 0x11805ee4 | b54 | byte1, byte2 /255 → 0x11817db8/dbc | two levels (low) |
| 0x43 | 0x11805e00 (+ ISR fast path) | b58 | byte1≠0 → 0.0625; byte2 → 10·b/8000 → da0 with ±0.1 slew | tone generator, e.g. CW sidetone? (medium-low) |
| 0x44 | 0x11805d40 | b5c | ignores own payload, re-derives P[0x42] → dc0/dc4 | commit of 0x42 (medium) |
| 0x48 | 0x11805c2c | b60 | bits 21..8 == 0 → zero the struct at DP+0x61c (the one the ISR flags); byte0 /255 → dc8; bits 23..22 mode → 0x11805b2c | effect block reset/level (low) |
| 0x49 / 0x4a | 0x1180592c / 0x11805750 | b64 / b68 | identical: byte1, byte2 through a 3-segment log taper → DP+344..348; 0xFF = off flags DP+1388/1389 | two log-taper levels, e.g. AF/RF gain? (low) |
| 0x4b, 0x4d, 0x4e, 0x4f | — | b6c, b74, b78, b7c | store only (0x4d's neighbour code re-reads P[0x4c]) | ? |
| 0x4c | 0x118055e0 | b70 | nibble → one of about 9 float constants → DP+179/180 | discrete time-constant preset (low) |
| 0x61 | 0x11805360 | b80 | byte2 clamped 0..51 → 52-float table 0x11823638 → DP+269; bits 9..0 signed → DP+261 | rate + signed fine offset (low) |
| 0x62 | 0x11805178 | b84 | nibble → 2-bit category + tables 0x11824e48 → 0x1181b0b8 | shape selector (low) |
| 0x6b | 0x11804e68 | b88 | bit1: DP math path vs merging bits into P[0x00] | ? (low, partial) |
| 0x80, 0x81 | 0x11804d30, 0x11804d08 | b8c, b90 | store only | ? |
| 0xA0–0xAF | 0x11804cd4 → 0x11804bf0 | ba0 | `buf[byte1] = byte0` (buffer 0x118185e8); byte1 = 0xFF commits the 256-byte page to flash **0x28000 + (bits 26..16)·256** ✔; bit27 = last | byte-wise writes to the 0x28000–0x4FFFF store (high) |
| 0xB0–0xBF | 0x11804bc4 → 0x118049e8 | ba4 | firmware page data, 3 bytes/word, running checksum into C5 | firmware update data (high) |
| 0xE0 | 0x11804728 | b94 | identity query, see below ✔ | (high) |
| 0xE1 | 0x118046f4 | b98 | erases 32 KB blocks 5..9 = 0x28000–0x4FFFF ✔ | erase the A0 store (high) |
| 0xE2 | 0x11804624 | b9c | sets page address, sets up C5, clears DP+0x1eb | begin firmware write (medium-high) |
| 0xE3 | 0x118044e4 → 0x11804430 | — | ignores payload; flushes a pending page, sets DP+0x1eb = 0xFF (back to normal TX rotation) | end write session (high) |

No handler in 0x00–0xE3 writes TX slots C0–C4. Only 0x22's mode branches touch C1 (clearing it),
and the flash family writes C5/C6.

### Identity (0xE0)

`w & 1` selects the half (0 = first 3 digits, 1 = last 4) and `(w >> 1) & 3` selects the record. The
reply goes into C6 bytes 2,1,0 = three ASCII characters, and the low nibble of byte 3 = hex value of
the 4th character, class 0xF. Then the stamp is incremented.

| CPU cmd | Record | Source | v1.42 reply |
|---|---|---|---|
| E0000000 | 0 DSP Program | compiled-in "3110" | **0xF0333131** ("311") |
| E0000001 | 0 | compiled-in "1070" | **0xF0313037** ("107") |
| E0000002 | 1 FPGA | read at runtime: if the 8-byte tag at 0x1181ec60 == "31501120", the constants "3150"/"1120"; else 7 BCD digits from 0x1181ec68 | "3.16" per CPU check; odd half unknown |
| E0000003 | 1 | as above | ? |
| E0000004 | 2 DSP Data | runtime tag at 0x1181ec58 (= last 8 bytes of DSP Data = "20001000") | **0xF0323030** ("200") |
| E0000005 | 2 | | **0xF0313030** ("100") |

The emulator mock (`scif5_dsp_identity_reply`) had cmd1/cmd3/cmd5 as guesses copied from the even
half. cmd1 and cmd5 are now known from the images. cmd3 depends on what the FPGA reports.

## DSP flash map and what "DSP Data" is (2026-09-24)

The DSP Program's SPI0 flash code has six users of the command buffer at 0x1181eb50 (found by
constant recovery). They are: read 0x03 (0x11815540), page program 0x02 (0x118156e4), 4 KB / 32 KB / 64 KB erases
0x20/0x52/0xD8 (0x1181580c / 0x11815964 / 0x11815874), and the FPGA configuration stream
(0x11815df4). The read routine is called **only** for two 8-byte version tags.

| Flash range | Contents | Evidence |
|---|---|---|
| 0x000000–0x027F08 | DSP Program (AIS, booted by the ROM) | size 0x27F08, tag "31101070" at its end |
| 0x028000–0x04FFFF | CPU-writable store: 0xE1 erases it, 0xA0–0xAF write pages, 0xE3 closes (settings/calibration?) | E1 loop; A0 page address 0x28000 + n·256 |
| 0x050000–0x0FFF08 | DSP Data (inferred: its size 0xAFF08 ends exactly at the tag) | tag read at 0xFFF00 → 0x1181ec58 |
| 0x100000– | FPGA: 8-byte tag, then the bitstream, which the DSP streams to the FPGA at boot | 0x11816080 (tag), 0x11815df4 (stream from 0x100008) |

**DSP Data is not executed or bulk-read by the DSP Program.** Only its version tag is read, for the
0xE0 identity reply. Its contents: a 776-entry (offset, length) directory (0x1840 bytes), then
blobs of about 1.5–2.5 KB with about 7.0 bits/byte entropy, i.e. compressed data. The earlier
"genuine C674x code" reading ([[multi-cpu-images-history]]) came from misaligned raw disassembly
and should be treated as wrong. What reads it is open. Candidates: code reached only through
pointers (not seen by the constant scan), or nothing in v1.42 at all.

**FPGA image (`dsp_data.bin`):** its first 8 bytes are the tag **"31601130"**, the 8 bytes the DSP reads from
flash 0x100000. The version is "3.16"/"1130", so identity cmd3 is *probably* 0xF0313133 ("113"+'0').
That assumes the BCD digits at 0x1181ec68 match the tag, which isn't verified; the special case
compares against "31501120", i.e. FPGA 3.15. The bitstream starts at +0x28 and ends at 0xd1cc9
(about 859 KB). An uncompressed EP4CE55 bitstream is roughly 1.8 MB (from memory, check the
Cyclone IV handbook), and the entropy is about 6.6 bits/byte in the dense middle, so the image is
probably Quartus-**compressed**.

## Emulator model (`qemu-machine/src/fake_dsp.c`, 2026-09-24)

scif.c passes every complete SCIF5 word to `fake_dsp_command()`. Each CPU one-word read (an arm
of `scif5_arm_retry_timer`) asks `fake_dsp_next_word()` for one frame. The model keeps P[] and the
C0..C6 / L slots, and picks the word with the ISR's priority. Its 0xE0 identity replies come from
the three version tags.

**Link timing learned while building it (CPU side, confirmed):**
- `scif5_arm_retry_timer` is "receive one word": a line turnaround via the P8_2/P8_11 pin mux, then a
  one-word read. Each arm is one DSP frame.
- `dsp_identity_query_cmd0..5` transmit the query, **discard 2 reads**, then accept class F
  within 18 more. So the DSP must answer no earlier than the 3rd frame after the command. The
  model applies commands 2 frames after arrival (`FAKE_DSP_CMD_LATENCY_FRAMES`). An eager model,
  or one with a 1-frame lag, answers inside the discarded reads and brings up "DSP is not
  working correctly".
- `scif5_classify_reply` stores **every** received word in a 16-entry per-class table at
  **0x20414C48**. `FUN_200b4f4c` unpacks it into the state struct at **0x203DF100**:
  - class 1: bits 25..16 → +0xcc, bit 31 → +0xce, low 16 bits → +0xd0 (gated)
  - class 7: bit 15 → +0xf5, signed low 10 bits → +0xf6
  - class 0: bits 15, 8, 5 and 4 → flags at −0x1a5/+0x105/−0x1ac/−0x1ab
  - class 2: byte 2 → +0x9e
- Traffic in a 150 s boot: ~2000 frames, but only **42 command words** (25 opcodes, sent on change
  only). Opcodes seen: 00 01 10 20 21 22 23 24 25 27 40 41 42 43 44 49 4a 4b 4c 4d 4e 4f 61 62 e0.
  Never seen at boot: 48, 6b, 80, 81 (and the flash family).

**Experiment knobs:** `RZA1H_DSP_SLOT="k=0xWORD,..."` plants slot values, and
`RZA1H_DSP_SWEEP="k:lo:width"` ramps a field. Planting C0, C1 or C3 values (byte-wide fields)
changed nothing on the main screen. The S/Po bar isn't driven by those fields as planted. C4
(class 8, never sent: the L[18] setter is unknown) and the SSIF stream are the remaining
candidates.

## McASP pin directions and audio format (DSP init 0x11813d24, 2026-09-24)

Read directly from the SRCTLn writes: SRMOD 1 = TX, 2 = RX, with DISMOD = 3 on all.
PDIR = 0x55, so AXR0[0], [2], [4] and [6] are outputs, and PFUNC = 0 (all pins are McASP).

| McASP0 pin | Dir | Net ([[ic7300-signal-chain]]) |
|---|---|---|
| AXR0[0] | TX | ? — likely DFX_DET or DFX_AGC → FPGA (user's schematic reading: FPGA → FPX_DET → IC991 PCM1754 speaker DAC, FPX_AGC → IC971 AGC DAC) |
| AXR0[1] | — | unused |
| AXR0[2] | TX | ? — the other of DFX_DET / DFX_AGC |
| AXR0[3] | RX | ? — probably from the FPGA (IF samples?) |
| AXR0[4] | TX | DX_REC → CPU SSIRxD0 |
| AXR0[5] | RX | DR_AF ← CPU SSITxD0 |
| AXR0[6] | TX | DX_FMT → CPU SSIRxD1 |
| AXR0[7] | RX | DR_RSV ← CPU SSITxD1 |

The CPU-facing directions match the net names (DX = DSP transmits, DR = DSP receives).

Format: XFMT/RFMT 0x180f0 = 32-bit slots, MSB first, 1-bit data delay. AFSX/RCTL 0x111 = 2-slot
(I2S) frame, word-wide sync, **external** frame sync. ACLKX/RCTL 0x81 = **external** bit clock. So
the DSP is an I2S slave; BCLK/FRM/MCLK come from elsewhere (most likely the FPGA, which also
clocks the DACs). McASP1 (the SCIF5 command link) is a clock slave too: SRCTL1 = TX, SRCTL2 = RX,
external clock and sync, so the CPU clocks every link frame.

Consequence: speaker and AGC audio go DSP → FPGA → DAC and never touch the CPU. The CPU gets
DX_REC/DX_FMT on its SSIF receive side (the spectrum-scope ring) and feeds DR_AF/DR_RSV to the DSP.

## DSP → CPU audio (SSIF receive) and the QSO recorder feed (2026-09-24)

Pins (user's schematic reading): **DFX_DET = DSP pin 113, DFX_AGC = DSP pin 111** (→ FPGA → FPX_DET /
FPX_AGC → IC991 / IC971). The spacing from the known pins (116/117/118/120 = AXR0[4]/[5]/[6]/[7])
suggests 113 = AXR0[2] and 111 = AXR0[0]. That's inferred, not checked against the datasheet. Both
are DSP TX serializers either way.

CPU side (body.bin), one driver cluster at 0x2005f880–0x200607xx:
- `FUN_200605fc` (bring-up): SSIFCR_0 = 0xCC (TX and RX interrupts), SSIFCR_1 = 0xC4 (RX only), DMAC
  channels 3/4/5. The literal pool holds SSIFRDR_0 0xE820B01C, SSIFRDR_1 0xE820B81C, SSIFTDR_0
  0xE820B018, and buffers 0x203faf40/b180/b3c0/b840/ba80/bcc0.
- It exposes a ring at 0x203fbdc0: 8 × 72-byte blocks (36 int16), write index +0x240, read index +0x241.
  The reader is `FUN_2005fb64`, whose only caller is `FUN_20067254`, which is called from `FUN_2006759c`.
- `FUN_20067254`: per block, `FUN_20066fc4` reduces 36 → 6 samples. Those go to
  (1) `FUN_20008868` → the FFT task named `spectrum_scope_fft_task`, which is therefore an **audio**
  FFT; and (2) `FUN_20066ed0`, which packs them into 216-byte blocks (mode 3 = zeros, i.e. mute; one
  mode scales by 0xB5/256 = −3 dB). `FUN_20066f18` then stores each block as a 0xDC-byte record in a
  **circular buffer of 0x77A records**: 1914 × 108 samples, about 26 s at an assumed 8 kHz. This
  looks like the QSO recorder's pre-record capture and is very likely the missing audio source of
  `voice_recording_file_task` ([[kernel-rtos]]). That last link isn't proven.
- Resolved in the next section: the ring is DX_REC L, and the rates are pinned.

## The CPU ↔ DSP audio link, both directions (2026-09-24, later)

Two independent reads agree: the CPU side from body.bin (Ghidra, names now in the DB) and the DSP
side from the DSP Program (tic6x disassembly, by a subagent, with key constants spot-checked).
Confidence: ✅ code on both sides agrees, 🟢 one side plus inference, 🟡 guess.

**Framing ✅.** I2S, 2 × 32-bit slots per frame, **96 kHz frame rate**, clocked externally (the
FPGA, probably). Both ends are clock slaves. Data is 24-bit left-justified: the DSP masks words
with 0xFFFFFF00 at 0x118123a0, and the CPU uses the top 16 bits (SSICR 0x3c2b0033: DWL 24,
SWL 32). The rate comes from both sides at once. The CPU's recorder WAV header
(`wav_write_fmt_8k_mono16` 0x20069008) is 8000 Hz mono 16-bit, built from 1 in 12 frames. The
DSP's tone and ADPCM generators run at F/12 with pitch/8000 phase steps, and its filter cutoffs
only make sense at 96/48 kHz. The effective sample rates are:

| Rate | What runs at it |
|---|---|
| 96 kHz | frames |
| 48 kHz | CPU-facing payloads (every 2nd frame) |
| 12 kHz | DSP demod block rate (8 frames per EDMA event pair) |
| 8 kHz | recorder, voice memory, CPU → DSP playback |

**Transport.**
- CPU: DMAC ch3 (SSIF0 RX), ch4 (SSIF0 TX) and ch5 (SSIF1 RX), set up by `ssif_dmac_ch345_setup`
  0x2005ff1c. Each has two register sets over 0x240-byte buffers (72 frames = 0.75 ms), CHCFG
  REN|RSW, so they ping-pong. DMARS: 0xe2 = SSIF0 RX, 0xe1 = SSIF0 TX, 0xe6 = SSIF1 RX.
  - Buffers: RX0 0x203faf40 / 0x203fb180; TX0 0x203fb3c0 / 0x203fb600; RX1 0x203fb840 / 0x203fba80.
  - No DMA interrupts: the 250 µs TGI3A tick ISR (0x20005bd4) polls CHSTAT END/SR and runs the
    three pumps, gated by 0x2039038c.
- DSP: EDMA3 PaRAM sets 0/35 (RX, AREVT0) and 1/33 (TX, AXEVT0), ping-pong. One event = one slot,
  moving one word per active serializer. A block is 8 frames. The main loop 0x11812668
  deinterleaves the block into a struct at 0x11818bd0 (0x11812324), then processes it at
  0x118104c0.

**Channels:**

| Net | Dir | Slot | Content | CPU side | DSP side |
|---|---|---|---|---|---|
| DX_REC (AXR0[4] → SSIF0 RX) | DSP→CPU | L | **RX audio**, taken before AF gain and low-passed. During TX it's the CW sidetone instead ✅ | `ssif0_rx_pump_dx_rec` 0x20060614 → 48 kHz ring 0x203fbdc0 (8×36) → `qso_recorder_rx_audio_block` 0x20067254: audio FFT (48 k and 8 k) + QSO recorder (36→6 ⇒ 8 kHz) | struct+0x180, 0x11811224 |
| DX_REC | DSP→CPU | R | **mic/TX modulation audio** 🟢 (the DSP's per-block scalar struct+0x274, ×4 interpolated) | ring 0x203fc002 (2 readers): `voice_tx_record_from_mic_block` (TX voice memory, 8 kHz), `voice_tx_record_mic_peak_level` (level meter on the record screen) | struct+0x1a0, 0x11810b34 |
| DX_FMT (AXR0[6] → SSIF1 RX) | DSP→CPU | L | per-mode demod output ×7.5, 0 in TX. An FIR variant is used for modes 0x0b–0x0d 🟡 | `ssif1_rx_pump_dx_fmt` → ring 0x203fc246 (2 readers): the RTTY decode screen's tuning scope and the CTCSS detector ✅ (see "DX_FMT consumers and the RTTY receive path" below) | struct+0x1c0, 0x118117f4 |
| DX_FMT | DSP→CPU | R | never written (0) | not read | struct+0x1e0 |
| DR_AF (SSIF0 TX → AXR0[5]) | CPU→DSP | L | **playback to the speaker** (recorder playback) ✅ | `ssif0_tx_pump_dr_af` 0x20060778 pops `txL` ring 0x203fbcc0 (13×6) | even samples → LPF 3.6 kHz → speaker mix ×0.703 (AXR0[2] R) and →0x1180fdd8 |
| DR_AF | CPU→DSP | R | **audio to the transmitter** (TX voice memory) ✅ | pops `txR` ring 0x203fbd5e | even samples → LPF → ×P[0x4b] byte1/255 (MOD level) → struct+0x298 → TX mode code |
| DR_RSV (SSIF1 TX → AXR0[7]) | CPU→DSP | L/R | **unused** ✅ | no SSIFTDR_1 reference; SSIFCR_1 is RX-only | deinterleaved, never read |

Which slot carries the playback: `voice_play_to_dsp_tick` (0x20067458) pops 36-sample records
(0x4c bytes, 2000-record ring) and pushes them to txL, or to txR when `FUN_2004b5d0` (byte
DAT_2004bc58+2) is set, i.e. transmitting. The CPU sends 8 kHz as a 96 kHz stream: each sample
is written to 2 frames, then 10 zero frames follow. The DSP reads only the even frames, so it
sees 48 kHz zero-stuffed ×6 and interpolates with its 3.6 kHz LPF (its ×0.138 gain is close to
1/6). When nothing plays, the pops return zeros, i.e. silence.

**Other DSP-side facts:**
- In-band control from the FPGA, on AXR0[3] L's low byte: `idx<<4 | val` goes into a 16-byte
  table at 0x1181ec68. It carries the FPGA version the 0xE0 identity reply reports, and idx 15
  bit 0 becomes C1 bit 31.
- The speaker path (AXR0[2] R) mixes RX audio, DR_AF L and an **IMA-ADPCM decoder**
  (0x11810d88) fed by opcode 0x48 bytes, most likely the voice synthesizer.
- AXR0[0] R is a ramped AGC control word (DFX_AGC 🟢).

**Emulator before the model (see below) ✅ (QMP):** DMAC ch3/4/5 are programmed exactly as above, but SSIF0/1 are
unmodelled (they read 0). Bring-up waits for 10 edges on each word-select pin, PPR2 bit 9
(P2_9 SSIWS0) and PPR3 bit 5 (P3_5), under a 1 ms MTU2 ch0 timeout (FUN_20063448(32000) /
flag 0x203903ac). The pins never move, so bring-up times out and the pump gate 0x2039038c stays
0. To bring audio up, a model needs:
- toggling word-select levels (derive them from the virtual clock at 96 kHz, no timer);
- SSIF registers;
- DMAC ch3/4/5 register-set ping-pong with CHSTAT END/SR, at one buffer per 0.75 ms (one ptimer
  per buffer, not per frame);
- a fake-DSP producer for DX_REC/DX_FMT.

**Model (built 2026-09-24) ✅:** `qemu-machine/src/ssif.c`, the gpio.c word-select pins and
the dmac.c streaming channels implement the list above; details and knobs are in
`qemu-machine/README.md` ("the DSP audio link streams"). A 1000 Hz DX_REC L tone arrives at
exactly 1000 Hz in both the 48 kHz ring and the 8 kHz recorder staging.

**Stimulus files (2026-09-25) ✅:** the fake DSP can instead play an audio file (.au/WAV, 16-bit,
resampled to 96 kHz) into any RX slot: DX_REC L/R and DX_FMT L/R. Set it with `RZA1H_AF_FILE` etc.
at boot, or at runtime with `qom-set /machine/ssif af-file`. The file arrives in the 48 kHz
ring sample-accurately at unity gain (`tools/audio_stimulus_check.py`, ncc ≥ 0.989; the limit
is the 0.01 FS noise floor). Knobs: `qemu-machine/README.md`. Next: feed the CW/FT8/SSTV test
files into DX_FMT and see which of its two readers (FUN_20020e18, FUN_200635ec) reacts. Done,
next section.

## DX_FMT consumers and the RTTY receive path (2026-09-25)

**DX_FMT has exactly two CPU readers**, and neither is a text decoder. Both are confirmed live
with stimulus files (`qemu-machine/tools/decode_stimulus_test.py`, all 13 checks pass):

| Reader | Runs when | What it does | Live result |
|---|---|---|---|
| `rtty_scope_dx_fmt_reader` 0x20020e18 → `rtty_scope_decimate8` 0x20020d80 → `rtty_scope_fft_1024` 0x20020a50, driven by `rtty_decode_scope_tick` 0x200211c4 | mode class 2 (RTTY/RTTY-R, table 0x2018b548) **and** screen state 0x203de180 = 10/12 (MENU > DECODE); enable flag 0x203a2de5 | ÷8 to 6 kHz, 1024-point FFT once per fill, bins 0x145–0x1ad (1.90–2.52 kHz) → 105 dB bytes at 0x203a2bdd → averaged (1–4) spectrum + 78-line waterfall on the decode screen | the user's RTTY sample shifted to 2125/2295: peaks at 2127–2133 / 2297 Hz; the scope and waterfall draw |
| `ctcss_dx_fmt_reader` 0x200635ec → `ctcss_resonator_sample` 0x20063e5c, in `ctcss_detector_tick` 0x200636b0 | FM with TSQL on: `FUN_200527b8` calls `ctcss_detector_start` 0x20063548(tone index) → `ctcss_detector_config` 0x200639d0(tone×10, 1); state byte 0x203fc6e7 = 1 (7 = another user, likely tone scan) | fractional resampler (step 0x203903d8) → tuned IIR resonator at the tone from the 50-entry CTCSS table 0x2018b558 (670…2541 = 67.0…254.1 Hz) → ±60 limiter ring → "tone present" 0x203fc6f3 | TSQL 88.5: 88.5 Hz detected at 0.1 and 0.01 FS; 85.4, 91.5, 100 Hz and silence rejected |

**RTTY text is decoded on the CPU, but not from DX_FMT** ✅ (static; the live half is blocked,
see below). The DSP demodulates FSK and drives the mark/space bit onto a GPIO wire. The CPU
does the rest:

1. **DSP**: mark/shift config at 0x11801250 picks mark from the float table 0x11825048 (1275,
   1615, **2125**, 1700, …) and shift from 0x11825250 (**170**, 200, 425, 850), and computes space
   = mark + shift. Mode 4/5 of the 0x22 jump table install DP+51 = 0x1180282c (every other mode
   installs 0x1180a12c). That callback is an envelope/level detector writing C1 byte 1. The code
   that drives the RTD output wasn't located (candidate: the shared per-block function
   0x1180a140). DSP side from a Sonnet subagent; the two float tables are verified in
   `front_cpu.bin`, the rest isn't.
2. **Wire**: `RTD` = P8_7 (DSP `GP7[7]`/`EMB_A[5]`), read as PPR8 bit 7 (0xFCFE3220).
3. **Sampler**: `rtty_rx_timer_start` 0x200b0850 sets TGRA_1 = TCNT_1 + 8000 and registers
   event 0x92 = GIC 146 = **MTU2 ch1 TGI1A** (TCR_1 0xFCFF0380, TIER_1 0x384, TSR_1 0x385, TCNT_1
   0x386, TGRA_1 0x388, per the SVD). Its ISR `rtty_rx_tgi1a_sample_rtd` 0x200b0ad8 re-arms +8000
   (1 ms) and passes the pin level to `rtty_rx_uart_bit` 0x200b0928.
4. **UART** (state 0x20414c3c): a start bit is a 1→0 edge. The first data bit is sampled 33
   ticks later, then one every 22 ticks (22 ms = 45.45 Bd). 5 bits are shifted into +5 and the
   stop bit is checked. Then code → +6, +7 bit 0 = new, bit 1 = framing error.
5. **Baudot**: `rtty_rx_baudot_to_ascii` 0x200b0bdc handles 0x1b = FIGS and 0x1f = LTRS. It maps
   the rest through the **interleaved** table 0x20335f20 (`code*2 + shift`: `E3`, `\n\n`, `A-`,
   …; interleaving is why a plain ITA2 string search missed it) into a 16-entry ring 0x203df1e2
   (write index 0x203df1f2).
6. **Display**: `rtty_decode_rx_char_drain` 0x200137e0 → `rtty_decode_rx_char_filter` 0x200135ac
   (CR/LF handling) → `rtty_rx_char_append` 0x20014604 (attribute 2 = receive colour; TX echo
   uses 3) → decode screen text + log.

The RX-enable decision (0x20414c3c byte 0) lives in the same Ghidra function,
`rtty_rx_enable_and_decode`: its entry is 0x20184970, and its body also covers the Baudot code at
0x200b0bdc, which carries the label `rtty_rx_baudot_to_ascii`. It needs byte 0x203def00+0x2df and a few flags; it then calls
`rtty_rx_timer_start(2, …)`.

**In the emulator** ✅ (2026-09-25): the RX path decodes end to end.
- `mtu2.c` models ch1: TSTR = 0xc3 at runtime, i.e. CST1 is set by init, and TGRA_1 advances by
  8000 per ISR run.
- `ssif.c`'s fake-DSP demodulator drives RTD from the fmt source. It is active only while the
  fake DSP's mode, opcode 0x22 byte 1, is 4 (RTTY) or 5 (RTTY-R). CI-V `06 08` gives DSP code 5,
  confirmed by the inversion working.
- Polarity: mark = 1, and mark is the lower tone.
- The user's 10 s sample decodes on screen to "WELCOME TO WIKIPEDIA, THE FREE ENCYCLOPEDIA
  THAT". The spectrum-mirrored copy decodes to the same text in RTTY-R. An offline Python copy
  of the demod plus the firmware's UART logic produces the identical string.
- Decode-screen text: the text object at 0x20397166 (`DAT_20013d14`), plain ASCII.
- Open: how the real DSP picks mark/shift. The CPU presumably sends the RTTY DECODE SET
  settings in some opcode; the model uses env knobs instead.

## CI-V settings sweep: what reaches the DSP vs the FPGA (2026-09-24)

`qemu-machine/tools/civ_dsp_sweep.py` boots the emulator with SCIF0 (CI-V) on a socket, sends one
CI-V command per step (`tools/civ.py`), and collects the device debug lines logged during that step.
To make CI-V work, `scif.c` gained RXI for chardev bytes and the **single-wire bus echo**: the
firmware's CI-V transmitter waits for each byte to come back before sending the next. The synthetic
EEPROM leaves the CI-V address at **0x00** (real default 0x94). Replies were correct: 03 →
14.100.000, 04 → USB FIL2, 19 00 → 0x94, 27 10 → scope OFF (the default).

**Scope settings never reach the DSP.** They go to the **FPGA over RSPI2** (the SCP* pins) as
short frames `[type][payload][seq<<4][0x90]`, where the sequence nibble increments per frame:

| CI-V change | RSPI2 frame |
|---|---|
| scope ON (27 10 01) | `00 06 00 05 dc 00 32 10 90` (2nd byte 06, 16 on later ONs) |
| center mode (27 14 00 00) | `01 00 05 dc 00 32 .. 90` |
| fixed mode (27 14 00 01) | `01 28 d9 e0 00 00 .. 90` |
| scroll-C / scroll-F | like center / fixed |
| span ±5/10/25/50/100/500 kHz | `04 hh ll .. 90`, hhll = half-span / 50 Hz (0x64…0x2710); `00 32` above = the ±2.5 kHz default |
| speed, hold, edge | nothing on any link (the CPU handles them) |

So the band scope is fed by the FPGA, and emulating it means modelling FPGA replies on RSPI2 MISO
(SCPR). The DSP's SSIF audio stream feeds the separate *audio* FFT (see above).

**Mode changes drive the DSP** (opcode 0x22 byte1 = mode code, confirmed):

| Mode | 0x22 | Other words that changed |
|---|---|---|
| USB | `22000000` | 21 `096001` |
| LSB | `22010100` | 21 `096201` |
| CW | `22020200` | 00 `600406`, 01 bit 3 set (`89`), 21 `000201`, 40 `5555` |
| RTTY | `22040400` | 10 f+1275 Hz (mark offset), 00 `400c06`, 21 `088201`, 23 cleared |
| AM | `220a0a00` | 00 `400406`, 20 `027804`, 23 `00ff00` |
| FM | `220c0c00` | 00 `400506`, 21 `000027` |

Frequency changes send the two 0x10 words (high half first): 7.100 MHz → `1000006c`, `1080e300`.
