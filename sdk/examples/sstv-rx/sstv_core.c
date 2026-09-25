/* Streaming SSTV decoder core -- see sstv_core.h. Integer-only. */
#include "sstv_core.h"
#include "sstv_tables.h"

#define MASK            (SSTV_RING - 1)
#define MS(x)           ((uint32_t)((x) * SSTV_FS / 1000))
#define LEAD_HZ         1900
#define SYNC_HZ         1200
#define SYNC_BELOW      1350            /* "sync" = frequency below this */
#define TONE_TOL        100
#define SYNC_WINDOW     MS(20)          /* re-lock search, +- around the prediction */
#define MAX_NOSYNC      30

/* Timings from slowrx's mode table (seconds there, ns here). */
static const sstv_mode g_modes[] = {
    { 60, "Scottie 1",  SSTV_SCOTTIE, 9000000, 1500000, 1500000,  432000,  428380000, 320, 256 },
    { 56, "Scottie 2",  SSTV_SCOTTIE, 9000000, 1500000, 1500000,  275200,  277692000, 320, 256 },
    { 76, "Scottie DX", SSTV_SCOTTIE, 9000000, 1500000, 1500000, 1080530, 1050300000, 320, 256 },
    { 44, "Martin 1",   SSTV_MARTIN,  4862000,  572000,  572000,  457600,  446446000, 320, 256 },
    { 40, "Martin 2",   SSTV_MARTIN,  4862000,  572000,  572000,  228800,  226798600, 320, 256 },
};

const sstv_mode *sstv_modes(int *count)
{
    *count = (int)(sizeof g_modes / sizeof g_modes[0]);
    return g_modes;
}

/* ns -> samples, Q16 */
static uint32_t ns_q16(uint32_t ns)
{
    return (uint32_t)(((uint64_t)ns * SSTV_FS * 65536 + 500000000u) / 1000000000u);
}

void sstv_init(sstv_rx *rx, sstv_line_fn on_line, sstv_event_fn on_event, void *ctx)
{
    uint8_t *p = (uint8_t *)rx;
    for (unsigned i = 0; i < sizeof *rx; i++)
        p[i] = 0;
    rx->on_line = on_line;
    rx->on_event = on_event;
    rx->ctx = ctx;
    rx->state = SSTV_HUNT;
    for (int i = 0; i < SSTV_RING; i++)
        rx->freq[i] = LEAD_HZ - 1000;   /* "nothing", far from every tone */
}

/* ---- discriminator ------------------------------------------------------------------------ */

/* atan2(y, x) in binary angle units (a full turn = 65536), by CORDIC vectoring. */
static int32_t cordic_atan2(int32_t y, int32_t x)
{
    int32_t a = 0;
    if (x < 0) {                        /* rotate by 180 degrees into the right half-plane */
        x = -x;
        y = -y;
        a = 32768;
    }
    for (int i = 0; i < SSTV_CORDIC_ITERS; i++) {
        int32_t xi = x, yi = y;
        if (y > 0) {
            x = xi + (yi >> i);
            y = yi - (xi >> i);
            a += sstv_cordic_atan[i];
        } else {
            x = xi - (yi >> i);
            y = yi + (xi >> i);
            a -= sstv_cordic_atan[i];
        }
    }
    a &= 0xffff;
    return a >= 32768 ? a - 65536 : a;
}

static int discriminate(sstv_rx *rx, int16_t x)
{
    uint32_t idx = rx->nco >> 22;
    int32_t s = sstv_sin1024[idx], c = sstv_sin1024[(idx + 256) & 1023];
    rx->nco += SSTV_MIX_STEP;

    int p = rx->fpos;
    rx->fi[p] = rx->fi[p + SSTV_FIR_TAPS] = (int16_t)((x * c) >> 15);
    rx->fq[p] = rx->fq[p + SSTV_FIR_TAPS] = (int16_t)((-x * s) >> 15);
    rx->fpos = p + 1 == SSTV_FIR_TAPS ? 0 : p + 1;

    int32_t zi = 0, zq = 0;
    const int16_t *bi = &rx->fi[rx->fpos], *bq = &rx->fq[rx->fpos];
    for (int k = 0; k < SSTV_FIR_TAPS; k++) {
        zi += bi[k] * sstv_fir[k];
        zq += bq[k] * sstv_fir[k];
    }
    zi >>= 15;
    zq >>= 15;

    /* phase advance = arg(z[n] * conj(z[n-1])) */
    int64_t re = (int64_t)zi * rx->pi + (int64_t)zq * rx->pq;
    int64_t im = (int64_t)zq * rx->pi - (int64_t)zi * rx->pq;
    rx->pi = zi;
    rx->pq = zq;
    while (re > (1 << 28) || re < -(1 << 28) || im > (1 << 28) || im < -(1 << 28)) {
        re >>= 1;
        im >>= 1;
    }
    int32_t f = LEAD_HZ + (cordic_atan2((int32_t)im, (int32_t)re) * SSTV_FS) / 65536;

    int32_t out = (f + rx->sm1 + rx->sm2) / 3;
    rx->sm2 = rx->sm1;
    rx->sm1 = f;
    return out;
}

/* ---- helpers on the frequency track ------------------------------------------------------- */

static inline int fr(const sstv_rx *rx, uint32_t t)
{
    return rx->freq[t & MASK];
}

/* Frequency at a Q16 sample position, linearly interpolated. */
static int fr_q16(const sstv_rx *rx, uint64_t t)
{
    uint32_t i = (uint32_t)(t >> 16), f = (uint32_t)(t & 0xffff);
    int a = fr(rx, i), b = fr(rx, i + 1);
    return a + (int)(((int64_t)(b - a) * f) >> 16);
}

static int mean(const sstv_rx *rx, uint32_t from, uint32_t n)
{
    int32_t s = 0;
    for (uint32_t k = 0; k < n; k++)
        s += fr(rx, from + k);
    return (int)(s / (int32_t)n);
}

static inline int near(int f, int hz)
{
    return f > hz - TONE_TOL && f < hz + TONE_TOL;
}

static uint8_t lum(int f)
{
    int v = (f - 1500) * 255 / 800;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* ---- decoder states ---------------------------------------------------------------------- */

static void start_image(sstv_rx *rx, const sstv_mode *m)
{
    rx->mode = m;
    rx->pix_q16 = ns_q16(m->pixel_ns);
    rx->chan_q16 = rx->pix_q16 * m->w;
    rx->sync_s = ns_q16(m->sync_ns) >> 16;
    rx->porch_q16 = ns_q16(m->porch_ns);
    rx->sep_q16 = ns_q16(m->sep_ns);
    rx->line_q16 = ns_q16(m->line_ns);
    uint64_t visend = (uint64_t)(rx->vis_start + MS(300)) << 16;
    if (m->family == SSTV_SCOTTIE)      /* sep G sep B [sync] porch R */
        rx->pred_q16 = visend + 2 * (uint64_t)rx->sep_q16 + 2 * (uint64_t)rx->chan_q16
                       + ns_q16(m->sync_ns);
    else                                /* [sync] porch G sep B sep R sep */
        rx->pred_q16 = visend + ns_q16(m->sync_ns);
    rx->pred = (uint32_t)(rx->pred_q16 >> 16);
    rx->y = 0;
    rx->nosync = 0;
    rx->state = SSTV_LINE;
    if (rx->on_event)
        rx->on_event(rx->ctx, SSTV_EV_VIS, m, m->vis);
}

static void hunt(sstv_rx *rx, int f)
{
    rx->run1200 = near(f, SYNC_HZ) ? rx->run1200 + 1 : 0;
    if (rx->run1200 != MS(25))
        return;
    /* 25 ms of 1200 Hz: a VIS start bit, if >= 90% of the 200 ms before it was leader */
    uint32_t start = rx->n - MS(25) + 1, lead = 0;
    if (start < MS(220))
        return;
    for (uint32_t t = start - MS(220); t < start - MS(20); t++)
        lead += near(fr(rx, t), LEAD_HZ);
    if (lead * 10 >= MS(200) * 9) {
        rx->vis_start = start;
        rx->state = SSTV_VIS;
    }
}

static void vis(sstv_rx *rx)
{
    if (rx->n < rx->vis_start + MS(300))
        return;
    int code = 0, ones = 0;
    for (int b = 0; b < 8; b++) {       /* 7 data bits LSB first, then even parity */
        uint32_t c = rx->vis_start + MS(30) * (uint32_t)(b + 1) + MS(15);
        int bit = mean(rx, c - MS(5), MS(10)) < SYNC_HZ;   /* 1100 Hz = 1, 1300 Hz = 0 */
        ones += bit;
        if (b < 7)
            code |= bit << b;
    }
    rx->state = SSTV_HUNT;
    rx->run1200 = 0;
    if (ones & 1) {
        if (rx->on_event)
            rx->on_event(rx->ctx, SSTV_EV_UNKNOWN_VIS, 0, code | 0x100);   /* parity error */
        return;
    }
    int count;
    const sstv_mode *m = sstv_modes(&count);
    for (int i = 0; i < count; i++)
        if (m[i].vis == code) {
            start_image(rx, &m[i]);
            return;
        }
    if (rx->on_event)
        rx->on_event(rx->ctx, SSTV_EV_UNKNOWN_VIS, 0, code);
}

/* Find the sync end near rx->pred: the middle of the best run of "sync-length window mostly
 * below 1350 Hz". Returns the quality, 0..100. */
static int find_sync(sstv_rx *rx, uint32_t *se)
{
    uint32_t L = rx->sync_s, from = rx->pred - SYNC_WINDOW, to = rx->pred + SYNC_WINDOW;
    uint32_t cnt = 0, best = 0, first = rx->pred, last = rx->pred;
    for (uint32_t t = from - L; t < from; t++)
        cnt += fr(rx, t + 1) < SYNC_BELOW;
    for (uint32_t t = from; t <= to; t++) {        /* window (t-L, t] */
        cnt += fr(rx, t) < SYNC_BELOW;
        cnt -= fr(rx, t - L) < SYNC_BELOW;
        if (cnt > best) {
            best = cnt;
            first = last = t;
        } else if (cnt == best && last == t - 1) {
            last = t;
        }
    }
    *se = (first + last) / 2;
    return (int)(best * 100 / L);
}

static void sample_channel(sstv_rx *rx, uint64_t start_q16, uint8_t *out)
{
    for (int x = 0; x < rx->mode->w; x++)
        out[x] = lum(fr_q16(rx, start_q16 + (uint64_t)rx->pix_q16 * (uint32_t)x
                            + rx->pix_q16 / 2));
}

static void line(sstv_rx *rx)
{
    const sstv_mode *m = rx->mode;
    uint64_t after = m->family == SSTV_SCOTTIE
        ? (uint64_t)rx->porch_q16 + rx->chan_q16
        : (uint64_t)rx->porch_q16 + 3 * (uint64_t)rx->chan_q16 + 2 * (uint64_t)rx->sep_q16;
    if (rx->n < rx->pred + SYNC_WINDOW + (uint32_t)(after >> 16) + 2)
        return;

    uint32_t se;
    int q = find_sync(rx, &se);
    rx->last_sync_quality = q;
    if (q < 60) {                       /* no usable sync: free-run on the prediction */
        se = rx->pred;
        rx->nosync++;
    } else {
        rx->nosync = 0;
    }

    uint64_t s = (uint64_t)se << 16, tg, tb, tr;
    if (m->family == SSTV_SCOTTIE) {
        tr = s + rx->porch_q16;
        tb = s - ((uint64_t)rx->sync_s << 16) - rx->chan_q16;
        tg = tb - rx->sep_q16 - rx->chan_q16;
    } else {
        tg = s + rx->porch_q16;
        tb = tg + rx->chan_q16 + rx->sep_q16;
        tr = tb + rx->chan_q16 + rx->sep_q16;
    }
    uint8_t g[SSTV_MAX_W], b[SSTV_MAX_W], r[SSTV_MAX_W];
    sample_channel(rx, tg, g);
    sample_channel(rx, tb, b);
    sample_channel(rx, tr, r);
    for (int x = 0; x < m->w; x++)
        rx->row[x] = (uint16_t)(((r[x] & 0xf8) << 8) | ((g[x] & 0xfc) << 3) | (b[x] >> 3));
    if (rx->on_line)
        rx->on_line(rx->ctx, m, rx->y, rx->row);

    rx->pred_q16 = s + rx->line_q16;
    rx->pred = (uint32_t)(rx->pred_q16 >> 16);
    if (++rx->y >= m->h || rx->nosync >= MAX_NOSYNC) {
        int ev = rx->y >= m->h ? SSTV_EV_DONE : SSTV_EV_LOST;
        rx->state = SSTV_HUNT;
        rx->run1200 = 0;
        if (rx->on_event)
            rx->on_event(rx->ctx, ev, m, rx->y);
    }
}

void sstv_feed(sstv_rx *rx, const int16_t *x, int n)
{
    for (int i = 0; i < n; i++) {
        int f = discriminate(rx, x[i]);
        rx->freq[rx->n & MASK] = (int16_t)f;
        rx->n++;
        switch (rx->state) {
        case SSTV_HUNT:
            hunt(rx, f);
            break;
        case SSTV_VIS:
            vis(rx);
            break;
        case SSTV_LINE:
            line(rx);
            break;
        }
    }
}

int sstv_current_freq(const sstv_rx *rx)
{
    return rx->n ? fr(rx, rx->n - 1) : 0;
}
