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
