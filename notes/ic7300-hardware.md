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
| IC351 | GT24C128B-2ZLI-TR | I2C EEPROM — config/calibration storage, holds the diode-matrix scan result and other settings (see [[eeprom-catalogue]], [[diode-matrix]]). **Correction (28th session, [[ic7300-signal-chain]])**: the main-CPU block diagram shows this on its own dedicated I2C pair (`ECK`/`EDT` → `P1_4`/`P1_5`), genuinely separate from the RTC's own 3 pins below — retracts the earlier "RTC integrated on the same package/position" guess. **Physical probing note (2026-09-10/11, live logic-analyzer attempt for `qemu-machine/`'s RIIC2 bus-timing check, see that project's README)**: this SOIC-8 package's own pins are too small/close-pitch for clip-on probes; its `SCL`/`SDA` pull-up resistors, checked as a fallback tap point, turned out too small as well, and an attempt at the pull-ups caused real PCB damage (a lifted pad/broken trace) needing a wire-wrap-wire jumper repair. **Any future live probe of this specific net needs pre-soldered fine wire (with proper mechanical strain relief) or a genuinely different tap point — clip probes and bare pull-up pads are not viable on this board without support gear (microscope, fine-tip iron) this session didn't have on hand.** |
| IC381 | RX-8803LC UB | Real-time clock — **same exact part as the IC-9700's IC251**. Confirmed (28th session) on its own dedicated CPU pins: `RTC_IRQ`/`RTC_SCL`/`RTC_SDA` → `P1_1`/`P1_2`/`P1_3` |
| IC361 | NJU7704F3 | Supply-voltage detector — drives `VDET`/`PDV` into the main CPU's `P1_6`, the power-fail/brownout-reset input confirmed in [[firmware-update]] |
| IC373 | NJU7704F3 | A second, separate voltage-detector instance — drives a net labeled `RESET` on the block diagram (not yet traced to a specific CPU pin) |
| IC391 | EN25Q64-104HIP | SPI NOR flash, 64Mbit — main CPU's boot flash (holds the XIP firmware image everything in this repo has been reverse-engineered from). **New find**: distinct chip from IC902 below — confirms two genuinely separate SPI flashes on this board, one per major CPU, not a shared/single flash |
| IC901 | TMS320C6745DPTPA3 | TI TMS320C674x DSP ("IF-DSP") — floating-point capable, more powerful core than the IC-9700's fixed-point TMS320C5517. Already identified via schematic in [[hardware-debug-access]]/[[multi-cpu-images]], now have the exact full part number |
| IC902 | EN25QH32A-104HIP | SPI NOR flash, 32Mbit, physically adjacent to IC901. **Corrected again, now settled at the pin level (2026-08-29, [[multi-cpu-images]]/[[ic7300-signal-chain]]): this is `IC901`'s (the DSP's) own dedicated SPI0 boot flash after all.** Its `CS`/`DO`/`DI`/`CLK` pins trace directly to DSP pins 9/17/18/11, which double as `BOOT[4:0]` mode-select straps — a direct point-to-point connection, stronger evidence than the net-label routing the "FPGA's own config flash" guess (previously written here) was based on. That FPGA-config reading wasn't wrong, just one hop removed: the FPGA is most likely configured *by* the DSP once it self-boots from IC902, not directly by the flash chip. The DSP self-boots autonomously from IC902 and reprograms it itself during a firmware update, driven by commands over `SCIF5` — the main CPU has no other bus access to this flash |
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
| IC152 | UC6528XBNQ4GRC | Likely display/touch controller — function not confirmed. **2026-09-09**: checked for a public datasheet, none found via easy sources — this part's protocol will need reverse engineering from scratch (firmware disassembly and/or live hardware tracing) if/when it becomes an actual blocker, not something to look up |
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

Source: service manual "[TUNER UNIT]" IC list screenshot (2026-08-27), refined 2026-08-30 with the
user's own parts-list-plus-relay-net reading of the tuner schematic sheet. **Correction**: the
BU2092FV-E2 ×4 were originally guessed as "motor driver ICs" for a motorized roller inductor — they're
actually serial-in/parallel-out latched relay drivers (shared `TDAT`/`TCLK` data/clock, `TOE` output-
enable, one `TSTBn` latch-strobe per chip — matches the schematic-confirmed `TSTB1`-`TSTB4`/`TCLK`/
`TDAT` cluster on main-CPU pins `P7_1`-`P7_6`, see `notes/ic7300-signal-chain.md`). The IC-7300's tuner
is a **relay-switched stepped L-network**, not a continuously-variable motor-driven roller inductor.

| Ref | Part | Role |
|---|---|---|
| IC1701 | BA2903SFVM-TR | Quad comparator — likely SWR/protection sensing |
| IC1901 | NJM2904CRB1-TE1 | Dual op-amp |
| IC2811 / IC2821 / IC2831 / IC2841 | BU2092FV-E2 ×4 | Serial-in/parallel-out relay drivers (Rohm) — share `TDAT`/`TCLK`/`TOE`, individually latched via their own `TSTB1`-`TSTB4` |

### Tuner relay network (living reference)

Each `BU2092FV-E2` drives a fixed group of the network's relays, one active-low output per relay
(`[signal]RS` naming per the schematic). `TSTBn` latches that chip's shifted-in byte to its outputs.

| Chip | Latch | Outputs → relay |
|---|---|---|
| IC2811 | `TSTB1` | `NL0RS`→RL2211+RL2221 (bypass), `NL1RS`→RL2011, `NL2RS`→RL2021, `NL3RS`→RL2031, `NL4RS`→RL2041, `NL5RS`→RL2051 |
| IC2821 | `TSTB2` | `NL6RS`→RL2061, `NL7RS`→RL2071, `NL8RS`→RL2081, `NL9RS`→RL2091, `NC12RS`→RL2121 |
| IC2831 | `TSTB3` | `NC3RS`→RL2131, `NC4RS`→RL2141, `NC5RS`→RL2151, `NC6RS`→RL2161, `NC7RS`→RL2171, `NC8RS`→RL2181 |
| IC2841 | `TSTB4` | `NC9RS`→RL2191, `NCINRS`→RL2251, `NCOUTRS`→RL2261, `NCREDRS`→RL2281, `NATT1RS`→RL1011, `NATT2RS`→RL1021 |

Working read of the network's function (user's hypothesis, not yet cross-checked against firmware
relay-pattern data): `RL20xx` relays (the `NL`-driven group) switch **inductance** steps into the
antenna line; `RL21xx` relays (`NC1`-`NC9`, despite the "C" suggesting capacitance, numbered
differently from the `NL` group) switch **capacitance** steps; `RL2211`/`RL2221` (both driven by
`NL0RS`) bypass the added L/C network entirely (straight-through); `RL1011`/`RL1021` (`NATT1`/`NATT2`)
bypass TX-related measuring components, plausibly disabled/don't-care during receive. This gives 9
inductance steps + 9 capacitance steps + a bypass + 2 measurement-bypass relays = the tuner's real
switched-element inventory — not yet matched against a firmware-side relay-pattern table (see
`notes/kernel-rtos.md`'s CI-V section for the `tuner_engage_gpio_toggle`/`tuner_start_tuning_sequence`
code that presumably drives this network; the exact code path that serializes a relay pattern out over
`TDAT`/`TCLK` hasn't been located yet — the raw `P7` port data register is touched only by the generic
one-time boot GPIO init, so the runtime path must go through a different (not yet found) indirection).

## Notably absent from this list

No Ethernet PHY chip appears here (the IC-9700 has one, IC1901
LAN8710AI) — consistent with the IC-7300 not having a wired-LAN remote
control feature the way the IC-9700 does.

## JTAG note

Per [[hardware-debug-access]], IC301 is confirmed to have its
TCK/TMS/TDI/TRST/TDO cluster routed to a physically-populated connector
(`J491`, JST `10FLT-SM2-TB`). Once JTAG works on the 7300, the plan is
to check whether the IC-9700's IC101 has an equivalent
exposed debug connector — not yet checked, no schematic access to the
9700 board confirmed either way.
