# SSTV receiver app (App 4): research and design (2026-09-25)

This is the research pass for App 4 in `app-requirements.md`. **Update, same day:** the
first version is built and runs in the emulator (`examples/sstv-rx/`, loader ABI v4). Its
README has the status and the lost-block finding. It covers
where the app gets its audio, what to reuse from slowrx / slowrx-cli, what to change to make it
run in real time on the radio, and a build plan. The algorithm was checked on the host with
`examples/sstv-rx/prototype/sstv_proto.py` against `scratch/samples/SSTV.test.au`.

## Findings

**1. The audio is reachable, and it's the 48 kHz RX-audio ring** ✅ The old "biggest open question"
in `api/audio.md` has been answered by the audio-link work in `notes/dsp-protocol.md`:

- DX_REC L (DSP → CPU, SSIF0 RX) is **RX audio after demodulation**. It's taken before AF gain
  (so the volume knob doesn't change it) and low-passed. During TX it carries the CW sidetone.
  In USB, which is what SSTV uses, this is exactly the audio a PC decoder would get.
- The firmware keeps it in a 48 kHz ring at 0x203fbdc0: 8 blocks × 36 int16 = **6 ms deep**. The
  pump is `ssif0_rx_pump_dx_rec` (0x20060614), which runs from the 250 µs TGI3A tick ISR and
  pushes one block per 0.75 ms through `ssif_rx0L_ring_push36` (0x2005fb28).
- The ring has **one reader**, `qso_recorder_rx_audio_block` (0x20067254), which also feeds the
  audio scope. There's one read index, so a second reader would steal blocks. At 6 ms deep it
  can't be polled from an app's idle tick either.
- **So the tap is a loader hook on `ssif_rx0L_ring_push36`.** The patched call copies the 36
  samples into an app-owned ring, decimating by 4 to 12 kHz, then does the original push. It
  runs in ISR context, 1333 times a second, and does nothing but copy (a lock-free
  single-producer ring; the app's coroutine is the consumer). This is a new loader hook point,
  so the loader has to be re-flashed and the ABI bumped (v4: an `audio_hook` field next to
  `idle_hook`).
- An alternative is the 8 kHz stream the recorder derives (`FUN_20066fc4`, 6 samples per block).
  SSTV fits (1100–2300 Hz, Nyquist 4 kHz), but it has more filtering between it and the
  source. 12 kHz from the 48 kHz ring is the cleaner choice.

**2. slowrx-cli decodes our sample, but its demodulator doesn't suit a real-time embedded
decoder.** Repo: https://github.com/sgarriga/slowrx-cli (ISC-style licence, from Oona
Räisänen's slowrx; reuse with the copyright notice kept). Built on the host with FFTW, it
decoded the original test recording (a third-party Scottie 2 recording of a broadcast test card,
not redistributed here; resampled to 44.1 kHz) as **Scottie 2** (VIS 56), with a blue colour
cast and a slightly shifted left edge. Why its design doesn't port as is:

| slowrx-cli | Problem on the radio |
|---|---|
| Whole WAV in RAM as `double`, offline passes (line-time autocorrelation, then decode) | Live stream; no file |
| `lum_cache`: 1 byte per input sample for the whole image (Martin 1 at 44.1 kHz ≈ 5 MB) | 448 KB heap |
| `PixelGrid`: a 20-byte struct per pixel per channel (320×256×3 ≈ 4.9 MB) | same |
| Frequency estimate: a Hann-windowed 1024-point FFT **every 6 samples** + Gaussian peak interpolation | ~7350 FFTs/s at 44.1 kHz, ~2000/s at 12 kHz; heavy for a 400 MHz Cortex-A9 that also runs the radio |
| FFTW, libc stdio, BMP writer | none on the radio |

**3. A quadrature discriminator with per-line sync lock is cheaper, and on this sample it
decodes better.** `prototype/sstv_proto.py` works at 12 kHz. It mixes by 1900 Hz, applies a
33-tap FIR at 900 Hz, and takes frequency = arg(z[n]·z*[n−1]). VIS is decoded from that same
frequency track. Each line re-locks on the end of its 9 ms 1200 Hz sync. On that same recording
it gave the correct colours and no slant, with a left-edge R/G/B misalignment of a few pixels at
most. [prototype/scottie2-testcard.png](examples/sstv-rx/prototype/scottie2-testcard.png) is its
decode of the synthetic test card that replaced the recording
(`qemu-machine/tools/gen_samples.py`). Cost in C: about 100 flops per sample (mixer 4, complex FIR 66, atan2 approximation about
20, sync filter), so about 1.2 MFLOP/s at 12 kHz, well under 1% of the CPU. The
real-time-budget question in `app-requirements.md` is answered for the discriminator.
Unmeasured on hardware.

## What to take from slowrx-cli

- `modespec.h`: the VIS codes and per-mode timing table (sync/porch/separator/pixel/line time,
  size, colour encoding, channel count) for Martin, Scottie, Robot, PD, Wraase and Pasokon.
  This is the most valuable part, and it's plain data.
- `image.c` `compute_channel_timing`: the per-family channel layout (Robot 36's alternating
  R-Y/B-Y lines, PD's two lines per frame, Scottie's sync mid-line). This is logic to port.
- The colour conversion (GBR/RGB/YUV/BW, with the YUV coefficients) and the luminance mapping
  `(f − 1500) / 3.137`.
- `vis.c`: the VIS framing rules (leader 1900 / break 1200 / leader 1900 / start 1200 / 7
  data bits LSB first + parity / stop, 30 ms bits). The prototype does the same thing on the
  discriminator output.
- Not taken: the FFT estimator, the offline line-time autocorrelation, whole-image buffers, and
  FFTW. The adaptive window (SNR → FFT length) becomes an SNR-dependent discriminator
  post-filter if it's needed at all.

## App design (proposed)

```
 tick ISR (250 µs)                          UI-thread coroutine (main(), hb_yield per pass)
 ssif0_rx_pump_dx_rec                       ┌─ pop 12 kHz samples ─→ discriminator ─→ freq
   └ ssif_rx0L_ring_push36 ─hook─→ app ring ┤   state: HUNT (VIS search) → LINE (per-line
     (36 @ 48 kHz → 9 @ 12 kHz, 1 s = 24 KB)│   sync lock, pixel sampling) → DONE
                                            └─ each finished line → GR3 overlay (hb gfx) + image row
```

- **States**: HUNT looks for the VIS leader and start bit and decodes the mode. LINE predicts
  each sync from the mode's line time, re-locks within ±20 ms, and samples pixels at their
  times. Then the line is drawn. DONE shows the image and offers SAVE (SD) and back to HUNT.
  EXIT always quits.
- **Streaming, not whole-image**: keep only a sliding frequency history, about one line plus a
  sync window (Scottie DX, the longest, is 1.05 s ≈ 12.6 k floats = 50 KB). Sample pixels
  when their time comes. The pixel positions are computed per line from the mode table, so
  there's no `PixelGrid`.
- **Memory** (448 KB heap): audio ring 24 KB, frequency history ≤ 50 KB, image 320×256 RGB565 =
  160 KB (kept for saving). For larger modes (PD 120: 640×496, PD 290: 800×616), draw scaled
  to the screen and write rows to SD as they finish instead of holding the image.
- **Display**: the image is at most 320×256 on the 480×272 GR3 overlay, with the mode name,
  VIS, a slant/sync indicator and a live frequency bar in the remaining 160 px column.
- **Save**: BMP (or a raw RGB565 with a header) through the SD path in `api/filesystem.md`.
  Watch for the emulator's known SD-card dropout after a save + load.
- **Mode priority**: Scottie 1/2/DX and Martin 1/2 (the HF common ones), then Robot 36/72, then
  PD 90/120/180 (ISS events). Wraase and Pasokon last; slowrx-cli itself calls parts of
  Wraase untested.

## Test plan

1. **Host**: port the C decoder core (`sstv_core.c`, no SDK dependencies) and run it against the
   prototype on the sample. Compare pixel-wise with `scottie2-testcard.png`, and with the transmitted
image (`SSTV.test.png`, written by `gen_samples.py`). Add samples per mode (`--sstv-mode`).
2. **Emulator, audio path**: `RZA1H_AF_FILE=scratch/samples/SSTV.test.au` feeds DX_REC L, and
   the fake DSP already resamples to 96 kHz (`qemu-machine/src/ssif.c`). Check that the hook's
   12 kHz ring matches the file (the same method as `tools/audio_stimulus_check.py`).
3. **Emulator, app**: `test_emu.py`-style. Launch SSTV.BIN, play the file at runtime with
   `qom-set /machine/ssif af-file`, wait for DONE, and screendump against a reference.
4. **Hardware**: last. The load comes from the per-sample work in the coroutine, and the ISR hook
   is copy-only.

## Open questions

- Does the DX_REC L low-pass cut into the SSTV band (1100–2300 Hz)? The DSP filter is described
  only as "low-passed" (`notes/dsp-protocol.md`). Check on hardware, or in the DSP code at
  struct+0x180 / 0x11811224.
- Does DX_REC L follow the IF filter width? SSTV wants the 2.4–3 kHz SSB filter; a narrow data
  filter would clip 2300 Hz.
- How often does `main_idle_loop` run under load (menus, scope)? The app ring must cover the
  longest gap: 1 s = 24 KB is generous, but it's unmeasured.
- Is hooking a function called from an ISR safe with respect to the app being unloaded? The
  hook must check a loader-owned "audio sink armed" flag that the loader clears before it frees
  the app region.
