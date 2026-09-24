# IC-7300 signal chain (from service manual "Circuit Description", §3-1–3-3)

Source: user-supplied service manual pages 3-1 through 3-6 (2026-08-27),
OCR'd/read directly via PDF page extraction from
`/data/misc/icom/7300/doc/IC-7300_Servicio.pdf` (the actual filename —
Spanish for "service", not a translated-content indicator; body text is
English). Complements [[ic7300-hardware]] (which chip is which) with
what each one actually *does* in the signal path — useful context for
interpreting DSP/FPGA-adjacent code and control registers if that work
ever resumes.

See [notes/ic7300-signal-chain-history.md](ic7300-signal-chain-history.md) for the full session-by-session narrative and evidence trail.

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


**Superseded, 2026-08-29** — this "refinement" was itself wrong, retracted in [[multi-cpu-images]]; see [notes/ic7300-signal-chain-history.md](ic7300-signal-chain-history.md) for the full reasoning.

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
| `DSPCK` | P8_0 | `SSL00` | (was: RSPI ch.0) — **found: SCIF5, see history** | 162, `ACLKX1`/`EPWM0A`/`GP3[15]` (**McASP1** bit clock) | `Y1`, `DIFFIO_L29n` | ✅ **found, next session — SCIF5's port-mux (see history)** |
| `DSPR` | P8_1 | `MOSI0` | (was: RSPI ch.0) — **found: SCIF5, see history** | 175, `AXR1[2]`/`GP4[2]` (McASP1 serializer 2) | `AA1`, `DIFFIO_L31n` | ✅ **found, next session — SCIF5's port-mux (see history)** |
| `DSPX` | P8_2 | `MISO0` | (was: RSPI ch.0) — **found: SCIF5, see history** | 176, `AXR1[1]`/`GP4[1]` (McASP1 serializer 1) | — | ✅ **found, next session — SCIF5's port-mux (see history)** |
| `DCSX` | P8_12 | — | (was: SPI Multi I/O ch.1) | 163, `AFSX1`/`EPWMSYNCI`/`EPWMSYNC0`/`GP4[10]` (McASP1 frame sync X) | — | ❌ no reference found |
| `DCSR` | P8_13 | — | (was: SPI Multi I/O ch.1) | 166, `AFSR1`/`GP4[13]` (McASP1 frame sync R) | — | ❌ no reference found |
| `HSK0` | P8_8 | — | (was: SPI Multi I/O ch.1) | 100, `EMB_A[3]`/`GP7[5]` (DSP **external memory bus address bit 3**) | — | ❌ no reference found |
| `HSK1` | P8_9 | — | (was: SPI Multi I/O ch.1) | 98, `EMB_A[4]`/`GP7[6]` (EMIF address bit 4) | — | ✅ **found, 27th session — see history** |
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
   trace~~ — **done: the consumer (`FUN_200b0f68`) does drive this RSPI2 link, among others, but NOT for the
   `chunk4`/`chunk5` (`0xb0`/`0xe2`-tagged) traffic specifically (see history) — that was a real, useful
   negative result, and [[multi-cpu-images]] has since found the actual consumer directly:
   `chunk_transport_send_data` → `dsp_page_transfer_verify` → `SCIF5` (the DSP link), not RSPI2/SSIF/
   front-panel-UART. See the correction added in history.**

## RSPI2 confirmed as a real, actively-used SPI link to the FPGA (22nd session)

**Bottom line**: RSPI2 is a genuine, IRQ-driven, ongoing CPU→FPGA SPI link — resolves this file's
long-standing "real traffic vs. init-only" question with a clear yes. It shares its dispatch/queue plumbing
with the front-panel UART and SSIF/audio-DMA paths (a real, useful map of this firmware's async-job
architecture), but is confirmed *not* the transport for the `chunk4`/`chunk5` mystery specifically.

## `DRESD` (P2_6) resolved — boot-time-only, held at a fixed level (13th session)

**Conclusion**: the main CPU sets `DRESD` (`P2_6`) as a plain GPIO output, drives it LOW once at boot/wake,
and **never touches it again** — there is no reset-pulse or write-protect-toggle sequence anywhere in the
traced firmware. **Polarity resolved (14th session)**: user re-checked the schematic and confirmed
`DRESD` lands on DSP (`IC901`, TMS320C6745) pin 146, labeled `\RESET` — the backslash is the schematic's
own active-low notation, so this is unambiguously the DSP's reset input (not, as first transcribed,
pin 145 — corrected here). **LOW means the DSP is held in reset**, and the same net's other leg
write-protects the neighbor `EN25QH32A` flash — both asserted together, permanently, by this firmware.

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

**`PWRK` handler located, 2026-09-20.** Firmware actually uses `P1_7`'s alt-function IRQ7 (the option
listed above turns out to be exactly what's used, not a red herring) — found by tracing
`register_event_handler(0x27, ...)` (`0x27` = 39 decimal = the real RZ/A1H GIC ID for IRQ7, confirmed
against `scratch/r01an5093ej0170-rza1-swpkg`'s `INTC_ID_IRQ7=39`), cross-checked independently by a
direct search for a `PPR1` (`g_ppr_register_base+4`) bit `0x80` test. Three functions, all renamed in
Ghidra:
- **`power_state_pwrk_wait_and_bringup`** (`0x20029ca4`) — the actual wait-for-power-key-press loop
  (`0x20029918`): busy-waits on `PPR1` bit `0x80` (`P1_7`/`PWRK`) reading `0` (active-low, plain
  pull-up + switch-to-ground). On press, sets a status byte (`DAT_2002a0dc+6` = 1), brings up the
  front-panel/SCIF1 drivers, arms the release-detect interrupt, then runs a CI-V/SCIF1 servicing loop
  gated on that same status byte staying nonzero.
- **`pwrk_irq7_config_init`** (`0x20029800`) — configures `P1_7`'s port-mux to IRQ7, programs the INTC
  `ICR1` IRQ7 sense bits, and registers `pwrk_irq7_isr` as GIC ID 39's handler.
- **`pwrk_irq7_isr`** (`0x20029774`) — the actual IRQ7 ISR: acks the raw `INTC IRQRR` pending flag
  (bit 7 = `IRQ7F`), checks `PPR1` bit `0x40` (`P1_6`/`PDV`, the brownout detector) as a safety
  interlock, then debounces by polling `PPR1` bit `0x80` again with a timeout, updating the same
  `DAT_2002a0dc+6` status byte.

**Important scoping caveat**: this whole mechanism sits on `power_state_pwrk_wait_and_bringup`, one of
two branches `power_state_dispatch`'s (renamed from `FUN_2002b29c`) power-state dispatcher chooses
between — the other is `cold_boot_mode_dispatch` → `cold_boot_hw_init`, the RIIC2/EEPROM path where
`qemu-machine`'s emulation currently traps (see `qemu-machine/README.md`). A genuine cold power-on takes
the `cold_boot_hw_init` branch and never reaches this PWRK-wait loop at all — it's specifically the
warm/standby-wake path, taken when the CPU is already running and software must poll for the button
press rather than have hardware bring-up proceed unconditionally. So the emulator isn't "close" to this
point in the sense of sequential progress along one boot path; it's on the *other* branch entirely, and
getting the emulator here would need forcing the dispatcher's `bVar8`/`*DAT_2002b4d4` condition down the
warm-wake side rather than just running further.

**The cold-boot-vs-wait-for-PWRK choice is EEPROM-backed, confirmed 2026-09-20 — matches the user's own
real-hardware observation exactly** (the radio remembers whether it was powered on when it lost power,
and auto-powers-on in that case; otherwise it waits for the power key). Traced the full round trip:

- **EEPROM offset `0x3e00`, 1 byte, bit 7 = "was the radio on".**
- **Read** at every boot by `pwrk_power_state_read_from_eeprom` (renamed from `FUN_2002b274`,
  `0x2002b274`) — called from `power_state_dispatch` immediately after `riic2_driver_init()` — via the
  already-known RIIC2 EEPROM read wrappers (`FUN_2001e510`/`FUN_2001dd58`), into `DAT_2002b504`. Bit 7
  of that byte (`*DAT_2002b504 >> 7`) directly becomes `power_state_dispatch`'s own decision variable
  (`bVar8`) in the two branches that aren't already-gated by the "already brought up once" RAM flag —
  `bVar8 == 1` sends the dispatcher into `cold_boot_mode_dispatch` (auto power-on); `bVar8 == 0` sends it
  into `power_state_pwrk_wait_and_bringup` (wait for `PWRK`).
- **Written** via `pwrk_power_state_write_to_eeprom_if_changed` (renamed from `FUN_20029c60`,
  `0x20029c60`) — compares the live flag byte (RAM address `0x20390303`, reached via the
  `DAT_2002a114` literal-pool pointer) against a shadow copy and only actually calls the EEPROM write
  wrapper (`FUN_2001e484`/`FUN_2001dcc4`, same offset `0x3e00`) if it changed — a write-through cache
  that avoids wearing the EEPROM on every check.
- The live flag itself is set to `1` (`*DAT_2002a114 |= 0x80`) by `cold_boot_hw_init` right after
  entering the auto-power-on path, and cleared to `0` (`*DAT_2002a114 &= 0x7f`) by
  `power_state_pwrk_wait_and_bringup` right as it enters the wait-for-`PWRK` path — both immediately
  followed by a call to the write-if-changed function above, so the EEPROM byte tracks the radio's
  actual power state continuously, not just at a clean shutdown.

All renamed and plate-commented in the live Ghidra project (saved).

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
| `P0_4` | `TCON` | Tuner control — one of 3 signals (with `EKEY`/`ESTA`) going to the external `[TUNER]` jack, confirmed on the PA/Tuner block diagram. **Confirmed at the code level, 2026-08-30**: `tuner_jack_signal_precheck` (`0x20066154`) reads this pin directly via the raw port-pin-read register (`PPR0`, bit 4) as one of its gating conditions, alongside `EKEY` (see below) — see `notes/kernel-rtos.md`'s CI-V/tuner section. |
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
| `P6_2`/`P6_3` | `EKEY`/`ESTA` | Tuner jack signals, confirmed on the PA/Tuner block diagram (`[TUNER]` connector `J20012`, pins `EKEY`/`ESTA`/`14V`). **`EKEY` (`P6_2`) confirmed at the code level, 2026-08-30**: `tuner_jack_poll_and_autotrigger` (`0x2006672c`, called every idle-loop tick) and `tuner_jack_signal_precheck` (`0x20066154`) both read this pin via the raw port-pin-read register (`PPR6`, bit 2) — `tuner_jack_poll_and_autotrigger` uses it as a gate before triggering the same internal tuner-engage primitive (`tuner_engage_gpio_toggle`) that the documented CI-V `1C 01` and undocumented `0x2A` commands also reach, making this the third independent trigger path into that primitive and the first hardware-pin-level tie of that code cluster to a real, schematic-named signal. `ESTA` (`P6_3`) not yet found read anywhere. See `notes/kernel-rtos.md`'s CI-V/tuner section for the full chain. |
| `P6_4`-`P6_6`, `P6_8`, `P6_11`-`P6_13`, `P6_15` | `USSENI`/`USKI`/`SDPWS`/`VBUS`/`UCLKS`/`UDTXD`/`UDRXD`(`/UDBSY`)/`UPWS` | **The USB subsystem** — the block diagram shows a `USB HUB` + `USB BRIDGE` + `USB CODEC` cluster fed by exactly this group of `U`-prefixed signals (plus `VBUS`, literally USB bus power sense). Reads as: USB audio (via the CODEC) and a USB-to-serial bridge (very plausibly CI-V-over-USB) combined behind one hub, presented as the single external USB-B port. **Correction, next session**: `P6_12`/`P6_13` specifically are confirmed by code (and now by the user's own PCB layout check) as **`SCIF1`** (the service/calibration link, see [[kernel-rtos]]), not this USB group's own signal — `P6_13` (`UDRXD`/`UDBSY`) is tied on the PCB to `P7_12` (same net) and both go to `IC641` `CP2102` pin 26 (`TXD`, bridge→CPU = `SCIF1` RX); `P6_12` (`UDTXD`) goes to `CP2102` pin 25 (`RXD`, CPU→bridge = `SCIF1` TX, enabled only at runtime — see [[kernel-rtos]]'s "SCIF1 TX/RX pin turnaround" section). This also independently confirms `SCIF1`'s calibration link is reachable over USB via this second `CP2102` channel, not just the REMOTE jack. The rest of this group (`P6_4-6/8/11/15`) is unaffected. |
| `P6_7` | `LCD_ON` | LCD panel power/enable |
| `P6_9`/`P6_10`, `P7_11` | `CTXD`/`CRXD`(`/CBSY`) | **Confirmed at both the hardware and code level.** Hardware: `CTXD`/`CRXD`/`CBSY` (main CPU) → `IC701` → `Q711`(`L2SC4081`)/`Q712`(`L2SA1576`) → fans out to **both** the `[REMOTE]` jack directly **and** a USB-side path through `Q691`(`L2SC4081`)/`Q602`(`L2SA1576`) → `IC701`(`TC74VHC04FT`, hex inverter) → `IC691`(`TC7W66FU`, analog switch) → **`IC641` (`CP2102GMR`, a genuine USB-to-UART bridge IC)** — CI-V really is available both over the physical `[REMOTE]` jack and over USB. Code (2026-08-29, 29th session): `P6_9`/`P6_10` are SCIF0's `TxD0`/`RxD0` alt-functions (RZ/A1H manual, base `0xE8007000`); `scif0_civ_rx_isr`→`civ_frame_rx_statemachine` (`0x20010b6c`/`0x2001099c`) is a genuine CI-V `FE`/`FD` byte-framing receiver with real destination-address filtering — proves this is CI-V, not just a hypothesis from pins. **Correction**: `civ_command_dispatch_task` (`0x200b9c00`) turned out NOT to be this protocol's consumer — retracted and renamed `sdcard_file_rpc_dispatch_task` (a generic internal file-access RPC service, confirmed unrelated to CI-V). The real consumer of parsed CI-V frames (the documented frequency/mode/etc. command processor) is still unfound — see [[kernel-rtos]]'s "civ_command_dispatch_task retraction" section for the full trace and open items. |
| `P6_14` | `PWRS` | Not yet traced individually |
| `P7_1`-`P7_6`, `P7_8`/`P7_9` | `TSTB1`-`TSTB4`/`TCLK`/`TDAT`/`PHASEI`/`IMPI` | **Tuner interface** — matches the PA/Tuner block diagram's own `TDAT`/`TCLK`/`TCON`/`TSTB1`-`4`/`IMPI`/`PHASEI` cluster feeding the antenna tuner control logic, alongside `P0_4`'s `TCON` and `P6_2`/`P6_3`'s `EKEY`/`ESTA` |
| `P7_12` | `UDRXD`(`/UDBSY`) | **Resolved, next session**: ~~same signal name as `P6_13` — likely a second reference/alias to the same USB-bridge receive line on a different pin, or a transcription duplicate; not resolved further~~ — it isn't a duplicate, and it isn't a coincidence either. `scif1_svc_driver_init` activates `P6_13` and `P7_12` **together** (different function codes — 4 on `P6_13`, 7 on `P7_12` — but both with `PMCn` genuinely enabled). **User-confirmed via PCB layout**: `P6_13` and `P7_12` are the literal same net, tied together on the board — both are `SCIF1`'s single RX line (`UDRXD`/`UDBSY`, into `IC641` `CP2102` pin 26/`TXD`), redundantly wired to two CPU pins, not a TX+RX pair. See [[kernel-rtos]]'s "SCIF1 and SCIF5 physical pins resolved" and "SCIF1 TX/RX pin turnaround" sections. |
| `P8_0`-`P8_15` | `DSPCK`/`DSPR`/`DSPX`/`SCPCK`/`SCPSS`/`CSPR`/`SCPX`/`RTD`/`HSK0`/`HSK1`/`FRWT`/`FPDX`/`DCSX`/`DCSR`/`FPSX`/`FPSR` | All confirmed prior sessions (McASP1 DSP link, RSPI2/FPGA differential I/O, `HSK1` handshake) — `CSPR` here is almost certainly the same signal previously called `SCPR` (`P8_5`), a transcription variant, not a new pin |
| `P9_0`/`P9_1` | `TXS`/`RXS` | Not yet traced individually — plausibly TX/RX band-state strobes given the naming pattern, unconfirmed |
| `P9_2`-`P9_7` | `SFLCK`/`SFLSS`/`SFLD0`/`SFLD1`/`SFLD2`/`SFLD0`(likely `SFLD3`, repeated label) | **The main CPU's own boot/program flash** (`IC391`, `EN25Q64`) — the block diagram shows this exact `SFLSS`/`SFLCK`/`SFLD0`-`SFLD3` naming for `IC391`'s quad-SPI bus, confirming Port 9 carries the CPU's own XIP flash interface (separate from the boot-mode-3 SPI Multi I/O controller signals already documented in [[base-loader]] — worth reconciling which is the real XIP path vs. a secondary/parallel access route if this matters later) |

### Relay/filter band-switching signal chain (new)

The PA/Tuner/RF-unit block diagram shows **two separate shift-register chains sharing one `MDAT`/`MCLK`
bus**, each latched by its own strobe: `IC751` (`SN74AHC595PW`, single 8-bit) in the **PA unit**, driving
`L1S`-`L7S` (the PA's own low-pass filter bank relay selects), and `IC1101`-`IC1103` (`SN74AHC595PW`×3, 24
bits) in the **RF unit**, driving `B0S`-`B12`/`TX` (the 15-filter RX/TX bandpass bank already documented in
this file's "Precise filter-bank cutoffs" section). **Flagged, not resolved**: [[ic7300-hardware]]'s RF Unit
parts-list table (sourced from a separate service-manual screenshot) gives these same 3 RF-unit shift
registers as `IC1301`/`IC1302`/`IC1303` instead — a straight designator conflict, not obviously explained by
either source retracting the other. That table also reuses `IC1301` a second time for one of the two RF
ADCs, an internal collision that suggests its own OCR read may be the less reliable one here, but this isn't
confirmed either way — worth checking directly against the schematic sheet (sheet 3 or 13) next time this
area is touched.

This is the concrete hardware mechanism behind the
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
