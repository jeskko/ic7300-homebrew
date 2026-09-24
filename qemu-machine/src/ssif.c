/*
 * RZ/A1H SSIF (Serial Sound Interface) channels 0 and 1 -- the CPU end of the CPU <-> DSP audio
 * link, plus the fake DSP's side of that link's content. Spec: notes/dsp-protocol.md, "The
 * CPU <-> DSP audio link, both directions" (2026-09-24).
 *
 * The link is I2S, two 32-bit slots, 24-bit left-justified data, 96 kHz frames clocked by the
 * FPGA (gpio.c drives the word-select pins the bring-up counts). The firmware moves every word
 * by DMA (dmac.c: ch3 SSIF0 RX, ch4 SSIF0 TX, ch5 SSIF1 RX, register-set ping-pong), so the data
 * registers are only ever touched by the DMAC:
 *   SSIFRDR_0  DX_REC  L = RX audio (QSO recorder, audio FFT), R = mic/TX audio
 *   SSIFRDR_1  DX_FMT  L = per-mode demod output (decoders), R = 0
 *   SSIFTDR_0  DR_AF   L = playback to the speaker, R = audio to the transmitter
 *   (SSIF1 TX = DR_RSV is unused by the firmware.)
 * Each data-register access is one slot, alternating L, R.
 *
 * What the fake DSP sends (env knobs, all optional):
 *   RZA1H_AF_TONE   "hz:level" RX audio tone on DX_REC L (default "1000:0.25"; level is
 *                   0..1 of full scale; "none" = silence), plus noise at RZA1H_AF_NOISE (0.01)
 *   RZA1H_MIC_TONE  same, for the mic on DX_REC R (default none)
 *   RZA1H_FMT_TONE  same, for DX_FMT L (default: follows RZA1H_AF_TONE)
 * What it receives: DR_AF L/R peak levels, logged once per second of emulated time when
 * non-zero (RZA1H_DEBUG=ssif).
 *
 * Registers modelled: SSICR (stored; TEN bit1 / REN bit0), SSISR (IIRQ, bit 25, reads 1 while
 * the channel is idle -- the stop path, FUN_200604c4, waits for it), SSIFRDR/SSIFTDR as above.
 * Everything else is plain storage. No SSIF interrupts: the firmware never uses them.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include <math.h>

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HSsifState, RZA1H_SSIF)

#define SSIF_CH_STRIDE 0x800
#define SSIF_CHANNELS  2
#define SSIF_SSICR   0x00
#define SSIF_SSISR   0x04
#define SSIF_SSIFTDR 0x18
#define SSIF_SSIFRDR 0x1c
#define SSICR_TEN (1u << 1)
#define SSICR_REN (1u << 0)
#define SSISR_IIRQ (1u << 25)
#define SSIF_FRAME_HZ 96000.0

typedef struct SsifTone {
    double hz, level, phase;
} SsifTone;

typedef struct SsifChannel {
    uint8_t regs[SSIF_CH_STRIDE];
    uint64_t rx_slot, tx_slot;   /* slot counters, even = L, odd = R */
    SsifTone tone[2];            /* per RX slot */
    double noise;
    uint32_t rng;
    int32_t tx_peak[2];
    uint64_t tx_words, tx_log_at;
    bool tx_seen;
} SsifChannel;

struct RZA1HSsifState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SsifChannel ch[SSIF_CHANNELS];
};

static void ssif_parse_tone(SsifTone *t, const char *env, const char *dflt)
{
    const char *e = getenv(env);
    double hz = 0, level = 0;

    if (!e) {
        e = dflt;
    }
    if (e && strcmp(e, "none") && sscanf(e, "%lf:%lf", &hz, &level) == 2) {
        t->hz = hz;
        t->level = MIN(MAX(level, 0.0), 1.0);
    } else {
        t->hz = t->level = 0;
    }
    t->phase = 0;
}

/* Next 24-bit left-justified word for this channel's RX slot. */
static uint32_t ssif_rx_word(SsifChannel *c)
{
    SsifTone *t = &c->tone[c->rx_slot & 1];
    double x = 0;

    if (t->level > 0) {
        x = t->level * sin(t->phase);
        t->phase += 2 * M_PI * t->hz / SSIF_FRAME_HZ;
        if (t->phase > 2 * M_PI) {
            t->phase -= 2 * M_PI;
        }
        c->rng = c->rng * 1103515245u + 12345u;
        x += c->noise * (((c->rng >> 8) & 0xffff) / 32768.0 - 1.0);
    }
    c->rx_slot++;
    x = MIN(MAX(x, -0.999999), 0.999999);
    return (uint32_t)(int32_t)(x * 2147483392.0) & 0xffffff00u;
}

static void ssif_tx_word(SsifChannel *c, int n, uint32_t w)
{
    int slot = c->tx_slot++ & 1;
    int32_t v = abs((int32_t)w >> 16);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    c->tx_peak[slot] = MAX(c->tx_peak[slot], v);
    c->tx_words++;
    if (!c->tx_seen) {
        c->tx_seen = true;
        rza1h_debug("ssif", "SSIF%d TX: first word from the CPU (%08x)", n, w);
    }
    if (now >= c->tx_log_at) {
        if (c->tx_peak[0] || c->tx_peak[1]) {
            rza1h_debug("ssif", "SSIF%d TX (%s) last 1 s: peak L %d R %d (of 32767), %" PRIu64
                        " words", n, n ? "DR_RSV" : "DR_AF", c->tx_peak[0], c->tx_peak[1],
                        c->tx_words);
        }
        c->tx_peak[0] = c->tx_peak[1] = 0;
        c->tx_words = 0;
        c->tx_log_at = now + NANOSECONDS_PER_SECOND;
    }
}

static uint64_t rza1h_ssif_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HSsifState *s = RZA1H_SSIF(opaque);
    SsifChannel *c = &s->ch[offset / SSIF_CH_STRIDE];
    hwaddr off = offset % SSIF_CH_STRIDE;
    uint64_t val = 0;

    if (off == SSIF_SSIFRDR) {
        return ssif_rx_word(c);
    }
    if (off == SSIF_SSISR) {
        uint32_t cr;
        memcpy(&cr, &c->regs[SSIF_SSICR], 4);
        return (cr & (SSICR_TEN | SSICR_REN)) ? 0 : SSISR_IIRQ;
    }
    memcpy(&val, &c->regs[off], size);
    return val;
}

static void rza1h_ssif_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    RZA1HSsifState *s = RZA1H_SSIF(opaque);
    int n = offset / SSIF_CH_STRIDE;
    SsifChannel *c = &s->ch[n];
    hwaddr off = offset % SSIF_CH_STRIDE;

    if (off == SSIF_SSIFTDR) {
        ssif_tx_word(c, n, value);
        return;
    }
    if (off == SSIF_SSICR) {
        uint32_t old;
        memcpy(&old, &c->regs[SSIF_SSICR], 4);
        if ((old ^ value) & (SSICR_TEN | SSICR_REN)) {
            rza1h_debug("ssif", "SSIF%d SSICR=%08x (TEN=%d REN=%d)", n, (uint32_t)value,
                        !!(value & SSICR_TEN), !!(value & SSICR_REN));
            c->rx_slot = c->tx_slot = 0;
        }
    }
    memcpy(&c->regs[off], &value, size);
}

static const MemoryRegionOps rza1h_ssif_ops = {
    .read = rza1h_ssif_read,
    .write = rza1h_ssif_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_ssif_reset(DeviceState *dev)
{
    RZA1HSsifState *s = RZA1H_SSIF(dev);
    const char *e = getenv("RZA1H_AF_TONE");

    memset(s->ch, 0, sizeof(s->ch));
    for (int n = 0; n < SSIF_CHANNELS; n++) {
        s->ch[n].rng = 0x7300 + n;
        s->ch[n].noise = 0.01;
    }
    if (getenv("RZA1H_AF_NOISE")) {
        s->ch[0].noise = s->ch[1].noise = atof(getenv("RZA1H_AF_NOISE"));
    }
    ssif_parse_tone(&s->ch[0].tone[0], "RZA1H_AF_TONE", "1000:0.25");
    ssif_parse_tone(&s->ch[0].tone[1], "RZA1H_MIC_TONE", NULL);
    ssif_parse_tone(&s->ch[1].tone[0], "RZA1H_FMT_TONE", e ? e : "1000:0.25");
    ssif_parse_tone(&s->ch[1].tone[1], "RZA1H_FMT_R_TONE", NULL);
    rza1h_debug("ssif", "reset: DX_REC L %.0f Hz @%.2f, R %.0f Hz @%.2f; DX_FMT L %.0f Hz @%.2f",
                s->ch[0].tone[0].hz, s->ch[0].tone[0].level, s->ch[0].tone[1].hz,
                s->ch[0].tone[1].level, s->ch[1].tone[0].hz, s->ch[1].tone[0].level);
}

static void rza1h_ssif_init(Object *obj)
{
    RZA1HSsifState *s = RZA1H_SSIF(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_ssif_ops, s, TYPE_RZA1H_SSIF,
                          RZA1H_SSIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void rza1h_ssif_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), rza1h_ssif_reset);
}

static const TypeInfo rza1h_ssif_info = {
    .name          = TYPE_RZA1H_SSIF,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HSsifState),
    .instance_init = rza1h_ssif_init,
    .class_init    = rza1h_ssif_class_init,
};

static void rza1h_ssif_register_types(void)
{
    type_register_static(&rza1h_ssif_info);
}

type_init(rza1h_ssif_register_types)
