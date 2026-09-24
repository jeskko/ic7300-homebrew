# Handoff: what the firmware expects to receive from the FPGA → a "fake FPGA"

> **Done 2026-09-24:** the static analysis is written up in [fpga-link.md](fpga-link.md) (receive path, header handshake, 475-byte sweep, DMA, timing, fake-FPGA checklist). Next: build the model.

**For a fresh session.** Written 2026-09-24 at the end of the session that built the fake DSP,
CI-V injection and the factory-default EEPROM. Goal of the next session: **static analysis** (Ghidra
on `body.bin`, loaded at 0x20005000) of how the main CPU reads data **from** the FPGA (IC1351,
Cyclone IV EP4CE55). The deliverable is a spec precise enough to write a fake FPGA model that makes
the band scope draw. Don't build the model until the receive path and data format are pinned down.

## Where things stand (don't re-derive)

- **Emulator:** boots v1.42 to the main screen in about 8 s (`-icount shift=1,sleep=off`) or about 32 s
  (real-time pacing). The EEPROM is the firmware's own factory-default image, so the screen shows
  14.100.00 USB FIL2 P.AMP1 AGC-M. Benchmark: `qemu-machine/tools/bench_boot.py` (pixel-exact vs
  `screenshots/2026-09-24-main-screen-factory-defaults.png`).
- **CI-V works** over SCIF0 at the default address 0x94: `tools/civ.py`, with QEMU started as
  `-chardev socket,id=civ,path=S,server=on,wait=off -serial chardev:civ`. The radio replies
  correctly. Reading 27 10 gives 00, so **the scope is OFF by default**; send `27 10 01` to switch it on.
- **What the CPU sends the FPGA is already catalogued**
  ([civ-dsp-fpga-catalogue.md](civ-dsp-fpga-catalogue.md), raw data in `assets/`). It's RSPI2 frames,
  scope configuration only: `00 …` scope on (+ `06 20`), `01 …` mode (center/fixed), `04 hh ll` span
  (half-span / 50 Hz). The last byte is `seq<<4`, and each frame is followed by a lone `90` transfer.
  Log them with `RZA1H_DEBUG=rspi2` (one `frame` line per transfer), or run
  `tools/civ_dsp_sweep.py --debug dsp,rspi2`.
- **The fake DSP** (`qemu-machine/src/fake_dsp.c`, notes in [dsp-protocol.md](dsp-protocol.md)) is
  the pattern to copy for the FPGA. It's a pure-C behavioural model fed by the SCIF device, with
  `RZA1H_DEBUG` logging and env-var knobs for experiments.
- **The DSP's SSIF audio stream is a different thing.** It feeds the *audio* FFT
  (`spectrum_scope_fft_task`, a misleading name) and the QSO-recorder buffer. It is not the band scope.

## The FPGA link, known facts

| Item | Value | Source |
|---|---|---|
| RSPI2 registers | SPCR2 0xE800D800, SPSR2 +3, SPDR2 +4, SPCMD2 +0x20 | [ic7300-signal-chain.md](ic7300-signal-chain.md) |
| Pins | SCPCK P8_3 (clock), SCPSS P8_4 (select), SCPX P8_6 (MOSI), **SCPR P8_5 (MISO: FPGA→CPU)** | same |
| TX function | `rspi2_transmit` 0x200b6c50: poll SPSR2 bit 6, toggle SPCMD2 bit 7, SPCR2=0x48, write bytes | same |
| Driver init | `rspi2_driver_init` 0x200b665c registers event handlers **0x115, 0x2a, 0x2b, 0xa2** (0xa2 = `rspi2_wait_ready` 0x200b6444) | same |
| Job ring | `shared_job_ring_dispatch` (0x200b0f68) case 3 = RSPI2 TX | same |
| Other FPGA-only CPU pins | **FPDX P8_11**, FPSX P8_14, FPSR P8_15 (no DSP pin) | same, pin table |
| SCIF5 ↔ FPGA | `scif5_arm_retry_timer(param)` (0x200b0cd4) switches SCIF5's receive pin from P8_2 to **P8_11 = FPDX** when param ≠ 0, so the FPGA can apparently send words over the SCIF5 UART. Every caller seen so far passes 0 | [multi-cpu-images-history.md](multi-cpu-images-history.md), decompile |
| Emulator today | `rspi2.c`: SPSR2 always TX-ready, SPDR2 reads return stale register bytes, **no receive modelled**; P8_11 mux not modelled in scif.c | |

## Questions for the static analysis, in order

1. **Which path carries FPGA→CPU data?** The candidates:
   - (a) RSPI2 receive: find who **reads** SPDR2 (a literal 0xE800D804 or base+4), and what the event
     handlers 0x115/0x2a/0x2b are (map the event IDs to GIC IDs via `register_event_handler`;
     RSPI2's SPRI/SPTI/SPEI interrupts are in the RZ/A1H manual);
   - (b) SCIF5 via FPDX: find callers of `scif5_arm_retry_timer` / `scif5_send_and_wait_reply`
     that pass param = 1;
   - (c) FPSX/FPSR (P8_14/15): plain GPIO handshake lines? Search PORT8 literal-pool uses.
   Probably (a) for the waveform, with (b)/(c) as handshakes. Don't assume it; confirm it.
2. **The frame format of what comes back:** header/sequence, sample count per sweep (the CI-V
   waveform output suggests ~475 points per sweep in 11 divisions of up to 50), sample width,
   amplitude scale, and any "sweep done" / "data ready" handshake.
3. **Where it lands and who consumes it.** Work backward from the consumers, which are easier to
   recognise:
   - the scope drawing code (OpenVG path draws in the scope area; `g_radio_ui_state_base`
     0x2040376c + 0xe04…, `scope_state_recompute` 0x20038e0c, `scope_freq_to_position` 0x200a6a54;
     see [band-scope-state.md](band-scope-state.md));
   - the **CI-V scope waveform output (27 00)**. Its wire format is publicly documented in the
     IC-7300 CI-V reference, so the handler that builds it tells you the internal buffer layout. It
     needs "scope data output" on (27 11 01 returned FA here; find which setting gates it, maybe
     CI-V USB/transceive in SET > Connectors);
   - the waterfall.
4. **How the pieces tie together:** the relation between the `00 …` / `01 …` / `04 …` config frames
   and the returned data, i.e. what fields `05dc`, `28d9e0` and the `06 20` frame mean (edge
   frequencies, reference level, averaging?).
5. **Timing:** how often the CPU expects a new sweep (scope speed 27 1A sends nothing to the FPGA,
   so the CPU paces or averages on its own side?), and what happens on timeout. Is there a "scope
   not working" error, like the DSP's?

## Suggested method

- Ghidra first (`mcp__ghidra__*`; plate comments already name many functions). Literal-pool
  sweeps work well in this image: every peripheral address sits in a literal pool, so a raw 4-byte
  search of `scratch/unpacked/142/body.bin` finds all users (add 0x20005000). Check both the exact
  register address and nearby base+offset literals.
- Confirm dynamically only after a static hypothesis. Useful probes: gdbstub breakpoints with
  remove/step/re-arm/continue (the gdbstub already stops on attach, so use `status()`, not
  `interrupt()`; registers are keyed `r0`…`r15` in `gdbrsp.py`), `RZA1H_DEBUG=rspi2,scif5`, and the
  CI-V sweep tool to switch the scope on and change span.
- Offload trivial or fan-out work to cheaper subagents (Sonnet/Haiku), e.g. "decompile these N
  functions and summarise". Keep the protocol reasoning in the main session and spot-check
  subagent claims: in the DSP work they were mostly right, but function boundaries and owners were
  often wrong.
- Write findings into a new `notes/fpga-link.md` as they're confirmed (tables, confidence marked).

## Gotchas

- The QEMU gdbstub perturbs timing (memory `icom-gdb-perturbation-resolved`). Prefer QMP/debug logs
  for "is X happening".
- `perf` isn't installed and ptrace is child-only. For host-side profiling, run QEMU as a gdb child
  with SIGINT-triggered sampling (see memory `icom-emulation-speed-idea`).
- Don't add high-rate timers in a new device model: every expiry is a vCPU↔main-loop handoff
  under icount (that was the 12× slowdown fixed 2026-09-24). Pace a fake FPGA's sweeps with one
  timer per sweep, not per sample.
- `riic2_eeprom.img` is generated (gitignored). Rebuild it with `tools/build_riic_eeprom_image.py`
  (plus `--pwrk-hold` for the PWRK variant); its base is `tools/eeprom_factory_defaults.bin`.
