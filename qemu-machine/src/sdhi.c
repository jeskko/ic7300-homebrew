/*
 * RZ/A1H SD Host Interface (SDHI) channel 0 -- the IC-7300's SD card slot.
 *
 * Why SDHI and not MMCIF (2026-09-24): body.bin never references MMCIF's
 * base (0xE804C800, mmc.c) -- the long-standing "zero xrefs" mystery -- but
 * it does pass SDHI0's base, 0xE804E000, to Renesas' SD driver library
 * (system_mode_request_dispatch -> FUN_2017df40(0, 0xE804E000, work), i.e.
 * sd_init(port, base, workarea)). A plain boot then configures SD_INFO1/2
 * masks, releases SOFT_RST, unmasks INFO3/INFO4 (SD_CD insert/remove) and
 * polls SD_INFO1: the driver was running all along, it just saw no card.
 *
 * Registers and bits: RZ/A1H hardware manual chapter 50 (Rev.6.00, pages
 * 50-3..50-42). 16-bit registers at their natural offsets (no bus shift);
 * SD_BUF0 is the 32-bit data port. GIC: SDHI0_3 = 302 (card detect),
 * SDHI0_0 = 303 (card access), SDHI0_1 = 304 (SDIO), all level.
 *
 * The card itself is upstream QEMU's sd-card (hw/sd/sd.c) on this device's
 * "sd-bus", so the SD protocol state machine, CSD/CID/SCR and block I/O are
 * the real, well-tested ones -- this file only plays the host controller.
 * Attach an image with `-drive if=sd,format=raw,file=sd.img` (the size must
 * be a power of two, sd.c enforces it). Card insert/eject follows the block
 * backend (QMP eject / blockdev-change-medium) through sd-bus set_inserted,
 * raising INFO3/INFO4 like the real SD_CD pin.
 *
 * Simplifications, deliberate: commands and blocks complete after a fixed
 * short delay (SDHI_EVENT_NS) rather than at SD_CLK rate -- never from inside
 * the triggering MMIO write, which is the race dmac.c's comment documents.
 * CRC/END/CMD errors never happen. With CC_EXT_MODE.DMASDRW the "dma-req"
 * line (to dmac.c) replaces BRE/BWE, as the manual says; the body.bin driver
 * uses PIO for the card-register reads and DMA ch7 for sectors. SDIO
 * (CMD52/53, SDIO_INFO1) is register storage only.
 *
 * RZA1H_DEBUG=sdhi logs every command, response and block.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HSdhiState, RZA1H_SDHI)

#define TYPE_RZA1H_SDHI_BUS "rza1h-sdhi-bus"

#define SD_CMD          0x00
#define SD_ARG0         0x04
#define SD_ARG1         0x06
#define SD_STOP         0x08
#define SD_SECCNT       0x0a
#define SD_RSP00        0x0c    /* ..SD_RSP07 at 0x1a */
#define SD_INFO1        0x1c
#define SD_INFO2        0x1e
#define SD_INFO1_MASK   0x20
#define SD_INFO2_MASK   0x22
#define SD_CLK_CTRL     0x24
#define SD_SIZE         0x26
#define SD_OPTION       0x28
#define SD_ERR_STS1     0x2c
#define SD_ERR_STS2     0x2e
#define SD_BUF0         0x30
#define SDIO_MODE       0x34
#define SDIO_INFO1      0x36
#define SDIO_INFO1_MASK 0x38
#define CC_EXT_MODE     0xd8
#define SOFT_RST        0xe0
#define VERSION         0xe2
#define EXT_SWAP        0xf0

/* SD_CMD */
#define CMD_INDEX(v)    ((v) & 0x3f)
#define CMD_IS_ACMD(v)  ((((v) >> 6) & 3) == 1)    /* C1:C0 = 01 */
#define CMD_MODE(v)     (((v) >> 8) & 7)    /* MD2..MD0: 0 = normal, 3..7 = extended */
#define CMD_MD3_DATA    (1 << 11)
#define CMD_MD4_READ    (1 << 12)
#define CMD_MD5_MULTI   (1 << 13)
#define CMD_MD6_NO_CMD12 (1 << 14)
#define CMD_MODE_NO_RESP 3

/* SD_STOP */
#define STOP_STP        (1 << 0)
#define STOP_SEC        (1 << 8)

/* SD_INFO1 */
#define INFO1_INFO0     (1 << 0)    /* response end */
#define INFO1_INFO2     (1 << 2)    /* access end */
#define INFO1_INFO3     (1 << 3)    /* SD_CD removal */
#define INFO1_INFO4     (1 << 4)    /* SD_CD insertion */
#define INFO1_INFO5     (1 << 5)    /* SD_CD state: 1 = card present */
#define INFO1_INFO7     (1 << 7)    /* SD_WP: 1 = writable to body.bin -- with 0 it never writes */
#define INFO1_INFO8     (1 << 8)
#define INFO1_INFO9     (1 << 9)
#define INFO1_INFO10    (1 << 10)   /* SD_D3 level */
#define INFO1_FLAGS     0x031d      /* the write-0-to-clear bits */
#define INFO1_ACCESS    (INFO1_INFO0 | INFO1_INFO2)
#define INFO1_CD        (INFO1_INFO3 | INFO1_INFO4 | INFO1_INFO8 | INFO1_INFO9)

/* SD_INFO2 */
#define INFO2_ERR3      (1 << 3)
#define INFO2_ERR4      (1 << 4)
#define INFO2_ERR5      (1 << 5)
#define INFO2_ERR6      (1 << 6)    /* response timeout */
#define INFO2_DAT0      (1 << 7)
#define INFO2_BRE       (1 << 8)
#define INFO2_BWE       (1 << 9)
#define INFO2_SCLKDIVEN (1 << 13)
#define INFO2_CBSY      (1 << 14)
#define INFO2_ILA       (1 << 15)
#define INFO2_FLAGS     0x8b7f

#define ERR_STS2_E0     (1 << 0)    /* response timeout */

#define SOFT_RST_SDRST  (1 << 0)
#define CC_EXT_DMASDRW  (1 << 1)

#define SDHI_SIZE       0x100
#define SDHI_EVENT_NS   20000
#define SDHI_TIMER_FREQ_HZ 1000000000

typedef enum {
    SDHI_IDLE,
    SDHI_CMD,           /* SD_CMD written, response pending */
    SDHI_READ,          /* a block is (or is about to be) in the buffer */
    SDHI_WRITE,
} SdhiPhase;

struct RZA1HSdhiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq_cd, irq_access, irq_sdio;
    qemu_irq dma_req;           /* to dmac.c: a block buffered (read) / buffer free (write) */
    ptimer_state *timer;
    bool in_event;              /* inside sdhi_event: the ptimer transaction is already open */

    uint16_t cmd, arg0, arg1, stop, seccnt, rsp[8];
    uint16_t info1, info2, info1_mask, info2_mask;
    uint16_t clk_ctrl, size, option, err_sts1, err_sts2;
    uint16_t sdio_mode, sdio_info1, sdio_info1_mask;
    uint16_t cc_ext_mode, soft_rst, ext_swap;

    SdhiPhase phase;
    bool multi, auto_cmd12;
    uint32_t blocks_left;       /* UINT32_MAX: open-ended until SD_STOP.STP */
    uint8_t buf[512];
    uint32_t buf_pos, buf_len;
    uint64_t commands, blocks;
};

static void sdhi_update_irq(RZA1HSdhiState *s)
{
    uint16_t i1 = s->info1 & ~s->info1_mask;

    qemu_set_irq(s->irq_access, (i1 & INFO1_ACCESS) ||
                               (s->info2 & ~s->info2_mask & INFO2_FLAGS));
    qemu_set_irq(s->irq_cd, i1 & INFO1_CD);
    qemu_set_irq(s->irq_sdio, s->sdio_info1 & ~s->sdio_info1_mask & 0xc001);
    qemu_set_irq(s->dma_req, (s->cc_ext_mode & CC_EXT_DMASDRW) &&
                             (s->phase == SDHI_READ || s->phase == SDHI_WRITE) &&
                             s->buf_pos < s->buf_len);
}

/* Re-arming from the ptimer's own callback must not open a nested transaction
 * (ptimer asserts; the same trap scif.c's front-panel pacing hit). */
static void sdhi_schedule(RZA1HSdhiState *s)
{
    if (!s->in_event) {
        ptimer_transaction_begin(s->timer);
    }
    ptimer_set_count(s->timer, SDHI_EVENT_NS);
    ptimer_run(s->timer, 1);
    if (!s->in_event) {
        ptimer_transaction_commit(s->timer);
    }
}

static uint32_t sdhi_block_size(RZA1HSdhiState *s)
{
    uint32_t n = s->size & 0x3ff;

    return n ? MIN(n, sizeof(s->buf)) : sizeof(s->buf);
}

static void sdhi_send_cmd12(RZA1HSdhiState *s)
{
    SDRequest req = { .cmd = 12, .arg = 0 };
    uint8_t resp[16];

    sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));
    rza1h_debug("sdhi", "auto CMD12");
}

/* The command sequence ends: access end, bus free. */
static void sdhi_finish(RZA1HSdhiState *s, bool cmd12)
{
    if (cmd12) {
        sdhi_send_cmd12(s);
    }
    s->phase = SDHI_IDLE;
    s->info1 |= INFO1_INFO2;
    s->info2 &= ~(INFO2_BRE | INFO2_BWE);
    sdhi_update_irq(s);
}

/* Which data phase follows command idx in normal mode (SD_CMD[10:8] = 000).
 * Writes are plain CMDs only: ACMD42 (SET_CLR_CARD_DETECT) has no data,
 * unlike CMD42 (LOCK_UNLOCK). */
static SdhiPhase sdhi_normal_mode_data(RZA1HSdhiState *s, uint8_t idx, uint32_t arg)
{
    if (sdbus_data_ready(&s->sdbus)) {
        return SDHI_READ;       /* CMD17/18, CMD6, ACMD13/22/51, ... */
    }
    if (CMD_IS_ACMD(s->cmd)) {
        return SDHI_IDLE;
    }
    if (idx == 24 || idx == 25 || idx == 27 || idx == 42 ||
        (idx == 56 && !(arg & 1))) {
        return SDHI_WRITE;
    }
    return SDHI_IDLE;
}

static void sdhi_run_command(RZA1HSdhiState *s)
{
    uint8_t idx = CMD_INDEX(s->cmd);
    int mode = CMD_MODE(s->cmd);
    SDRequest req = { .cmd = idx, .arg = (uint32_t)s->arg1 << 16 | s->arg0 };
    uint8_t resp[16];
    size_t rlen;
    bool want_resp;
    SdhiPhase data;

    rlen = sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));
    s->commands++;
    want_resp = mode == 0 ? !(idx == 0 || idx == 4 || idx == 15)
                          : mode != CMD_MODE_NO_RESP;

    if (want_resp && rlen == 0) {
        rza1h_debug("sdhi", "CMD%d arg=%08x (SD_CMD=%04x): response timeout",
                   idx, req.arg, s->cmd);
        s->info2 |= INFO2_ERR6;
        s->err_sts2 |= ERR_STS2_E0;
        s->info1 |= INFO1_INFO0 | INFO1_INFO2;
        s->phase = SDHI_IDLE;
        sdhi_update_irq(s);
        return;
    }

    memset(s->rsp, 0, sizeof(s->rsp));
    if (rlen == 4) {
        /* R1/R3/R6/R7 [39:8]: the 32-bit payload */
        uint32_t r = ldl_be_p(resp);

        s->rsp[0] = r;
        s->rsp[1] = r >> 16;
    } else if (rlen == 16) {
        /* R2 [127:8]: sd.c's 16 bytes are bits 127..0, the CRC byte last */
        for (int k = 0; k < 8; k++) {
            int lo = 14 - 2 * k;    /* byte holding bits 8+16k+7 .. 8+16k */

            s->rsp[k] = resp[lo] | (lo > 0 ? resp[lo - 1] << 8 : 0);
        }
        s->rsp[7] &= 0xff;
    }
    rza1h_debug("sdhi", "%sCMD%d arg=%08x (SD_CMD=%04x): %zu-byte response %04x%04x",
               CMD_IS_ACMD(s->cmd) ? "A" : "", idx, req.arg, s->cmd, rlen, s->rsp[1], s->rsp[0]);

    if (mode == 0) {
        data = sdhi_normal_mode_data(s, idx, req.arg);
        s->multi = idx == 18 || idx == 25;
        s->auto_cmd12 = s->multi && (s->stop & STOP_SEC);
    } else if (s->cmd & CMD_MD3_DATA) {
        data = (s->cmd & CMD_MD4_READ) ? SDHI_READ : SDHI_WRITE;
        s->multi = s->cmd & CMD_MD5_MULTI;
        s->auto_cmd12 = s->multi && !(s->cmd & (CMD_MD6_NO_CMD12 | CMD_MD6_NO_CMD12 << 1)) &&
                        (s->stop & STOP_SEC);
    } else {
        data = SDHI_IDLE;
    }

    s->info1 |= INFO1_INFO0;
    if (data == SDHI_IDLE) {
        s->phase = SDHI_IDLE;
        sdhi_update_irq(s);
        return;
    }

    if (!s->multi) {
        s->blocks_left = 1;
    } else if (s->stop & STOP_SEC) {
        s->blocks_left = s->seccnt ? s->seccnt : 0x10000;
    } else {
        s->blocks_left = UINT32_MAX;
    }
    s->phase = data;
    s->buf_pos = s->buf_len = 0;
    sdhi_update_irq(s);
    sdhi_schedule(s);           /* first block ready / buffer free */
}

static void sdhi_event(void *opaque)
{
    RZA1HSdhiState *s = RZA1H_SDHI(opaque);

    s->in_event = true;
    switch (s->phase) {
    case SDHI_CMD:
        sdhi_run_command(s);
        break;
    case SDHI_READ:
        s->buf_len = sdhi_block_size(s);
        s->buf_pos = 0;
        sdbus_read_data(&s->sdbus, s->buf, s->buf_len);
        rza1h_debug("sdhi", "read block buffered (%u bytes)", s->buf_len);
        if (!(s->cc_ext_mode & CC_EXT_DMASDRW)) {
            s->info2 |= INFO2_BRE;      /* with DMA the request line says it instead */
        }
        sdhi_update_irq(s);
        break;
    case SDHI_WRITE:
        s->buf_len = sdhi_block_size(s);
        s->buf_pos = 0;
        if (!(s->cc_ext_mode & CC_EXT_DMASDRW)) {
            s->info2 |= INFO2_BWE;
        }
        sdhi_update_irq(s);
        break;
    default:
        break;
    }
    s->in_event = false;
}

static void sdhi_block_done(RZA1HSdhiState *s)
{
    s->blocks++;
    rza1h_debug("sdhi", "%s block done, %u left", s->phase == SDHI_READ ? "read" : "write",
               s->blocks_left == UINT32_MAX ? UINT32_MAX : s->blocks_left - 1);
    if (s->blocks_left != UINT32_MAX && --s->blocks_left == 0) {
        sdhi_finish(s, s->auto_cmd12);
    } else {
        sdhi_schedule(s);
    }
}

static uint32_t sdhi_buf_read(RZA1HSdhiState *s, unsigned size)
{
    uint32_t v = 0;

    if (s->phase != SDHI_READ || s->buf_pos >= s->buf_len) {
        s->info2 |= INFO2_ERR5;
        sdhi_update_irq(s);
        return 0;
    }
    for (unsigned i = 0; i < size && s->buf_pos < s->buf_len; i++) {
        v |= (uint32_t)s->buf[s->buf_pos++] << (8 * i);
    }
    if (s->buf_pos >= s->buf_len) {
        sdhi_block_done(s);
    }
    return v;
}

static void sdhi_buf_write(RZA1HSdhiState *s, uint32_t v, unsigned size)
{
    if (s->phase != SDHI_WRITE || !s->buf_len || s->buf_pos >= s->buf_len) {
        s->info2 |= INFO2_ERR4;
        sdhi_update_irq(s);
        return;
    }
    for (unsigned i = 0; i < size && s->buf_pos < s->buf_len; i++) {
        s->buf[s->buf_pos++] = v >> (8 * i);
    }
    if (s->buf_pos >= s->buf_len) {
        sdbus_write_data(&s->sdbus, s->buf, s->buf_len);
        s->buf_len = 0;
        sdhi_block_done(s);
    }
}

/* Registers with note "initial value at reset and when SDRST is 0". */
static void sdhi_soft_reset(RZA1HSdhiState *s)
{
    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_transaction_commit(s->timer);
    s->phase = SDHI_IDLE;
    s->stop = 0;
    s->seccnt = 0;
    s->info1 = 0;
    s->info2 = 0;
    s->clk_ctrl &= ~(1 << 8);   /* SCLKEN */
    s->option = 0x40ee;
    s->err_sts1 = 0x2000;
    s->err_sts2 = 0;
    s->sdio_info1 = 0;
    s->buf_pos = s->buf_len = 0;
}

static uint64_t sdhi_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HSdhiState *s = RZA1H_SDHI(opaque);
    bool inserted = sdbus_get_inserted(&s->sdbus);

    if (offset >= SD_RSP00 && offset < SD_INFO1) {
        return s->rsp[(offset - SD_RSP00) / 2];
    }
    switch (offset) {
    case SD_CMD:        return s->cmd;
    case SD_ARG0:       return s->arg0;
    case SD_ARG1:       return s->arg1;
    case SD_STOP:       return s->stop;
    case SD_SECCNT:     return s->seccnt;
    case SD_INFO1:
        return (s->info1 & INFO1_FLAGS) |
               (inserted ? INFO1_INFO5 | INFO1_INFO10 : 0) |
               (inserted && !sdbus_get_readonly(&s->sdbus) ? INFO1_INFO7 : 0);
    case SD_INFO2:
        return (s->info2 & INFO2_FLAGS) | INFO2_DAT0 |
               (s->phase == SDHI_IDLE ? INFO2_SCLKDIVEN : INFO2_CBSY);
    case SD_INFO1_MASK: return s->info1_mask;
    case SD_INFO2_MASK: return s->info2_mask;
    case SD_CLK_CTRL:   return s->clk_ctrl;
    case SD_SIZE:       return s->size;
    case SD_OPTION:     return s->option;
    case SD_ERR_STS1:   return s->err_sts1;
    case SD_ERR_STS2:   return s->err_sts2;
    case SD_BUF0:
    case SD_BUF0 + 2:   return sdhi_buf_read(s, size);
    case SDIO_MODE:     return s->sdio_mode;
    case SDIO_INFO1:    return s->sdio_info1;
    case SDIO_INFO1_MASK: return s->sdio_info1_mask;
    case CC_EXT_MODE:   return s->cc_ext_mode;
    case SOFT_RST:      return s->soft_rst;
    case VERSION:       return 0x820b;
    case EXT_SWAP:      return s->ext_swap;
    default:
        qemu_log_mask(LOG_UNIMP, "rza1h-sdhi: read of unknown offset 0x%02" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void sdhi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    RZA1HSdhiState *s = RZA1H_SDHI(opaque);
    uint16_t v = value;

    switch (offset) {
    case SD_CMD:
        if (s->phase != SDHI_IDLE) {
            s->info2 |= INFO2_ILA;
            break;
        }
        s->cmd = v;
        rza1h_debug("sdhi", "SD_CMD=%04x written: INFO1_MASK=%04x INFO2_MASK=%04x DMA=%d",
                   v, s->info1_mask, s->info2_mask, !!(s->cc_ext_mode & CC_EXT_DMASDRW));
        s->err_sts1 = 0x2000;
        s->err_sts2 = 0;
        s->phase = SDHI_CMD;
        sdhi_schedule(s);
        break;
    case SD_ARG0:       s->arg0 = v; break;
    case SD_ARG1:       s->arg1 = v; break;
    case SD_STOP:
        s->stop = v & (STOP_SEC | STOP_STP);
        if ((v & STOP_STP) && (s->phase == SDHI_READ || s->phase == SDHI_WRITE)) {
            rza1h_debug("sdhi", "SD_STOP.STP during a %s transfer",
                       s->multi ? "multi-block" : "single-block");
            sdhi_finish(s, s->multi);
        }
        break;
    case SD_SECCNT:     s->seccnt = v; break;
    case SD_INFO1:      s->info1 &= v | ~INFO1_FLAGS; break;
    case SD_INFO2:      s->info2 &= v | ~INFO2_FLAGS; break;
    case SD_INFO1_MASK: s->info1_mask = v & INFO1_FLAGS; break;
    case SD_INFO2_MASK: s->info2_mask = v & INFO2_FLAGS; break;
    case SD_CLK_CTRL:   s->clk_ctrl = v & 0x3ff; break;
    case SD_SIZE:       s->size = v & 0x3ff; break;
    case SD_OPTION:     s->option = (v & 0x80ff) | 0x4000; break;
    case SD_BUF0:
    case SD_BUF0 + 2:   sdhi_buf_write(s, value, size); break;
    case SDIO_MODE:     s->sdio_mode = v & 0x0305; break;
    case SDIO_INFO1:    s->sdio_info1 &= v | ~0xc001; break;
    case SDIO_INFO1_MASK: s->sdio_info1_mask = v & 0xc007; break;
    case CC_EXT_MODE:
        s->cc_ext_mode = (v & CC_EXT_DMASDRW) | 0x1010;
        break;
    case SOFT_RST:
        if (!(v & SOFT_RST_SDRST)) {
            sdhi_soft_reset(s);
        } else if (!(s->soft_rst & SOFT_RST_SDRST) && sdbus_get_inserted(&s->sdbus)) {
            s->info1 |= INFO1_INFO4;    /* card detect sees the inserted card */
        }
        s->soft_rst = (v & SOFT_RST_SDRST) | 0x0006;
        break;
    case EXT_SWAP:      s->ext_swap = v & 0x1900; break;
    default:
        qemu_log_mask(LOG_UNIMP, "rza1h-sdhi: write of unknown offset 0x%02" HWADDR_PRIx
                      " = 0x%04x\n", offset, v);
        break;
    }
    sdhi_update_irq(s);
}

static const MemoryRegionOps sdhi_ops = {
    .read = sdhi_read,
    .write = sdhi_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    /* Registers are 16-bit; the driver reads pairs (SD_RSP00/01, SD_INFO1/2) with one
     * 32-bit load, which memory.c splits into two 16-bit accesses, low half first. The
     * 32-bit SD_BUF0 port is then two halfword accesses at 0x30/0x32 -- in order. */
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void sdhi_set_inserted(DeviceState *dev, bool inserted)
{
    RZA1HSdhiState *s = RZA1H_SDHI(dev);

    rza1h_debug("sdhi", "card %s", inserted ? "inserted" : "removed");
    if (s->soft_rst & SOFT_RST_SDRST) {
        s->info1 |= inserted ? INFO1_INFO4 : INFO1_INFO3;
    }
    if (!inserted && s->phase != SDHI_IDLE) {
        sdhi_finish(s, false);
    }
    sdhi_update_irq(s);
}

static void sdhi_reset(DeviceState *dev)
{
    RZA1HSdhiState *s = RZA1H_SDHI(dev);

    s->cmd = s->arg0 = s->arg1 = 0;
    memset(s->rsp, 0, sizeof(s->rsp));
    s->info1_mask = 0x031d;
    s->info2_mask = 0x8b7f;
    s->clk_ctrl = 0x0020;
    s->size = 0x0200;
    s->sdio_mode = 0;
    s->sdio_info1_mask = 0xc007;
    s->cc_ext_mode = 0x1010;
    s->soft_rst = 0x0006;
    s->ext_swap = 0;
    sdhi_soft_reset(s);
    sdhi_update_irq(s);
}

static void sdhi_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HSdhiState *s = RZA1H_SDHI(obj);

    memory_region_init_io(&s->iomem, obj, &sdhi_ops, s, TYPE_RZA1H_SDHI, SDHI_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_cd);
    sysbus_init_irq(sbd, &s->irq_access);
    sysbus_init_irq(sbd, &s->irq_sdio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dma_req, "dma-req", 1);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_RZA1H_SDHI_BUS, DEVICE(obj), "sd-bus");
    s->timer = ptimer_init(sdhi_event, s, PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                         PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, SDHI_TIMER_FREQ_HZ);
    ptimer_transaction_commit(s->timer);
}

static void sdhi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, sdhi_reset);
}

static void sdhi_bus_class_init(ObjectClass *oc, const void *data)
{
    SDBusClass *sbc = SD_BUS_CLASS(oc);

    sbc->set_inserted = sdhi_set_inserted;
}

static const TypeInfo sdhi_types[] = {
    {
        .name          = TYPE_RZA1H_SDHI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RZA1HSdhiState),
        .instance_init = sdhi_init,
        .class_init    = sdhi_class_init,
    },
    {
        .name          = TYPE_RZA1H_SDHI_BUS,
        .parent        = TYPE_SD_BUS,
        .instance_size = sizeof(SDBus),
        .class_init    = sdhi_bus_class_init,
    },
};

DEFINE_TYPES(sdhi_types)
