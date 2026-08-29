# IC-7300 hardware (from service manual parts lists, all boards)

Source: user-supplied service manual IC parts list screenshots
(2026-08-27) — **complete survey per the user**, covering all five
boards: Main Unit, Display Unit, RF Unit, PA Unit, Tuner Unit.
Consolidates chip references that were previously scattered across
[[hardware-debug-access]], [[multi-cpu-images]], [[memory-map]], and
[[diode-matrix]] into one reference, plus new chips from these
screenshots. See [[ic9700-hardware]] for the equivalent IC-9700 list —
several roles map directly across the two radios (RTC, USB hub, USB
audio codec parts are literally identical components).

## Main unit

## Programmable/firmware-relevant chips

| Ref | Part | Role |
|---|---|---|
| IC301 | R7S721000VCFP | **Main CPU** — Renesas RZ/A1H, ARM Cortex-A9. This is the chip all of the Ghidra work in this repo targets (see [[base-loader]], [[memory-map]]). Same family as the IC-9700's R7S721001, see [[ic9700-hardware]] |
| IC351 | GT24C128B-2ZLI-TR | I2C EEPROM — config/calibration storage, holds the diode-matrix scan result and other settings (see [[eeprom-catalogue]], [[diode-matrix]]). **Correction (28th session, [[ic7300-signal-chain]])**: the main-CPU block diagram shows this on its own dedicated I2C pair (`ECK`/`EDT` → `P1_4`/`P1_5`), genuinely separate from the RTC's own 3 pins below — retracts the earlier "RTC integrated on the same package/position" guess |
| IC381 | RX-8803LC UB | Real-time clock — **same exact part as the IC-9700's IC251**. Confirmed (28th session) on its own dedicated CPU pins: `RTC_IRQ`/`RTC_SCL`/`RTC_SDA` → `P1_1`/`P1_2`/`P1_3` |
| IC361 | NJU7704F3 | Supply-voltage detector — drives `VDET`/`PDV` into the main CPU's `P1_6`, the power-fail/brownout-reset input confirmed in [[firmware-update]] |
| IC373 | NJU7704F3 | A second, separate voltage-detector instance — drives a net labeled `RESET` on the block diagram (not yet traced to a specific CPU pin) |
| IC391 | EN25Q64-104HIP | SPI NOR flash, 64Mbit — main CPU's boot flash (holds the XIP firmware image everything in this repo has been reverse-engineered from). **New find**: distinct chip from IC902 below — confirms two genuinely separate SPI flashes on this board, one per major CPU, not a shared/single flash |
| IC901 | TMS320C6745DPTPA3 | TI TMS320C674x DSP ("IF-DSP") — floating-point capable, more powerful core than the IC-9700's fixed-point TMS320C5517. Already identified via schematic in [[hardware-debug-access]]/[[multi-cpu-images]], now have the exact full part number |
| IC902 | EN25QH32A-104HIP | SPI NOR flash, 32Mbit, physically adjacent to IC901. **Likely the FPGA's (IC1351) own configuration flash, not the DSP's** — the service manual states the FPGA loads its config from "external EEPROM" at power-on (standard Altera passive-serial behavior matching IC902's traced `DCLK`/`DATA0` routing), which reframes the earlier "DSP's own boot flash" assumption in [[multi-cpu-images]] that was based on physical proximity alone. See [[ic7300-signal-chain]] for the full reasoning — leaves open where the DSP's own program actually comes from |
| IC1351 | EP4CE55F23I7N | Altera/Intel **Cyclone IV FPGA** — already identified in [[hardware-debug-access]]; IC-9700's equivalent is a newer Cyclone V (IC7601, see [[ic9700-hardware]]) |
| IC1401 | 23LC1024T-I/SN | SPI serial SRAM (1Mbit), sits next to the FPGA — already noted by the user early in this project as "next to the FPGA"; almost certainly a scratch buffer for the FPGA, not itself holding firmware (see [[multi-cpu-images]]) |

## SDR RF front-end

| Ref | Part | Role |
|---|---|---|
| IC1261 / IC1301 | "RF ADC" ×2 (part number not legible in this scan) | Two separate RF ADCs — consistent with an SDR direct-sampling architecture with two independent receive paths (main receiver + sub-receiver/panadapter scope) |
| IC1331 | ISL5857IAZ | **Transmit D/A converter with digital up-conversion** ("TxDAC+") — confirmed by the service manual's own block diagram (see [[ic7300-signal-chain]]): converts the FPGA's digitally up-converted transmit IF into analog RF. Not a receive-side down-converter — the receive-side digital down-conversion/demod happens entirely inside the FPGA itself (mixer + image-rejection mixer + on-chip DDS), per the manual's FPGA block diagram. *(Corrects an earlier guess in this file that assumed a receive-side DDC role, extrapolated from the chip's general datasheet capability rather than checked against how Icom actually uses it here.)* |
| IC1212 / IC1315 | SN65LVDS1DBVR ×2 | LVDS transceivers — likely a high-speed serial link between the ADC(s)/FPGA and another board (e.g. display, or the sub-board), not yet traced |

## External interfaces / audio

| Ref | Part | Role |
|---|---|---|
| IC621 | TUSB2046BIVFRG4 | USB 2.0 hub controller — **same exact part as the IC-9700's IC1601** |
| IC661 | PCM2901E/2K | USB audio codec (TI/Burr-Brown) — **same exact part as the IC-9700's IC1532** |
| IC971 / IC991 | PCM1754DBQR ×2 | Stereo audio DACs — likely separate main/sub-receiver audio paths |
| IC1001 | PCM1802DBR | Stereo audio ADC (mic input) — **same exact part as the IC-9700's IC2101** |

## Power/supervisory (not individually notable, listed for completeness)

Numerous XC6209/XC6222 (Torex) regulators, TPS62110 buck regulators,
NJM7805/NJM2904 (op-amps/LDOs), NJU7704F3 voltage detectors — same
general vendor mix as the IC-9700's power tree, nothing unusual.

## Display unit (separate board)

Source: service manual "[DISPLAY UNIT]" IC list screenshot (2026-08-27).

| Ref | Part | Role |
|---|---|---|
| IC101 | TPS61161ADRVR | LED backlight boost driver |
| IC152 | UC6528XBNQ4GRC | Likely display/touch controller — function not confirmed |
| IC501 | R5F104LCAFB, marked **`SX-3765C-1`** | **Display unit's own MCU** (Renesas RL78 family) — this **confirms** the long-running "SX3765" identity question from [[multi-cpu-images]]: every `"SX3765 Vx.xx-yyy"` string found in the main firmware is a compatibility/version check against *this* chip's part number, not an embedded second-processor firmware image. Matches the partially-legible schematic label `(UX-3765C)` guessed at in [[hardware-debug-access]] — that was a misread of `S` as `U` |

## RF unit (separate board)

Source: service manual "[RF UNIT]" IC list screenshot (2026-08-27). No
additional programmable/firmware-relevant chips here — this board is
almost entirely band-filter/relay switching driven from the main board.

| Ref | Part | Role |
|---|---|---|
| IC1031 | BGA2866115 | RF gain-block MMIC amplifier |
| IC1061 | XC6209F502MR-G | Voltage regulator |
| IC1301 / IC1302 / IC1303 | SN74AHC595PWR ×3 | 8-bit serial-in/parallel-out shift registers — almost certainly driving the band-pass-filter/antenna relay bank from a handful of main-board control lines, same idiom as the diode-matrix-adjacent GPIO control elsewhere on this radio (see [[diode-matrix]]) |
| IC1501 | NJM13403V-TE1 | Dual comparator |
| IC1502 | NJM2904CRB1-TE1 | Dual op-amp |

## PA unit (separate board)

Source: service manual "[PA UNIT]" IC list screenshot (2026-08-27). Also
no firmware-relevant chips — power-amp current sensing/protection and
relay control.

| Ref | Part | Role |
|---|---|---|
| IC211 | INA199A2DCKR | Current-sense amplifier — PA current monitoring/protection |
| IC221 | BA09FP-E2 | 9V regulator |
| IC751 | SN74AHC595PWR | Shift register — same relay/control idiom as the RF unit above |
| IC981 | NJM2904CRB1-TE1 | Dual op-amp |

## Tuner unit (separate board)

Source: service manual "[TUNER UNIT]" IC list screenshot (2026-08-27).
No firmware-relevant chips — comparators (SWR/protection sensing) and
motor drivers for the internal antenna tuner's motorized L-network.

| Ref | Part | Role |
|---|---|---|
| IC1701 | BA2903SFVM-TR | Quad comparator — likely SWR/protection sensing |
| IC1901 | NJM2904CRB1-TE1 | Dual op-amp |
| IC2811 / IC2821 / IC2831 / IC2841 | BU2092FV-E2 ×4 | Motor driver ICs (Rohm) — drive the tuner's motorized/relay-switched inductor and capacitor banks |

## Notably absent from this list

No Ethernet PHY chip appears here (the IC-9700 has one, IC1901
LAN8710AI) — consistent with the IC-7300 not having a wired-LAN remote
control feature the way the IC-9700 does.

## JTAG note

Per [[hardware-debug-access]], IC301 is confirmed to have its
TCK/TMS/TDI/TRST/TDO cluster routed to a physically-populated connector
(`J491`, JST `10FLT-SM2-TB`). Once the JTAG hardware (ordered, ETA per
[[icom-ic7300-re-project]] memory) is in hand and working on the 7300,
user intends to check whether the IC-9700's IC101 has an equivalent
exposed debug connector — not yet checked, no schematic access to the
9700 board confirmed either way.
