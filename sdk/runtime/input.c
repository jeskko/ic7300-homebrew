/* Touch and time (see hb/input.h). */
#include "hb/firmware.h"
#include "hb/input.h"

void hb_touch_read(hb_touch *t)
{
    volatile uint8_t *r = fw_fp_regs;
    /* The SCIF3 RX path can update the register file between these reads; a torn X/Y pair
     * is at worst one sample off, the next read is whole. */
    t->down = r[FW_FP_TOUCH_TAG] == 0;
    t->x = r[FW_FP_TOUCH_X] << 8 | r[FW_FP_TOUCH_X + 1];
    t->y = r[FW_FP_TOUCH_Y] << 8 | r[FW_FP_TOUCH_Y + 1];
}

uint32_t hb_millis(void)
{
    return fw_tick_ms;
}
