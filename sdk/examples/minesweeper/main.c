/* Minesweeper on a 10x10 board.
 *
 * Tap a cell to dig it, hold it to plant or pull a flag; the DIG/FLAG button swaps the two.
 * Tapping a dug number whose flags are all placed digs its other neighbours. NEW deals a fresh
 * board; the X or the EXIT key quits. Mines are laid on the first dig, never on or next to
 * the dug cell, so the game always opens with a free area.
 */
#include "hb/app.h"
#include "hb/gfx.h"
#include "hb/input.h"

#define N           10
#define CELLS       (N * N)
#define MINES       12

#define CELL        26                  /* pixels, including the 1-pixel grid line */
#define GRID_X      6
#define GRID_Y      6
#define GRID_S      (N * CELL)

#define PANEL_X     284
#define EXIT_X      (HB_SCREEN_W - 48)  /* exit button: top right, 48x48 */
#define EXIT_S      48
#define BTN_Y       164
#define BTN_W       90
#define BTN_H       52
#define MODE_X      PANEL_X
#define NEW_X       (PANEL_X + BTN_W + 10)

#define HOLD_MS     450                 /* press this long on a cell for the other action */

enum { READY, PLAYING, WON, LOST };     /* READY: no mines laid yet */
enum { HIDDEN, DUG, FLAGGED };
enum { NONE, B_CELL, B_MODE, B_NEW, B_EXIT };   /* what a touch started on */

/* Game state. Exported (non-static) so sdk/loader/test_emu.py can find it with nm. */
uint8_t g_mine[CELLS];                  /* 1 = mine */
uint8_t g_cell[CELLS];                  /* HIDDEN / DUG / FLAGGED */
uint8_t g_state;
uint8_t g_flag_mode;                    /* tap plants flags, hold digs */
int8_t  g_boom = -1;                    /* the mine that went off */
uint32_t g_frames;                      /* frames presented */

static uint8_t g_adj[CELLS];            /* neighbouring mines */
static int g_dug, g_flags;
static uint32_t g_t_start, g_t_end, g_rng;

static const hb_color num_rgb[9] = {
    0, HB_RGB(30, 60, 255), HB_RGB(0, 150, 0), HB_RGB(230, 30, 30), HB_RGB(20, 20, 150),
    HB_RGB(150, 20, 20), HB_RGB(0, 150, 150), HB_RGB(20, 20, 20), HB_RGB(110, 110, 110),
};

#define BG          HB_RGB(24, 28, 40)
#define GRID_LINE   HB_RGB(60, 64, 76)
#define HIDDEN_C    HB_RGB(150, 158, 176)
#define HIDDEN_HI   HB_RGB(200, 206, 220)
#define HIDDEN_LO   HB_RGB(96, 102, 118)
#define PRESSED_C   HB_RGB(120, 128, 146)
#define DUG_C       HB_RGB(214, 214, 206)
#define BOOM_C      HB_RGB(230, 40, 40)
#define TEXT_C      HB_RGB(230, 232, 240)
#define DIM_C       HB_RGB(140, 146, 160)
#define BTN_C       HB_RGB(70, 74, 90)
#define BTN_HOT     HB_RGB(110, 116, 140)
#define FLAG_C      HB_RGB(230, 30, 30)

static uint32_t rnd(void)               /* xorshift32 */
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static bool near(int a, int b)          /* same cell or touching, including diagonally */
{
    int dx = a % N - b % N, dy = a / N - b / N;
    return dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
}

/* FOR_NEIGHBOURS(c, i) stmt; runs stmt with i set to each in-board neighbour of cell c. */
#define FOR_NEIGHBOURS(c, i)                                                              \
    for (int _dy = -1; _dy <= 1; _dy++)                                                   \
        for (int _dx = -1, i; _dx <= 1; _dx++)                                            \
            if ((_dx || _dy) && (unsigned)((c) % N + _dx) < N &&                          \
                (unsigned)((c) / N + _dy) < N && ((i = (c) + _dy * N + _dx), 1))

static void new_game(void)
{
    for (int i = 0; i < CELLS; i++)
        g_mine[i] = g_cell[i] = g_adj[i] = 0;
    g_state = READY;
    g_boom = -1;
    g_dug = g_flags = 0;
    g_t_start = g_t_end = 0;
}

static void lay_mines(int safe)
{
    g_rng = hb_millis() * 2654435761u ^ (uint32_t)safe << 16 ^ 0x9e3779b9u;
    if (!g_rng)
        g_rng = 1;
    for (int placed = 0; placed < MINES;) {
        int i = rnd() % CELLS;
        if (g_mine[i] || near(i, safe))
            continue;
        g_mine[i] = 1;
        placed++;
    }
    for (int c = 0; c < CELLS; c++)
        FOR_NEIGHBOURS(c, i)
            g_adj[c] += g_mine[i];
    g_state = PLAYING;
    g_t_start = hb_millis();
}

static void finish(int state)
{
    g_state = state;
    g_t_end = hb_millis();
    for (int i = 0; i < CELLS; i++)
        if (g_mine[i] && state == WON && g_cell[i] != FLAGGED) {
            g_cell[i] = FLAGGED;
            g_flags++;
        }
}

/* Dig cell c, flooding outwards from cells with no neighbouring mines. */
static void dig(int c)
{
    if (g_state == READY)
        lay_mines(c);
    if (g_state != PLAYING || g_cell[c] != HIDDEN)
        return;
    if (g_mine[c]) {
        g_boom = c;
        g_cell[c] = DUG;
        finish(LOST);
        return;
    }
    static uint8_t stack[CELLS];
    int sp = 0;
    g_cell[c] = DUG;
    g_dug++;
    stack[sp++] = c;
    while (sp) {
        int k = stack[--sp];
        if (g_adj[k])
            continue;
        FOR_NEIGHBOURS(k, i)
            if (g_cell[i] == HIDDEN) {
                g_cell[i] = DUG;
                g_dug++;
                stack[sp++] = i;
            }
    }
    if (g_dug == CELLS - MINES)
        finish(WON);
}

/* Tap on a dug number: if its flags are all placed, dig the rest of its neighbours. */
static void chord(int c)
{
    int flags = 0;
    FOR_NEIGHBOURS(c, i)
        flags += g_cell[i] == FLAGGED;
    if (flags != g_adj[c])
        return;
    FOR_NEIGHBOURS(c, i)
        if (g_cell[i] == HIDDEN)
            dig(i);
}

static void toggle_flag(int c)
{
    if (g_state == WON || g_state == LOST || g_cell[c] == DUG)
        return;
    g_cell[c] = g_cell[c] == FLAGGED ? HIDDEN : FLAGGED;
    g_flags += g_cell[c] == FLAGGED ? 1 : -1;
}

/* A tap (hold = false) or hold (hold = true) on cell c; FLAG mode swaps what the two do. */
static void act(int c, bool hold)
{
    if (g_state == WON || g_state == LOST)
        return;
    if (g_cell[c] == DUG) {
        if (!hold)
            chord(c);
    } else if (hold != (bool)g_flag_mode) {
        toggle_flag(c);
    } else {
        dig(c);
    }
}

/* ---- drawing ------------------------------------------------------------------------ */

static void fmt_num(char *out, int v, int digits)
{
    if (v < 0)
        v = 0;
    for (int i = digits - 1; i >= 0; i--, v /= 10)
        out[i] = '0' + v % 10;
    out[digits] = 0;
}

static void text_centered(hb_canvas *c, int x, int y, int w, int h, const char *s, int scale,
                          hb_color col)
{
    hb_text(c, x + (w - hb_text_width(s, scale) + scale) / 2, y + (h - HB_FONT_H * scale) / 2,
            s, scale, col);
}

static void draw_mine(hb_canvas *c, int x, int y)
{
    int cx = x + (CELL - 1) / 2, cy = y + (CELL - 1) / 2;
    static const int8_t half[] = {1, 3, 4, 5, 5, 6, 6, 6, 5, 5, 4, 3, 1};  /* disc, r = 6 */
    for (int r = 0; r < 13; r++)
        hb_fill_rect(c, cx - half[r], cy - 6 + r, 2 * half[r] + 1, 1, HB_BLACK);
    hb_fill_rect(c, cx - 9, cy, 19, 1, HB_BLACK);                   /* spikes */
    hb_fill_rect(c, cx, cy - 9, 1, 19, HB_BLACK);
    hb_fill_rect(c, cx - 3, cy - 3, 2, 2, HB_WHITE);                /* glint */
}

static void draw_flag(hb_canvas *c, int x, int y)
{
    int px = x + 13;
    hb_fill_triangle(c, px, y + 5, px, y + 14, px - 9, y + 9, FLAG_C);
    hb_fill_rect(c, px, y + 5, 2, 14, HB_BLACK);                    /* pole */
    hb_fill_rect(c, px - 5, y + 19, 12, 2, HB_BLACK);               /* base */
}

static void draw_cell(hb_canvas *c, int i, bool pressed)
{
    int x = GRID_X + i % N * CELL + 1, y = GRID_Y + i / N * CELL + 1, s = CELL - 1;
    bool over = g_state == WON || g_state == LOST;
    if (g_cell[i] == DUG || (over && g_mine[i] && g_cell[i] != FLAGGED)) {
        hb_fill_rect(c, x, y, s, s, i == g_boom ? BOOM_C : DUG_C);
        if (g_mine[i])
            draw_mine(c, x, y);
        else if (g_adj[i]) {
            char d[2] = {(char)('0' + g_adj[i]), 0};
            text_centered(c, x, y, s, s, d, 2, num_rgb[g_adj[i]]);
        }
        return;
    }
    if (pressed) {
        hb_fill_rect(c, x, y, s, s, PRESSED_C);
    } else {
        hb_fill_rect(c, x, y, s, s, HIDDEN_C);
        hb_fill_rect(c, x, y, s, 2, HIDDEN_HI);                     /* bevel */
        hb_fill_rect(c, x, y, 2, s, HIDDEN_HI);
        hb_fill_rect(c, x, y + s - 2, s, 2, HIDDEN_LO);
        hb_fill_rect(c, x + s - 2, y, 2, s, HIDDEN_LO);
    }
    if (g_cell[i] == FLAGGED) {
        draw_flag(c, x, y);
        if (over && !g_mine[i]) {                                   /* wrong flag */
            hb_line(c, x + 3, y + 3, x + s - 4, y + s - 4, HB_BLACK);
            hb_line(c, x + s - 4, y + 3, x + 3, y + s - 4, HB_BLACK);
        }
    }
}

static void draw_button(hb_canvas *c, int x, int y, int w, int h, const char *label, bool hot)
{
    hb_fill_rect(c, x, y, w, h, hot ? BTN_HOT : BTN_C);
    text_centered(c, x, y, w, h, label, 3, TEXT_C);
}

static void draw(hb_canvas *c, int pressed_cell, int held_button)
{
    hb_clear(c, BG);
    hb_fill_rect(c, GRID_X, GRID_Y, GRID_S + 1, GRID_S + 1, GRID_LINE);
    for (int i = 0; i < CELLS; i++)
        draw_cell(c, i, i == pressed_cell);

    hb_text(c, PANEL_X, 12, "MINESWEEPER", 2, TEXT_C);

    char buf[8];
    hb_text(c, PANEL_X, 50, "MINES", 2, DIM_C);
    fmt_num(buf, MINES - g_flags, 2);
    hb_text(c, PANEL_X + 84, 46, buf, 3, TEXT_C);

    uint32_t now = g_state == PLAYING ? hb_millis() : g_t_end;
    int secs = g_state == READY ? 0 : (int)((now - g_t_start) / 1000);
    hb_text(c, PANEL_X, 84, "TIME", 2, DIM_C);
    fmt_num(buf, secs > 999 ? 999 : secs, 3);
    hb_text(c, PANEL_X + 84, 80, buf, 3, TEXT_C);

    if (g_state == LOST)
        hb_text(c, PANEL_X, 122, "BOOM!", 4, BOOM_C);
    else if (g_state == WON)
        hb_text(c, PANEL_X, 122, "CLEARED!", 3, HB_RGB(80, 220, 110));

    draw_button(c, MODE_X, BTN_Y, BTN_W, BTN_H, g_flag_mode ? "FLAG" : "DIG",
                held_button == B_MODE || g_flag_mode);
    draw_button(c, NEW_X, BTN_Y, BTN_W, BTN_H, "NEW", held_button == B_NEW);
    hb_text(c, PANEL_X, 236, g_flag_mode ? "HOLD A CELL TO DIG" : "HOLD A CELL TO FLAG", 1,
            DIM_C);
    hb_text(c, PANEL_X, 250, "X OR EXIT KEY TO QUIT", 1, DIM_C);

    hb_fill_rect(c, EXIT_X, 0, EXIT_S, EXIT_S, held_button == B_EXIT ? HB_RGB(200, 40, 40)
                                                                      : BTN_C);
    for (int d = -1; d <= 1; d++) {
        hb_line(c, EXIT_X + 14 + d, 14, EXIT_X + 33 + d, 33, HB_WHITE);
        hb_line(c, EXIT_X + 33 + d, 14, EXIT_X + 14 + d, 33, HB_WHITE);
    }
}

/* ---- input -------------------------------------------------------------------------- */

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int cell_at(int x, int y)
{
    if (!in_rect(x, y, GRID_X, GRID_Y, GRID_S, GRID_S))
        return -1;
    return (y - GRID_Y) / CELL * N + (x - GRID_X) / CELL;
}

static int button_at(int x, int y)
{
    if (cell_at(x, y) >= 0)
        return B_CELL;
    if (in_rect(x, y, MODE_X, BTN_Y, BTN_W, BTN_H))
        return B_MODE;
    if (in_rect(x, y, NEW_X, BTN_Y, BTN_W, BTN_H))
        return B_NEW;
    if (in_rect(x, y, EXIT_X, 0, EXIT_S, EXIT_S))
        return B_EXIT;
    return NONE;
}

static bool exit_key_up(void *arg)
{
    (void)arg;
    return !hb_key_down(HB_KEY_EXIT);
}

int main(void)
{
    if (!hb_gfx_open())
        return 1;
    new_game();

    int target = NONE;          /* what the current touch started on */
    int cell = -1;              /* ...and which cell, for B_CELL */
    bool held = false;          /* the hold action has fired for this touch */
    bool was_down = false, dirty = true;
    uint32_t t_down = 0;
    int last_secs = -1;

    for (;;) {
        if (hb_key_down(HB_KEY_EXIT)) {
            hb_wait_until(exit_key_up, 0);
            break;
        }

        hb_touch t;
        hb_touch_read(&t);
        int under = t.down ? button_at(t.x, t.y) : NONE;
        int under_cell = t.down ? cell_at(t.x, t.y) : -1;

        if (t.down && !was_down) {                          /* new touch */
            target = under;
            cell = under_cell;
            held = false;
            t_down = hb_millis();
            dirty = true;
        } else if (t.down && target != NONE) {
            if (under != target || under_cell != cell) {    /* slid off: cancel */
                target = NONE;
                dirty = true;
            } else if (target == B_CELL && !held && hb_millis() - t_down >= HOLD_MS) {
                act(cell, true);
                held = true;
                dirty = true;
            }
        } else if (!t.down && was_down) {                   /* released */
            if (target == B_CELL && !held)
                act(cell, false);
            else if (target == B_MODE)
                g_flag_mode = !g_flag_mode;
            else if (target == B_NEW)
                new_game();
            else if (target == B_EXIT)
                break;
            target = NONE;
            dirty = true;
        }
        was_down = t.down;

        int secs = g_state == PLAYING ? (int)((hb_millis() - g_t_start) / 1000) : -1;
        if (secs != last_secs) {
            last_secs = secs;
            dirty = true;
        }

        if (dirty) {
            /* Both buffers get a full redraw, so there's nothing stale to track. */
            draw(hb_gfx_canvas(), target == B_CELL && !held ? cell : -1, target);
            hb_gfx_present();
            g_frames++;
            dirty = false;
        } else {
            hb_yield();
        }
    }
    hb_gfx_close();
    return 0;
}
