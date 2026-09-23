/*
 * RZ/A1H VDC5 channel 0 (LCD controller, 0xFCFF7400) -- register storage plus a
 * free-running frame-timing interrupt source. Nothing is composited or scanned
 * out; this only reproduces "the panel finished another frame".
 *
 * 2026-09-23 (icom-openvg-rendering thread). Why this exists, end to end:
 *
 *   With the factory-default "Opening Message = ON" setting (reset-table item
 *   0x70, EEPROM 0x1a8f -- see tools/build_riic_eeprom_image.py),
 *   system_mode_request_dispatch runs the boot splash fade driver
 *   (FUN_2002a2a4 -> opening_screen_build_frame), which posts a render request
 *   (*0x2039064c = 2). ui_graphics_lifecycle_task then draws the first real
 *   frame this project has ever seen (~340 OpenVG command words), swaps, and
 *   inside ui_graphics_present_frame calls FUN_2007ee08:
 *       *0x20390634 = 1; wait(*(0x20390634 + 0x14), forever);
 *   i.e. "wait for the panel to pick up the new buffer". The only signaller is
 *   FUN_2007ee20 (give + clear the flag), reached only from FUN_20073878 --
 *   byte-for-byte Renesas' own VDC_Ch0_vline_ISR (r_vdc_interrupt.c in the
 *   RZ/A1H software package under scratch/): SYSCNT_INT4 = enable,
 *   SYSCNT_INT1 = status, bit 12 = VLINE, callback type 3 = VDC_INT_TYPE_VLINE,
 *   GIC 78 = INTC_ID_GR3_VLINE0. Located live from a QMP pmemsave of the
 *   blocked task's saved context frame (PC in the yield trap, return address
 *   0x2007eed4 right after `bl FUN_2007ee08`).
 *
 *   Before this device VDC50 was a plain-RAM region: no status bit ever set,
 *   no GIC line existed, so the render task blocked forever after its very
 *   first frame and the splash (and everything behind it) never progressed.
 *
 * Interrupt layout (straight from r_vdc_interrupt.c / r_intc.h, channel 0):
 *   type i (0..22) -> GIC 75 + i. Status/enable register pairs:
 *     types  0..7   SYSCNT_INT1 (+0x680) / SYSCNT_INT4 (+0x68c)
 *     types  8..15  SYSCNT_INT2 (+0x684) / SYSCNT_INT5 (+0x690)
 *     types 16..22  SYSCNT_INT3 (+0x688) / SYSCNT_INT6 (+0x694)
 *   Status bits are write-0-to-clear / write-1-no-effect: the Renesas ISR
 *   clears with `INT1 = enable & ~bit` and then writes `INT1 = enable` back,
 *   which a plain store would turn into an interrupt storm.
 *
 * Each frame tick latches the output-timing events only (S0/S1/OIR LO_VSYNC,
 * GR3 VLINE, OIR VLINE). Input-capture vsyncs, field/write-line events and
 * all error bits are never raised -- there is no video input, and nothing is
 * ever late. The VLINE line-number register is ignored: one event per frame
 * is what the driver waits for, wherever in the frame it lands.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "ui/console.h"
#include "system/address-spaces.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HVdc5State, RZA1H_VDC5)

#define VDC5_NUM_INT        23
#define VDC5_SYSCNT_INT1    0x680   /* status 1..3 at +0x680/0x684/0x688 */
#define VDC5_SYSCNT_INT4    0x68c   /* enable 4..6 at +0x68c/0x690/0x694 */
#define VDC5_FRAME_HZ       60

struct RZA1HVdc5State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    ptimer_state *timer;
    qemu_irq irq[VDC5_NUM_INT];
    uint32_t status[3];
    uint32_t enable[3];
    uint64_t frames;
    QemuConsole *con;
    uint8_t regs[RZA1H_VDC50_SIZE];
};

/* interrupt_bit_table[] from r_vdc_interrupt.c, VDC_INT_TYPE_* order. */
static const uint32_t vdc5_int_bit[VDC5_NUM_INT] = {
    0x00000001, 0x00000010, 0x00000100, 0x00001000,     /* INT1 */
    0x00010000, 0x00100000, 0x01000000, 0x10000000,
    0x00000001, 0x00000010, 0x00000100, 0x00001000,     /* INT2 */
    0x00010000, 0x00100000, 0x01000000, 0x10000000,
    0x00000001, 0x00000010, 0x00000100, 0x00001000,     /* INT3 */
    0x00010000, 0x00100000, 0x01000000,
};

/* Per-frame events: S0_LO_VSYNC, VLINE | S1_LO_VSYNC | OIR_LO_VSYNC, OIR_VLINE. */
static const uint32_t vdc5_frame_events[3] = {
    0x00000010 | 0x00001000,
    0x00001000,
    0x00000100 | 0x00001000,
};

static inline int vdc5_int_reg(int type)
{
    return type < 8 ? 0 : type < 16 ? 1 : 2;
}

static void rza1h_vdc5_update_irq(RZA1HVdc5State *s)
{
    for (int i = 0; i < VDC5_NUM_INT; i++) {
        int r = vdc5_int_reg(i);
        qemu_set_irq(s->irq[i],
                     (s->status[r] & s->enable[r] & vdc5_int_bit[i]) != 0);
    }
}

static void rza1h_vdc5_frame_tick(void *opaque)
{
    RZA1HVdc5State *s = RZA1H_VDC5(opaque);

    s->frames++;
    for (int r = 0; r < 3; r++) {
        s->status[r] |= vdc5_frame_events[r];
    }
    rza1h_vdc5_update_irq(s);
}

static uint64_t rza1h_vdc5_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HVdc5State *s = RZA1H_VDC5(opaque);
    uint64_t val = 0;

    if (size == 4 && offset >= VDC5_SYSCNT_INT1 && offset < VDC5_SYSCNT_INT1 + 12) {
        return s->status[(offset - VDC5_SYSCNT_INT1) / 4];
    }
    if (size == 4 && offset >= VDC5_SYSCNT_INT4 && offset < VDC5_SYSCNT_INT4 + 12) {
        return s->enable[(offset - VDC5_SYSCNT_INT4) / 4];
    }
    memcpy(&val, &s->regs[offset], size);
    return val;
}

static void rza1h_vdc5_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HVdc5State *s = RZA1H_VDC5(opaque);

    if (size == 4 && offset >= VDC5_SYSCNT_INT1 && offset < VDC5_SYSCNT_INT1 + 12) {
        s->status[(offset - VDC5_SYSCNT_INT1) / 4] &= (uint32_t)value;
        rza1h_vdc5_update_irq(s);
        return;
    }
    if (size == 4 && offset >= VDC5_SYSCNT_INT4 && offset < VDC5_SYSCNT_INT4 + 12) {
        int r = (offset - VDC5_SYSCNT_INT4) / 4;
        if (s->enable[r] != (uint32_t)value) {
            rza1h_debug("vdc5", "SYSCNT_INT%d <- %08x (frame %" PRIu64 ")",
                        r + 4, (uint32_t)value, s->frames);
        }
        s->enable[r] = (uint32_t)value;
        rza1h_vdc5_update_irq(s);
        return;
    }
    memcpy(&s->regs[offset], &value, size);
}


/*
 * Live display (2026-09-24): a QEMU graphic console showing what the LCD
 * would show -- run with `-display gtk` (or sdl) instead of -nographic. Scans
 * out the first graphics plane with a framebuffer pointer (GR2 in practice:
 * GRn_FLM2 = base, FLM3[30:16] = stride, FLM6[31:28] = format, [26:16] =
 * width-1; same decoding as tools/vdc5_framebuffer_peek.py). No blending of
 * several planes, no scaling -- the real panel is 480x272.
 */
#define VDC5_LCD_W 480
#define VDC5_LCD_H 272

static uint32_t vdc5_reg32(RZA1HVdc5State *s, hwaddr off)
{
    uint32_t v;

    memcpy(&v, &s->regs[off], 4);
    return v;
}

static bool rza1h_vdc5_gfx_update(void *opaque)
{
    static const hwaddr planes[] = { 0x300, 0x380, 0x200, 0x900 }; /* GR2 GR3 GR0 GR1 */
    RZA1HVdc5State *s = RZA1H_VDC5(opaque);
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint8_t row[VDC5_LCD_W * 4];
    uint32_t base = 0, stride = 0, fmt = 0;

    if (!surf || surface_bits_per_pixel(surf) != 32) {
        return true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(planes) && !base; i++) {
        base = vdc5_reg32(s, planes[i] + 0x0c);
        stride = (vdc5_reg32(s, planes[i] + 0x10) >> 16) & 0x7fff;
        fmt = vdc5_reg32(s, planes[i] + 0x1c) >> 28;
    }
    for (int y = 0; y < VDC5_LCD_H; y++) {
        uint32_t *out = (uint32_t *)((uint8_t *)surface_data(surf) +
                                     y * surface_stride(surf));
        if (!base || !stride || (fmt != 0 && fmt != 4)) {
            memset(out, 0, VDC5_LCD_W * 4);
            continue;
        }
        address_space_read(&address_space_memory, base + y * stride,
                           MEMTXATTRS_UNSPECIFIED, row,
                           VDC5_LCD_W * (fmt == 4 ? 4 : 2));
        for (int x = 0; x < VDC5_LCD_W; x++) {
            if (fmt == 4) {
                out[x] = ldl_le_p(row + x * 4) & 0xffffff;
            } else {
                uint16_t v = lduw_le_p(row + x * 2);
                out[x] = ((v >> 11) & 31) * 255 / 31 << 16 |
                         ((v >> 5) & 63) * 255 / 63 << 8 | (v & 31) * 255 / 31;
            }
        }
    }
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps rza1h_vdc5_gfx_ops = {
    .gfx_update = rza1h_vdc5_gfx_update,
};

static const MemoryRegionOps rza1h_vdc5_ops = {
    .read = rza1h_vdc5_read,
    .write = rza1h_vdc5_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_vdc5_reset(DeviceState *dev)
{
    RZA1HVdc5State *s = RZA1H_VDC5(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->status, 0, sizeof(s->status));
    memset(s->enable, 0, sizeof(s->enable));
    s->frames = 0;
    rza1h_vdc5_update_irq(s);

    /* The panel timing runs from power-on regardless of what the driver has
     * configured; the status bits only matter once it enables them. */
    ptimer_transaction_begin(s->timer);
    ptimer_set_limit(s->timer, 1, 1);
    ptimer_run(s->timer, 0);
    ptimer_transaction_commit(s->timer);
}

static void rza1h_vdc5_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HVdc5State *s = RZA1H_VDC5(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_vdc5_ops, s,
                          TYPE_RZA1H_VDC5, RZA1H_VDC50_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (int i = 0; i < VDC5_NUM_INT; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void rza1h_vdc5_realize(DeviceState *dev, Error **errp)
{
    RZA1HVdc5State *s = RZA1H_VDC5(dev);

    s->timer = ptimer_init(rza1h_vdc5_frame_tick, s,
                           PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
                           PTIMER_POLICY_CONTINUOUS_TRIGGER |
                           PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, VDC5_FRAME_HZ);
    ptimer_transaction_commit(s->timer);

    s->con = qemu_graphic_console_create(dev, 0, &rza1h_vdc5_gfx_ops, s);
    qemu_console_resize(s->con, VDC5_LCD_W, VDC5_LCD_H);
}

static void rza1h_vdc5_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_vdc5_realize;
    device_class_set_legacy_reset(dc, rza1h_vdc5_reset);
}

static const TypeInfo rza1h_vdc5_info = {
    .name          = TYPE_RZA1H_VDC5,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HVdc5State),
    .instance_init = rza1h_vdc5_init,
    .class_init    = rza1h_vdc5_class_init,
};

static void rza1h_vdc5_register_types(void)
{
    type_register_static(&rza1h_vdc5_info);
}

type_init(rza1h_vdc5_register_types)
