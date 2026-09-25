/* Streaming SSTV decoder core: 12 kHz int16 audio in, image lines out.
 *
 * Integer-only (the SDK builds apps soft-float) and free of SDK dependencies, so the same file
 * builds for the host test (host_test.c) and the app (main.c). Design: sdk/sstv-app-design.md.
 *
 *   audio -> quadrature FM discriminator (mix by 1900 Hz, 33-tap FIR, CORDIC phase difference)
 *         -> frequency track (Hz, one value per sample, kept in a 16384-sample ring)
 *         -> HUNT: VIS header (1900 Hz leader, 30 ms 1200 Hz start bit, 7 bits + parity)
 *         -> LINE: per line, predict the sync from the mode's line time, re-lock on the actual
 *            sync within +-20 ms, sample G/B/R pixels at their times, emit the line
 *         -> DONE after the last line (or 30 lines without a sync), back to HUNT
 *
 * Mode timings and VIS codes are slowrx's (Oona Raisanen, via sgarriga/slowrx-cli, ISC
 * licence). Supported: Scottie 1/2/DX and Martin 1/2 (320x256, GBR).
 */
#ifndef SSTV_CORE_H
#define SSTV_CORE_H

#include <stdint.h>

#define SSTV_RING       16384           /* frequency history, samples (1.37 s at 12 kHz) */
#define SSTV_MAX_W      320

enum sstv_family { SSTV_SCOTTIE, SSTV_MARTIN };

typedef struct sstv_mode {
    uint8_t vis;
    const char *name;
    uint8_t family;
    uint32_t sync_ns, porch_ns, sep_ns, pixel_ns, line_ns;
    uint16_t w, h;
} sstv_mode;

enum sstv_event {
    SSTV_EV_VIS,            /* a supported VIS header: mode set, image starts */
    SSTV_EV_UNKNOWN_VIS,    /* a VIS header we can't decode: arg = the code */
    SSTV_EV_DONE,           /* last line decoded */
    SSTV_EV_LOST,           /* too many lines without a sync: image abandoned */
};

enum sstv_state { SSTV_HUNT, SSTV_VIS, SSTV_LINE };

typedef void (*sstv_line_fn)(void *ctx, const sstv_mode *m, int y, const uint16_t *rgb565);
typedef void (*sstv_event_fn)(void *ctx, int event, const sstv_mode *m, int arg);

typedef struct sstv_rx {
    /* discriminator */
    uint32_t nco;
    int16_t fi[2 * 33], fq[2 * 33];     /* FIR delay lines, doubled (index i and i+33) */
    int fpos;
    int32_t pi, pq;                     /* previous filtered sample */
    int32_t sm1, sm2;                   /* 3-tap smoothing */
    /* frequency track */
    int16_t freq[SSTV_RING];
    uint32_t n;                         /* samples in the track so far */
    /* decoder */
    int state;
    uint32_t run1200, vis_start;
    const sstv_mode *mode;
    uint32_t pred;                      /* predicted sync end, samples */
    uint32_t chan_q16, pix_q16, sync_s, porch_q16, sep_q16, line_q16;
    uint64_t pred_q16;
    int y, nosync;
    int last_sync_quality;              /* 0..100, percent of the sync window at < 1350 Hz */
    uint16_t row[SSTV_MAX_W];
    sstv_line_fn on_line;
    sstv_event_fn on_event;
    void *ctx;
} sstv_rx;

void sstv_init(sstv_rx *rx, sstv_line_fn on_line, sstv_event_fn on_event, void *ctx);
void sstv_feed(sstv_rx *rx, const int16_t *x, int n);
int sstv_current_freq(const sstv_rx *rx);           /* Hz, the latest sample */
const sstv_mode *sstv_modes(int *count);

#endif
