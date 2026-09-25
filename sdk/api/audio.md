# Audio

Current state (2026-09-25). How we got here, including the dead ends: [`audio-history.md`](audio-history.md).

## The CPU ↔ DSP audio link ✅

Fully decoded in `notes/dsp-protocol.md` ("The CPU ↔ DSP audio link, both directions"): 96 kHz I2S,
24-bit left-justified, DMAC ch3/4/5 ping-pong, pumped from the 250 µs TGI3A tick ISR.

| Stream | Content | CPU buffer | Consumers |
|---|---|---|---|
| DX_REC L (SSIF0 RX) | **RX audio**, demodulated, before AF gain, low-passed (CW sidetone during TX) | 48 kHz ring 0x203fbdc0, 8 × 36 int16 (6 ms), pushed by `ssif_rx0L_ring_push36` 0x2005fb28 | `qso_recorder_rx_audio_block` 0x20067254 only (audio scope + QSO recorder, 8 kHz) |
| DX_REC R | mic / TX modulation audio | ring 0x203fc002 | TX voice memory, record-level meter |
| DX_FMT L (SSIF1 RX) | per-mode demod output | ring 0x203fc246 | RTTY decode screen FFT scope, CTCSS detector |
| DR_AF L / R (SSIF0 TX) | speaker playback / audio to the transmitter | txL 0x203fbcc0 / txR 0x203fbd5e | the DSP |

RTTY text doesn't come over this link: the DSP drives the demodulated bit onto pin P8_7 (RTD).

## For apps

- **Reading RX audio**: the 48 kHz ring has a single read index and is only 6 ms deep, so an app
  can't read it directly. The planned tap is a loader hook on `ssif_rx0L_ring_push36` that
  copies into an app-owned ring (ABI v4). Design: [`../sstv-app-design.md`](../sstv-app-design.md).
- **Playing audio** (open): DR_AF L carries recorder playback as an 8 kHz stream written into
  96 kHz frames (`voice_play_to_dsp_tick` 0x20067458); an app could feed it the same way.
  Untested.
- **Emulator**: the fake DSP can play a stimulus file into any RX slot (`RZA1H_AF_FILE`, or
  `qom-set /machine/ssif af-file` at runtime). See `qemu-machine/README.md`.
