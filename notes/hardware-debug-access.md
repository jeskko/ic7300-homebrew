# Hardware debug access (JTAG) — found via schematic, not firmware

Investigated whether a serial/debug interface already exists (before
considering any software code-loading route for live RAM access — see
[[firmware-update]]'s security section for that alternative, now
deprioritized by this finding). Checked the two USB CDC serial ports
first as the user's own lead, then the schematics
(`/data/misc/icom/7300/doc/IC-7300_Schematic_Diagram_2.pdf`, extracted
via `pdftotext`, cross-checked visually by rendering the actual pages —
schematics lose too much positional information as flat text to trust
without the image).

## Firmware-side check: no software debug console found

Searched `body.bin` for `debug`, `console` — zero hits. `monitor` has 4
hits, all confirmed false positives (the TX audio self-monitor UI
feature — "Monitor", "Direct", "Squelch Auto" — not a debug console).
No evidence of a hidden/disabled debug shell in the firmware strings.

## Found instead: real JTAG headers, two separate chips

**Main CPU (IC301, R7S721000VCFP = the RZ/A1H) — schematic page 8
("MAIN UNIT (MAIN-2)")**: `TCK`/`TMS`/`TDI`/`TRST`/`TDO` cluster routes
to what looks like a small (~10-pin) header near the CPU, bottom-right
of the page. **This is the actually valuable target** — the chip
running everything we've been reverse-engineering (FreeRTOS, CI-V,
update logic). Standard ARM JTAG/SWD debug, not a Renesas proprietary
protocol.

**Front-panel MCU (schematic page 6, "FRONT UNIT")** — a large IC
(label partially legible, looked like `(UX-3765C)` — plausibly our
long-unresolved **"SX3765"** string mystery from [[multi-cpu-images]]:
this may just be the front-panel controller's part number, not a
companion DSP image embedded in the firmware container at all) with
`TOOL0`/`TOOL1` pins — Renesas's single-wire on-chip-debug UART protocol
used on their non-ARM MCU families (RL78/78K/RX), distinct from the main
CPU's ARM JTAG. Drives the front panel: buttons, rotary encoders, LCD
("To the MAIN UNIT").

**Bonus, unrelated to debug access but found along the way** (block
diagram, schematic page 4, "MAIN UNIT" overview): the actual DSP is a
**Texas Instruments TMS320C6745** (`IC901`, labeled "IF-DSP"), and there's
an Altera/Intel Cyclone IV **FPGA** (`IC1351`, `EP4CE55F23I7N`) handling
display/signal routing (LCD timing, `FPDX/FPSR/FPSX`, `SCPCK/SCPRK/SCPSS`).
Neither of these was previously identified. Worth reconsidering
[[multi-cpu-images]]'s framing in light of this — there may be a real
second/third processor's firmware to find after all, just not
necessarily "SX3765" (that looks like the front-panel MCU) and not
necessarily inside the update container (the TI DSP likely loads its own
firmware through a completely different mechanism, worth checking).

## Assessment: this should take priority over any software code-loading route

A working JTAG/SWD connection to the main CPU gives halt/step, arbitrary
live memory read/write (including the runtime-populated globals that are
blank in our static Ghidra image — e.g. the TCB pointer investigated in
[[kernel-rtos]]), hardware breakpoints, and full register access — with
no risk of bricking anything (doesn't touch flash unless directed to).
That's strictly more capable than the "trick a subroutine into running
SD-card code" idea from [[firmware-update]], and far lower risk. **This
should be tried before any bug-hunting route.**

## Confirmed physically populated (user-provided photos)

Cross-checked against a labeled reference photo and a third-party
teardown photo of actual radio internals: **the JTAG connector next to
`IC301` (R7S721000VC) is physically populated**, not just routed/bare
pads. It's a small **ribbon/FFC-style connector** (fine-pitch, looks like
possibly 0.5mm or 1.0mm pitch from the photo — needs confirming), not a
standard 2.54mm pin header. A nearby separate connector labeled `J101`
(standard pin-header-style) and a round "BACKUP BATTERY" component (RTC
backup, presumably a supercap) are also visible in the same photographed
corner of the board.

**Practical implication**: needs an FFC/FPC-to-pin breakout adapter
matching the connector's pitch/pin count, not just jumper wires.

**Connector identified precisely**: `J491`, Icom part `6510025142`,
package marking `10FLT-SM2-TB(LF)(SN)(M)` — this is a genuine, documented
**JST FLT-series** FPC/FFC connector: **10 positions, 0.5mm pitch**,
surface-mount female ZIF socket, straight orientation, 500mA/50V rating.
(Confirmed via web search against DigiKey/JST/Octopart listings for this
exact part number — not guessed from the photo.) Being a female ZIF
socket, connecting to it needs a matching 10-pin/0.5mm-pitch FFC ribbon
cable terminating in a breakout board or pitch-matched test clip — plain
jumper wires won't work at this pitch. 10 pins comfortably fits
`TCK`/`TMS`/`TDI`/`TDO`/`TRST` plus power/ground/reset with a couple
spare pins, consistent with the schematic.

## Next steps (physical, not further Ghidra work)
1. ~~Physically locate the header near `IC301`~~ — done, confirmed
   populated via photos.
2. **Confirm exact pin order/assignment against schematic page 8 before
   wiring anything** — getting `TRST` or a power pin wrong risks
   damaging the board. Not yet done precisely; the schematic render
   showed the TCK/TMS/TDI/TRST/TDO cluster but pin-1 orientation and
   exact pin-to-signal mapping needs a closer read.
3. Confirm signal voltage (expected 3.3V given the SoC, not yet verified
   against the schematic's power rail for this specific connector).
4. Identify the connector's exact pitch/pin count to source a matching
   FFC/FPC breakout adapter.
5. A standard ARM debug probe (J-Link or SWD/JTAG-compatible) should
   work once wired, assuming standard ARM JTAG/SWD signal behavior.
