/*
 * RZ/A1H SPI multi I/O bus controller (SPIBSC0) + the IC-7300's boot flash (IC391, EN25Q64).
 *
 * Replaces spi_boot.c's one-register "always ready" stub (2026-09-25) so the firmware's own
 * SD-card updater can erase and program flash. External address space read mode (XIP at
 * 0x18000000) stays what it was: the flash contents are the "rza1h.flash" ROM region that
 * -kernel loads, read directly by the CPU. This device adds SPI operating mode, the register
 * sequence body.bin's flash driver uses (spi_flash_program 0x20024a60, spi_flash_erase_range
 * 0x20024bb8, status polls 0x2002497c/0x20024904; notes/firmware-update.md):
 *
 *   SMCMR[23:16] command, SMADR address, SMENR enables (CDE bit 14, ADE bits 11:8 = 0x7 for
 *   24-bit / 0xf for 32-bit, SPIDE bits 3:0 = 0x8/0xc/0xf for 8/16/32 data bits), SMWDR0 write
 *   data, SMRDR0 read data; SMCR = SPIE (bit 0, start) | SPIWE (1) | SPIRE (2) | SSLKP (8, keep
 *   SSL asserted after this transfer); CMNSR bit 0 TEND always 1 (transfers are instant).
 *
 * One SSL-asserted stretch is one flash command: the first transfer carries the opcode (+
 * address), later transfers only data, and the command completes when SSL is released. Data
 * words go to flash lowest byte first, so XIP reads return what the firmware had in RAM (it
 * memcmp()s flash against its buffer afterwards). NOR semantics: program ANDs, erase sets 0xff.
 *
 * Flash commands: 06 WREN, 04 WRDI, 05 RDSR, 01 WRSR, 9F RDID (1C 70 17), 03/0B read,
 * 02 page program (wraps in the 256-byte page), 20 4K erase, D8 64K erase, C7/60 chip erase.
 * Program/erase need WEL, which they clear. The flash is never busy (WIP reads 0).
 *
 * `-global rza1h-spibsc.save-file=PATH` writes the whole 8 MiB flash to PATH after every
 * erase/program, so an update's result survives into the next run (-kernel PATH).
 *
 * RZA1H_DEBUG=spibsc logs each command.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HSpibscState, RZA1H_SPIBSC)

#define SPIBSC_SIZE   0x100
#define SMCR          0x20
#define SMCMR         0x24
#define SMADR         0x28
#define SMENR         0x30
#define SMRDR0        0x38
#define SMWDR0        0x40
#define CMNSR         0x48

#define SMCR_SPIE     (1 << 0)
#define SMCR_SPIWE    (1 << 1)
#define SMCR_SPIRE    (1 << 2)
#define SMCR_SSLKP    (1 << 8)
#define SMENR_CDE     (1 << 14)
#define CMNSR_TEND    (1 << 0)

#define FLASH_SIZE    (8 * 1024 * 1024)     /* EN25Q64 */
#define FLASH_PAGE    256
#define SR_WIP        (1 << 0)
#define SR_WEL        (1 << 1)

struct RZA1HSpibscState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[SPIBSC_SIZE / 4];
    char *save_file;

    bool ssl;                   /* a command is open (SSL asserted) */
    uint8_t cmd;
    uint32_t addr;              /* flash byte address the next data byte goes to / comes from */
    uint8_t sr;                 /* status register */
    uint8_t rdid_pos;
    uint32_t prog_bytes;
    bool dirty;
};

static void flash_read(uint32_t addr, uint8_t *buf, int len)
{
    address_space_read(&address_space_memory, RZA1H_FLASH_BASE + (addr % FLASH_SIZE),
                       MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void flash_write(uint32_t addr, const uint8_t *buf, int len)
{
    address_space_write_rom(&address_space_memory, RZA1H_FLASH_BASE + (addr % FLASH_SIZE),
                            MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void spibsc_save(RZA1HSpibscState *s)
{
    g_autofree uint8_t *img = NULL;
    g_autoptr(GError) err = NULL;

    if (!s->dirty || !s->save_file || !*s->save_file) {
        return;
    }
    img = g_malloc(FLASH_SIZE);
    flash_read(0, img, FLASH_SIZE);
    if (!g_file_set_contents(s->save_file, (const char *)img, FLASH_SIZE, &err)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rza1h-spibsc: saving flash to %s: %s\n",
                      s->save_file, err->message);
    }
    s->dirty = false;
}

static void flash_erase(RZA1HSpibscState *s, uint32_t start, uint32_t len)
{
    g_autofree uint8_t *ff = g_malloc(len);

    memset(ff, 0xff, len);
    flash_write(start, ff, len);
    s->dirty = true;
}

/* The command's opcode (+ address) has just been clocked out with SSL asserted. */
static void flash_begin(RZA1HSpibscState *s, uint8_t cmd, uint32_t addr, bool have_addr)
{
    s->cmd = cmd;
    s->addr = addr % FLASH_SIZE;
    s->rdid_pos = 0;
    s->prog_bytes = 0;
    switch (cmd) {
    case 0x06:
        s->sr |= SR_WEL;
        break;
    case 0x04:
        s->sr &= ~SR_WEL;
        break;
    case 0x20:
    case 0xd8:
        if (!have_addr || !(s->sr & SR_WEL)) {
            break;
        }
        {
            uint32_t size = cmd == 0x20 ? 0x1000 : 0x10000;
            uint32_t start = s->addr & ~(size - 1);

            rza1h_debug("spibsc", "erase %uK at 0x%06x", size / 1024, start);
            flash_erase(s, start, size);
        }
        s->sr &= ~SR_WEL;
        break;
    case 0xc7:
    case 0x60:
        if (s->sr & SR_WEL) {
            rza1h_debug("spibsc", "chip erase");
            flash_erase(s, 0, FLASH_SIZE);
        }
        s->sr &= ~SR_WEL;
        break;
    default:
        break;
    }
}

static void flash_data_out(RZA1HSpibscState *s, uint8_t b)      /* host -> flash */
{
    switch (s->cmd) {
    case 0x01:                  /* WRSR: keep only the block-protect bits' storage */
        if (s->sr & SR_WEL) {
            s->sr = (b & 0xbc) | (s->sr & SR_WEL);
        }
        break;
    case 0x02:
        if (s->sr & SR_WEL) {
            uint8_t old;
            uint32_t page = s->addr & ~(FLASH_PAGE - 1);

            flash_read(s->addr, &old, 1);
            old &= b;
            flash_write(s->addr, &old, 1);
            s->addr = page | ((s->addr + 1) & (FLASH_PAGE - 1));
            s->prog_bytes++;
            s->dirty = true;
        }
        break;
    default:
        break;
    }
}

static uint8_t flash_data_in(RZA1HSpibscState *s)               /* flash -> host */
{
    static const uint8_t rdid[3] = { 0x1c, 0x70, 0x17 };
    uint8_t b = 0xff;

    switch (s->cmd) {
    case 0x05:
        return s->sr;
    case 0x9f:
        return s->rdid_pos < 3 ? rdid[s->rdid_pos++] : 0;
    case 0x03:
    case 0x0b:
        flash_read(s->addr, &b, 1);
        s->addr = (s->addr + 1) % FLASH_SIZE;
        return b;
    default:
        return 0xff;
    }
}

static void flash_end(RZA1HSpibscState *s)
{
    if (s->cmd == 0x02 || s->cmd == 0x01) {
        if (s->cmd == 0x02 && s->prog_bytes) {
            rza1h_debug("spibsc", "page program: %u bytes up to 0x%06x", s->prog_bytes,
                       s->addr);
        }
        s->sr &= ~SR_WEL;
    }
    s->ssl = false;
    spibsc_save(s);
}

static void spibsc_transfer(RZA1HSpibscState *s, uint32_t smcr)
{
    uint32_t enr = s->regs[SMENR / 4];
    int nbytes = 0;

    if ((enr & SMENR_CDE) && !s->ssl) {
        int ade = (enr >> 8) & 0xf;

        s->ssl = true;
        flash_begin(s, (s->regs[SMCMR / 4] >> 16) & 0xff, s->regs[SMADR / 4],
                    ade == 0x7 || ade == 0xf);
    } else if (!s->ssl) {
        s->ssl = true;      /* data-only transfer with no open command: talk to nothing */
        s->cmd = 0;
    }
    switch (enr & 0xf) {
    case 0x8: nbytes = 1; break;
    case 0xc: nbytes = 2; break;
    case 0xf: nbytes = 4; break;
    default: break;
    }
    if (nbytes && (smcr & SMCR_SPIWE)) {
        uint32_t w = s->regs[SMWDR0 / 4];

        for (int i = 0; i < nbytes; i++) {
            flash_data_out(s, w >> (8 * i));
        }
    }
    if (nbytes && (smcr & SMCR_SPIRE)) {
        uint32_t r = 0;

        for (int i = 0; i < nbytes; i++) {
            r |= (uint32_t)flash_data_in(s) << (8 * i);
        }
        s->regs[SMRDR0 / 4] = r;
    }
    if (!(smcr & SMCR_SSLKP)) {
        flash_end(s);
    }
}

static uint64_t spibsc_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HSpibscState *s = RZA1H_SPIBSC(opaque);
    uint32_t v;

    if (offset == CMNSR) {
        return CMNSR_TEND;      /* bit 0 ready -- also what base.dat's boot poll waits for */
    }
    v = s->regs[offset / 4];
    return extract32(v, (offset & 3) * 8, size * 8);
}

static void spibsc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    RZA1HSpibscState *s = RZA1H_SPIBSC(opaque);
    uint32_t *r = &s->regs[offset / 4];

    *r = deposit32(*r, (offset & 3) * 8, size * 8, value);
    if ((offset & ~3) == SMCR && (*r & SMCR_SPIE)) {
        spibsc_transfer(s, *r);
        *r &= ~SMCR_SPIE;
    }
}

static const MemoryRegionOps spibsc_ops = {
    .read = spibsc_read,
    .write = spibsc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void spibsc_reset(DeviceState *dev)
{
    RZA1HSpibscState *s = RZA1H_SPIBSC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ssl = false;
    s->sr = 0;
}

static void spibsc_init(Object *obj)
{
    RZA1HSpibscState *s = RZA1H_SPIBSC(obj);

    memory_region_init_io(&s->iomem, obj, &spibsc_ops, s, TYPE_RZA1H_SPIBSC, SPIBSC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property spibsc_props[] = {
    DEFINE_PROP_STRING("save-file", RZA1HSpibscState, save_file),
};

static void spibsc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, spibsc_reset);
    device_class_set_props(dc, spibsc_props);
}

static const TypeInfo spibsc_info = {
    .name          = TYPE_RZA1H_SPIBSC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HSpibscState),
    .instance_init = spibsc_init,
    .class_init    = spibsc_class_init,
};

static void spibsc_register_types(void)
{
    type_register_static(&spibsc_info);
}

type_init(spibsc_register_types)
