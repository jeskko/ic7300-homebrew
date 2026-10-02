# Hardware debug access (JTAG) — found via schematic, not firmware

Investigated whether a serial/debug interface already exists (before
considering any software custom-code loading route for live RAM access — see
[[firmware-update]]'s security section for that alternative, now
deprioritized by this finding). Checked the two USB CDC serial ports
first as the user's own lead, then the schematics
(`docs/IC-7300_Schematic_Diagram_2.pdf`, extracted
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

**Resolved (later session, [[ic7300-hardware]]/[[multi-cpu-images]]):** this IC is `IC501`
(`R5F104LCAFB`, Renesas RL78), confirmed via the service manual's Display Unit parts list and
independently visible marked `(SX-3765C)` on the schematic itself — the `(UX-3765C)` reading above was a
misread of `S` as `U`. Every `"SX3765 Vx.xx-yyy"` string in the main firmware is confirmed to be a
compatibility/version check against this chip's part marking, not an embedded second-processor firmware
image — the "may just be"/"plausibly" hedging above is settled. Its `TOOL0`/`TOOL1` on-chip-debug UART
pins remain a real, separate debug path from the main CPU's JTAG, not yet pursued.

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

**Followed up, since resolved: yes, this thread paid off.** [[multi-cpu-images]] later confirmed the
update container's 3 extra components do include real DSP (`IC901`) program/data images (component1/2),
and separately traced the DSP's own boot flash (`IC902`) at the pin level — see [[ic7300-hardware]]'s
`IC902` entry. So the "real second/third processor's firmware to find" speculation above was correct; the
container does carry more than main-CPU code.

## Assessment: this should take priority over any software custom-code loading route

A working JTAG/SWD connection to the main CPU gives halt/step, arbitrary
live memory read/write (including the runtime-populated globals that are
blank in our static Ghidra image — e.g. the TCB pointer investigated in
[[kernel-rtos]]), hardware breakpoints, and full register access — with
no risk of bricking anything (doesn't touch flash unless directed to).
That's strictly more capable than the "trick a subroutine into running
SD-card code" idea from [[firmware-update]], and far lower risk. **This
should be tried before any code-defect-hunting route.**

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

## Same connector, same pinout, confirmed on both radios (2026-08-30)

User checked the IC-9700's own schematic (separate radio, separate ongoing thread — see
[[ic9700-container-format]]) and confirmed its JTAG connector is the **exact same part**,
`10FLT-SM2-TB` — the identical JST FLT-series 10-position/0.5mm-pitch FPC/FFC connector documented
above for the IC-7300's `J491`. **Full pin assignment now confirmed too, and identical on both
radios** (pins referenced from the IC-9700's schematic, checked as "similar on IC-7300"):

| Pin | Signal | Pull | Net/alt name |
|---|---|---|---|
| 1 | `3.3V` | — | — |
| 2 | `RES1` | 10k to 3.3V | `RES` |
| 3 | `MTDO` | 10k to 3.3V | `JP0_1` |
| 4 | *(unused)* | 10k to GND, no other connection | confirmed genuinely unconnected, not `BSCANP` — see below |
| 5 | `MTCK` | 10k to GND | `TCK` |
| 6 | `MTMS` | 10k to 3.3V | `TMS` |
| 7 | `MTDI` | 10k to 3.3V | `JP0_0` |
| 8 | `MTRST` | 10k to GND | `TRST` |
| 9 | *(NC)* | — | — |
| 10 | `GND` | — | — |

Confirms several things this file's earlier "next steps" had flagged as unconfirmed: **signal
voltage is 3.3V** (pin 1, directly on the connector — matches the SoC's expected level, no separate
verification against the power rail needed), and the RZ/A1H's own `MTCK`/`MTMS`/`MTDI`/`MTDO`/`MTRST`
naming confirms standard ARM JTAG (not a Renesas-proprietary protocol), consistent with the schematic
read in the "Found instead" section above. ~~The `M`-prefixed names and the `JP0_0`/`JP0_1` alt-names on
`MTDI`/`MTDO` indicate these are muxed pins shared with GPIO port 0 bits 0/1 — the chip likely needs a
mode/boot-strap configuration to actually route them to the JTAG function rather than plain GPIO (not
yet confirmed which strap, if any, controls this — worth checking before assuming the interface is
"live" without configuration). Pin 4's role is unclear (no signal name given, just pulled to GND) —
possibly a debug-enable strap, not confirmed.~~ **Both retracted, 2026-08-30** — see the "Firmware
readiness check" section below for the real picture: `JP0_0`/`JP0_1` are a separate pin group from GPIO
Port 0 (not muxed with it), default to JTAG mode with no strap needed, and pin 4 is just an unconnected
pulldown, not a debug-enable strap.

**Practical implication**: the FT2232H adapter + FFC breakout chosen for the IC-7300
should now work for both radios as-is — pinout is confirmed identical,
not just the connector part.

## Firmware readiness check: are the JTAG pins actually live by default? (2026-08-30)

User's ask: trace whether `TCK`/`TMS`/`JP0_0`/`JP0_1`/`TRST` are left in a JTAG-usable state by default, or
whether the firmware does anything that would need working around first. Read the RZ/A1H hardware manual's
Ports (54) and Debugger Interface (56) chapters in full and cross-checked every relevant register address
against both `body.bin` (via the live Ghidra project's exhaustive literal-pool search, the same method
already validated for `DRESD`/`P2` in [[ic7300-signal-chain]]) and the raw `base.dat` (byte-search, since
it isn't loaded as its own Ghidra program). **Clean result: JTAG is left fully at its hardware power-on-
reset defaults for this entire firmware's boot and runtime lifetime** — the exact detail follows.

**Correction to this file's own earlier hedge**: the "`JP0_0`/`JP0_1` are muxed with GPIO port 0 bits 0/1"
guess above is **wrong** — checked directly against the manual's Port chapter. `JP0_0`/`JP0_1` (**JTAG
Port 0**, alternate functions `TDI`/`TDO`) and `P0_0`-`P0_5` (**general Port 0**, where `P0_0`/`P0_1` are
the `MD_BOOT0`/`MD_BOOT1` boot-mode straps from [[memory-map]]) are two **entirely separate** pin groups
on this SoC, each with its own dedicated register block (`<JPORTn_base>` = `0xFCFE7B00` vs. the general
`PORTn_base` = `0xFCFE3000` family) — they only share a superficially similar "port 0, bits 0/1" naming
convention. The boot-mode-strap concern doesn't apply to the JTAG data pins at all.

**`JP0_0`/`JP0_1` (`TDI`/`TDO`) — default state confirmed JTAG-active, and never touched by firmware**:
- The manual's own reset value for the JTAG port's mode-control register (`JPMC0`, `<JPORTn_base>+0x40` =
  `0xFCFE7B40`) is `0xFFFF` — bit=1 means "Alternative mode" (i.e. `TDI`/`TDO`, not GPIO) per the register's
  own bit description. **The chip powers up with these pins already in JTAG mode**, no boot-strap or
  firmware configuration needed.
- Exhaustively searched for every literal reference to the JTAG-port register block
  (`0xFCFE7B00`/`0xFCFE7B20`(`JPPR0`)/`0xFCFE7B40`(`JPMC0`)/`0xFCFE7B90`(`JPMCSR0`)/`0xFCFE7F00`(`JPIBC0`))
  as a 4-byte little-endian constant: **zero hits anywhere in `body.bin`** (Ghidra's own memory search over
  the analyzed image) **and zero hits anywhere in the raw `base.dat`** (direct byte search, v1.42). Neither
  boot stage ever writes to these registers — the power-on-reset default holds for the device's entire
  operating lifetime as shipped.

**`TCK`/`TMS`/`TRST` — not part of any port structure at all, can't be reconfigured by firmware even in
principle**: the manual's Port Function table (54.4) lists exactly one JTAG-related port entry, "JTAG Port
0 (`JP0_0` to `1`)" — `TCK`/`TMS`/`TRST` don't appear anywhere in the ports chapter. They're dedicated,
non-multiplexed pins with no documented GPIO alternative, matching this file's own schematic-derived pinout
table above (only `MTDI`/`MTDO` carry a `JP0_x` alt-name; `MTCK`/`MTMS`/`MTRST` don't). Firmware has no
register-level path to disable or repurpose these three pins.

**The deeper "is invasive debug actually enabled" question — also defaults on, also untouched by
firmware**: the ARM core's own `DBGEN`/`NIDEN` debug-enable signals (gating whether the CoreSight TAP can
do invasive things like halt/breakpoint, not just boundary-scan) are controlled by `ICEREGMDRSTCTL`
(`0xFC00F000`) *only* when `ICEREGJTTRCSEL`'s (`0xFC00F004`) `PINSETEN` bit is `0`. **`PINSETEN`'s own
reset value is `1`** — meaning by default, `DBGEN`/`NIDEN` are driven directly by the physical **`BSCANP`**
pin's hardware level, not by any software register at all (Table 56.1: `BSCANP`=0 selects "Normal
operation (CoreSight debug mode)", the mode real invasive JTAG debugging needs, vs. `BSCANP`=1 for
boundary-scan-only). Searched for all 4 ICE-register addresses (`ICEREGMDRSTCTL`/`ICEREGJTTRCSEL`/
`ICEREGCLKPWRCTRL` `0xFC00F014`/`ICEREGLOCKACCESS` `0xFC00FFB0`) the same way — **zero hits in either
`body.bin` or `base.dat`**. Firmware never touches this register block either, so whatever `BSCANP` is
tied to on the board is what actually decides this, with no software override anywhere in the traced boot/
runtime path.

**`BSCANP`'s physical net — confirmed, 2026-08-30, user read directly off the schematic**: `BSCANP` has a
dedicated **10 kΩ pulldown to GND (`R311`)** and **no other connection anywhere in the schematic** — not
routed to the JTAG connector (`J491`) or anywhere else, just permanently tied low by this one resistor.
**This retracts the "plausibly connector pin 4" guess above** — pin 4 is a separate, still-unidentified
signal, not `BSCANP`. The practical answer is actually simpler than that guess: `BSCANP` is **hardwired
low at all times**, full stop, no jumper/strap/connector pin involved at all. Combined with the firmware-
side result above (`PINSETEN` reset value already routes `DBGEN`/`NIDEN` from this pin, and nothing in
either boot stage ever changes that), this closes the loop completely: **`BSCANP`=0 permanently** →
Normal operation (CoreSight debug mode) is the board's only possible state, hardware-fixed, with real
invasive debug enabled by the same hardwiring. Connector pin 4's own identity remains open (see next
steps below) but no longer matters for this question.

**Bottom line for the user's question**: no extra hurdles, confirmed both ways. Every register this
firmware *could* have used to disable or reconfigure JTAG (`JPMC0`, the ICE debug-enable block) is left at
its power-on-reset default across the entire traced boot and runtime path, and `BSCANP` — the one signal
that could have overridden that from the hardware side — is permanently hardwired low by `R311`, with no
firmware or jumper involvement possible at all. A standard ARM debug probe should be able to attach and do
real invasive debug (halt, breakpoints, memory read/write) as soon as it's wired up, no unlock step of any
kind needed on either the firmware or hardware side.

## Next steps (physical, not further Ghidra work)
1. ~~Physically locate the header near `IC301`~~ — done, confirmed
   populated via photos.
2. ~~Confirm exact pin order/assignment against schematic~~ — done, see the pinout table above
   (confirmed identical on both the IC-7300 and IC-9700).
3. ~~Confirm signal voltage~~ — done, pin 1 is a direct `3.3V` line, matches the SoC's expected level.
4. ~~Identify the connector's exact pitch/pin count~~ — done, see "Connector identified precisely"
   above: 10-position, 0.5mm pitch, JST FLT-series — this is what the already-ordered FFC breakout
   was sourced against.
5. A standard ARM debug probe (J-Link or SWD/JTAG-compatible) should
   work once wired, assuming standard ARM JTAG/SWD signal behavior.
6. ~~Confirm the firmware doesn't disable/reconfigure JTAG or the ARM debug-enable signals at runtime~~ —
   done, see "Firmware readiness check" above: everything relevant is left at hardware power-on-reset
   defaults, both boot stages never touch the relevant registers.
7. ~~Confirm `BSCANP`'s physical net~~ — done, user read it directly off the schematic: hardwired low via
   `R311`, no connector involvement. See above.
8. ~~Connector pin 4's own identity~~ — done, user read the schematic: pin 4 goes only to a 10 kΩ pulldown
   to GND, no other connection anywhere (same "isolated pulldown, no routed net" shape as `BSCANP` itself,
   just a different resistor). Genuinely just an unused/reserved connector pin, not a signal worth
   chasing further — closes this file's last open item.
