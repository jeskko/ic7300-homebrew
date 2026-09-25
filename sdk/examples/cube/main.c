/* A spinning 3D cube. Tap the cube to switch between wireframe and filled faces; tap the X in
 * the top right corner to exit. */
#include "hb/app.h"
#include "hb/gfx.h"
#include "hb/input.h"
#include "hb/math.h"

#define CX      (HB_SCREEN_W / 2)
#define CY      (HB_SCREEN_H / 2)
#define FOCAL   250.0f              /* perspective scale, pixels */
#define DIST    4.2f                /* camera distance from the cube's centre */

#define EXIT_X  (HB_SCREEN_W - 48)  /* exit button: top right, 48x48 */
#define EXIT_Y  0
#define EXIT_S  48

static const float verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

/* Faces as quads, all wound the same way (see the culling test in draw_filled). */
static const int faces[6][4] = {
    {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
    {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3},
};
static const int face_rgb[6][3] = {
    {230, 60, 60}, {60, 200, 90}, {70, 120, 240},
    {240, 200, 50}, {200, 80, 220}, {60, 210, 220},
};
static const int edges[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
    {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static int sx[8], sy[8];            /* projected vertices */
static float cam[8][3];             /* camera-space vertices */
uint32_t g_frames;                  /* frames drawn (the emulator test reads it) */
static int bbox[4];                 /* x0, y0, x1, y1 of the projected cube */

static void transform(float t)
{
    float ay = t * 0.9f, ax = t * 0.55f;
    float cy = hb_cosf(ay), syn = hb_sinf(ay), cx = hb_cosf(ax), sxn = hb_sinf(ax);
    bbox[0] = bbox[1] = 1 << 20;
    bbox[2] = bbox[3] = -(1 << 20);
    for (int i = 0; i < 8; i++) {
        float x = verts[i][0], y = verts[i][1], z = verts[i][2];
        float x1 = x * cy + z * syn, z1 = -x * syn + z * cy;       /* about Y */
        float y2 = y * cx - z1 * sxn, z2 = y * sxn + z1 * cx;      /* about X */
        float zc = z2 + DIST;
        cam[i][0] = x1;
        cam[i][1] = y2;
        cam[i][2] = zc;
        sx[i] = CX + (int)(x1 * FOCAL / zc);
        sy[i] = CY - (int)(y2 * FOCAL / zc);
        if (sx[i] < bbox[0]) bbox[0] = sx[i];
        if (sy[i] < bbox[1]) bbox[1] = sy[i];
        if (sx[i] > bbox[2]) bbox[2] = sx[i];
        if (sy[i] > bbox[3]) bbox[3] = sy[i];
    }
}

static void draw_filled(hb_canvas *c)
{
    for (int f = 0; f < 6; f++) {
        const int *q = faces[f];
        /* Back-face cull on the projected winding: with screen y pointing down, faces turned
         * towards the viewer come out with a positive cross product (checked numerically:
         * the nearest face passes, the farthest and edge-on ones don't). */
        int cross = (sx[q[1]] - sx[q[0]]) * (sy[q[2]] - sy[q[0]]) -
                    (sy[q[1]] - sy[q[0]]) * (sx[q[2]] - sx[q[0]]);
        if (cross <= 0)
            continue;
        /* Lambert shading, light from the viewer's upper left: the face normal is the cross
         * product of two edges (length 4, since the cube's edges are 2 long). */
        const float *a = cam[q[0]], *b = cam[q[1]], *d = cam[q[3]];
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        float vx = d[0] - a[0], vy = d[1] - a[1], vzz = d[2] - a[2];
        float nx = uy * vzz - uz * vy, ny = uz * vx - ux * vzz, nz = ux * vy - uy * vx;
        if (nx * a[0] + ny * a[1] + nz * a[2] > 0) {       /* orient towards the camera */
            nx = -nx; ny = -ny; nz = -nz;
        }
        float lit = (nx * -0.40f + ny * 0.45f + nz * -0.80f) * 0.25f;   /* |n| = 4 */
        if (lit < 0)
            lit = 0;
        int k = 70 + (int)(185.0f * lit);
        if (k > 255) k = 255;
        hb_color col = HB_RGB(face_rgb[f][0] * k / 255, face_rgb[f][1] * k / 255,
                              face_rgb[f][2] * k / 255);
        hb_fill_triangle(c, sx[q[0]], sy[q[0]], sx[q[1]], sy[q[1]], sx[q[2]], sy[q[2]], col);
        hb_fill_triangle(c, sx[q[0]], sy[q[0]], sx[q[2]], sy[q[2]], sx[q[3]], sy[q[3]], col);
    }
}

static void draw_wireframe(hb_canvas *c)
{
    for (int e = 0; e < 12; e++)
        hb_line(c, sx[edges[e][0]], sy[edges[e][0]], sx[edges[e][1]], sy[edges[e][1]],
                HB_RGB(120, 255, 140));
}

static void draw_exit_button(hb_canvas *c, bool pressed)
{
    hb_fill_rect(c, EXIT_X, EXIT_Y, EXIT_S, EXIT_S, pressed ? HB_RGB(200, 40, 40)
                                                           : HB_RGB(70, 70, 80));
    for (int d = -1; d <= 1; d++) {
        hb_line(c, EXIT_X + 14 + d, EXIT_Y + 14, EXIT_X + 33 + d, EXIT_Y + 33, HB_WHITE);
        hb_line(c, EXIT_X + 33 + d, EXIT_Y + 14, EXIT_X + 14 + d, EXIT_Y + 33, HB_WHITE);
    }
}

static bool in_exit(int x, int y)
{
    return x >= EXIT_X && y >= EXIT_Y && y < EXIT_Y + EXIT_S;
}

static bool in_cube(int x, int y)
{
    return x >= bbox[0] && x <= bbox[2] && y >= bbox[1] && y <= bbox[3];
}

int main(void)
{
    if (!hb_gfx_open())
        return 1;

    bool filled = false, was_down = false, exit_armed = false;
    uint32_t t0 = hb_millis();
    for (;;) {
        hb_touch t;
        hb_touch_read(&t);
        if (t.down && !was_down) {                      /* new touch */
            if (in_exit(t.x, t.y))
                exit_armed = true;
            else if (in_cube(t.x, t.y))
                filled = !filled;
        }
        if (!t.down && was_down && exit_armed)          /* released: exit if still on X */
            break;
        if (t.down && exit_armed && !in_exit(t.x, t.y))
            exit_armed = false;                         /* slid off the button: cancel */
        was_down = t.down;

        transform((float)(hb_millis() - t0) * 0.001f);
        hb_canvas *c = hb_gfx_canvas();
        hb_clear(c, HB_RGB(10, 12, 24));
        if (filled)
            draw_filled(c);
        else
            draw_wireframe(c);
        draw_exit_button(c, exit_armed);
        hb_gfx_present();
        g_frames++;
    }
    hb_gfx_close();
    return 0;
}
