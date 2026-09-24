# IC-7300 signal chain — session history

Full session-by-session narrative and evidence trail behind [notes/ic7300-signal-chain.md](ic7300-signal-chain.md),
which carries only the current-state summary (confirmed facts, reference tables, and open
questions). Sections below are archived verbatim, in their original order.

## Archived from ic7300-signal-chain.md on 2026-09-24

### FPGA configuration — superseded IC902-as-EEPROM reasoning (retracted 2026-08-29)

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

### DSP/FPGA control signal mapping — "Genuinely negative" pin-group analysis, 14th-session corrections

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
  **Correction, next session (`SCIF5`/DSP-comms thread)**: this "second McASP1 audio link" reading for
  `DSPCK`/`DSPR`/`DSPX` (`P8_0/1/2`) specifically is very likely wrong. `scif5_dsp_link_driver_init`
  (the real DSP command/data link's own driver, see [[multi-cpu-images]]) was found to configure exactly
  these 3 port-8 bits' `PFCn`/`PFCEn`/`PMCn` registers (peripheral mode genuinely enabled, function code 3)
  — i.e. the CPU side drives them as **`SCIF5`** (a UART), not McASP1. The DSP-side pin names being
  nominally McASP1-capable doesn't mean the DSP uses them that way on this board — TI DSP pins are commonly
  multiplexable between McASP and UART/GPIO, and this design apparently picked UART mode here for the
  DSP boot/control link. `DCSX`/`DCSR` (`P8_12/13`) are untouched by this and still unconfirmed either way.
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
consumes the `0xb0`/`0xe2`-tagged entries specifically") was, at the time, narrowed rather than answered by
this session's ruling-out of RSPI2/SSIF/front-panel-UART. **Resolved since, in [[multi-cpu-images]]**: the
real consumer is `chunk_transport_send_data` → `dsp_page_transfer_verify` → `SCIF5` — i.e. the firmware-update
chunk transport straight to the DSP link, not any of the 3 candidates this session ruled out. This session's
negative result stands as a correct (if superseded-in-scope) finding, not an error.

**Driver init found too**: `FUN_200b665c` configures Port 8's pins (via the same bit-manipulation idiom as
`port_bulk_gpio_init_pass1`/`pass2`) and registers four event handlers via `register_event_handler` — IDs
`0x115`, `0x2a`, `0x2b`, and `0xa2` (the last one wired to `FUN_200b6444`, RSPI2's own status-wait/init
counterpart to the transmit function above). This is the real RSPI2 driver's setup routine.

## `DRESD` (P2_6) resolved — boot-time-only, held at a fixed level (13th session)

Traced by cross-referencing the RZ/A1H manual's port register map (§54.3, `PORTn_base` = `0xFCFE3000`,
sub-block bases `Pn`=`+0`, `PSRn`=`+0x100`, `PPRn`=`+0x200`, `PMn`=`+0x300`, `PMCn`=`+0x400`,
`PFCn`=`+0x500`, `PFCEn`=`+0x600`, `PNOTn`=`+0x700`, `PMSRn`=`+0x800`, `PMCSRn`=`+0x900`,
**`PFCAEn`=`+0xA00`** (found later, see [[kernel-rtos]]'s "SCIF1 and SCIF5 physical pins resolved" section —
the alt-function 3rd bit, missed by this session's search since it wasn't needed for `DRESD`),
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


This confirms the first reading above and fits the existing hypothesis in this file's "FPGA
configuration" section cleanly: **the DSP has no persistent program flash of its own — the main CPU
holds it in reset from boot and never releases it in `body.bin`'s traced control flow**, consistent with
it being boot-loaded into RAM by the main CPU instead (via the `FUN_20025044` ring-buffer mechanism
documented in [[multi-cpu-images]]). Release, if it ever happens, must come from somewhere `body.bin`
doesn't reach — worth keeping in mind as a possible role for the still-unexplained `chunk4`/`chunk5-tail`
consumer once that's pinned down, or it genuinely never gets released in software and something else
(fixed hardware timing, a one-shot power-on RC delay wired directly to the DSP rather than through the
CPU) brings the DSP out of reset independently of this GPIO.
