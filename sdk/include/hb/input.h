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

/* Front-panel keys: true while the key is held. Keys are bit indices into the front panel's
 * 40-bit key field (notes/front-panel-report.md, "Key-code table"). While the app has grabbed
 * input the radio ignores them; otherwise it reacts to them too. POWER is not readable. The
 * dials are never grabbed. */
bool hb_key_down(int key);

#define HB_KEY_TRANSMIT     0
#define HB_KEY_TUNER        1
#define HB_KEY_VOX          2
#define HB_KEY_MENU         3
#define HB_KEY_FUNCTION     4
#define HB_KEY_MSCOPE       5
#define HB_KEY_QUICK        6
#define HB_KEY_XFC          7
#define HB_KEY_PAMP_ATT     8
#define HB_KEY_NOTCH        9
#define HB_KEY_NB           10
#define HB_KEY_NR           11
#define HB_KEY_EXIT         12
#define HB_KEY_AUTO_TUNE    13
#define HB_KEY_SPEECH       14
#define HB_KEY_MPAD         15
#define HB_KEY_AB           16
#define HB_KEY_VM           17
#define HB_KEY_MCH_UP       18
#define HB_KEY_MCH_DN       19
#define HB_KEY_RIT          20
#define HB_KEY_DELTA_TX     21
#define HB_KEY_CLEAR        22
#define HB_KEY_SPLIT        23
#define HB_KEY_MULTI_PUSH   26
#define HB_KEY_PBT_CLR      27

/* Milliseconds since the radio started (the RTOS tick; wraps after ~49 days). */
uint32_t hb_millis(void);

#endif
