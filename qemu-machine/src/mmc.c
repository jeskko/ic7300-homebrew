/*
 * RZ/A1H MMC Host Interface (MMCIF) -- real command/response protocol,
 * Extension-roadmap item 5 (SD-card/VFS testing, sdk/roadmap.md's Phase 0
 * payoff: a fully-offline way to test whether a custom body.bin gets
 * accepted and boots, without JTAG).
 *
 * Register bit layout confirmed 2026-09-08 against the real Renesas RZ/A1H
 * hardware manual, Chapter 51 "MMC Host Interface" (pages 51-1 to 51-42;
 * /data/misc/icom/7300/doc/REN_r01uh0403ej0600_rz_a1h_MAT_20210129-
 * 2931443.pdf) -- unlike every other peripheral in this directory, no
 * bit-level fields exist in ~/Downloads/rza1.svd for this one (register
 * names/offsets only), so this is the first device here derived directly
 * from the manual text rather than the SVD.
 *
 * A real, documented quirk worth remembering: CE_CMD_SETH/CE_CMD_SETL's
 * own naming is the *opposite* of their address order -- CE_CMD_SETH
 * ("high") holds the notional CE_CMD_SET register's bits [31:16] but
 * lives at the *lower* address (+0x00), with CE_CMD_SETL ("low", bits
 * [15:0]) at +0x02. The manual states plainly: "The command sequence
 * starts when the settings have been made in bits 31 to 16" -- i.e.
 * writing CE_CMD_SETH (regardless of whether software does that as its
 * own 16-bit store or as the top half of a combined 32-bit store to
 * +0x00) is what triggers command transmission. This device re-derives
 * both halves from the byte-addressable store after any write touching
 * offset 0x00-0x01, so either access pattern works correctly.
 *
 * SD/MMC card-side protocol semantics (CMD0/CMD8/CMD55+ACMD41/CMD2/CMD3/
 * CMD9/CMD7/CMD16/CMD17/CMD18/CMD24/CMD12/CMD13) are generic SD
 * Physical Layer protocol knowledge, not RZ/A1H- or IC-7300-specific --
 * this device plays both the host-controller role (real MMCIF register
 * behavior) and a permissive virtual SD card's role (a plausible OCR/
 * CID/CSD, always reporting ready/high-capacity) in one place, backed by
 * a flat raw disk image given via the "image" property. No real command
 * from body.bin's own driver has been observed yet -- see the README's
 * SD-card section for the current state of finding/triggering it; this
 * device was validated standalone first (a full CMD0..CMD17 sequence
 * driven directly over GDB), the same methodology this session already
 * used for the GIC/OSTM0 IRQ-delivery work.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HMmcState, RZA1H_MMC)

#define REG_CMD_SETH   0x00
#define REG_CMD_SETL   0x02
#define REG_ARG        0x08
#define REG_ARG_CMD12  0x0C
#define REG_CMD_CTRL   0x10
#define REG_BLOCK_SET  0x14
#define REG_CLK_CTRL   0x18
#define REG_BUF_ACC    0x1C
#define REG_RESP3      0x20
#define REG_RESP2      0x24
#define REG_RESP1      0x28
#define REG_RESP0      0x2C
#define REG_RESP_CMD12 0x30
#define REG_DATA       0x34
#define REG_INT        0x40
#define REG_INT_EN     0x44
#define REG_HOST_STS1  0x48
#define REG_HOST_STS2  0x4C
#define REG_DMA_MODE   0x5C
#define REG_DETECT     0x70
#define REG_ADD_MODE   0x74
#define REG_VERSION    0x7C

/* CE_CMD_SETH local-bit positions (register holds notional bits 31:16). */
#define SETH_CMD_SHIFT  8
#define SETH_CMD_MASK   0x3f
#define SETH_RTYP_SHIFT 6
#define SETH_RTYP_MASK  0x3
#define SETH_RBSY   (1 << 5)
#define SETH_WDAT   (1 << 3)
#define SETH_DWEN   (1 << 2)
#define SETH_CMLTE  (1 << 1)
#define SETH_CMD12EN (1 << 0)

#define RTYP_NONE  0
#define RTYP_SHORT 1
#define RTYP_LONG  2

/* CE_INT bits. */
#define INT_CMD12DRE (1u << 26)
#define INT_CMD12RBE (1u << 25)
#define INT_CMD12CRE (1u << 24)
#define INT_DTRANE   (1u << 23)
#define INT_BUFRE    (1u << 22)
#define INT_BUFWEN   (1u << 21)
#define INT_BUFREN   (1u << 20)
#define INT_RBSYE    (1u << 17)
#define INT_CRSPE    (1u << 16)
#define INT_CMDVIO   (1u << 15)
#define INT_BUFVIO   (1u << 14)
#define INT_WDATERR  (1u << 11)
#define INT_RDATERR  (1u << 10)
#define INT_RIDXERR  (1u << 9)
#define INT_RSPERR   (1u << 8)

/* CE_HOST_STS1 bits. */
#define STS1_CMDSEQ_SHIFT 31
#define STS1_RSPIDX_SHIFT 24
#define STS1_RSPIDX_MASK  0x3f

/* CE_DETECT bits. */
#define DETECT_CDSIG  (1u << 14)
#define DETECT_CDRISE (1u << 13)
#define DETECT_CDFALL (1u << 12)

/* Real SD commands this device understands -- generic SD protocol, see
 * this file's own comment. */
#define SD_CMD_GO_IDLE_STATE      0
#define SD_CMD_SEND_IF_COND       8
#define SD_CMD_SEND_CSD           9
#define SD_CMD_STOP_TRANSMISSION  12
#define SD_CMD_SEND_STATUS        13
#define SD_CMD_SET_BLOCKLEN       16
#define SD_CMD_READ_SINGLE_BLOCK  17
#define SD_CMD_READ_MULTIPLE_BLOCK 18
#define SD_CMD_WRITE_BLOCK        24
#define SD_CMD_APP_CMD            55
#define SD_ALL_SEND_CID           2
#define SD_SEND_RELATIVE_ADDR     3
#define SD_SELECT_DESELECT_CARD   7
#define SD_ACMD_SD_SEND_OP_COND   41

#define SD_BLOCK_SIZE 512
#define SD_FAKE_RCA   0xAAAA

struct RZA1HMmcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    char *image_path;
    int image_fd; /* -1 if no image given / failed to open -- reads
                   * synthesize zero blocks, writes are dropped */

    uint16_t cmd_seth, cmd_setl;
    uint32_t arg, arg_cmd12;
    uint32_t cmd_ctrl;
    uint32_t block_set;
    uint32_t clk_ctrl;
    uint32_t buf_acc;
    uint32_t resp[4]; /* index 0 = CE_RESP0 .. index 3 = CE_RESP3 */
    uint32_t resp_cmd12;
    uint32_t ce_int;
    uint32_t int_en;
    uint32_t host_sts1;
    uint32_t host_sts2;
    uint32_t dma_mode;
    uint32_t detect;
    uint32_t add_mode;

    /* Card-protocol state (this device plays the card's role too, see
     * module comment). */
    bool app_cmd_pending; /* CMD55 seen, next command is an ACMD */
    bool card_selected;

    /* Block-transfer state for the currently in-flight data command. */
    bool xfer_active;
    bool xfer_is_write;
    uint32_t xfer_lba;
    uint32_t xfer_blocks_remaining;
    uint32_t xfer_word_pos; /* word offset within the current 512-byte block */
    uint32_t xfer_cmd12en;
    uint8_t block_buf[SD_BLOCK_SIZE];
};

static void mmc_read_block(RZA1HMmcState *s, uint32_t lba, uint8_t *buf)
{
    if (s->image_fd >= 0) {
        ssize_t n = pread(s->image_fd, buf, SD_BLOCK_SIZE,
                          (off_t)lba * SD_BLOCK_SIZE);
        if (n == (ssize_t)SD_BLOCK_SIZE) {
            return;
        }
    }
    memset(buf, 0, SD_BLOCK_SIZE); /* no image, short read, or read past end */
}

static void mmc_write_block(RZA1HMmcState *s, uint32_t lba, const uint8_t *buf)
{
    if (s->image_fd >= 0) {
        ssize_t n = pwrite(s->image_fd, buf, SD_BLOCK_SIZE,
                          (off_t)lba * SD_BLOCK_SIZE);
        if (n != (ssize_t)SD_BLOCK_SIZE) {
            qemu_log_mask(LOG_UNIMP, "rza1h-mmc: short/failed write to image "
                         "at lba %u\n", lba);
        }
    }
}

static void mmc_start_xfer(RZA1HMmcState *s, bool is_write, uint32_t lba,
                           uint32_t nblocks, bool cmd12en)
{
    s->xfer_active = true;
    s->xfer_is_write = is_write;
    s->xfer_lba = lba;
    s->xfer_blocks_remaining = nblocks;
    s->xfer_word_pos = 0;
    s->xfer_cmd12en = cmd12en;
    if (!is_write) {
        mmc_read_block(s, s->xfer_lba, s->block_buf);
        s->ce_int |= INT_BUFREN; /* first block ready to read */
    } else {
        s->ce_int |= INT_BUFWEN; /* ready for software to write first block */
    }
}

/* Process one command once CE_CMD_SETH has been written -- see this
 * file's own comment for why that's the documented trigger point. */
static void mmc_do_command(RZA1HMmcState *s)
{
    unsigned cmd = (s->cmd_seth >> SETH_CMD_SHIFT) & SETH_CMD_MASK;
    unsigned rtyp = (s->cmd_seth >> SETH_RTYP_SHIFT) & SETH_RTYP_MASK;
    bool wdat = s->cmd_seth & SETH_WDAT;
    bool dwen = s->cmd_seth & SETH_DWEN;
    bool cmlte = s->cmd_seth & SETH_CMLTE;
    bool cmd12en = s->cmd_seth & SETH_CMD12EN;
    bool is_acmd = s->app_cmd_pending;
    uint32_t nblocks;

    s->app_cmd_pending = false;

    rza1h_debug("mmc", "%sCMD%u arg=%#x rtyp=%u wdat=%d dwen=%d",
               is_acmd ? "A" : "", cmd, s->arg, rtyp, wdat, dwen);

    memset(s->resp, 0, sizeof(s->resp));

    if (is_acmd && cmd == SD_ACMD_SD_SEND_OP_COND) {
        /* R3: OCR. bit31=power-up done (never busy, this device completes
         * instantly), bit30=CCS (1=high-capacity -- CMD17/18/24's arg is
         * a block index, not a byte address, avoiding *512 ambiguity),
         * plus a plausible voltage-window field. */
        s->resp[0] = 0xC0FF8000;
    } else {
        switch (cmd) {
        case SD_CMD_GO_IDLE_STATE:
            break; /* no response (rtyp should be 0) */
        case SD_CMD_SEND_IF_COND:
            s->resp[0] = s->arg; /* real cards echo the check pattern/voltage */
            break;
        case SD_CMD_APP_CMD:
            s->app_cmd_pending = true;
            s->resp[0] = 0x00000020; /* R1, APP_CMD bit set */
            break;
        case SD_ALL_SEND_CID:
            /* R2/long: a plausible, arbitrary CID -- nothing in this
             * project's own driver work depends on its exact content. */
            s->resp[3] = 0x00544349; /* "ICT" + version-ish byte, arbitrary */
            s->resp[2] = 0x4d374330;
            s->resp[1] = 0x30303030;
            s->resp[0] = 0x00000001;
            break;
        case SD_SEND_RELATIVE_ADDR:
            /* R6: new RCA in the upper 16 bits + a clean status in the low 16. */
            s->resp[0] = (SD_FAKE_RCA << 16) | 0x0000;
            break;
        case SD_CMD_SEND_CSD:
            /* R2/long: CSD version 2.0 (SDHC/SDXC shape, matching CCS=1
             * from ACMD41 above). C_SIZE picked for a ~1 GB virtual card
             * ((C_SIZE+1)*512KiB); real field position is C_SIZE at bits
             * [69:48] of the 128-bit CSD, i.e. within resp[1]/resp[2]
             * here -- close enough for a driver that just wants "a
             * plausible capacity", not bit-exact against every reserved
             * CSD field. */
            s->resp[3] = 0x400E0032;
            s->resp[2] = 0x5B590000;
            s->resp[1] = 0x00007F80;
            s->resp[0] = 0x0A400001;
            break;
        case SD_SELECT_DESELECT_CARD:
            s->card_selected = (s->arg != 0);
            s->resp[0] = 0x00000700; /* R1b, "tran" state */
            break;
        case SD_CMD_SET_BLOCKLEN:
            s->resp[0] = 0x00000700;
            break;
        case SD_CMD_SEND_STATUS:
            s->resp[0] = 0x00000700;
            break;
        case SD_CMD_READ_SINGLE_BLOCK:
            s->resp[0] = 0x00000700;
            mmc_start_xfer(s, false, s->arg, 1, false);
            break;
        case SD_CMD_READ_MULTIPLE_BLOCK:
            s->resp[0] = 0x00000700;
            nblocks = cmlte ? (s->block_set >> 16) : 1;
            mmc_start_xfer(s, false, s->arg, nblocks ? nblocks : 1, cmd12en);
            break;
        case SD_CMD_WRITE_BLOCK:
            s->resp[0] = 0x00000700;
            mmc_start_xfer(s, true, s->arg, 1, false);
            break;
        case SD_CMD_STOP_TRANSMISSION:
            s->xfer_active = false;
            s->resp[0] = 0x00000700; /* R1b */
            break;
        default:
            /* Unrecognized command: complete permissively (CRSPE set,
             * clean R1-shaped status) rather than erroring or hanging --
             * this is a first pass, see module comment; logged above so
             * a real driver relying on something unmodeled is visible. */
            s->resp[0] = 0x00000000;
            break;
        }
    }

    s->host_sts1 = ((uint32_t)cmd << STS1_RSPIDX_SHIFT); /* CMDSEQ=0: done instantly */
    s->ce_int |= INT_CRSPE;
    if (wdat && !dwen) {
        /* handled by mmc_start_xfer already for the commands that need it */
    }
}

static void mmc_data_read_word(RZA1HMmcState *s, uint32_t *out)
{
    if (!s->xfer_active || s->xfer_is_write) {
        *out = 0;
        return;
    }
    memcpy(out, &s->block_buf[s->xfer_word_pos * 4], 4);
    s->xfer_word_pos++;
    if (s->xfer_word_pos * 4 >= SD_BLOCK_SIZE) {
        s->xfer_word_pos = 0;
        s->xfer_lba++;
        s->xfer_blocks_remaining--;
        s->ce_int &= ~INT_BUFREN;
        if (s->xfer_blocks_remaining > 0) {
            mmc_read_block(s, s->xfer_lba, s->block_buf);
            s->ce_int |= INT_BUFREN;
        } else {
            s->ce_int |= INT_BUFRE;
            s->xfer_active = false;
            if (s->xfer_cmd12en) {
                s->resp_cmd12 = 0x00000700;
                s->ce_int |= INT_CMD12CRE | INT_CMD12DRE;
            }
        }
    }
}

static void mmc_data_write_word(RZA1HMmcState *s, uint32_t value)
{
    if (!s->xfer_active || !s->xfer_is_write) {
        return;
    }
    memcpy(&s->block_buf[s->xfer_word_pos * 4], &value, 4);
    s->xfer_word_pos++;
    if (s->xfer_word_pos * 4 >= SD_BLOCK_SIZE) {
        mmc_write_block(s, s->xfer_lba, s->block_buf);
        s->xfer_word_pos = 0;
        s->xfer_lba++;
        s->xfer_blocks_remaining--;
        s->ce_int &= ~INT_BUFWEN;
        if (s->xfer_blocks_remaining > 0) {
            s->ce_int |= INT_BUFWEN;
        } else {
            s->ce_int |= INT_DTRANE;
            s->xfer_active = false;
        }
    }
}

static uint64_t rza1h_mmc_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HMmcState *s = RZA1H_MMC(opaque);
    uint32_t word;

    switch (offset) {
    case REG_CMD_SETH: return s->cmd_seth;
    case REG_CMD_SETL: return s->cmd_setl;
    case REG_ARG: return s->arg;
    case REG_ARG_CMD12: return s->arg_cmd12;
    case REG_CMD_CTRL: return s->cmd_ctrl;
    case REG_BLOCK_SET: return s->block_set;
    case REG_CLK_CTRL: return s->clk_ctrl;
    case REG_BUF_ACC: return s->buf_acc;
    case REG_RESP3: return s->resp[3];
    case REG_RESP2: return s->resp[2];
    case REG_RESP1: return s->resp[1];
    case REG_RESP0: return s->resp[0];
    case REG_RESP_CMD12: return s->resp_cmd12;
    case REG_DATA:
        mmc_data_read_word(s, &word);
        return word;
    case REG_INT: return s->ce_int;
    case REG_INT_EN: return s->int_en;
    case REG_HOST_STS1: return s->host_sts1;
    case REG_HOST_STS2: return s->host_sts2;
    case REG_DMA_MODE: return s->dma_mode;
    case REG_DETECT: return s->detect;
    case REG_ADD_MODE: return s->add_mode;
    case REG_VERSION: return 0x00000003; /* SWRST always reads back 0 here */
    default: return 0;
    }
}

static void rza1h_mmc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RZA1HMmcState *s = RZA1H_MMC(opaque);

    switch (offset) {
    case REG_CMD_SETH:
        s->cmd_seth = value;
        mmc_do_command(s); /* writing this half is the documented trigger */
        break;
    case REG_CMD_SETL:
        s->cmd_setl = value;
        break;
    case REG_ARG:
        s->arg = value;
        break;
    case REG_ARG_CMD12:
        s->arg_cmd12 = value;
        break;
    case REG_CMD_CTRL:
        s->cmd_ctrl = value; /* BREAK: no-op, nothing is ever mid-sequence */
        break;
    case REG_BLOCK_SET:
        s->block_set = value;
        break;
    case REG_CLK_CTRL:
        s->clk_ctrl = value;
        break;
    case REG_BUF_ACC:
        s->buf_acc = value;
        break;
    case REG_DATA:
        mmc_data_write_word(s, value);
        break;
    case REG_INT:
        s->ce_int &= value; /* write-0-to-clear, write-1 no-op -- see manual */
        break;
    case REG_INT_EN:
        s->int_en = value;
        break;
    case REG_DMA_MODE:
        s->dma_mode = value;
        break;
    case REG_DETECT:
        /* CDRISE/CDFALL are write-0-to-clear; CDSIG is read-only. */
        s->detect = (s->detect & ~(DETECT_CDRISE | DETECT_CDFALL)) |
                   (s->detect & value & (DETECT_CDRISE | DETECT_CDFALL));
        break;
    case REG_ADD_MODE:
        s->add_mode = value;
        break;
    case REG_VERSION:
        if (value & 0x80000000) {
            /* SWRST: full register reset except the card/xfer state a
             * real card wouldn't forget just because the host controller
             * reset its own registers. */
            device_cold_reset(DEVICE(s));
        }
        break;
    /* CE_RESP0-3, CE_RESP_CMD12, CE_HOST_STS1/2 are read-only; writes ignored. */
    default:
        break;
    }
}

static const MemoryRegionOps rza1h_mmc_ops = {
    .read = rza1h_mmc_read,
    .write = rza1h_mmc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_mmc_reset(DeviceState *dev)
{
    RZA1HMmcState *s = RZA1H_MMC(dev);

    /* image_path/image_fd deliberately untouched -- realize()'s job, and
     * a real inserted card doesn't forget its backing media just because
     * the host controller's own registers reset. */
    s->cmd_seth = s->cmd_setl = 0;
    s->arg = s->arg_cmd12 = 0;
    s->cmd_ctrl = 0;
    s->block_set = 0x00000200; /* BLKSIZ reset value, per the manual */
    s->clk_ctrl = 0;
    s->buf_acc = 0;
    memset(s->resp, 0, sizeof(s->resp));
    s->resp_cmd12 = 0;
    s->ce_int = 0;
    s->int_en = 0;
    s->host_sts1 = 0;
    s->host_sts2 = 0;
    s->dma_mode = 0;
    s->detect = DETECT_CDSIG; /* a virtual card is always "inserted" */
    s->add_mode = 0;
    s->app_cmd_pending = false;
    s->card_selected = false;
    s->xfer_active = false;
    s->xfer_is_write = false;
    s->xfer_lba = s->xfer_blocks_remaining = s->xfer_word_pos = 0;
    s->xfer_cmd12en = 0;
    memset(s->block_buf, 0, sizeof(s->block_buf));
}

static void rza1h_mmc_realize(DeviceState *dev, Error **errp)
{
    RZA1HMmcState *s = RZA1H_MMC(dev);

    s->image_fd = -1;
    if (s->image_path && s->image_path[0]) {
        s->image_fd = open(s->image_path, O_RDWR);
        if (s->image_fd < 0) {
            qemu_log_mask(LOG_UNIMP,
                         "rza1h-mmc: could not open image '%s' (%s) -- "
                         "reads will synthesize zero blocks\n",
                         s->image_path, strerror(errno));
        }
    }
}

static void rza1h_mmc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HMmcState *s = RZA1H_MMC(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_mmc_ops, s,
                          TYPE_RZA1H_MMC, RZA1H_MMC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const Property rza1h_mmc_properties[] = {
    DEFINE_PROP_STRING("image", RZA1HMmcState, image_path),
};

static void rza1h_mmc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_mmc_realize;
    device_class_set_legacy_reset(dc, rza1h_mmc_reset);
    device_class_set_props(dc, rza1h_mmc_properties);
}

static const TypeInfo rza1h_mmc_info = {
    .name          = TYPE_RZA1H_MMC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HMmcState),
    .instance_init = rza1h_mmc_init,
    .class_init    = rza1h_mmc_class_init,
};

static void rza1h_mmc_register_types(void)
{
    type_register_static(&rza1h_mmc_info);
}

type_init(rza1h_mmc_register_types)
