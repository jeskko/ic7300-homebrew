# FPGA ↔ CPU link (band scope): spec for a fake FPGA

Static analysis of `body.bin` v1.42 (Ghidra, base 0x20005000), 2026-09-24, with QMP checks in the
emulator. Picks up from [HANDOFF-fake-fpga.md](HANDOFF-fake-fpga.md). Confidence: ✅ confirmed
(decompile, disassembly and a live check agree), 🟢 static only but unambiguous, 🟡 inferred.

**Summary.** Everything goes over **RSPI2**. Neither SCIF5/FPDX nor FPSX/FPSR is involved: the
whole driver (0x200b6444–0x200b7910) has one literal pool, and it holds no PORT8 or SCIF5
address. The CPU writes a 7-byte register image to the FPGA, then pulls one sweep: it sends
`90`, reads a **1-byte header**, and reads **475 amplitude bytes** by DMA. The CPU polls as fast
as its main loop runs. The FPGA paces the sweeps with a 4-bit counter in the header.

## 1. Hardware bindings ✅

| Item | Value |
|---|---|
| RSPI2 | SPCR 0xE800D800, SPSR +3, SPDR +4 (byte access), **SPBFCR +0x20** (the old notes called this "SPCMD2"; it's the buffer control register: bit7 TXRST, bit6 RXRST) |
| RSPI2 init | SPPCR=0x20, SPBR=1, SPDCR=0x20, SPCMD0=0xE78C (8-bit, MSB first, CPOL=CPHA=0, SSLKP), SPBFCR=0x30 (RX trigger = 1 byte) |
| Events (= GIC IDs, checked against `Renesas_RZ_A1.h`) | 0x115 = 277 **RSPISPRI2** → `fpga_spri2_rx_isr` 0x200b6500; 0x2a = 42 **DMAINT1** → `fpga_rx_dma1_done_isr` 0x200b64d4; 0x2b = 43 **DMAINT2** → `fpga_tx_dma2_done_isr` 0x200b64c0; 0xa2 = 162 TGI4D → `rspi2_wait_ready` (TX-frame completion, already modelled) |
| DMA ch1 (RX) | N0SA=SPDR2, N0DA=**0x20415a20**, N0TB=**0x1db (475)**, CHCFG=0x00100261 (SEL=1, source fixed, dest incrementing, 8-bit), DMARS_ch1=**0x12a** (RSPI2 RX) |
| DMA ch2 (TX dummy) | N0SA=0x20336004 (a ROM byte = 0x00), N0DA=SPDR2, N0TB=475, CHCFG=0x0030026a (SEL=2, REQD, source and dest fixed), DMARS_ch2=**0x129** (RSPI2 TX) |
| DMA start | before the transfer, the cache is invalidated over 0x20415a20..0x20415bfb; for each channel, CHCTRL bit3 (SWRST) then bit0 (SETEN); then SPCR2=0xE8 (SPRIE, SPE, SPTIE, MSTR) |
| DMA ack | both ISRs do CHCTRL \|= 0x62 (CLREN, CLREND, CLRTC). ch1's also sets SPCR2=8 and **state=4** |

## 2. Registers the CPU writes (CPU → FPGA) ✅

The CPU keeps a 7-byte image at **0x20390753** and the last-sent copy at 0x2039075a.
`fpga_scope_build_regs` (0x200b7518) rebuilds the image every tick, then diffs it against the
copy. The changed range, widened to whole registers using the ROM table at 0x2033600c (start
index per byte: `00 01 01 01 04 04 06`; size: `01 03 03 03 02 02 01`), goes out as one RSPI2
frame: `[first reg index][bytes up to and including the last changed register]`. It is queued as
job type 3 on the shared job ring via `fpga_reg_write_enqueue` (0x200b6bcc), and only sent while
the sweep state is 0 or 4.

| Reg | Bytes | Content |
|---|---|---|
| 0 | 1 | bit7: select the reg1-3 source (scope on and not TX-inhibited, and 0x2039071f bit4) 🟡; bit6: 0x203def98[0x11]&1; bits4:3: 0x203def98[0x10]&3; **bit2: one-shot resync** (set on a mode/band/span change or on the 3 s watchdog, and cleared from the sent copy after sending, so it's a pulse); bit1: `fpga_reg0_bit1_state` (TX/tuner-related 🟡); bit0: `fpga_scope_tx_inhibit` |
| 1-3 | 3 | signed 24-bit **frequency offset from the VFO, in Hz**. Centre mode: the per-mode offset from `fpga_mode_freq_offset` (+1500 = 0x5dc in USB, −1500 in LSB, 0 otherwise). Fixed mode: (edge centre − VFO) minus a per-band table value, where the FPGA's reach is ±1,010,000 Hz; beyond that the CPU shifts the samples itself (§4) |
| 4-5 | 2 | half-span·2/100 = **half-span / 50 Hz** (±25 kHz → 0x01f4) |
| 6 | 1 | **seq<<4**, where seq = 0x203def98[0x18]. It is bumped for each new configuration, and the FPGA echoes it (§3) |

A seq change while a read is in flight calls `fpga_sweep_read_abort` (0x200b6d60), which
disables DMA ch1/ch2, sets SPCR2=8 and state=0.

The live values after `27 10 01` match the catalogued frames. The image is `02 00 05 dc 01 f4
20`; the scope struct 0x204159e0 holds VFO 14,100,000 (+0xc), half-span 25,000 (+0x10) and
offset 1500 (+0x14/+0x18). Open 🟡: in fixed mode the emulator sends `01 28 d9 e0 00 00`
(offset 2.68 MHz, span 0). That's outside the ±1.01 MHz clamp, so the fixed-edge settings are
probably not initialised in the emulator.

## 3. Sweep read (FPGA → CPU) ✅ (live check: the emulator sits in state 1)

State byte **0x20390751**. Flags: 0x2039074f "busy" (a sweep is requested), 0x20390750
"requested", 0x2039074e "register frame in flight". The previous header nibble is at
0x20390752 (initialised to 0xff).

```
fpga_sweep_poll (0x200b7478, inlined in FUN_200b517c, once per main-loop tick):
  if !busy: return
  if state==0 && !tx_in_flight:   fpga_sweep_read_start   (0x200b6d0c)
        requires SPSR2.SPTEF; state=1; DMARS ch1 cleared; SPCR2=0xC8 (SPRIE|SPE|MSTR); SPDR2 = 0x90
  if state==4: fpga_sweep_apply_reflevel(); busy=0; state=0

fpga_spri2_rx_isr (SPRI2):
  state 1: SPBFCR RXRST pulse (byte clocked in during the 0x90 is discarded); SPDR2 = 0x00; state=2
  state 2: H = SPDR2
           if (H>>4) == (last_sent_reg6>>4) && (H & 0xf) != prev_nibble:
               prev_nibble = H & 0xf; state=3; start DMA ch1+ch2 for 475 bytes; SPCR2=0xE8
           else: state=0; SPCR2=8           # stale or no new sweep: retry next tick
DMAINT1: ack; SPCR2=8; state=4
```

On the wire, per sweep attempt: MOSI `90 00 [00 ×475]`; MISO `xx H [s0 … s474]`. **The header
is the whole handshake.** The high nibble echoes the config seq the sweep was taken with, so
sweeps from an older configuration get rejected. The low nibble is a free-running sweep counter
that must change between accepted sweeps. There is no checksum or length field.

**Today's emulator:** after scope on, `busy=1 req=1 state=1`, stuck. `rspi2.c` never raises
SPRI2, so the state machine never leaves 1. Because frames only go out in state 0 or 4, no
further config frames follow either. That's why the catalogue shows exactly one lone `90` per
change: each is issued after an abort resets the state.

## 4. Samples, where they go, who reads them ✅

1. **Raw bytes → display units** (`fpga_sweep_apply_reflevel` 0x200b7410, in place at 0x20415a20):
   `out = clamp(raw + ref/5 − 0x50, 0, 0xA0)`, where `ref` = u16 at 0x203def00+0xaa. Live, ref
   0 dB gives 200 and "+20 dB" gives 400 (🟡 depends on reading 27 19 as BCD). So
   `out = raw + 2·ref_dB − 40`: **0.5 dB per count**, and at ref 0 **raw 40..200 maps to the full
   0..160 display**. 160 is also the CI-V 27 00 waveform maximum.
2. **Delivery** (`fpga_sweep_deliver` 0x200b7788, via `fpga_scope_tick` 0x200b78b0 from the tick):
   when `req && !busy`, it copies the 475 bytes to **0x203def98+0x19**, shifted by the signed
   sample offset at 0x204159e0+0x1c (fixed-mode edges beyond the FPGA's ±1.01 MHz reach) with
   zero fill. It then sets 0x203def98[0x1f4]=1 ("new waveform"), reloads the 3 s watchdog, and
   re-arms (busy=req=1).
3. **UI:** `scope_waveform_copy_to_ui` (0x20055acc) is called from FUN_20056ab0 in
   main_idle_loop. It copies 475 bytes through an identity transform to 0x203fa1a0 and adds the
   metadata: centre 0x203fa170+0x1c, span, lower and upper edge. Not yet traced: the drawing
   itself, the waterfall, and the CI-V 27 00 builder.

## 5. Timing ✅

- MTU2 ch3/4 count at 32 MHz. TGI3A fires every 8000 counts = **250 µs** and signals the main
  loop (0x20390315++). `main_idle_loop` runs a pass once the counter reaches 2, so it ticks at up
  to **2 kHz** and FUN_200b517c runs every pass.
- The CPU therefore asks for a sweep every tick and accepts one at most every 2 ticks, plus the
  SPI transfer time: 477 bytes, about 2 ms at SPBR=1/BRDV=3. **Sweep rate is the FPGA's choice:**
  bump the header counter only when a new sweep exists. Scope speed (27 1A) never reaches the
  FPGA, so speed and averaging are handled CPU-side.
- Watchdog: 0x2039077c is decremented once per 100 ms (FUN_200b7910, the 500 µs tick, at count
  200). Each delivery sets it to 30. If it reaches 0 with the scope active,
  `fpga_scope_watchdog_check` raises reg0 bit2 (resync) and resends the config. So **after 3 s
  without a sweep the CPU resyncs**. No error message was found on this path.

## 6. What a fake FPGA must do

1. **RSPI2 receive:** on every SPDR2 write, clock one MISO byte into a receive latch (SPDR2 read
   returns it), set SPRF, and raise GIC 277 while SPCR.SPRIE is set. Deliver it after roughly the
   byte time, not synchronously.
2. **Frame parse:** 0x90 = read command. Otherwise frames are `[reg][data…]` into a 7-byte
   register file. Remember reg6>>4.
3. **Response:** the byte after `90` is don't-care. The next is `H = (reg6>>4)<<4 | counter`.
   Then 475 samples.
4. **DMA ch1/ch2:** `dmac.c` only models ch0. The minimum is: when ch1 gets SETEN with DMARS
   0x12a, write the 475 samples to N0DA_1 and pulse GIC 42 (and GIC 43 for ch2) after about
   2 ms. One timer per sweep, not per byte (handoff gotcha).
5. **Pacing:** advance `counter` once per desired sweep (e.g. 20–30 Hz). An unchanged counter
   is what "no new sweep" looks like.
6. **Content:** at ref 0 dB, noise floor ≈ raw 50–60 and a signal peak ≈ 150–190. The centre
   sample index 237 is the VFO frequency plus the reg1-3 offset; the span is ±(reg4-5 × 50) Hz
   over 475 points.
