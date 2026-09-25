/* Full-screen graphics on VDC5 GR3 (see hb/gfx.h and hb/firmware.h).
 *
 * GR3 gets the same geometry as the firmware's own GR2 (copied from its live registers, so it
 * lines up with the panel whatever the timing), our framebuffer, and DISP_SEL = CURRENT, which
 * shows GR3 alone. hb_gfx_close() sets it back to LOWER (transparent) and turns its read off,
 * exactly the state the firmware leaves it in. Register sequence per the Renesas VDC5 driver
 * (R_VDC_ReadDataControl + R_VDC_StartProcess, r_vdc_register.c).
 */
#include "hb/firmware.h"
#include "hb/gfx.h"
#include "hb/input.h"
#include "runtime_internal.h"

#define GR2(off) FW_VDC5_REG(FW_VDC5_GR2, off)
#define GR3(off) FW_VDC5_REG(FW_VDC5_GR3, off)
#define STRIDE   (HB_SCREEN_W * 2)
#define FB_BYTES (STRIDE * HB_SCREEN_H)

static hb_canvas g_canvas[2] = {
    { (hb_color *)HB_FB0, HB_SCREEN_W, HB_SCREEN_H, HB_SCREEN_W },
    { (hb_color *)HB_FB1, HB_SCREEN_W, HB_SCREEN_H, HB_SCREEN_W },
};
static int g_back;
static bool g_open;
static uint32_t g_flip_ms;

/* Write the CPU's cached pixels out to RAM, where the VDC5 reads them. Unverified on real
 * hardware (QEMU models no caches). */
static void dcache_clean(const void *p, uint32_t len)
{
    for (uint32_t a = (uint32_t)p & ~31u; a < (uint32_t)p + len; a += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");  /* DCCMVAC */
    __asm__ volatile("dsb" ::: "memory");
}

static bool gr3_applied(void *arg)
{
    (void)arg;
    return (GR3(GR_UPDATE) & 0x111) == 0 || hb_millis() - g_flip_ms > 50;
}

static void gr3_update(uint32_t bits)
{
    GR3(GR_UPDATE) |= bits;
    g_flip_ms = hb_millis();
    hb_wait_until(gr3_applied, 0);
}

static bool touch_up(void *arg)
{
    (void)arg;
    hb_touch t;
    hb_touch_read(&t);
    return !t.down;
}

bool hb_gfx_open(void)
{
    if (g_open)
        return true;
    /* Only take over a panel laid out the way we expect. */
    if ((GR2(GR_FLM6) >> 28) != 0 || ((GR2(GR_FLM6) >> 16) & 0x7ff) != HB_SCREEN_W - 1 ||
        ((GR2(GR_FLM5) >> 16) & 0x7ff) != HB_SCREEN_H - 1 ||
        ((GR2(GR_FLM3) >> 16) & 0x7fff) != STRIDE)
        return false;

    for (int i = 0; i < 2; i++) {
        hb_clear(&g_canvas[i], HB_BLACK);
        dcache_clean(g_canvas[i].pixels, FB_BYTES);
    }
    GR3(GR_FLM1) = GR2(GR_FLM1);
    GR3(GR_FLM2) = HB_FB0;
    GR3(GR_FLM3) = GR2(GR_FLM3);
    GR3(GR_FLM4) = GR2(GR_FLM4);
    GR3(GR_FLM5) = GR2(GR_FLM5);
    GR3(GR_FLM6) = GR2(GR_FLM6);
    GR3(GR_AB2) = GR2(GR_AB2);
    GR3(GR_AB3) = GR2(GR_AB3);
    GR3(GR_AB4) = GR2(GR_AB2);
    GR3(GR_AB5) = GR2(GR_AB3);
    GR3(GR_AB6) = 0;
    GR3(GR_AB7) = 0x00ff0000;                           /* opaque, no chroma key */
    GR3(GR_AB1) = (GR3(GR_AB1) & ~0xd003u) | 2;         /* DISP_SEL = CURRENT */
    GR3(GR_FLM_RD) |= 1;
    g_back = 1;
    g_open = true;
    hb__runtime_api()->input_grab = 1;
    hb__cleanup = hb_gfx_close;
    gr3_update(0x111);
    return true;
}

void hb_gfx_close(void)
{
    if (!g_open)
        return;
    GR3(GR_AB1) = (GR3(GR_AB1) & ~3u) | 1;              /* DISP_SEL = LOWER */
    GR3(GR_FLM_RD) &= ~1u;
    gr3_update(0x11);
    g_open = false;
    hb__cleanup = 0;
    /* Hold the grab until the finger lifts, or the lift would land on the radio's UI. */
    hb_wait_until(touch_up, 0);
    hb__runtime_api()->input_grab = 0;
}

hb_canvas *hb_gfx_canvas(void)
{
    return &g_canvas[g_back];
}

void hb_gfx_present(void)
{
    if (!g_open)
        return;
    hb_canvas *c = &g_canvas[g_back];
    dcache_clean(c->pixels, FB_BYTES);
    GR3(GR_FLM2) = (uint32_t)c->pixels;
    gr3_update(0x01);                                   /* IBUS_VEN: base takes effect at vsync */
    g_back ^= 1;
}

/* ---- drawing ------------------------------------------------------------------------ */

void hb_clear(hb_canvas *c, hb_color color)
{
    uint32_t v = (uint32_t)color | (uint32_t)color << 16;
    uint32_t *p = (uint32_t *)c->pixels;
    for (int n = c->stride * c->h / 2; n > 0; n--)
        *p++ = v;
}

static void hspan(hb_canvas *c, int y, int xa, int xb, hb_color color)
{
    if (y < 0 || y >= c->h)
        return;
    if (xa > xb) {
        int t = xa; xa = xb; xb = t;
    }
    if (xa < 0)
        xa = 0;
    if (xb > c->w - 1)
        xb = c->w - 1;
    hb_color *row = c->pixels + y * c->stride;
    for (int x = xa; x <= xb; x++)
        row[x] = color;
}

void hb_fill_rect(hb_canvas *c, int x, int y, int w, int h, hb_color color)
{
    for (int yy = y; yy < y + h; yy++)
        hspan(c, yy, x, x + w - 1, color);
}

void hb_line(hb_canvas *c, int x0, int y0, int x1, int y1, hb_color color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if ((unsigned)x0 < (unsigned)c->w && (unsigned)y0 < (unsigned)c->h)
            c->pixels[y0 * c->stride + x0] = color;
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void hb_fill_triangle(hb_canvas *c, int x0, int y0, int x1, int y1, int x2, int y2,
                      hb_color color)
{
    /* sort by y: (x0,y0) top, (x2,y2) bottom */
#define SWAP(a, b) do { int t = a; a = b; b = t; } while (0)
    if (y1 < y0) { SWAP(x0, x1); SWAP(y0, y1); }
    if (y2 < y0) { SWAP(x0, x2); SWAP(y0, y2); }
    if (y2 < y1) { SWAP(x1, x2); SWAP(y1, y2); }
#undef SWAP
    if (y2 == y0) {
        int lo = x0 < x1 ? x0 : x1, hi = x0 > x1 ? x0 : x1;
        hspan(c, y0, lo < x2 ? lo : x2, hi > x2 ? hi : x2, color);
        return;
    }
    int ystart = y0 < 0 ? 0 : y0, yend = y2 > c->h - 1 ? c->h - 1 : y2;
    for (int y = ystart; y <= yend; y++) {
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);                 /* long edge */
        int xb = y < y1 ? (y1 == y0 ? x1 : x0 + (x1 - x0) * (y - y0) / (y1 - y0))
                        : (y2 == y1 ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1));
        hspan(c, y, xa, xb, color);
    }
}
