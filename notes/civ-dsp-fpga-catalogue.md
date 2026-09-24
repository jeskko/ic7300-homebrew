# CI-V command → DSP / FPGA traffic catalogue (IC-7300 v1.42, emulator)

Built 2026-09-24 with `qemu-machine/tools/civ_dsp_sweep.py --debug dsp,rspi2`: boot with the
factory-default EEPROM (CI-V address 0x94), send one CI-V command per step, and record the fake
DSP's changed command words (SCIF5, [[dsp-protocol]]) and the RSPI2 frames to the FPGA that the
step caused. Raw output: [assets/2026-09-24-civ-sweep-dsp-fpga.txt](assets/2026-09-24-civ-sweep-dsp-fpga.txt).
FB/FA = the radio's own OK/NG reply. "none" = the command was accepted but nothing changed on
either link, i.e. the CPU handles it itself (or it only matters in TX).

## FPGA (RSPI2): scope configuration only

Each change is two transfers: a command frame whose last byte is a sequence number (`seq<<4`,
+0x10 per frame), then a lone `90`. **Correction (2026-09-24):** `90` is the sweep-read command, not a latch or commit. The CPU then reads a header byte and 475 samples; see [fpga-link.md](fpga-link.md), which also decodes every register.

| Frame | Meaning | Evidence |
|---|---|---|
| `00 06 00 05 dc 01 f4 ss` + `06 20` | scope ON (initial config) | 27 10 01 |
| `01 00 05 dc 01 f4 ss` | mode center / scroll-C: `05dc` (1500, unknown) + span `01f4` | 27 14 00 00/02 |
| `01 28 d9 e0 00 00 ss` | mode fixed / scroll-F: `28d9e0` probably an edge frequency (unit unknown) | 27 14 00 01/03 |
| `04 hh ll ss` | span: hhll = half-span / 50 Hz (±2.5k 0x32, ±5k 0x64, ±25k 0x1f4, ±250k 0x1388, ±500k 0x2710) | 27 15 |

No FPGA traffic for: ref level (27 19), speed (27 1A), VBW (27 1D), scope during TX (27 1B),
hold (27 17), edge (27 16). The factory default span is ±25 kHz (0x1f4).

## DSP (SCIF5 opcodes)

| Control (CI-V) | DSP word(s) that change | Reading |
|---|---|---|
| NB on/off (16 22) | 0x00 bit 14 | `00404506` / `00400506` |
| NB level (14 12) | 0x24 byte 0 = level | `240780c8` for 200 |
| NR on/off (16 40) | 0x00 bit 13 | + 0x4c = NR level |
| NR level (14 06) | 0x4c = level 0..15 | 200 → `0c` |
| auto notch (16 41) | 0x00 bit 12 | |
| manual notch (16 48) | 0x00 bit 6; 0x25 = notch position/width | `2501012c`, pos 200 → `2501024c` |
| AGC fast/mid/slow (16 12) | 0x23 bits 7..0 = 02 / 07 / 0c | time-constant index |
| RF gain (14 02) | 0x23 byte 1 | 100 → `0x64` |
| AF gain (14 01) | 0x42 byte 0 = level (bytes 1..2 vary too) | `42808032` (50), `422e2ec8` (200) |
| PBT in/out (14 07/08) | 0x20 low / high 9-bit fields | centre `2002986c` |
| filter FIL1/2/3 (06 01 0n) | 0x20 | `20028078` / `2002986c` / `2002b060` |
| filter shape soft/sharp (16 56) | 0x20 (sharp restores `2002986c`) | |
| data mode (1A 06) | 0x22 byte 0 (0x13 → 0x19) + 0x20 | |
| comp on (16 44) | 0x40 bit 18 | `40045555` |
| mode (06 0m) | 0x22 byte 1 = mode (USB 00, LSB 01, CW 02, RTTY 04, AM 0a, FM 0c), 0x20/0x21/0x23/0x41 | see [[dsp-protocol]] |
| frequency (05) | two 0x10 words (f + 36 kHz IF) | |
| PTT (1C 00) | 0x01 bit 7 cleared = TX; 0x00 changes; 0x4a = TX parameters (`4a6464c8`) | TX off restores |
| preamp (16 02), att (11) | none | probably front-end GPIO/latches |
| squelch, RF power, mic gain, comp level, monitor, VOX, key speed, break-in, CW pitch | none in RX | probably only sent in TX |

Rejected (FA) in this setup: 27 11 (scope data out, may need a CI-V/USB setting), 27 1C (not an
IC-7300 command), 1C 01 (no tuner), 50.100 MHz via 05 (not investigated). "No reply" steps
(att 20 dB, FIL1, CW) are probably collisions with the radio's own CI-V transceive broadcasts,
which the factory defaults turn on.
