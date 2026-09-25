/* SSTV receiver: decodes Scottie 1/2/DX and Martin 1/2 images from the radio's RX audio.
 *
 * Tune to an SSTV frequency in USB and start the app: it listens for a VIS header, then draws
 * the image line by line. CLEAR drops the current image and listens again; EXIT or the X
 * quits. The decoder is sstv_core.c (integer-only, shared with host_test.c); the audio comes
 * from hb/audio.h (loader ABI v4). Design: sdk/sstv-app-design.md.
 */
#include "hb/app.h"
#include "hb/audio.h"
#include "hb/gfx.h"
#include "hb/input.h"

#include "sstv_core.h"

#define IMG_X       4
#define IMG_Y       8
#define IMG_W       320
#define IMG_H       256
#define PANEL_X     334
#define EXIT_S      48
#define EXIT_X      (HB_SCREEN_W - EXIT_S)
#define REDRAW_MS   100

#define BG          HB_RGB(20, 22, 30)
#define EMPTY       HB_RGB(40, 44, 56)
#define FG          HB_RGB(220, 224, 232)
#define DIM         HB_RGB(130, 136, 150)
#define ACCENT      HB_RGB(90, 200, 120)
#define WARN        HB_RGB(230, 170, 60)

enum { ST_HUNT, ST_RX, ST_DONE, ST_LOST };

/* Exported (non-static) so the emulator test can read them with nm + QMP. */
uint16_t g_image[IMG_H][IMG_W];
volatile uint32_t g_status, g_lines, g_vis, g_unknown_vis, g_images, g_samples;
const char *g_mode_name = "";

static sstv_rx g_rx;
static bool g_dirty = true;

static void on_line(void *ctx, const sstv_mode *m, int y, const uint16_t *px)
{
    (void)ctx;
    if (y < IMG_H)
        for (int x = 0; x < m->w && x < IMG_W; x++)
            g_image[y][x] = px[x];
    g_lines = (uint32_t)y + 1;
    g_dirty = true;
}

static void clear_image(void)
{
    uint32_t *p = (uint32_t *)g_image, v = EMPTY | (uint32_t)EMPTY << 16;
    for (unsigned i = 0; i < sizeof g_image / 4; i++)
        p[i] = v;
    g_lines = 0;
}

static void on_event(void *ctx, int ev, const sstv_mode *m, int arg)
{
    (void)ctx;
    switch (ev) {
    case SSTV_EV_VIS:
        clear_image();
        g_mode_name = m->name;
        g_vis = (uint32_t)arg;
        g_status = ST_RX;
        break;
    case SSTV_EV_UNKNOWN_VIS:
        g_unknown_vis = (uint32_t)arg;
        break;
    case SSTV_EV_DONE:
        g_status = ST_DONE;
        g_images++;
        break;
    case SSTV_EV_LOST:
        g_status = ST_LOST;
        break;
    }
    g_dirty = true;
}

static char *utoa(uint32_t v, char *end)       /* writes backwards, returns the start */
{
    *--end = 0;
    do {
        *--end = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return end;
}

static void label_value(hb_canvas *c, int y, const char *label, uint32_t v, hb_color col)
{
    char buf[12];
    int w = hb_text(c, PANEL_X, y, label, 1, DIM);
    hb_text(c, PANEL_X + w + 6, y, utoa(v, buf + sizeof buf), 1, col);
}

static void draw(hb_canvas *c)
{
    hb_clear(c, BG);
    for (int y = 0; y < IMG_H; y++) {           /* the image, a word at a time */
        uint32_t *d = (uint32_t *)&c->pixels[(IMG_Y + y) * c->stride + IMG_X];
        const uint32_t *s = (const uint32_t *)g_image[y];
        for (int x = 0; x < IMG_W / 2; x++)
            d[x] = s[x];
    }

    hb_fill_rect(c, EXIT_X, 0, EXIT_S, EXIT_S, HB_RGB(70, 40, 40));
    hb_text(c, EXIT_X + 17, 17, "X", 2, FG);
    hb_text(c, PANEL_X, 12, "SSTV RX", 2, FG);

    static const char *const status[] = { "LISTENING", "RECEIVING", "DONE", "SIGNAL LOST" };
    static const hb_color scol[] = { DIM, ACCENT, FG, WARN };
    hb_text(c, PANEL_X, 60, status[g_status], 1, scol[g_status]);
    if (g_status != ST_HUNT) {
        hb_text(c, PANEL_X, 76, g_mode_name, 1, FG);
        label_value(c, 92, "LINE", g_lines, FG);
        label_value(c, 108, "SYNC %", (uint32_t)g_rx.last_sync_quality, FG);
    }

    /* live tone: 1100..2400 Hz as a bar, with the sync (1200) and black/white marks */
    int f = sstv_current_freq(&g_rx);
    label_value(c, 136, "HZ", (uint32_t)(f < 0 ? 0 : f), FG);
    int bx = PANEL_X, bw = 136, by = 150;
    hb_fill_rect(c, bx, by, bw, 10, EMPTY);
    int fx = (f - 1100) * bw / 1300;
    if (fx >= 0 && fx < bw)
        hb_fill_rect(c, bx + fx - 1, by, 3, 10, ACCENT);
    static const int marks[] = { 1200, 1500, 2300 };
    for (int i = 0; i < 3; i++)
        hb_fill_rect(c, bx + (marks[i] - 1100) * bw / 1300, by + 11, 1, 4, DIM);

    label_value(c, 176, "IMAGES", g_images, FG);
    if (g_unknown_vis)
        label_value(c, 192, "OTHER VIS", g_unknown_vis & 0xff, WARN);
    if (hb_audio_overruns())
        label_value(c, 208, "AUDIO OVR", hb_audio_overruns(), WARN);
    label_value(c, 220, "GAPS", hb_audio_gaps(), DIM);
    hb_text(c, PANEL_X, 238, "CLEAR  NEW IMAGE", 1, DIM);
    hb_text(c, PANEL_X, 250, "EXIT   QUIT", 1, DIM);
}

int main(void)
{
    sstv_init(&g_rx, on_line, on_event, 0);
    clear_image();
    if (!hb_audio_open()) {
        ui_message_box("SSTV RX needs the homebrew\nloader with ABI v4 (audio).");
        return 0;
    }
    if (!hb_gfx_open()) {
        hb_audio_close();
        return 0;
    }
    uint32_t last = 0;
    bool clear_held = false;
    for (;;) {
        int16_t buf[1024];
        int n;
        while ((n = hb_audio_read(buf, 1024)) > 0) {
            sstv_feed(&g_rx, buf, n);
            g_samples += (uint32_t)n;
        }

        if (hb_key_down(HB_KEY_EXIT))
            break;
        hb_touch t;
        hb_touch_read(&t);
        if (t.down && t.x >= EXIT_X && t.y < EXIT_S)
            break;
        bool clear = hb_key_down(HB_KEY_CLEAR);
        if (clear && !clear_held) {
            sstv_init(&g_rx, on_line, on_event, 0);
            clear_image();
            g_status = ST_HUNT;
            g_dirty = true;
        }
        clear_held = clear;

        uint32_t now = hb_millis();
        if (now - last >= REDRAW_MS) {          /* the live tone bar changes all the time */
            last = now;
            g_dirty = false;
            draw(hb_gfx_canvas());
            hb_gfx_present();
        } else {
            hb_yield();
        }
    }
    hb_audio_close();
    hb_gfx_close();
    return 0;
}
