/* Receiver audio for homebrew apps (ABI v4).
 *
 * hb_audio_open() starts capturing the radio's demodulated RX audio (DX_REC L: what the
 * demodulator hands the speaker path, before the AF gain knob, so volume doesn't change it)
 * at 12 kHz, mono, int16. hb_audio_read() then returns whatever has arrived since the last
 * call, without waiting. The runtime keeps 16384 samples (1.37 s): read at least that often
 * -- once per UI loop pass is plenty -- or new samples are dropped (and counted in
 * hb_audio_overruns()) until there is room again.
 *
 *     hb_audio_open();
 *     for (;;) {
 *         int16_t buf[1024];
 *         int n = hb_audio_read(buf, 1024);
 *         process(buf, n);
 *         hb_yield();
 *     }
 *
 * Capture stops with hb_audio_close(), and in any case when main() returns. Under the hood:
 * the loader hands every 36-sample 48 kHz block to the runtime from the system-tick ISR, and
 * the runtime averages groups of 4 into a single-producer/single-consumer ring. The DSP has
 * already low-passed the stream, so the plain 4-sample average is the only decimation filter.
 *
 * Blocks the firmware itself missed (its DMA buffer was overwritten before the tick got to it)
 * are detected from the time between blocks and filled with interpolated samples, so the
 * stream keeps its length and timing; hb_audio_gaps() counts them.
 */
#ifndef HB_AUDIO_H
#define HB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define HB_AUDIO_RATE   12000

bool hb_audio_open(void);
void hb_audio_close(void);
int hb_audio_read(int16_t *buf, int max);
uint32_t hb_audio_overruns(void);
uint32_t hb_audio_gaps(void);           /* 0.75 ms blocks filled in (see above) */

#endif
