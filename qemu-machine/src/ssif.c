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
 * What the fake DSP sends: each RX slot is a "source", named af (DX_REC L), mic (DX_REC R),
 * fmt (DX_FMT L) and fmt-r (DX_FMT R). A source plays a tone or a stimulus file, plus noise
 * at RZA1H_AF_NOISE (0.01 of full scale) whenever either is active. Env knobs, all optional:
 *   RZA1H_AF_TONE   "hz:level" RX audio tone (default "1000:0.25"; level is 0..1 of full
 *                   scale; "none" = silence). RZA1H_MIC_TONE, RZA1H_FMT_TONE and
 *                   RZA1H_FMT_R_TONE are the same for the other sources. fmt follows af unless
 *                   it's set itself; the others default to none.
 *   RZA1H_AF_FILE   "path[,gain=G][,delay=S][,loop]" stimulus file (Sun .au or WAV, 16-bit
 *                   PCM, any rate up to 96 kHz, first channel of several). While a file is set
 *                   it replaces the source's tone. It starts S seconds (default 0) of link time
 *                   after it's armed (boot, or the qom-set below) and plays once, or looped.
 *                   It is resampled to the 96 kHz frame rate with a windowed-sinc interpolator
 *                   (passband 0.45 of the file's rate). RZA1H_MIC_FILE, RZA1H_FMT_FILE and
 *                   RZA1H_FMT_R_FILE likewise; fmt follows af here too.
 * The same settings are QOM properties of /machine/ssif, settable at runtime over QMP:
 * "<source>-tone" and "<source>-file" (the same syntax; "none" clears a file, which brings the
 * tone back, and re-setting a file restarts it), plus a read-only "<source>-status"
 * ("tone", "waiting 1.0 s", "playing 12.3/77.6 s", "done", "off").
 * RTTY demodulator (the DSP's FSK demod, notes/dsp-protocol.md "RTTY receive path"): while
 * the fake DSP's mode (opcode 0x22) is RTTY (4) or RTTY-R (5), the fmt source's samples go
 * through mark/space tone detectors (complex mix to 0 Hz, 2-pole 60 Hz low-pass each) and the
 * stronger tone drives the "rtd" GPIO line = P8_7 (1 = mark; inverted for RTTY-R; mark when
 * both tones are below the squelch or in any other mode). The firmware samples it at 1 kHz
 * and does the UART framing and Baudot itself. Knobs: RZA1H_RTTY_MARK (2125 Hz),
 * RZA1H_RTTY_SHIFT (170 Hz), RZA1H_RTTY_SQUELCH (0.01 of full scale, tone amplitude).
 * The transitions land on DMA-block edges (0.75 ms), well inside a 22 ms bit.
 * What it receives: DR_AF L/R peak levels, logged once per second of emulated time when
 * non-zero (RZA1H_DEBUG=ssif).
 *
 * Registers modelled: SSICR (stored; TEN bit1 / REN bit0), SSISR (IIRQ, bit 25, reads 1 while
 * the channel is idle -- the stop path, FUN_200604c4, waits for it), SSIFRDR/SSIFTDR as above.
 * Everything else is plain storage. No SSIF interrupts: the firmware never uses them.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include <math.h>

#include "rz_a1h.h"
#include "rza1h_debug.h"
#include "fake_dsp.h"

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

#define SSIF_SOURCES (SSIF_CHANNELS * 2)
#define RS_TAPS   16             /* resampler kernel length, in input samples */
#define RS_PHASES 256            /* fractional positions the kernel is tabulated at */

typedef struct SsifTone {
    double hz, level, phase;
} SsifTone;

/* A stimulus file, decoded to mono float PCM. */
typedef struct SsifFile {
    char *spec;
    float *pcm;
    uint32_t n, rate;
    double gain;
    bool loop, started, done;
    uint64_t start;              /* source frame it starts at */
} SsifFile;

/* One RX slot's content (index = channel * 2 + slot). Survives device reset. */
typedef struct SsifSource {
    const char *name;
    char *tone_spec;
    SsifTone tone;
    SsifFile *file;
    uint64_t frames;             /* words read from this slot since reset */
} SsifSource;

typedef struct SsifChannel {
    uint8_t regs[SSIF_CH_STRIDE];
    uint64_t rx_slot, tx_slot;   /* slot counters, even = L, odd = R */
    double noise;
    uint32_t rng;
    int32_t tx_peak[2];
    uint64_t tx_words, tx_log_at;
    bool tx_seen;
} SsifChannel;

/* Mark/space tone detector (see the file comment). */
typedef struct SsifFskTone {
    double c, s;                 /* phasor e^{-j w n} */
    double dc, ds;               /* its per-frame rotation */
    double i1, q1, i2, q2;       /* two one-pole stages of the mixed product */
} SsifFskTone;

typedef struct SsifFsk {
    SsifFskTone mark, space;
    double alpha, squelch2;
    unsigned renorm;
    int level;                   /* last level driven on rtd, -1 = not yet */
    uint64_t edges;
} SsifFsk;

struct RZA1HSsifState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq rtd;
    SsifFsk fsk;
    SsifChannel ch[SSIF_CHANNELS];
    SsifSource src[SSIF_SOURCES];
    bool fmt_follows_tone, fmt_follows_file;
};

static const char *const ssif_source_names[SSIF_SOURCES] = { "af", "mic", "fmt", "fmt-r" };
static float rs_kernel[RS_PHASES + 1][RS_TAPS];

static void ssif_parse_tone(SsifTone *t, const char *spec)
{
    double hz = 0, level = 0;

    if (spec && strcmp(spec, "none") && sscanf(spec, "%lf:%lf", &hz, &level) == 2) {
        t->hz = hz;
        t->level = MIN(MAX(level, 0.0), 1.0);
    } else {
        t->hz = t->level = 0;
    }
    t->phase = 0;
}

static void ssif_set_tone(SsifSource *src, const char *spec)
{
    g_free(src->tone_spec);
    src->tone_spec = g_strdup(spec ? spec : "none");
    ssif_parse_tone(&src->tone, src->tone_spec);
}

/* Blackman-windowed sinc, cutoff 0.45 of the input rate, one row per fractional position. */
static void ssif_build_kernel(void)
{
    for (int p = 0; p <= RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES, sum = 0;

        for (int k = 0; k < RS_TAPS; k++) {
            double x = (k - (RS_TAPS / 2 - 1)) - frac, h = 0.9;
            double w = 0.42 + 0.5 * cos(M_PI * x / (RS_TAPS / 2))
                       + 0.08 * cos(2 * M_PI * x / (RS_TAPS / 2));

            if (x != 0) {
                h = sin(0.9 * M_PI * x) / (M_PI * x);
            }
            rs_kernel[p][k] = fabs(x) < RS_TAPS / 2 ? h * w : 0;
            sum += rs_kernel[p][k];
        }
        for (int k = 0; k < RS_TAPS; k++) {
            rs_kernel[p][k] /= sum;
        }
    }
}

static void ssif_file_free(SsifFile *f)
{
    if (f) {
        g_free(f->spec);
        g_free(f->pcm);
        g_free(f);
    }
}

/* Decode 16-bit PCM from a Sun .au or a RIFF WAV image into f->pcm (first channel). */
static bool ssif_file_decode(SsifFile *f, const uint8_t *d, size_t len, Error **errp)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    unsigned channels = 0, bits = 0;
    bool be = false;

    if (len >= 24 && !memcmp(d, ".snd", 4)) {
        uint32_t off = ldl_be_p(d + 4), sz = ldl_be_p(d + 8), enc = ldl_be_p(d + 12);

        if (enc != 3) {
            error_setg(errp, "au encoding %u: only 3 (16-bit linear PCM) is supported", enc);
            return false;
        }
        if (off > len) {
            error_setg(errp, "au data offset %u past end of file", off);
            return false;
        }
        f->rate = ldl_be_p(d + 16);
        channels = ldl_be_p(d + 20);
        bits = 16;
        be = true;
        data = d + off;
        size = MIN((size_t)sz, len - off);  /* sz 0xffffffff = unknown */
    } else if (len >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {
        size_t pos = 12;

        while (pos + 8 <= len) {
            uint32_t csz = ldl_le_p(d + pos + 4);
            const uint8_t *c = d + pos + 8;

            if (!memcmp(d + pos, "fmt ", 4) && csz >= 16 && pos + 8 + 16 <= len) {
                unsigned tag = lduw_le_p(c);

                if (tag != 1 && tag != 0xfffe) {
                    error_setg(errp, "WAV format tag 0x%x: only PCM is supported", tag);
                    return false;
                }
                channels = lduw_le_p(c + 2);
                f->rate = ldl_le_p(c + 4);
                bits = lduw_le_p(c + 14);
            } else if (!memcmp(d + pos, "data", 4)) {
                data = c;
                size = MIN((size_t)csz, len - (pos + 8));
                break;
            }
            pos += 8 + csz + (csz & 1);
        }
        if (!data || !channels) {
            error_setg(errp, "WAV without fmt/data chunks");
            return false;
        }
        if (bits != 16) {
            error_setg(errp, "WAV with %u-bit samples: only 16-bit is supported", bits);
            return false;
        }
    } else {
        error_setg(errp, "not a Sun .au or WAV file");
        return false;
    }
    if (!channels || !f->rate || f->rate > SSIF_FRAME_HZ) {
        error_setg(errp, "unsupported format: %u channels at %u Hz (max 96000 Hz)",
                   channels, f->rate);
        return false;
    }
    f->n = size / (2 * channels);
    if (!f->n) {
        error_setg(errp, "no samples");
        return false;
    }
    f->pcm = g_new(float, f->n);
    for (uint32_t i = 0; i < f->n; i++) {
        const uint8_t *p = data + (size_t)i * 2 * channels;
        f->pcm[i] = (int16_t)(be ? lduw_be_p(p) : lduw_le_p(p)) / 32768.0f;
    }
    return true;
}

/* Parse "path[,gain=G][,delay=S][,loop]" and load the file. */
static SsifFile *ssif_file_load(const char *spec, Error **errp)
{
    g_auto(GStrv) parts = g_strsplit(spec, ",", -1);
    g_autofree gchar *buf = NULL;
    gsize len;
    GError *gerr = NULL;
    SsifFile *f = g_new0(SsifFile, 1);
    double delay = 0;

    f->spec = g_strdup(spec);
    f->gain = 1.0;
    for (int i = 1; parts[i]; i++) {
        if (g_str_has_prefix(parts[i], "gain=")) {
            f->gain = atof(parts[i] + 5);
        } else if (g_str_has_prefix(parts[i], "delay=")) {
            delay = MAX(atof(parts[i] + 6), 0.0);
        } else if (!strcmp(parts[i], "loop")) {
            f->loop = true;
        } else {
            error_setg(errp, "%s: unknown option '%s' (gain=, delay=, loop)", spec, parts[i]);
            goto fail;
        }
    }
    if (!g_file_get_contents(parts[0], &buf, &len, &gerr)) {
        error_setg(errp, "%s", gerr->message);
        g_error_free(gerr);
        goto fail;
    }
    if (!ssif_file_decode(f, (const uint8_t *)buf, len, errp)) {
        error_prepend(errp, "%s: ", parts[0]);
        goto fail;
    }
    f->start = (uint64_t)(delay * SSIF_FRAME_HZ + 0.5);
    return f;

fail:
    ssif_file_free(f);
    return NULL;
}

/* Arm a new file on a source (NULL/"none" = clear), starting relative to its current frame. */
static bool ssif_set_file(SsifSource *src, const char *spec, Error **errp)
{
    SsifFile *f = NULL;

    if (spec && *spec && strcmp(spec, "none")) {
        f = ssif_file_load(spec, errp);
        if (!f) {
            return false;
        }
        f->start += src->frames;
        rza1h_debug("ssif", "%s: file %s, %u samples at %u Hz (%.1f s), gain %.2f%s, "
                    "starts in %.1f s", src->name, f->spec, f->n, f->rate,
                    (double)f->n / f->rate, f->gain, f->loop ? ", looped" : "",
                    (f->start - src->frames) / SSIF_FRAME_HZ);
    }
    ssif_file_free(src->file);
    src->file = f;
    return true;
}

static double ssif_file_at(SsifFile *f, int64_t i)
{
    if (f->loop) {
        i %= f->n;
        i += i < 0 ? f->n : 0;
    } else if (i < 0 || i >= f->n) {
        return 0;
    }
    return f->pcm[i];
}

/* The file's sample for this source frame (0 before it starts and after it ends). */
static double ssif_file_sample(SsifSource *src)
{
    SsifFile *f = src->file;
    uint64_t t, num, frac;
    int64_t i0;
    const float *kern;
    double x = 0;

    if (src->frames < f->start || f->done) {
        return 0;
    }
    t = src->frames - f->start;
    num = t * f->rate;
    if (f->loop) {
        num %= (uint64_t)f->n * (uint64_t)SSIF_FRAME_HZ;
    }
    i0 = num / (uint64_t)SSIF_FRAME_HZ;
    frac = num % (uint64_t)SSIF_FRAME_HZ;
    if (!f->loop && i0 >= (int64_t)f->n + RS_TAPS / 2) {
        f->done = true;
        rza1h_debug("ssif", "%s: file %s finished", src->name, f->spec);
        return 0;
    }
    if (!f->started) {
        f->started = true;
        rza1h_debug("ssif", "%s: file %s playing", src->name, f->spec);
    }
    if (f->rate == (uint32_t)SSIF_FRAME_HZ) {
        return f->gain * ssif_file_at(f, i0);
    }
    kern = rs_kernel[(frac * RS_PHASES + (uint64_t)SSIF_FRAME_HZ / 2) / (uint64_t)SSIF_FRAME_HZ];
    for (int k = 0; k < RS_TAPS; k++) {
        x += kern[k] * ssif_file_at(f, i0 - (RS_TAPS / 2 - 1) + k);
    }
    return f->gain * x;
}

static void ssif_fsk_tone_init(SsifFskTone *t, double hz)
{
    memset(t, 0, sizeof(*t));
    t->c = 1;
    t->dc = cos(2 * M_PI * hz / SSIF_FRAME_HZ);
    t->ds = -sin(2 * M_PI * hz / SSIF_FRAME_HZ);
}

static void ssif_fsk_init(SsifFsk *f)
{
    const char *e;
    double mark = 2125, shift = 170, sq = 0.01;

    if ((e = getenv("RZA1H_RTTY_MARK"))) {
        mark = atof(e);
    }
    if ((e = getenv("RZA1H_RTTY_SHIFT"))) {
        shift = atof(e);
    }
    if ((e = getenv("RZA1H_RTTY_SQUELCH"))) {
        sq = atof(e);
    }
    ssif_fsk_tone_init(&f->mark, mark);
    ssif_fsk_tone_init(&f->space, mark + shift);
    f->alpha = 1 - exp(-2 * M_PI * 60 / SSIF_FRAME_HZ);
    f->squelch2 = sq * sq / 4;   /* |mixed|^2 of a tone of amplitude sq is (sq/2)^2 */
    f->renorm = 0;
    f->level = -1;
    f->edges = 0;
}

/* One tone's detector step; returns |lowpassed mix|^2. */
static double ssif_fsk_tone_step(SsifFskTone *t, double x, double a)
{
    double c = t->c * t->dc - t->s * t->ds;

    t->s = t->c * t->ds + t->s * t->dc;
    t->c = c;
    t->i1 += a * (x * t->c - t->i1);
    t->q1 += a * (x * t->s - t->q1);
    t->i2 += a * (t->i1 - t->i2);
    t->q2 += a * (t->q1 - t->q2);
    return t->i2 * t->i2 + t->q2 * t->q2;
}

/* One DX_FMT L sample through the RTTY demodulator; drives rtd on a change. */
static void ssif_fsk_sample(RZA1HSsifState *s, double x)
{
    SsifFsk *f = &s->fsk;
    int level = 1;

    if (fake_dsp_mode == 4 || fake_dsp_mode == 5) {
        double m = ssif_fsk_tone_step(&f->mark, x, f->alpha);
        double sp = ssif_fsk_tone_step(&f->space, x, f->alpha);

        if (++f->renorm >= 4096) {   /* keep the phasors on the unit circle */
            for (SsifFskTone *t = &f->mark; t <= &f->space; t++) {
                double r = 1 / hypot(t->c, t->s);
                t->c *= r;
                t->s *= r;
            }
            f->renorm = 0;
        }
        if (MAX(m, sp) >= f->squelch2) {
            level = (m >= sp) ^ (fake_dsp_mode == 5);
        }
    }
    if (level != f->level) {
        if (f->level >= 0) {
            f->edges++;
        }
        f->level = level;
        qemu_set_irq(s->rtd, level);
    }
}

/* Next 24-bit left-justified word for this channel's RX slot. */
static uint32_t ssif_rx_word(RZA1HSsifState *s, int n)
{
    SsifChannel *c = &s->ch[n];
    SsifSource *src = &s->src[n * 2 + (c->rx_slot & 1)];
    SsifTone *t = &src->tone;
    bool active = false;
    double x = 0;

    if (src->file) {
        x = ssif_file_sample(src);
        active = true;
    } else if (t->level > 0) {
        x = t->level * sin(t->phase);
        t->phase += 2 * M_PI * t->hz / SSIF_FRAME_HZ;
        if (t->phase > 2 * M_PI) {
            t->phase -= 2 * M_PI;
        }
        active = true;
    }
    if (active) {
        c->rng = c->rng * 1103515245u + 12345u;
        x += c->noise * (((c->rng >> 8) & 0xffff) / 32768.0 - 1.0);
    }
    src->frames++;
    c->rx_slot++;
    x = MIN(MAX(x, -0.999999), 0.999999);
    if (src == &s->src[2]) {
        ssif_fsk_sample(s, x);
    }
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
        return ssif_rx_word(s, offset / SSIF_CH_STRIDE);
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

    memset(s->ch, 0, sizeof(s->ch));
    for (int n = 0; n < SSIF_CHANNELS; n++) {
        s->ch[n].rng = 0x7300 + n;
        s->ch[n].noise = 0.01;
    }
    if (getenv("RZA1H_AF_NOISE")) {
        s->ch[0].noise = s->ch[1].noise = atof(getenv("RZA1H_AF_NOISE"));
    }
    ssif_fsk_init(&s->fsk);
    /* Rewind the sources; their tone and file settings survive the reset. */
    for (int i = 0; i < SSIF_SOURCES; i++) {
        SsifSource *src = &s->src[i];

        if (src->file) {
            src->file->start -= MIN(src->file->start, src->frames);
            src->file->started = src->file->done = false;
        }
        src->frames = 0;
        src->tone.phase = 0;
    }
    rza1h_debug("ssif", "reset: DX_REC L %.0f Hz @%.2f, R %.0f Hz @%.2f; DX_FMT L %.0f Hz @%.2f",
                s->src[0].tone.hz, s->src[0].tone.level, s->src[1].tone.hz,
                s->src[1].tone.level, s->src[2].tone.hz, s->src[2].tone.level);
}

/* QOM properties: <source>-tone, <source>-file (read/write), <source>-status (read-only). The
 * opaque is the SsifSource; its index finds the device, so fmt can follow af. */
static RZA1HSsifState *ssif_of(Object *obj)
{
    return RZA1H_SSIF(obj);
}

static char *ssif_get_tone(Object *obj, Error **errp, SsifSource *src)
{
    return g_strdup(src->tone_spec);
}

static char *ssif_get_file(Object *obj, Error **errp, SsifSource *src)
{
    return g_strdup(src->file ? src->file->spec : "none");
}

static char *ssif_get_status(Object *obj, Error **errp, SsifSource *src)
{
    SsifFile *f = src->file;

    if (!f) {
        return g_strdup(src->tone.level > 0 ? "tone" : "off");
    }
    if (f->done) {
        return g_strdup("done");
    }
    if (src->frames < f->start) {
        return g_strdup_printf("waiting %.1f s", (f->start - src->frames) / SSIF_FRAME_HZ);
    }
    return g_strdup_printf("playing %.1f/%.1f s%s",
                           (src->frames - f->start) / SSIF_FRAME_HZ, (double)f->n / f->rate,
                           f->loop ? " (looped)" : "");
}

#define SSIF_PROP_ACCESSORS(i)                                                               \
    static char *ssif_get_tone_##i(Object *o, Error **e)                                     \
    { return ssif_get_tone(o, e, &ssif_of(o)->src[i]); }                                     \
    static char *ssif_get_file_##i(Object *o, Error **e)                                     \
    { return ssif_get_file(o, e, &ssif_of(o)->src[i]); }                                     \
    static char *ssif_get_status_##i(Object *o, Error **e)                                   \
    { return ssif_get_status(o, e, &ssif_of(o)->src[i]); }                                   \
    static void ssif_set_tone_##i(Object *o, const char *v, Error **e)                       \
    {                                                                                        \
        RZA1HSsifState *s = ssif_of(o);                                                      \
        ssif_set_tone(&s->src[i], v);                                                        \
        if (i == 0 && s->fmt_follows_tone) {                                                 \
            ssif_set_tone(&s->src[2], v);                                                    \
        } else if (i == 2) {                                                                 \
            s->fmt_follows_tone = false;                                                     \
        }                                                                                    \
    }                                                                                        \
    static void ssif_set_file_##i(Object *o, const char *v, Error **e)                       \
    {                                                                                        \
        RZA1HSsifState *s = ssif_of(o);                                                      \
        if (ssif_set_file(&s->src[i], v, e)) {                                               \
            if (i == 0 && s->fmt_follows_file) {                                             \
                ssif_set_file(&s->src[2], v, &error_abort);                                  \
            } else if (i == 2) {                                                             \
                s->fmt_follows_file = false;                                                 \
            }                                                                                \
        }                                                                                    \
    }
SSIF_PROP_ACCESSORS(0)
SSIF_PROP_ACCESSORS(1)
SSIF_PROP_ACCESSORS(2)
SSIF_PROP_ACCESSORS(3)

static void ssif_add_props(Object *obj, int i, char *(*gt)(Object *, Error **),
                           void (*st)(Object *, const char *, Error **),
                           char *(*gf)(Object *, Error **),
                           void (*sf)(Object *, const char *, Error **),
                           char *(*gs)(Object *, Error **))
{
    g_autofree char *tone = g_strdup_printf("%s-tone", ssif_source_names[i]);
    g_autofree char *file = g_strdup_printf("%s-file", ssif_source_names[i]);
    g_autofree char *status = g_strdup_printf("%s-status", ssif_source_names[i]);

    object_property_add_str(obj, tone, gt, st);
    object_property_add_str(obj, file, gf, sf);
    object_property_add_str(obj, status, gs, NULL);
}

/* Env settings are read once, at realize: a device reset must not undo a runtime qom-set. */
static void rza1h_ssif_realize(DeviceState *dev, Error **errp)
{
    static const char *const tone_env[SSIF_SOURCES] = {
        "RZA1H_AF_TONE", "RZA1H_MIC_TONE", "RZA1H_FMT_TONE", "RZA1H_FMT_R_TONE" };
    static const char *const file_env[SSIF_SOURCES] = {
        "RZA1H_AF_FILE", "RZA1H_MIC_FILE", "RZA1H_FMT_FILE", "RZA1H_FMT_R_FILE" };
    RZA1HSsifState *s = RZA1H_SSIF(dev);
    const char *af_tone = getenv(tone_env[0]) ? getenv(tone_env[0]) : "1000:0.25";

    s->fmt_follows_tone = !getenv(tone_env[2]);
    s->fmt_follows_file = !getenv(file_env[2]);
    for (int i = 0; i < SSIF_SOURCES; i++) {
        const char *tone = getenv(tone_env[i]);
        const char *file = getenv(file_env[i]);

        if (i == 0 || (i == 2 && s->fmt_follows_tone)) {
            tone = af_tone;
        }
        if (i == 2 && s->fmt_follows_file) {
            file = getenv(file_env[0]);
        }
        ssif_set_tone(&s->src[i], tone);
        if (file && !ssif_set_file(&s->src[i], file, errp)) {
            error_prepend(errp, "%s: ", file_env[i]);
            return;
        }
    }
}

static void rza1h_ssif_init(Object *obj)
{
    RZA1HSsifState *s = RZA1H_SSIF(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_ssif_ops, s, TYPE_RZA1H_SSIF,
                          RZA1H_SSIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (int i = 0; i < SSIF_SOURCES; i++) {
        s->src[i].name = ssif_source_names[i];
    }
    qdev_init_gpio_out_named(DEVICE(obj), &s->rtd, "rtd", 1);
    /* RTTY demodulator activity: RTD level changes since reset (read-only) */
    object_property_add_uint64_ptr(obj, "rtd-edges", &s->fsk.edges, OBJ_PROP_FLAG_READ);
    ssif_add_props(obj, 0, ssif_get_tone_0, ssif_set_tone_0, ssif_get_file_0, ssif_set_file_0,
                   ssif_get_status_0);
    ssif_add_props(obj, 1, ssif_get_tone_1, ssif_set_tone_1, ssif_get_file_1, ssif_set_file_1,
                   ssif_get_status_1);
    ssif_add_props(obj, 2, ssif_get_tone_2, ssif_set_tone_2, ssif_get_file_2, ssif_set_file_2,
                   ssif_get_status_2);
    ssif_add_props(obj, 3, ssif_get_tone_3, ssif_set_tone_3, ssif_get_file_3, ssif_set_file_3,
                   ssif_get_status_3);
}

static void rza1h_ssif_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_ssif_realize;
    device_class_set_legacy_reset(dc, rza1h_ssif_reset);
    ssif_build_kernel();
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
