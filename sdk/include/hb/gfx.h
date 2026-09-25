/* Full-screen graphics for homebrew apps.
 *
 * hb_gfx_open() puts a 480x272 RGB565 canvas on top of the radio's own UI (VDC5 graphics
 * plane GR3, the topmost; the firmware keeps drawing into GR2 underneath, unseen) and grabs the
 * touchscreen and keys, so the screen underneath never reacts to what the app gets. Draw into
 * hb_gfx_canvas(), then hb_gfx_present() shows it at the next vsync and hands you the other
 * buffer. hb_gfx_close() -- also done automatically when main() returns -- puts the radio's UI
 * back.
 *
 *     hb_gfx_open();
 *     while (running) {
 *         hb_canvas *c = hb_gfx_canvas();
 *         hb_clear(c, HB_BLACK);
 *         hb_fill_rect(c, 10, 10, 50, 50, HB_RGB(255, 0, 0));
 *         hb_gfx_present();               // blocks your code until the flip; the radio runs
 *     }
 *     hb_gfx_close();
 */
#ifndef HB_GFX_H
#define HB_GFX_H

#include <stdbool.h>
#include <stdint.h>

#define HB_SCREEN_W 480
#define HB_SCREEN_H 272

typedef uint16_t hb_color;                      /* RGB565 */
#define HB_RGB(r, g, b) ((hb_color)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#define HB_BLACK        HB_RGB(0, 0, 0)
#define HB_WHITE        HB_RGB(255, 255, 255)

typedef struct {
    hb_color *pixels;                           /* row-major, `stride` pixels per row */
    int w, h, stride;
} hb_canvas;

bool hb_gfx_open(void);
void hb_gfx_close(void);
hb_canvas *hb_gfx_canvas(void);                 /* the back buffer: draw here */
void hb_gfx_present(void);

/* Software drawing; everything clips to the canvas. */
void hb_clear(hb_canvas *c, hb_color color);
void hb_fill_rect(hb_canvas *c, int x, int y, int w, int h, hb_color color);
void hb_line(hb_canvas *c, int x0, int y0, int x1, int y1, hb_color color);
void hb_fill_triangle(hb_canvas *c, int x0, int y0, int x1, int y1, int x2, int y2,
                      hb_color color);

#endif
