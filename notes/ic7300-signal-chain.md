# IC-7300 signal chain (from service manual "Circuit Description", §3-1–3-3)

Source: user-supplied service manual pages 3-1 through 3-6 (2026-08-27),
OCR'd/read directly via PDF page extraction from
`/data/misc/icom/7300/doc/IC-7300_Servicio.pdf` (the actual filename —
Spanish for "service", not a translated-content indicator; body text is
English). Complements [[ic7300-hardware]] (which chip is which) with
what each one actually *does* in the signal path — useful context for
interpreting DSP/FPGA-adjacent code and control registers if that work
ever resumes.

## Overall architecture: Direct Sampling / Direct Conversion

The IC-7300 has **no analog heterodyne IF stage at all** — explicitly
stated in the manual: "Since the IF signal is directly processed as a
digital signal in the FPGA, any analog heterodyne circuits, such as IF
circuits, are not needed." RF goes straight into an ADC; everything else
(downconversion, demod, filtering) is digital.

## Receive chain

```
Antenna → TUNER UNIT → RF UNIT (atten + 15-filter bank + preamp)
        → MAIN UNIT: PIN-diode attenuator (D1251, AGC-controlled by IC971 DAC)
        → A/D converter
        → FPGA (IC1351): digital mixer → 36 kHz BPF → image-rejection
          mixer (90° phase shift pair) → LO from on-chip DDS
        → DSP (IC901, TMS320C6745): BPF → IF AMP → DET (demod) → AGC
        → D/A (IC991) → AF AMP (IC992) → D-class power amp (IC721)
        → speaker (with external-speaker jack cutover via Q746)
```

Key quote on the AGC redesign versus older Icom radios: *"In a
traditional transceiver, the received signal level (gain) was controlled
by the DSP prior to the external AGC circuit. But this method will not
work in the Direct Sampling system since the RF gain control cannot
prevent over input to the A/D converter. This method has been replaced
by adding an external AGC circuit, instead of using the DSP's internal
AGC circuitry."* — i.e. gain control had to move *outside* the DSP and
in front of the ADC specifically because of the direct-sampling
architecture (see IC971 DAC → PIN attenuator D1251 above) — worth
remembering if DSP AGC-related code is ever found and looks
unexpectedly inert; the real AGC loop is elsewhere.

**DSP note with a real research lead**: the manual states the DSP
(IC901)'s "design is based totally on that of the IC-7100" — internal
AGC, demod, noise reduction (NB/NR/notch), and squelch are stated as
literally the same as the IC-7100's. If IC-7100 DSP firmware or format
documentation ever surfaces publicly, it may be directly informative for
understanding IC-7300 DSP firmware structure — worth a search if the DSP
image location/format ever becomes the active thread again (see
[[multi-cpu-images]]).

## Transmit chain

```
MIC → MIC AMP (IC1002) → A/D (IC1001) → DSP (IC901): BPF → MOD → 36 kHz
    transmit IF → FPGA (IC1351): mixer/image-rejection-mixer up-conversion
    (LO from on-chip DDS) → D/A (IC1331) → LPF (L1281/L1282/C1281/C1283/C1285)
    → RF UNIT: gain-adjust (D1051/D1052) → YGR AMP (IC1031) → PIN attenuator
    → BPF bank → RF UNIT drive amp chain
    → PA UNIT: pre-drive (Q101) → drive (Q111/Q121) → push-pull power amp
      (Q131/Q132, 100W HF/50MHz or 50W 70MHz) → 7-filter Chebyshev LPF bank
      (relay-selected) → SWR/forward-power detection (D961/D962 + IC981 buffer)
    → TUNER UNIT → antenna
```

**Correction to [[ic7300-hardware]]'s IC1331 entry**: I'd initially
guessed IC1331 (`ISL5857IAZ`) was a receive-side digital down-converter,
extrapolating from the chip's general-purpose datasheet capability. The
manual's own block diagram (§3-2, "RF signal processing") explicitly
labels it as the **D/A converter for the transmit path** — it converts
the FPGA's digitally up-converted transmit IF into an analog RF signal
feeding the RF unit's LPF. The ISL5857 is in fact specifically an
interpolating TX DAC with digital up-conversion (Intersil calls it a
"TxDAC+"), so this is consistent with the part itself — my error was
guessing it ran the receive-side down-conversion instead, which the FPGA
block diagram shows is done *digitally inside the FPGA*, not by a
separate DDC chip at all. Fixed in [[ic7300-hardware]].

## FPGA configuration — refines a [[multi-cpu-images]] hypothesis

Manual, verbatim: *"The program that determines how it operates is
written in the external EEPROM, and loaded when the transceiver power is
turned ON."* — stated specifically in the FPGA (IC1351) description,
standard Altera/Intel passive-serial configuration behavior (matches the
`DCLK`/`DATA0` pins the user identified on the FPGA package early in
this project).

This **refines, and likely corrects**, the working assumption in
[[multi-cpu-images]]'s "Strong new lead" section that `IC902`
(`EN25QH32A`) is "the DSP's own flash" (based on physical proximity to
IC901 on the schematic). Given the FPGA's config source is described
here as a dedicated external EEPROM, and IC902's `DO`/`DI`/`CLK`/`CS`
lines were independently found routed toward the FPGA's `DCLK`/`DATA0`
pins (the standard passive-serial config pair) — **IC902 is more likely
the FPGA's own configuration flash, not the DSP's**, with physical
proximity to IC901 being coincidental board layout rather than a
functional link. This leaves an open question the multi-cpu-images
investigation hadn't previously framed correctly: **where does the DSP's
own program actually come from**, if not IC902? Plausible answer: the
DSP has no dedicated persistent flash at all and is boot-loaded by the
main CPU into DSP-accessible RAM at power-on — which would be consistent
with the already-found genuinely-separate async queue/DMA-style write
mechanism (`FUN_20025044`, see [[multi-cpu-images]]) used for the "3
extra chunks" during firmware updates, rather than that mechanism being
(as previously framed) about updating "the DSP's flash". Worth
re-reading that section's conclusions with this correction in mind
before extending the investigation further.

## DSP/FPGA control signal mapping — user-derived from schematics, cross-referenced against the RZ/A1H manual (12th session)

User read the schematics directly and produced a CPU-pin → DSP-pin → FPGA-pin table for every
DSP/FPGA-adjacent signal (20 signals total, all on CPU Ports 2/3/8). Cross-referenced every CPU pin
against the RZ/A1H manual's multi-function pin table to identify the actual on-chip peripheral involved,
then checked `body.bin` for real register references — this directly bears on [[multi-cpu-images]]'s open
"where does the DSP get its program, and what does the `FUN_20025044`/ring-buffer mechanism actually
drive" question.

| Signal(s) | CPU pin(s) | Manual alt-function | Peripheral | Confirmed referenced in `body.bin`? |
|---|---|---|---|---|
| `BCLK_` | P2_8 | `SSISCK0` | SSIF0 bit clock | ✅ yes (see below) |
| `FRM_` | P2_9 | `SSIWS0` | SSIF0 word select (frame sync) | ✅ yes |
| `DX_REC` | P2_10 | `SSIRxD0` | SSIF0 receive data | ✅ yes |
| `DR_AF` | P2_11 | `SSITxD0` | SSIF0 transmit data | ✅ yes |
| `DX_FMT` | P3_6 | `SSIRxD1` | SSIF1 receive data | ✅ yes |
| `DR_RSV` | P3_7 | `SSITxD1` | SSIF1 transmit data | ✅ yes |
| `SCPCK`/`SCPSS`/`SCPX`/`SCPR` | P8_3/4/6/5 | `RSPCK2`/`SSL20`/`MOSI2`/`MISO2` | RSPI channel 2, base `0xE800D800` | 🟡 only inside a generic multi-peripheral init table (`~0x200b7310`), not a dedicated driver |
| `DSPCK`/`DSPR`/`DSPX` | P8_0/1/2 | `SSL00`/`MOSI0`/`MISO0` | RSPI channel 0, base `0xE800C800` | ❌ no reference found anywhere |
| `HSK0`/`HSK1`/`FRWT`/`FPDX`/`DCSX`/`DCSR`/`FPSX`/`FPSR` | P8_8-15 | `SPBIO0x_`/`SPBCLK_`/`SPBSSL_` | **SPI Multi I/O Bus Controller channel 1**, base `0x3FEFB000` (channel 0, base `0x3FEFA000`, is the confirmed XIP boot-flash controller — see [[base-loader]]) | ❌ no reference found anywhere, in `body.bin` or `base.dat` |
| `DRESD` | P2_6 | (plain GPIO — no alt function claimed) | Reset/write-protect line, wired to both `IC901` (DSP) pin 145 and `EN25QH32A`'s `WP` pin | not checked this session |

**The clean, confirmed finding: `BCLK_`/`FRM_`/`DX_REC`/`DR_AF`/`DX_FMT`/`DR_RSV` together form a real,
actively-used CPU↔DSP digital audio link** via the RZ/A1H's Serial Sound Interface, using **both SSIF
channels 0 and 1 together** (a standard RZ/A1H pairing mode where SSIF1 shares SSIF0's clock/word-select
to create a synchronized 2-channel stream). Found a dedicated setup table around `0x20060700`-`0x20060770`
referencing `SSICR_0` (`0xE820B000`), `SSICR_1`/other SSIF1 registers (`0xE820B800`+), *and* what look like
paired DMAC channel addresses (`0xE8200000` range) in the same table — consistent with a genuine
DMA-driven continuous audio/IQ sample stream between the main CPU and the DSP, not just a one-time control
handshake. This is almost certainly the digitized-audio path the service manual's block diagram already
describes (DSP ↔ FPGA ↔ analog front end), now traced to real register-level firmware evidence.

**Genuinely negative, and telling**: the SPI Multi I/O Bus Controller's *second* channel (`0x3FEFB000`,
which P8_8-15's alternate-function names point straight at) has **zero references anywhere** in either
`body.bin` or `base.dat` — checked via the same page-prefix hex search technique used successfully
elsewhere in this project. If this channel really is what those 8 pins use, **this firmware doesn't
configure or use it** — a real, if inconclusive, result: either this hardware capability goes unused (a
populated-but-inert design, similar to several diode-matrix findings), or these particular pins are
actually driven as plain GPIO after all (their Port 8 data register also isn't referenced directly as a
literal, so this doesn't resolve cleanly either way — see next paragraph). RSPI0 (the `DSPCK`/`DSPX`/
`DSPR` candidate) is similarly unreferenced as a direct literal.

**Important caveat carried over from the diode-matrix work**: every register base found here (`P2`/`P3`/
`P8` at `0xFCFE3008`/`300C`/`3020`, and `RSPI2` at `0xE800D800`) turns up **only inside large, generic,
multi-peripheral bulk-initialization tables** (the same `~0x200bXXXX`-region tables already documented for
the diode matrix's Port 5 setup) — not as a dedicated runtime driver reading/writing that specific
register on its own. That's expected for one-time boot/mode configuration, but it means **this session
hasn't yet found the actual runtime code that drives ongoing RSPI2/FPGA traffic**, if any exists beyond
initial setup — the SSIF0/1 audio path is the one clear exception, with its own dedicated-looking table.

**Concrete next steps, in priority order**:
1. Decompile the function containing the `0x20060700` SSIF/DMAC table fully, to confirm the DMA-driven
   audio-streaming read and nail down exactly which RAM buffers it moves data to/from.
2. Decompile whichever function contains the `~0x200b7310` RSPI2 entries — this is the best lead for
   understanding *if and how* the main CPU talks to the FPGA post-configuration (recall the FPGA's
   *bitstream* comes from `IC902` per the manual; this RSPI2 link, if actually used at runtime, would be a
   separate control/status channel, not bitstream loading).
3. `DRESD` (P2_6) — a plain GPIO reset/write-protect line touching both the DSP and its neighbor flash —
   not checked this session at all; would settle whether/when the main CPU actually resets or halts the
   DSP, directly relevant to the "how does the DSP get its program" question.
4. None of this has yet been tied back to the unidentified ring-buffer consumer task from
   [[multi-cpu-images]]'s `FUN_20025044` trace — worth checking whether that consumer ultimately calls
   into the RSPI2 or SSIF/DMAC code found here, which would finally connect the two open threads.

## Schematic sheet map (2026-08-27 sweep)

Swept all 17 pages of `/data/misc/icom/7300/doc/IC-7300_Schematic_Diagram_2.pdf`
(A3, one 17-page PDF — confirmed duplicated in the service manual itself
as §8 General Wiring/§9 Block Diagram, pages 57-61, just with TX/RX
color-coding overlaid — no unique content there beyond the color, don't
re-process). Wholesale reading is reliable for big landmarks (unit
boundaries, IC references, named signal groups); *not* reliable for
fine pin/net-level detail at this render resolution — see the
discussion earlier in this conversation. Sheet map, useful for jumping
straight to the right page instead of guessing:

| Sheet | Content |
|---|---|
| 1 | General wiring — unit-to-unit cable/connector map, `[ALC]`/`[SEND]`/`[TUNER]`/`[DC]`/`[ANT]`/`[USB]`/`[REMOTE]`/`[EXT-SP]` jacks |
| 2 | General wiring — Front Unit's own sub-boards (Mic/Phone/RIT/VR/PBT/Display/SD boards) pinouts |
| 3 | Block diagram — PA/Tuner/RF unit signal flow, exact filter-bank cutoffs (see below) |
| 4 | Block diagram — Main Unit (IC301 main CPU, IC901 DSP, IC1351 FPGA, IC902/IC391 flashes) |
| 5 | Block diagram — Display/Front Unit (IC501 front CPU, button matrix, LCD) |
| 6 | Front Unit detailed schematic — `IC501 R5F104LCAFB` visibly marked `(SX-3765C)` right on the schematic (independent confirmation #4 of the SX3765 finding) |
| 7 | Main Unit detailed schematic (MAIN-1) — PA/RF unit power supply feeds, audio/IF regulator cluster |
| 8 | Main Unit detailed schematic (MAIN-2) — **full IC301 pinout**, EEPROM (IC351)/RTC (IC381) detail, **JTAG cluster** (`TCK`/`TMS`/`TDI`/`TRST` labeled pins + connector, matches [[hardware-debug-access]]'s finding exactly) |
| 9 | Main Unit detailed schematic (MAIN-3) — USB subsystem, ACC connector (J771) pinout |
| 10 | Main Unit detailed schematic (MAIN-4) — **IC901 (DSP) full pinout**, IC902 flash detail, audio DAC/ADC cluster (IC971/IC991/IC1001/IC1002) |
| 11 | Main Unit detailed schematic (MAIN-5) — RX-ADC block, IC1331 (D/A) detail |
| 12 | Main Unit detailed schematic (MAIN-6) — **IC1351 (FPGA) full pinout**, including the `DONE`/`STAT`/`CFG`/`SPCK` config-handshake cluster central to the IC902 question above |
| 13 | RF Unit detailed schematic — full filter-bank component values |
| 14-15 | Tuner Unit detailed schematic — relay-driven L-network, motor control |
| 16-17 | PA Unit detailed schematic — LPF bank component values, drive/final amplifier stages |

## Precise filter-bank cutoffs (sheet 3, exact values not previously in notes)

**PA unit transmit LPFs (7 total)**: `0.03-2.0`, `2.0-4.0`, `4.0-7.3`,
`7.3-14.35`, `14.35-21.45`, `21.45-33.0`, `33.0-76.0 MHz`.

**RF unit receive filters (confirms/extends the "15 filters" estimate in
this file's earlier section)**: `0.03-1.59`, `1.60-1.99` (plus wideband
`1.60-30.00`/`1.60-74.80` HPF/LPF stages ahead of the per-segment bank),
`2.00-2.99`, `3.00-4.49`, `4.50-6.49`, `6.50-7.99`, `8.00-9.99`,
`10.00-14.99`, `15.00-21.99`, `22.00-29.99`, `30.00-49.99`/`54.01-69.99`
(shared filter), `50.00-54.00`, `70.00-74.80 MHz`, plus two **TX-only**
segments `50.00-54.00 MHz (TX)` and `70.00-72.00 MHz (TX)` not used on
receive.

## Reference clocks (useful if FPGA-bitstream-adjacent timing ever matters)

- `X1201`: 41.344 MHz crystal — feeds `IC1211` (buffer amp) as the main
  reference, also related to `IC1221`'s output
- `IC1221`: crystal buffer producing 124.032 MHz (= 3 × 41.344 MHz) —
  labeled `DACLK` in the diagram, almost certainly the ADC/DAC sample
  clock
- `IC1212`: "LVDS DRIVE" block, sits in the same clock-distribution
  cluster — likely carries a clock or the digital IF stream over LVDS
  to/from another board; not traced further

## Official specifications (§1, pages 1-1/1-2) — cross-checks for [[diode-matrix]]

- Receive coverage: `0.030000~74.800000 MHz` (footnoted "some frequency
  ranges are not guaranteed") — matches D416's confirmed general-coverage
  RX unlock finding in [[diode-matrix]] (`0.030–74.795 MHz` there; the
  ~5 kHz difference is almost certainly just table-granularity/rounding
  between the firmware's internal segment table and the spec sheet's
  headline number, not a real conflict).
- Transmit bands, each marked "*2 depending on the transceiver version"
  (i.e. regionally gated, see [[diode-matrix]]): `1.8`, `3.5`, `5.255~
  5.405` (the 60m/5MHz segment — exact match for D405's confirmed
  finding), `7`, `10.1`, `14`, `18.068~18.168`, `21`, `24.89~24.99`,
  `28`, `50`, and `70~70.5 MHz` (the 4m band — see the version/band table
  above, `EUR`/`ITR`/`ESP` only).
- Receive system: "Direct sampling superheterodyne", IF = 36 kHz —
  matches [[ic7300-signal-chain]]'s own architecture section above
  exactly (confirms the manual is internally consistent between the
  circuit-description and specifications sections, as expected, but
  worth having both independently for cross-checking).
- Antenna tuner: 16.7–150 Ω tunable range, <3:1 VSWR, 2–3s typical tuning
  time (15s max) — general reference, not yet tied to any specific
  firmware code.

## Other useful facts from this section

- RF UNIT has **15 receive filters** and **~14 transmit BPFs/LPFs**
  (band-segmented, see the per-band frequency ranges in the circuit
  diagrams — e.g. `0.03-1.59 MHz`, `1.60-1.99 MHz`, ... `70.00-74.80
  MHz`) — the same kind of band segmentation the diode-matrix regional
  gating (see [[diode-matrix]]) ultimately restricts access to.
- Receive path has **two selectable preamps** (P.AMP1 normal gain,
  P.AMP2 higher gain) plus bypass, and an attenuator (D1081–D1083, 0–20
  dB) — all under digital control from the main CPU, same general idiom
  as the diode-matrix-driven config elsewhere in this radio.
- PA unit's LPF bank is **7 Chebyshev filters**, relay-selected by
  transmit frequency — the harmonic-filtering equivalent of the RF
  unit's receive filter bank.
- SWR/forward-power sensing: `D961`/`D962` (CM coupler + rectifier
  diodes) → buffered by `IC981` → read by the main-unit CPU for
  monitoring/protection — this is the physical sensor path behind
  whatever SWR-protection logic exists in the main firmware, worth
  knowing if that code is ever traced.
