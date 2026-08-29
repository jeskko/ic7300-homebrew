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

**Superseded, 2026-08-29 — this "refinement" was itself wrong, retracted in [[multi-cpu-images]].** The
FPGA-EEPROM manual quote above is real, but IC902 isn't that EEPROM: the user traced IC902's `CS`/`DO`/`DI`/
`CLK` pins directly to `IC901` (the DSP) pins 9/17/18/11 — which double as `BOOT[4]`/`BOOT[0]`/`BOOT[1]`/
`BOOT[2]`, the DSP's own boot-mode strapping pins. That's a direct point-to-point connection, stronger
evidence than the net-label routing this section's correction was based on. **IC902 is IC901's own SPI
boot flash** — the DSP self-boots from it via its internal ROM bootloader. See [[multi-cpu-images]]'s
"`IC902` identity, corrected again" section for the full reasoning and what it means for DSP firmware
updates (short version: `IC901`, already running from its current `IC902` contents, is almost certainly
what reprograms `IC902` on the main CPU's behalf during a firmware update — the DSP is the only thing with
electrical access to that flash at all).

## DSP/FPGA control signal mapping — user-derived from schematics, cross-referenced against the RZ/A1H manual (12th session)

User read the schematics directly and produced a CPU-pin → DSP-pin → FPGA-pin table for every
DSP/FPGA-adjacent signal (20 signals total, all on CPU Ports 2/3/8). Cross-referenced every CPU pin
against the RZ/A1H manual's multi-function pin table to identify the actual on-chip peripheral involved,
then checked `body.bin` for real register references — this directly bears on [[multi-cpu-images]]'s open
"where does the DSP get its program, and what does the `FUN_20025044`/ring-buffer mechanism actually
drive" question.

| Signal(s) | CPU pin(s) | Manual alt-function | Peripheral | DSP pin (`IC901`, TMS320C6745) | FPGA pin (`IC1351`) | Confirmed referenced in `body.bin`? |
|---|---|---|---|---|---|---|
| `BCLK_` | P2_8 | `SSISCK0` | SSIF0 bit clock | 126, `ACLKX0`/`ECAP0`/`APWM0`/`GP2[12]` (McASP0 bit clock) | `A19`, `RDN4` | ✅ yes (see below) |
| `FRM_` | P2_9 | `SSIWS0` | SSIF0 word select (frame sync) | 127, `AFSX0`/`GP2[13]`/`BOOT[10]` (McASP0 frame sync) | `A16`, `DIFFIO_T30n` | ✅ yes |
| `DX_REC` | P2_10 | `SSIRxD0` | SSIF0 receive data | 116, `AXR0[4]`/`RMII_RXD[0]`/`GP3[4]` (McASP0 serializer 4) | — | ✅ yes |
| `DR_AF` | P2_11 | `SSITxD0` | SSIF0 transmit data | 117, `AXR0[5]`/`RMII_RXD[1]`/`GP3[5]` (McASP0 serializer 5) | — | ✅ yes |
| `DX_FMT` | P3_6 | `SSIRxD1` | SSIF1 receive data | 118, `AXR0[6]`/`RMII_RXER`/`GP3[6]` (McASP0 serializer 6) | — | ✅ yes |
| `DR_RSV` | P3_7 | `SSITxD1` | SSIF1 transmit data | 120, `AXR0[7]`/`MDIO_CLK`/`GP3[7]` (McASP0 serializer 7) | — | ✅ yes |
| `DSPCK` | P8_0 | `SSL00` | (was: RSPI ch.0) | 162, `ACLKX1`/`EPWM0A`/`GP3[15]` (**McASP1** bit clock) | `Y1`, `DIFFIO_L29n` | ❌ no CPU-side reference found |
| `DSPR` | P8_1 | `MOSI0` | (was: RSPI ch.0) | 175, `AXR1[2]`/`GP4[2]` (McASP1 serializer 2) | `AA1`, `DIFFIO_L31n` | ❌ no CPU-side reference found |
| `DSPX` | P8_2 | `MISO0` | (was: RSPI ch.0) | 176, `AXR1[1]`/`GP4[1]` (McASP1 serializer 1) | — | ❌ no CPU-side reference found |
| `DCSX` | P8_12 | — | (was: SPI Multi I/O ch.1) | 163, `AFSX1`/`EPWMSYNCI`/`EPWMSYNC0`/`GP4[10]` (McASP1 frame sync X) | — | ❌ no reference found |
| `DCSR` | P8_13 | — | (was: SPI Multi I/O ch.1) | 166, `AFSR1`/`GP4[13]` (McASP1 frame sync R) | — | ❌ no reference found |
| `HSK0` | P8_8 | — | (was: SPI Multi I/O ch.1) | 100, `EMB_A[3]`/`GP7[5]` (DSP **external memory bus address bit 3**) | — | ❌ no reference found |
| `HSK1` | P8_9 | — | (was: SPI Multi I/O ch.1) | 98, `EMB_A[4]`/`GP7[6]` (EMIF address bit 4) | — | ✅ **found, 27th session — see below** |
| `FRWT` | P8_10 | — | (was: SPI Multi I/O ch.1) | 96, `EMB_A[6]`/`GP7[8]` (EMIF address bit 6) | — | ❌ no reference found |
| `RTD` | P8_7 | — | (not in original 20-signal table; DSP-side companion of the `EMB_A` group) | 97, `EMB_A[5]`/`GP7[7]` (EMIF address bit 5) | — | ❌ no reference found |
| `FPDX` | P8_11 | — | (was: SPI Multi I/O ch.1) | — (no DSP pin) | `W1`, `DIFFIO_L28n` | ❌ no reference found |
| `FPSX` | P8_14 | — | (was: SPI Multi I/O ch.1) | — (no DSP pin) | `U1`, `DIFFIO_L24n` | ❌ no reference found |
| `FPSR` | P8_15 | — | (was: SPI Multi I/O ch.1) | — (no DSP pin) | `V1`, `DIFFIO_L25n` | ❌ no reference found |
| `SCPCK` | P8_3 | `RSPCK2` | RSPI channel 2, base `0xE800D800` | — (no DSP pin) | `G1`, `DIFFCLK_0n` | ✅ **confirmed live driver, see below (22nd session)** |
| `SCPSS` | P8_4 | `SSL20` | RSPI channel 2 | — (no DSP pin) | `F1`, `DIFFIO_L9n` | ✅ same driver |
| `SCPX` | P8_6 | `MOSI2` | RSPI channel 2 | — (no DSP pin) | `C1`, `DIFFIO_L4n` | ✅ same driver |
| `SCPR` | P8_5 | `MISO2` | RSPI channel 2 | — (no DSP pin) | `E1`, `DIFFIO_L8n` | ✅ same driver |
| `DRESD` | P2_6 | (plain GPIO — no alt function claimed) | Reset/write-protect line | 146, `\RESET` (**confirmed DSP reset input**, active-low per schematic notation — corrected from an earlier "pin 145" transcription) | — | ✅ yes — see below (13th/14th sessions) |

**Note on this table's evolution**: the "Peripheral" column's "(was: ...)" entries are the CPU-manual-alt-function-only guesses from the 12th session, kept visible rather than silently deleted — the 14th session's DSP-side pin data (below) refines or replaces several of them.

**The clean, confirmed finding: `BCLK_`/`FRM_`/`DX_REC`/`DR_AF`/`DX_FMT`/`DR_RSV` together form a real,
actively-used CPU↔DSP digital audio link** via the RZ/A1H's Serial Sound Interface, using **both SSIF
channels 0 and 1 together** (a standard RZ/A1H pairing mode where SSIF1 shares SSIF0's clock/word-select
to create a synchronized 2-channel stream). Found a dedicated setup table around `0x20060700`-`0x20060770`
referencing `SSICR_0` (`0xE820B000`), `SSICR_1`/other SSIF1 registers (`0xE820B800`+), *and* what look like
paired DMAC channel addresses (`0xE8200000` range) in the same table — consistent with a genuine
DMA-driven continuous audio/IQ sample stream between the main CPU and the DSP, not just a one-time control
handshake. This is almost certainly the digitized-audio path the service manual's block diagram already
describes (DSP ↔ FPGA ↔ analog front end), now traced to real register-level firmware evidence.

**DSP-side pin data (14th session) independently confirms this from the other end**: `BCLK_`/`FRM_` land
on DSP pins `ACLKX0`/`AFSX0` (McASP0's shared bit-clock/frame-sync) and `DX_REC`/`DR_AF`/`DX_FMT`/`DR_RSV`
land on `AXR0[4]`/`AXR0[5]`/`AXR0[6]`/`AXR0[7]` — four serializer pins on that **same** McASP0 port. TI's
McASP natively supports exactly this "one clock/frame-sync pair, multiple serializer data pins" topology,
which is precisely the CPU-side "SSIF0+SSIF1 paired, sharing SSIF0's clock/word-select" mechanism already
found in the `0x20060700` table — two completely independent readings (CPU manual + register table vs.
DSP datasheet pinout) landing on the same structure is about as solid as static confirmation gets here.
Also notable: `BCLK_`/`FRM_` **also** land on FPGA pins (`A19`/`RDN4` and `A16`/`DIFFIO_T30n`) — so this
clock/frame-sync pair is shared three ways (CPU/DSP/FPGA), meaning the FPGA is very likely a *listener or
co-participant* on this same synchronized audio/IQ stream, not just adjacent hardware.

**Genuinely negative, refined by 14th-session DSP-pin data**: the SPI Multi I/O Bus Controller's *second*
channel (`0x3FEFB000`) still has **zero references anywhere** in either `body.bin` or `base.dat`, and the
DSP-side pinout now explains why the CPU-manual-alt-function guess was likely wrong to begin with, for two
different sub-groups of these 8 pins:
- `DSPCK`/`DSPR`/`DSPX`/`DCSX`/`DCSR` (`P8_0/1/2/12/13`) land on DSP pins `ACLKX1`/`AXR1[2]`/`AXR1[1]`/
  `AFSX1`/`AFSR1` — **McASP1**, the DSP's *second* audio serial port instance, not SPI at all. Same
  "shared clock/frame-sync + serializer data pin" McASP topology as the confirmed McASP0/SSIF0+1 link
  above, just a second, independent instance — plausibly a second audio/IQ stream (e.g. TX audio, or a
  second IQ pair) running in parallel with the first. `DSPCK`/`DSPR` also land on FPGA pins (`Y1`/
  `DIFFIO_L29n`, `AA1`/`DIFFIO_L31n`), the same three-way sharing pattern as `BCLK_`/`FRM_` above.
- `HSK0`/`HSK1`/`FRWT`/`RTD` (`P8_7/8/9/10`) land on DSP pins `EMB_A[3]`/`EMB_A[4]`/`EMB_A[6]`/`EMB_A[5]` —
  the DSP's **external memory bus address lines**, not a handshake/flow-control protocol as the CPU-side
  alt-function names ("SPBIO"/handshake-style naming) had suggested. ~~Genuinely unclear yet what the main
  CPU does with 4 bits of the DSP's own EMIF address bus tied to its GPIOs~~ — **correction, 27th session**:
  `HSK1` (`P8_9`) *is* referenced after all — the earlier "zero CPU-side references" search only checked the
  port **data** register family (`Pn`, base `0xFCFE3000`); it's actually read via the port **pin-read**
  register family instead (`PPRn`, base `0xFCFE3200` — a different literal the earlier sweep never covered).
  Found in `notes/firmware-update.md`'s dissection of the "3 extra chunks" mechanism: `FUN_20025044` and
  `chunk_transport_send_reload_cmd` (`FUN_20025288`) both poll `PPR8` bit `9` (= `P8_9` = `HSK1`) as a real
  hardware ready/handshake signal while moving Front CPU/DSP Program/DSP Data update data — so `HSK1`
  genuinely *is* used as a handshake line after all, just read rather than the CPU-alt-function names'
  implied "SPBIO" framing, and only from this one specific mechanism. **`HSK0`/`FRWT`/`RTD` haven't been
  re-checked against the `PPRn` family yet** — worth doing before re-asserting "no reference" for those too.
- `FPDX`/`FPSX`/`FPSR` (`P8_11/14/15`) have **no DSP pin at all** — FPGA-only (`W1`/`DIFFIO_L28n`,
  `U1`/`DIFFIO_L24n`, `V1`/`DIFFIO_L25n`). Combined with `SCPCK`/`SCPSS`/`SCPX`/`SCPR` also landing on
  FPGA-only differential pins (`G1`/`DIFFCLK_0n`, `F1`/`DIFFIO_L9n`, `C1`/`DIFFIO_L4n`, `E1`/`DIFFIO_L8n`),
  this whole 7-pin group is a genuine **CPU↔FPGA-only differential I/O bus that never touches the DSP** —
  likely related to the `IC1212` "LVDS DRIVE" clock-distribution block noted separately in this file's
  "Reference clocks" section (worth checking if that's the same net cluster). Still zero confirmed runtime
  driver on the CPU side beyond the generic RSPI2 init-table entry for the `SCP*` subset — see the caveat
  below.

Either way, **this firmware doesn't configure or use the SPI Multi I/O Bus Controller's second channel** —
a real, if inconclusive, result: either that hardware capability goes genuinely unused (a populated-but-
inert design, similar to several diode-matrix findings), or (now the better-supported reading, per the
DSP-side data above) these pins were never SPI-Multi-I/O pins to begin with and the CPU manual's
alternate-function table simply wasn't the right lens for this particular pin group.

**Important caveat carried over from the diode-matrix work, now resolved for RSPI2 specifically**: every
register base found here (`P2`/`P3`/`P8` at `0xFCFE3008`/`300C`/`3020`, and `RSPI2` at `0xE800D800`) turns
up inside large, generic, multi-peripheral bulk-initialization tables (the same `~0x200bXXXX`-region
tables already documented for the diode matrix's Port 5 setup) — that part still stands for one-time
boot/mode configuration in general, but **RSPI2 itself is no longer just a generic-table entry — see
"RSPI2 confirmed as a real, actively-used SPI link to the FPGA" below (22nd session).**

**Concrete next steps, in priority order**:
1. Decompile the function containing the `0x20060700` SSIF/DMAC table fully, to confirm the DMA-driven
   audio-streaming read and nail down exactly which RAM buffers it moves data to/from.
2. ~~Decompile whichever function contains the `~0x200b7310` RSPI2 entries~~ — **done, see "RSPI2 confirmed
   as a real, actively-used SPI link to the FPGA" below (22nd session).**
3. ~~`DRESD` (P2_6)~~ — **done, see "DRESD (P2_6) resolved" below (13th session).**
4. ~~Tie this back to the unidentified ring-buffer consumer task from [[multi-cpu-images]]'s `FUN_20025044`
   trace~~ — **partially done: the consumer (`FUN_200b0f68`) does drive this RSPI2 link, among others, but
   NOT for the `chunk4`/`chunk5` (`0xb0`/`0xe2`-tagged) traffic specifically — see below, this is a real,
   useful negative result, not a full connection.**

## RSPI2 confirmed as a real, actively-used SPI link to the FPGA (22nd session)

Decompiled the function containing the `~0x200b7310` table (it's a literal-pool/data block, not code —
the actual driver functions sit just before it in memory) and traced its users precisely.

**`FUN_200b6c50` — a genuine, byte-at-a-time SPI transmit function over RSPI channel 2**:
```c
void FUN_200b6c50(byte first_byte, int buf, uint count)
{
    do { } while (!(*SPSR2 & 0x40));       // poll RSPI2 status register (0xE800D803) for TX-ready
    *SPCMD2 |= 0x80; *SPCMD2 &= ~0x80;     // toggle a command-register bit (0xE800D820) — direction/mode
    *SPCR2 = 0x48;                          // 0xE800D800 — RSPI2 control register
    *SPDR2_byte = first_byte;               // 0xE800D804 — RSPI2 data register, first byte out
    for (i = 0; i < count; i++)
        *SPDR2_byte = buf[i];                // the rest of the buffer, one byte per iteration
    // then updates a RAM ring-buffer write-position field and signals event 0xa2
}
```
This is unambiguous: real register polling, a real per-byte transmit loop over actual RSPI2 hardware
registers (`SPCR2`/`SPCMD2`/`SPSR2`/`SPDR2`, all at `0xE800D800`-`0xE800D820` exactly matching the RZ/A1H
manual's RSPI channel 2 register block) — **this firmware unambiguously drives real, ongoing SPI traffic
over the pins already mapped to the FPGA's differential I/O (`SCPCK`/`SCPSS`/`SCPX`/`SCPR`)**. The
"not confirmed live" caveat on this link from earlier sessions is retracted.

**Wired into the same generic async job-queue infrastructure documented for `chunk4`/`chunk5` in
[[multi-cpu-images]]** — a genuinely new, useful connection, though not the one originally hoped for.
`FUN_200b0f68` (the ring-buffer drain/consumer function [[multi-cpu-images]] already found servicing the
`chunk4`/`chunk5` producer) turns out to have a real `switch` on each queued job's tag byte, not just
"queue mechanics" as characterized there — 5 concrete cases:
- `0`: signals event `0xa1` (the exact event `chunk4`/`chunk5`'s own tag range was already tied to)
- `1`: a readiness/retry check gating a Port-8 reconfiguration (`FUN_200b0cd4`, already found this session
  while tracing `DRESD` — sets `PSR8` bits `0x10000000`/`0x40000000`)
- `2`: `FUN_200b5cdc`/`FUN_200b5dc0` — the SSIF/DMAC cache-flush-and-transfer function from this file's
  DSP/FPGA audio section
- `3`: **`FUN_200b6c50` — our new RSPI2 transmit function**
- `4`: `FUN_200b38ac` — the same front-panel/SCIF3-adjacent function referenced in this file's front-panel
  section

**Important correction to [[multi-cpu-images]]'s ring-buffer writeup**: `chunk4`/`chunk5`'s own entries are
tagged with `0xb0`-`0xb7`/`0xe2` in the tag byte (`FUN_200b2fc8`/`FUN_200b3040`'s packing scheme) — **none
of which match this switch's small-integer cases (`0`-`4`)**. So `chunk4`/`chunk5`'s tagged jobs, if pushed
through this exact ring buffer, would hit none of these 5 cases and fall through unhandled by
`FUN_200b0f68` itself — **this confirms RSPI2 is real and active, but confirms (doesn't refute) that it is
*not* how `chunk4`/`chunk5` data reaches wherever it goes.** [[multi-cpu-images]]'s open question ("who
consumes the `0xb0`/`0xe2`-tagged entries specifically") remains genuinely open — this session narrows it
by ruling out RSPI2/SSIF/front-panel-UART as the answer, rather than by answering it directly.

**Driver init found too**: `FUN_200b665c` configures Port 8's pins (via the same bit-manipulation idiom as
`port_bulk_gpio_init_pass1`/`pass2`) and registers four event handlers via `register_event_handler` — IDs
`0x115`, `0x2a`, `0x2b`, and `0xa2` (the last one wired to `FUN_200b6444`, RSPI2's own status-wait/init
counterpart to the transmit function above). This is the real RSPI2 driver's setup routine.

**Bottom line**: RSPI2 is a genuine, IRQ-driven, ongoing CPU→FPGA SPI link — resolves this file's
long-standing "real traffic vs. init-only" question with a clear yes. It shares its dispatch/queue plumbing
with the front-panel UART and SSIF/audio-DMA paths (a real, useful map of this firmware's async-job
architecture), but is confirmed *not* the transport for the `chunk4`/`chunk5` mystery specifically.

## `DRESD` (P2_6) resolved — boot-time-only, held at a fixed level (13th session)

Traced by cross-referencing the RZ/A1H manual's port register map (§54.3, `PORTn_base` = `0xFCFE3000`,
sub-block bases `Pn`=`+0`, `PSRn`=`+0x100`, `PPRn`=`+0x200`, `PMn`=`+0x300`, `PMCn`=`+0x400`,
`PFCn`=`+0x500`, `PFCEn`=`+0x600`, `PNOTn`=`+0x700`, `PMSRn`=`+0x800`, `PMCSRn`=`+0x900`,
`PIBCn`=`+0x4000`, `PBDCn`=`+0x4100`, `PIPCn`=`+0x4200`, each `+n×4` for port `n`) against every
literal-pool reference to any of these bases in `body.bin`. This is the same "search for the base literal,
check every candidate's offset" method used successfully for `P5` (diode scan) and `RSPI2`/`SSIF` earlier
in this file — confirmed exhaustive here too: a raw hex search for every P2-register address as a
standalone 4-byte constant (`P2`, `PM2`, `PMC2`, `PFC2`, `PFCE2`, `PPR2`, `PNOT2`, `PMSR2`, `PMCSR2`,
`PIBC2`, `PBDC2`, `PIPC2` — all 12 sub-registers) turns up **zero** hits, and a full `objdump` disassembly
(both Thumb-forced and ARM32) of `body.bin` has **zero** `movw`/`movt` pairs building `0xFCFE3xxx`
anywhere either — so this codebase never builds a port address any way other than a literal-pool
constant, making the literal-pool sweep genuinely exhaustive, not just "nothing obvious found."

The *only* literal-pool constant equal to a P2-block base anywhere in `body.bin` is `0xFCFE7000` (the
`PIBCn` block base), used by exactly two functions — renamed `port_bulk_gpio_init_pass1`
(`FUN_200b4320`) and `port_bulk_gpio_init_pass2` (`FUN_200b4494`) in Ghidra, PLATE-commented with the full
address derivation. Between them they configure **every** port 0-9's data/direction/function-control/
buffer registers in one generic boot-time sweep (P5's direction setup, previously flagged in
[[diode-matrix]] as "very likely folded into one of these two functions" without being verified, is
confirmed present here too — `PM5 <- 0x700`, bits 8-10 input matching the diode-scan row-read pins,
bits 0-7 output matching the column-drive pins, exactly as the scan routine requires).

For `P2` specifically, the two passes leave:

| Register | Pass 1 (`..pass1`) | Pass 2 (`..pass2`, final) | Bit 6 (`DRESD`) |
|---|---|---|---|
| `P2` (data/output) | `0x0000` | `0x0080` (bit7 only) | `0` — **driven LOW** |
| `PM2` (direction) | `0xff80` | `0xf700` | `0` in both — **OUTPUT** |
| `PMC2` (GPIO vs alt-func) | `0x0000` | `0x0000` | `0` — **plain GPIO**, never switched to an alt function |
| `PIBC2` (input buffer enable) | `0x0000` | `0xf700` | not in the enabled set — **input buffer stays off** (consistent with a CPU-drives-only line that's never read back) |

`port_bulk_gpio_init_pass1` is called from `FUN_20029ca4` (the power-state main loop — the one with the
watchdog-kick magic-number sequences `0x5a5f`/`0x5afe`/`0xa57f` and the `WaitForInterrupt()` loop), so it
re-runs on every power-on/standby-wake transition, not just once ever. `port_bulk_gpio_init_pass2` is
called once from `FUN_2002afc0`, the cold-boot init sequence, immediately before `itron_act_tsk()` starts
the RTOS scheduler's first task — i.e. pass 2 is the one whose values actually stick for the rest of
runtime. **No other reference to any P2 register exists anywhere in `body.bin`** (the same exhaustive
sweep that found these two functions), and [[base-loader]] already established `base.dat` never touches
the port-register block at all.

**Conclusion**: the main CPU sets `DRESD` (`P2_6`) as a plain GPIO output, drives it LOW once at boot/wake,
and **never touches it again** — there is no reset-pulse or write-protect-toggle sequence anywhere in the
traced firmware. **Polarity resolved (14th session)**: user re-checked the schematic and confirmed
`DRESD` lands on DSP (`IC901`, TMS320C6745) pin 146, labeled `\RESET` — the backslash is the schematic's
own active-low notation, so this is unambiguously the DSP's reset input (not, as first transcribed,
pin 145 — corrected here). **LOW means the DSP is held in reset**, and the same net's other leg
write-protects the neighbor `EN25QH32A` flash — both asserted together, permanently, by this firmware.

This confirms the first reading above and fits the existing hypothesis in this file's "FPGA
configuration" section cleanly: **the DSP has no persistent program flash of its own — the main CPU
holds it in reset from boot and never releases it in `body.bin`'s traced control flow**, consistent with
it being boot-loaded into RAM by the main CPU instead (via the `FUN_20025044` ring-buffer mechanism
documented in [[multi-cpu-images]]). Release, if it ever happens, must come from somewhere `body.bin`
doesn't reach — worth keeping in mind as a possible role for the still-unexplained `chunk4`/`chunk5-tail`
consumer once that's pinned down, or it genuinely never gets released in software and something else
(fixed hardware timing, a one-shot power-on RC delay wired directly to the DSP rather than through the
CPU) brings the DSP out of reset independently of this GPIO.

## Front panel connection — confirmed, active UART driver found (12th session, continued)

User provided the front-panel connector's signal table: `FRES`(`P1_0`), `LRXD`(`P6_0`), `PWRK`(`P1_7`,
"probably the power key"), `LTDX`(`P6_1`).

**`LRXD`/`LTDX` (Local RX/TX Data) — confirmed real, active UART link to the front-panel/display unit.**
`P6_0`/`P6_1`'s manual alt-functions are `RxD3`/`TxD3` — **SCIF channel 3** (base `0xE8008800`). Searched
`body.bin` for this register page and found a **tight cluster of 5 references** at `~0x2003757c`
(`SCSMR_3`/`SCSCR_3`/`SCFSR_3`/`SCFCR_3`/`SCFTDR_3` — the mode/control/status/FIFO-control/transmit-FIFO
registers, all together), unlike the DSP/FPGA candidates above which only ever showed up inside generic
multi-peripheral tables. Decompiled the driver function directly: real status-bit dispatch (tests bits
`0x02`/`0x08`/`0x10`/`0x20`/`0x40`/`0x80` of the SCIF status byte — framing/overrun/break/RX-full/TX-empty
style flags), **fixed `0x21`-byte (33-byte) packet framing** (`FUN_2017c710`/`FUN_20037214` both operate
on exactly 33 bytes at a time), a retry counter with a threshold of 5, and IRQ-disable/re-enable bracketing
around the shared state — a genuine, complete link-layer driver, not a stub. This is almost certainly the
protocol carrying keypad/encoder/VFO-knob input and display/UI update commands to and from the front-panel
MCU (`IC501`, the already-identified `R5F104LCAFB` RL78 — see [[multi-cpu-images]]'s "SX3765" resolution).
Also calls `FUN_200b8308(0xec)` — the **same** generic RTOS event-flag-set utility found gating the
`chunk4`/`chunk5` ring-buffer consumer in [[multi-cpu-images]], just with a different event number (`0xec`
here vs `0xa1` there) — confirms it's a shared OS primitive used by multiple independent subsystems, not a
sign the two paths are related.

**`FRES`/`PWRK` — no dedicated peripheral alt-function found for either `P1_0` or `P1_7`** in the manual's
pin table (both show only unrelated alternates: `RIIC0SCL`/`TCLKA`/`IRQ0`/etc. for `P1_0`,
`RIIC3SDA`/`RLIN30RX`/`IRQ7`/etc. for `P1_7`) — consistent with both being plain GPIO, as expected for a
front-panel reset line and a physical power-button read. Not traced further this session (no obvious
register-level lead the way the UART/audio signals had).

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

## Complete main-CPU port pinout, user's full schematic sweep (28th session)

User did a full pass over the general wiring/block-diagram sheets and supplied a near-complete `Pn_m` →
signal-name table for every CPU port (`P0`-`P9`), filling in some purposes directly and asking for the rest
to be cross-checked against the block diagrams. Read the **MAIN UNIT block diagram** (sheet 4,
"BLOCK DIAGRAM-2") and the **PA/Tuner/RF UNIT block diagram** (sheet 3, "BLOCK DIAGRAM-1") in full to fill
in the gaps. One new hardware fact up front, confirmed directly from the block diagram's own labels (the
main CPU's part number, `IC301`/`R7S721000VCFP`, was already on record in [[ic7300-hardware]] — this block
diagram just independently re-confirms it, not a new find):
- **Two separate `NJU7704F3` voltage detectors**, not one: `IC373` drives a net simply labeled `RESET`
  (plausibly a wider system reset, not yet traced to a specific pin), and `IC361` drives `VDET`/`PDV` into
  the main CPU's `P1_6` — the one already confirmed as the power-fail/brownout input in
  [[firmware-update]]'s restart-mechanism section. Added to [[ic7300-hardware]]'s BOM table.

### Port-by-port table

| Port pin | Signal | What it's confirmed to be (this session unless noted) |
|---|---|---|
| `P0_4` | `TCON` | Tuner control — one of 3 signals (with `EKEY`/`ESTA`) going to the external `[TUNER]` jack, confirmed on the PA/Tuner block diagram |
| `P0_5` | `USSPD` | Goes toward the USB subsystem cluster (see below) — likely a USB suspend indicator, not individually traced further |
| `P1_0` | `FRES` | Front-panel MCU reset (user's own label; matches the already-confirmed front-panel connector finding) |
| `P1_1` | `RTC_IRQ` | RTC (`IC381`, `RX-8803LC`) interrupt line — confirms the RTC connects via 3 dedicated CPU pins (`P1_1/2/3`), not just the I2C pair previously assumed |
| `P1_2` | `RTC_SCL` | RTC I2C clock — direct line into the main CPU per the block diagram, alongside `RTC_SDA` |
| `P1_3` | `RTC_SDA` | RTC I2C data |
| `P1_4` | `ECK` | EEPROM (`IC351`, `GT24C128B`) I2C clock — **matches [[base-loader]]'s already-confirmed `RIIC2` register-level finding exactly** (EEPROM on `P1_4`/`P1_5`) |
| `P1_5` | `EDT` | EEPROM I2C data |
| `P1_6` | `PDV` | Confirmed last session: `VOUT` of `IC361` (`NJU7704F3`) — power-fail/brownout detector input, see [[firmware-update]] |
| `P1_7` | `PWRK` | Front-panel power key (user's own label; matches earlier finding) |
| `P1_8` | `VDL` | PA voltage sense |
| `P1_9` | `IDL` | PA idle-current sense |
| `P1_10` | `THML` | PA heatsink temperature sense |
| `P1_11` | `FORL` | Forward power sense |
| `P1_12` | `REFL` | Reflected power sense |
| `P1_13` | `ALCL` | ALC level sense |
| `P1_14` | `TPWRL` | Transmit power level sense |
| `P1_15` | `SWRL` | SWR sense |
| `P2_0` | `MDAT` | Shift-register serial data — see "relay/filter band-switching" below |
| `P2_1` | `MCLK` | Shift-register serial clock — same bus as `MDAT` |
| `P2_2` | `MSTB1` | Shift-register latch strobe 1 — PA unit's own relay register (`IC751`, per the PA/Tuner block diagram) |
| `P2_3` | `MSTB2` | Shift-register latch strobe 2 — a second register on the same `MDAT`/`MCLK` bus (RF unit's own filter-bank register, `IC1101`-`IC1103` on the block diagram, uses the identical `MDAT`/`MCK`/`MSTB1`-labeled bus — `MSTB2` most likely selects this one instead) |
| `P2_4` | `DSTB` | A third shift-register/latch strobe on the same style of bus (exact register not pinned down individually) |
| `P2_5` | `PSTB` | A fourth shift-register/latch strobe (ditto) |
| `P2_6` | `DRESD` | DSP reset, confirmed prior sessions |
| `P2_7` | `DRESH` | Not yet traced individually — name suggests a second, related DSP reset/halt-style line, distinct from `DRESD` |
| `P2_8`-`P2_11` | `BCLK_`/`FRM_`/`DX_REC`/`DR_AF` | SSIF0 audio link to the DSP, confirmed prior sessions |
| `P3_0`-`P3_3` | `LCD_CLK`/`LCD_VS`/`LCD_HS`/`LCD_DE` | LCD panel timing signals. **Correction (same session, user corrected the block-diagram read)**: these go **directly to the front panel**, not through the FPGA — retracting the "FPGA sits between the main CPU and the LCD" claim below; the FPGA is not part of the LCD path. |
| `P3_4`/`P3_5` | `BCLK_`/`FRM_` | Same signal *names* as `P2_8`/`P2_9` — almost certainly SSIF1's own bit-clock/frame-sync inputs (SSIF1 shares SSIF0's timing in this pairing mode, per the already-confirmed audio-link finding — this is the CPU-side pin pair that carries that shared clock to SSIF1 specifically, completing the 8-signal SSIF0+SSIF1 link across `P2_8`-`P2_11` and `P3_4`-`P3_7`) |
| `P3_6`/`P3_7` | `DX_FMT`/`DR_RSV` | SSIF1 data, confirmed prior sessions |
| `P3_8`-`P3_15`, `P4_0`-`P4_7` | LCD data lines | Direct main-CPU-to-front-panel LCD data bus, per the correction above |
| `P4_8`-`P4_14` | `SD_CMD`(×2)/`SD_CLK`/`SD_D0`-`SD_D3`/`SD_WP` | **The SD-card interface, mapped for the first time** — a standard 4-bit SDIO/SD bus (command, clock, 4 data lines) plus a write-protect sense line. Directly relevant to [[firmware-update]]'s SD-card-based update mechanism. |
| `P5_8`-`P5_10` | `IMR0`/`IMR1`/`IMR2` | The diode-matrix scan's row-read pins — [[diode-matrix]] already fully mapped these three as the scan's row-bottom/middle/top inputs; this session adds their real schematic net names (`IMR0`/`1`/`2`, almost certainly "Input Matrix Row") and confirms they go to a 47 kΩ resistor network (a pull network for the scan), nothing about the row/column mapping itself changes. |
| `P6_0`/`P6_1` | `LRXD`/`LTXD` | Front-panel UART, confirmed prior sessions (SCIF3) |
| `P6_2`/`P6_3` | `EKEY`/`ESTA` | Tuner jack signals, confirmed on the PA/Tuner block diagram (`[TUNER]` connector `J20012`, pins `EKEY`/`ESTA`/`14V`) |
| `P6_4`-`P6_6`, `P6_8`, `P6_11`-`P6_13`, `P6_15` | `USSENI`/`USKI`/`SDPWS`/`VBUS`/`UCLKS`/`UDTXD`/`UDRXD`(`/UDBSY`)/`UPWS` | **The USB subsystem** — the block diagram shows a `USB HUB` + `USB BRIDGE` + `USB CODEC` cluster fed by exactly this group of `U`-prefixed signals (plus `VBUS`, literally USB bus power sense). Reads as: USB audio (via the CODEC) and a USB-to-serial bridge (very plausibly CI-V-over-USB) combined behind one hub, presented as the single external USB-B port. |
| `P6_7` | `LCD_ON` | LCD panel power/enable |
| `P6_9`/`P6_10`, `P7_11` | `CTXD`/`CRXD`(`/CBSY`) | **Confirmed at both the hardware and code level.** Hardware: `CTXD`/`CRXD`/`CBSY` (main CPU) → `IC701` → `Q711`(`L2SC4081`)/`Q712`(`L2SA1576`) → fans out to **both** the `[REMOTE]` jack directly **and** a USB-side path through `Q691`(`L2SC4081`)/`Q602`(`L2SA1576`) → `IC701`(`TC74VHC04FT`, hex inverter) → `IC691`(`TC7W66FU`, analog switch) → **`IC641` (`CP2102GMR`, a genuine USB-to-UART bridge IC)** — CI-V really is available both over the physical `[REMOTE]` jack and over USB. Code (2026-08-29, 29th session): `P6_9`/`P6_10` are SCIF0's `TxD0`/`RxD0` alt-functions (RZ/A1H manual, base `0xE8007000`); `scif0_civ_rx_isr`→`civ_frame_rx_statemachine` (`0x20010b6c`/`0x2001099c`) is a genuine CI-V `FE`/`FD` byte-framing receiver with real destination-address filtering — proves this is CI-V, not just a hypothesis from pins. **Correction**: `civ_command_dispatch_task` (`0x200b9c00`) turned out NOT to be this protocol's consumer — retracted and renamed `sdcard_file_rpc_dispatch_task` (a generic internal file-access RPC service, confirmed unrelated to CI-V). The real consumer of parsed CI-V frames (the documented frequency/mode/etc. command processor) is still unfound — see [[kernel-rtos]]'s "civ_command_dispatch_task retraction" section for the full trace and open items. |
| `P6_14` | `PWRS` | Not yet traced individually |
| `P7_1`-`P7_6`, `P7_8`/`P7_9` | `TSTB1`-`TSTB4`/`TCLK`/`TDAT`/`PHASEI`/`IMPI` | **Tuner interface** — matches the PA/Tuner block diagram's own `TDAT`/`TCLK`/`TCON`/`TSTB1`-`4`/`IMPI`/`PHASEI` cluster feeding the antenna tuner control logic, alongside `P0_4`'s `TCON` and `P6_2`/`P6_3`'s `EKEY`/`ESTA` |
| `P7_12` | `UDRXD`(`/UDBSY`) | Same signal name as `P6_13` — likely a second reference/alias to the same USB-bridge receive line on a different pin, or a transcription duplicate; not resolved further |
| `P8_0`-`P8_15` | `DSPCK`/`DSPR`/`DSPX`/`SCPCK`/`SCPSS`/`CSPR`/`SCPX`/`RTD`/`HSK0`/`HSK1`/`FRWT`/`FPDX`/`DCSX`/`DCSR`/`FPSX`/`FPSR` | All confirmed prior sessions (McASP1 DSP link, RSPI2/FPGA differential I/O, `HSK1` handshake) — `CSPR` here is almost certainly the same signal previously called `SCPR` (`P8_5`), a transcription variant, not a new pin |
| `P9_0`/`P9_1` | `TXS`/`RXS` | Not yet traced individually — plausibly TX/RX band-state strobes given the naming pattern, unconfirmed |
| `P9_2`-`P9_7` | `SFLCK`/`SFLSS`/`SFLD0`/`SFLD1`/`SFLD2`/`SFLD0`(likely `SFLD3`, repeated label) | **The main CPU's own boot/program flash** (`IC391`, `EN25Q64`) — the block diagram shows this exact `SFLSS`/`SFLCK`/`SFLD0`-`SFLD3` naming for `IC391`'s quad-SPI bus, confirming Port 9 carries the CPU's own XIP flash interface (separate from the boot-mode-3 SPI Multi I/O controller signals already documented in [[base-loader]] — worth reconciling which is the real XIP path vs. a secondary/parallel access route if this matters later) |

### Relay/filter band-switching signal chain (new)

The PA/Tuner/RF-unit block diagram shows **two separate shift-register chains sharing one `MDAT`/`MCLK`
bus**, each latched by its own strobe: `IC751` (`SN74AHC595PW`, single 8-bit) in the **PA unit**, driving
`L1S`-`L7S` (the PA's own low-pass filter bank relay selects), and `IC1101`-`IC1103` (`SN74AHC595PW`×3, 24
bits) in the **RF unit**, driving `B0S`-`B12`/`TX` (the 15-filter RX/TX bandpass bank already documented in
this file's "Precise filter-bank cutoffs" section). This is the concrete hardware mechanism behind the
band-segmented filter selection this file already described from the service-manual text alone — now with
real CPU pins (`MDAT`/`MCLK`/`MSTB1`/`MSTB2`, plus `DSTB`/`PSTB` for additional latches not individually
resolved) attached to it. Worth a `body.bin` reference search on `MSTB1`/`MSTB2`'s literal port bits if the
band-switching logic itself is ever traced.

### Notable non-finding

`P1_8`-`P1_15` (`VDL`/`IDL`/`THML`/`FORL`/`REFL`/`ALCL`/`TPWRL`/`SWRL`) read, from their names, like direct
analog PA-protection measurements — but the MAIN UNIT block diagram shows the *actual* analog readings
(`AGCV`, `REFL`, etc.) going through dedicated A/D converter chips elsewhere on the board, not raw into
`Port 1`. **These are more likely fast digital threshold/fault flags** (e.g. "SWR over limit" as a single
bit) supplementing the slower, precise ADC path, rather than raw analog values read directly by a GPIO
port — a plausible dual-path protection design, not confirmed by any firmware reference yet.
