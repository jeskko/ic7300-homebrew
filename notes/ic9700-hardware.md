# IC-9700 main-unit hardware (from service manual parts list)

Source: user-supplied service manual "[MAIN UNIT]" IC parts list
screenshots (2026-08-27). Not independently verified against a physical
board or schematic yet — transcribed as given. See
[[ic9700-container-format]] for how this maps onto the firmware update
container investigation.

## Programmable/firmware-relevant chips

| Ref | Part | Role |
|---|---|---|
| IC101 | R7S721001VCBG | **Main CPU** — Renesas RZ/A1 series, ARM Cortex-A9. Same family as the IC-7300's main CPU (R7S721000, see [[memory-map]]) — different specific part, but boot conventions (boot-mode-3 serial-flash XIP) and general architecture likely transfer. |
| IC102 | MT48LC4M16A2P-6A | External SDRAM (4Mx16 = 64Mbit) for the main CPU |
| IC181 | S25FL127SABMFI100 | SPI NOR flash, 128Mbit — very likely the main CPU's boot flash (holds the XIP firmware image, parallel to the 7300's boot flash) |
| IC191 / IC192 | GT24C1024-2CLI-TR ×2 | I2C EEPROM, 1Mbit each — config/calibration storage. IC-7300 has one smaller EEPROM (GT24C128B, see [[eeprom-catalogue]]); here there's two bigger ones |
| IC251 | RX-8803LC UB | Real-time clock |
| IC301 | LCMXO256C-4TN100C | Lattice MachXO2-256 CPLD/small FPGA — separate from the main SDR FPGA (IC7601), role not yet identified (glue logic / control plane?) |
| IC311 | STM32F030C6T6 | Separate Cortex-M0 MCU — role not yet identified. Has its own address space/firmware, likely another sub-image somewhere in the update container |
| IC331 | S25FL127SABMFI100 | Second 128Mbit SPI NOR flash — likely paired with IC301 or IC311 for their own firmware/bitstream, analogous to the 7300's separate DSP flash (EN25QH32A, see [[multi-cpu-images]]) |
| IC502 | TMS320C5517AZCHA20 | TI TMS320C55x DSP — SDR/audio processing, parallel role to whatever chip handles this on the 7300 |
| IC7601 | 5CEFA9F23I7N | **Intel/Altera Cyclone V FPGA** — the main SDR digital front-end FPGA (found by user on a follow-up read of the same screenshot; not a separate image). One generation newer than the 7300's Cyclone IV (EP4CE55) — makes sense for a radio doing simultaneous dual-band SDR (V/UHF + satellite mode) |

## SDR RF front-end (around the Cyclone V)

| Ref | Part | Role |
|---|---|---|
| IC7501 | LTC2156IUP-14 | High-speed ADC — RF receive path into the FPGA |
| IC7201 | DAC3482IRKD | High-speed dual DAC — RF transmit path out of the FPGA |
| IC8701 | AD9518-1ABCPZ-RL7 | Clock distribution/PLL synthesizer — feeds clean sample clocks to the ADC/DAC pair. This ADC+DAC+clock-synth trio around the FPGA is a textbook SDR front-end, same architectural role as the 7300's chain |

## External interfaces

| Ref | Part | Role |
|---|---|---|
| IC1901 | LAN8710AI-EZK-ABC | Ethernet PHY — confirms real network connectivity (remote control / D-STAR reflector access) |
| IC1601 | TUSB2046IBVFR | USB 2.0 hub controller (TI) |
| IC1551 / IC1571 | CP2102N-A01-GQFN28R ×2 | USB-to-UART bridges (Silicon Labs), fed from the USB hub — likely CI-V and/or debug UARTs |
| IC1532 | PCM2901E/2K | USB audio codec (TI/Burr-Brown) |
| IC2101 | PCM1802DBR | Stereo audio ADC |

## RF switching / gain (band and antenna control)

| Ref | Part | Role |
|---|---|---|
| IC3031 | BGA616 | RF gain-block MMIC amplifier (Infineon) |
| IC3061 | NJG1650HB-TE1 | GaAs RF SPDT switch |
| IC3101/3111/3121/3181/3182/3381/3382 | NJG1801K75-TE1 ×7 | GaAs RF switches — band/antenna switching, likely including the dual-antenna satellite-mode routing |
| IC3151 / IC3351 | PGA-103+ | RF preamp gain blocks (Mini-Circuits) |

## Power/supervisory (not individually notable, listed for completeness)

Numerous XC62xx/XC6209/XC6230 (Torex) regulators, S-1200/S-1313 (ABLIC)
voltage supervisors, MAX17503/TPS544xx/TPS62110/MP2224/MP7741 switching
regulators, NJM2881/NJM7805 linear regs — standard multi-rail power
tree for a board with this many distinct chips, nothing unusual.

## Why this matters for the container-format investigation

Confirms the IC-9700 has **at least four independently programmable
components** with their own firmware/config images (main CPU, Cyclone V
FPGA, STM32 MCU, TMS320C55x DSP) — directly parallel to what
[[multi-cpu-images]] found for the IC-7300 (main CPU + DSP + FPGA
bitstream all packed into one update container). This is strong
supporting evidence for the working theory in
[[ic9700-container-format]] that the container's distinct byte regions
(the J/E-shared-but-mostly-padding boot region, the region that differs
J vs E, the large `0x2a6000`–`0x800000` region that's identical between
J/E) correspond to different sub-images for different chips rather than
being one monolithic blob — worth revisiting the region boundaries with
this chip list in mind once the compression is cracked.

Two SPI NOR flash chips beyond the main boot flash (IC181, IC331) plus
IC301 (a CPLD, not typically field-updated the same way as an FPGA
bitstream target) suggests the update might target: main CPU (IC181),
FPGA bitstream and/or STM32 firmware (IC331), with IC301's CPLD image
either baked in at manufacturing or updated some other way — unconfirmed,
worth checking once real content is extractable.
