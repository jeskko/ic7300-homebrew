/* RX audio capture (hb/audio.h): the loader's audio_hook runs in the tick ISR and fills a
 * single-producer/single-consumer ring; the app drains it from its coroutine. */
#include "hb/audio.h"
#include "runtime_internal.h"

#define RING 16384u                     /* power of two */

/* OSTM0 CNT: free-running up-counter at P0phi = 32 MHz (qemu-machine/src/ostm.c). A block
 * normally arrives every 0.75 ms = 24000 counts; the pump polls from the 250 us tick, so the
 * spacing jitters by up to 8000 counts either way. */
#define OSTM0_CNT       (*(volatile uint32_t *)0xFCFEC004u)
#define BLOCK_COUNTS    24000u
#define MAX_FILL        4u              /* longer gaps: a restart, not lost blocks */

static int16_t g_ring[RING];
static volatile uint32_t g_w, g_r;      /* free-running; g_w written only by the ISR */
static volatile uint32_t g_overruns, g_gaps;
static uint32_t g_expect;               /* earliest time the next block can arrive */
static bool g_have_last;
static int16_t g_last_sample;

static void put(uint32_t *w, int16_t v)
{
    if (*w - g_r >= RING) {             /* full: drop this sample, the reader is too slow */
        g_overruns++;
        return;
    }
    g_ring[*w & (RING - 1)] = v;
    (*w)++;
}

/* 36 samples at 48 kHz -> 9 at 12 kHz. ISR context: no firmware calls, no floating point.
 *
 * Blocks the firmware's pump never saw (its DMA ping-pong was overwritten before it ran;
 * the emulator loses ~0.1-0.4% this way) would shift everything after them by 9 samples.
 * So the ISR tracks when each block can arrive at the earliest: arrivals come 0..0.75 ms
 * after that (the pump polls, and interrupts can be late); a block that was never seen pushes
 * the next arrival a whole block later. Every missing block is filled with 9 samples
 * interpolated between its neighbours, keeping the timeline intact. Comparing only with the
 * previous arrival instead gives false fills (an early pump run followed by a late one). */
static void audio_isr(const int16_t *s, uint32_t n)
{
    uint32_t w = g_w, now = OSTM0_CNT;
    int16_t first = (int16_t)(((int32_t)s[0] + s[1] + s[2] + s[3]) >> 2);
    if (!g_have_last) {
        g_expect = now;
    } else {
        g_expect += BLOCK_COUNTS;
        int32_t late = (int32_t)(now - g_expect);
        if (late < 0) {                 /* earlier than ever: that's the new lower edge */
            g_expect = now;
            late = 0;
        } else if (late >= (int32_t)BLOCK_COUNTS) {
            uint32_t missing = (uint32_t)late / BLOCK_COUNTS;
            if (missing <= MAX_FILL) {
                uint32_t fill = missing * (n / 4);
                for (uint32_t k = 1; k <= fill; k++)
                    put(&w, (int16_t)(g_last_sample + ((int32_t)(first - g_last_sample)
                                                       * (int32_t)k) / (int32_t)(fill + 1)));
                g_gaps += missing;
                g_expect += missing * BLOCK_COUNTS;
                late -= (int32_t)(missing * BLOCK_COUNTS);
            } else {                    /* a long stall: resynchronise, don't invent audio */
                g_expect = now;
                late = 0;
            }
        }
        g_expect += (uint32_t)(late >> 10);     /* follow slow drift between the two clocks */
    }
    g_have_last = true;
    for (uint32_t i = 0; i + 4 <= n; i += 4)
        put(&w, (int16_t)(((int32_t)s[i] + s[i + 1] + s[i + 2] + s[i + 3]) >> 2));
    g_last_sample = (int16_t)(((int32_t)s[n - 4] + s[n - 3] + s[n - 2] + s[n - 1]) >> 2);
    __asm__ volatile("dmb" ::: "memory");   /* samples before the index */
    g_w = w;
}

bool hb_audio_open(void)
{
    struct hb_loader_api *api = hb__runtime_api();
    if (!api || api->abi_version < 4)
        return false;
    g_r = g_w;
    g_overruns = 0;
    g_gaps = 0;
    g_have_last = false;
    api->audio_hook = audio_isr;
    return true;
}

void hb_audio_close(void)
{
    struct hb_loader_api *api = hb__runtime_api();
    if (api)
        api->audio_hook = 0;
}

int hb_audio_read(int16_t *buf, int max)
{
    uint32_t w = g_w, r = g_r;
    __asm__ volatile("dmb" ::: "memory");   /* index before the samples */
    int n = 0;
    while (r != w && n < max)
        buf[n++] = g_ring[r++ & (RING - 1)];
    g_r = r;
    return n;
}

uint32_t hb_audio_overruns(void)
{
    return g_overruns;
}

uint32_t hb_audio_gaps(void)
{
    return g_gaps;
}
