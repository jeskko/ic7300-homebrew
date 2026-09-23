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
| C0 | 0x11817ba8 | 0x00000000 | 0 | top priority; writers TBD |
| C1 | 0x11817bb0 | 0x10000000 | 1 | CPU: "step towards target using byte 2" (level slew); writers TBD |
| C2 | 0x11817bb8 | 0x20000000 | 2 | CPU: trivial ack |
| C3 | 0x11817bc0 | 0x70000000 | 7 | TBD |
| C4 | 0x11817bc8 | 0x80000000 | 8 | CPU: 4-field status payload |
| C5 | 0x11817bd0 | 0xE0000000 | E | firmware page verify: page counter + −checksum (B0–BF handler) |
| C6 | 0x11817bd8 | 0xF0000000 | F | identity reply (E0 handler), stamp at 0x11817bdc |

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

| Opcode | Handler | P slot | Notes |
|---|---|---|---|
| 0x00 | 0x1180a030 | 0x11817b20 | bit 23 selects TX fallback C1 vs C2 |
| 0x01 | 0x1180a00c | 0x11817b24 | store only |
| 0x10 | 0x11809fb8 | 0x11817b28 (+0x2c?) | RX frequency, 2 words split on bit 23: `0x10 0 hh` = f>>16, `0x10 8 llll` = f & 0xffff (CPU sends f + 36 kHz) |
| 0x20–0x25, 0x27 | … | 0x11817b30–b48 | TBD |
| 0x40–0x44, 0x48–0x4f | … | 0x11817b4c–b7c | TBD (0x43 also fast-pathed in the ISR) |
| 0x61, 0x62, 0x6b | … | 0x11817b80–b88 | TBD |
| 0x80, 0x81 | 0x11804d30, 0x11804d08 | 0x11817b8c, b90 | TBD |
| 0xA0–0xAF | 0x11804cd4 | 0x11817ba0 | → 0x11804bf0, TBD |
| 0xB0–0xBF | 0x11804bc4 | 0x11817ba4 | **firmware page data**: 3 data bytes per word (bytes 2,1,0) into buffer 0x118185e8; 8-bit running sum; updates C5 |
| 0xE0 | 0x11804728 | 0x11817b94 | **identity query**, see below |
| 0xE1 | 0x118046f4 | 0x11817b98 | flash: loops k=5..10 calling 0x11815964(k<<15), i.e. erases 32 KB blocks 0x28000–0x50000 (probable) |
| 0xE2 | 0x11804624 | 0x11817b9c | flash: set page address (sets up C5, clears the update-mode flag, may erase via 0x1181580c) |
| 0xE3 | 0x118044e4 | ? | → 0x11804430, TBD |

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
