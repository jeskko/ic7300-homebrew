/* Touch and time.
 *
 * The touchscreen reports calibrated screen pixels. The radio's own UI reacts to the same
 * touches too, unless the app has grabbed input -- hb_gfx_open() does.
 */
#ifndef HB_INPUT_H
#define HB_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool down;                  /* finger on the screen */
    int x, y;                   /* 0..479, 0..271; valid while down */
} hb_touch;

void hb_touch_read(hb_touch *t);

/* Milliseconds since the radio started (the RTOS tick; wraps after ~49 days). */
uint32_t hb_millis(void);

#endif
