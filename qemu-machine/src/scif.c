/*
 * RZ/A1H SCIF (Serial Communication Interface with FIFO) -- Extension-
 * roadmap item 4. No emu/peripherals/ Python original exists for this one
 * (genuinely new work, not a port) -- register offsets/widths/bit layout
 * confirmed via ~/Downloads/rza1.svd (the same SVD every other peripheral
 * in this directory cites), a real Renesas FIFO-capable SCIF, distinct
 * from the byte-addressed non-FIFO "SCI" QEMU already ships a model for
 * (hw/char/renesas_sci.c, for the RX62N family) -- that device's register
 * map doesn't match this SoC's SCIF at all, though its CharBackend wiring
 * idiom (qemu_chr_fe_write_all() on transmit, qemu_chr_fe_set_handlers()
 * for receive) is the pattern this file follows.
 *
 * Eight real instances exist (SCIF0 at 0xE8007000 through SCIF7 at
 * 0xE800A800, each +0x800 apart) -- notes/ic7300-signal-chain.md has
 * confirmed real roles for several: SCIF0 is the CI-V UART, SCIF1 the
 * service/calibration link, SCIF3 the front-panel link, SCIF5 the DSP
 * link. All eight are wired identically here (see rz_a1h.c) since nothing
 * in this device's own logic depends on which channel it is beyond a
 * `channel` property used only to label log output.
 *
 * Deliberately minimal, matching this roadmap item's own scope ("SCIF UART
 * output", not "a fully interrupt-driven SCIF driver"): no baud-rate-
 * accurate transmit pacing (bytes go out immediately, unlike
 * renesas_sci.c's own timer-paced send_byte()). FSR's TDFE/TEND (transmit-
 * FIFO-empty/transmit-end) bits are always reported set so any
 * polling-based transmit driver never blocks; FDR always reports both
 * FIFOs empty for the same reason.
 *
 * 2026-09-09 (third pass): a real TXI (transmit-complete) IRQ output was
 * added, found needing this by live testing -- body.bin's SCIF3 driver
 * (scif3_driver_pump_tick) sets a software "busy" flag before a send and
 * only an interrupt handler ever clears it (confirmed: nothing else in the
 * whole image touches it), so with no line ever raised, every send after
 * the first permanently wedged the driver. See rz_a1h.h's
 * RZA1H_SCIF_TXI_BASE0 comment for the GIC-ID derivation (independently
 * cross-checked against a real 0xec literal in the firmware's own
 * IRQ-enable call) and this file's own REG_SCR/REG_FTDR/REG_FSR write
 * cases for the three real bugs found getting a *reliable* signal out of
 * it (level- vs. edge-triggered, a redundant-raise-is-a-no-op gotcha, and
 * an emulator-only lost-edge artifact) -- verified end-to-end via a real
 * register-write trace, see qemu-machine/README-history.md's 2026-09-09
 * section. One IRQ line per instance; RXI is not wired -- no traced boot
 * path needs it yet.
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HScifState, RZA1H_SCIF)

struct RZA1HScifState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq; /* TXI (transmit-complete) only -- see this file's comment */
    uint32_t channel; /* 0-7, log-label only -- see this file's own comment */

    uint16_t smr;
    uint8_t  brr;
    uint16_t scr;
    uint16_t fcr;
    uint16_t sptr;
    uint16_t lsr;
    uint16_t emr;

    uint8_t  frdr;
    bool     rx_pending;
};

#define REG_SMR  0x00
#define REG_BRR  0x04
#define REG_SCR  0x08
#define REG_FTDR 0x0C
#define REG_FSR  0x10
#define REG_FRDR 0x14
#define REG_FCR  0x18
#define REG_FDR  0x1C
#define REG_SPTR 0x20
#define REG_LSR  0x24
#define REG_EMR  0x28

/* FSR bit positions, confirmed via the SVD (see this file's own comment). */
#define FSR_DR   (1 << 0)
#define FSR_RDF  (1 << 1)
#define FSR_TDFE (1 << 5)
#define FSR_TEND (1 << 6)

/* SCR bit positions (same SVD). */
#define SCR_RE  (1 << 4)
#define SCR_TIE (1 << 7)

static uint64_t rza1h_scif_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    switch (offset) {
    case REG_SMR:
        return s->smr;
    case REG_BRR:
        return s->brr;
    case REG_SCR:
        return s->scr;
    case REG_FTDR:
        return 0; /* write-only on real hardware */
    case REG_FSR:
        /* TDFE/TEND always set -- see module comment; DR/RDF reflect
         * whether a real received byte (from the chardev backend) is
         * waiting in frdr. */
        return FSR_TDFE | FSR_TEND | (s->rx_pending ? (FSR_DR | FSR_RDF) : 0);
    case REG_FRDR:
        s->rx_pending = false; /* reading consumes the byte, real hardware too */
        return s->frdr;
    case REG_FCR:
        return s->fcr;
    case REG_FDR:
        return 0; /* both FIFOs always empty -- see module comment */
    case REG_SPTR:
        return s->sptr;
    case REG_LSR:
        return s->lsr;
    case REG_EMR:
        return s->emr;
    default:
        return 0;
    }
}

static void rza1h_scif_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);
    uint8_t byte;

    switch (offset) {
    case REG_SMR:
        s->smr = value;
        break;
    case REG_BRR:
        s->brr = value;
        break;
    case REG_SCR:
        /* TXI is level-triggered on real hardware (TIE & TDFE), reconfirmed
         * live for this project's own TXI3 (GICD_ICFGR14 bits 8-9 = 0,
         * ISENABLER7 bit12 set) -- the exact same class of gotcha riic.c's
         * own file comment already documents in detail (a pulse gets
         * silently dropped by arm_gic for a level-sensitive SPI). Since
         * TDFE is unconditionally true in this model, enabling TIE alone
         * makes the condition true right away.
         *
         * A second, less obvious bug from the same family found getting
         * this far: a bare qemu_irq_raise() while the line is *already*
         * high is a silent no-op (qemu_set_irq only forwards an actual
         * value change) -- fine for a genuinely one-shot send, but a real
         * multi-byte frame's own ISR (FUN_20036e34) re-primes TIE-gated
         * TXI on every subsequent byte without ever clearing TIE in
         * between, so only the frame's first byte was ever delivered.
         * riic.c's own DRT-write cases already establish the fix for this
         * exact shape (explicit lower-then-raise to force a real
         * transition even when the condition was "already true") --
         * applied the same way here on every TIE-enabling write, not just
         * the rising edge. */
        if (s->channel == 3) {
            qemu_log_mask(LOG_UNIMP, "rza1h-scif3: SCR write %#x -> %#x\n",
                         s->scr, (unsigned)value);
        }
        if (value & SCR_TIE) {
            qemu_irq_lower(s->irq);
            qemu_irq_raise(s->irq);
        } else {
            qemu_irq_lower(s->irq);
        }
        s->scr = value;
        break;
    case REG_FTDR:
        byte = value;
        /* Always surfaced, chardev or not -- the whole point of this
         * roadmap item is *observability*, and most testing won't bother
         * wiring a real -chardev per channel. */
        qemu_log_mask(LOG_UNIMP, "rza1h-scif%u: TX %02x ('%c')\n",
                     s->channel, byte, (byte >= 0x20 && byte < 0x7f) ? byte : '.');
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, &byte, 1);
        }
        /* Covers the other write ordering (TIE already enabled before this
         * byte, as every byte after a frame's first will be) -- explicit
         * lower+raise for the same reason as the REG_SCR case above. */
        if (s->scr & SCR_TIE) {
            qemu_irq_lower(s->irq);
            qemu_irq_raise(s->irq);
        }
        break;
    case REG_FSR:
        /* Deliberately NOT wired to lower the TXI line -- found needing
         * this the hard way: body.bin's own per-byte TXI ISR (FUN_20036e34)
         * writes FSR (clearing TDFE/TEND, real-hardware bookkeeping, not a
         * deliberate interrupt acknowledgement) immediately after every
         * FTDR write, in the same host call burst as the REG_FTDR case's
         * own lower+raise above. A version of this case that also called
         * qemu_irq_lower() here immediately undid that raise before the
         * CPU ever got a chance to sample the line -- the first byte of a
         * frame went out, but no second interrupt ever followed, an
         * emulator-only artifact (nothing here happens fast enough on real
         * hardware to lose a genuine edge this way). Nothing in this
         * model needs FSR writes to have any effect at all. */
        break;
    case REG_FCR:
        s->fcr = value;
        break;
    case REG_FDR:
        break; /* read-only on real hardware */
    case REG_SPTR:
        s->sptr = value;
        break;
    case REG_LSR:
        s->lsr = value;
        break;
    case REG_EMR:
        s->emr = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rza1h_scif_ops = {
    .read = rza1h_scif_read,
    .write = rza1h_scif_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int rza1h_scif_can_receive(void *opaque)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    return (!s->rx_pending) && (s->scr & SCR_RE) ? 1 : 0;
}

static void rza1h_scif_receive(void *opaque, const uint8_t *buf, int size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    /* No overrun modeling (LSR.ORER) -- can_receive() already refuses a
     * new byte until the previous one is read, so QEMU's chardev core
     * won't call this while one is still pending. */
    s->frdr = buf[0];
    s->rx_pending = true;
}

static void rza1h_scif_reset(DeviceState *dev)
{
    RZA1HScifState *s = RZA1H_SCIF(dev);

    s->smr = 0;
    s->brr = 0xff; /* real reset value -- slowest possible rate until set */
    s->scr = 0;
    s->fcr = 0;
    s->sptr = 0;
    s->lsr = 0;
    s->emr = 0;
    s->frdr = 0;
    s->rx_pending = false;
}

static void rza1h_scif_realize(DeviceState *dev, Error **errp)
{
    RZA1HScifState *s = RZA1H_SCIF(dev);

    qemu_chr_fe_set_handlers(&s->chr, rza1h_scif_can_receive,
                             rza1h_scif_receive, NULL, NULL, s, NULL, true);
}

static void rza1h_scif_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HScifState *s = RZA1H_SCIF(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_scif_ops, s,
                          TYPE_RZA1H_SCIF, 0x2C);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property rza1h_scif_properties[] = {
    DEFINE_PROP_CHR("chardev", RZA1HScifState, chr),
    DEFINE_PROP_UINT32("channel", RZA1HScifState, channel, 0),
};

static void rza1h_scif_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_scif_realize;
    device_class_set_legacy_reset(dc, rza1h_scif_reset);
    device_class_set_props(dc, rza1h_scif_properties);
}

static const TypeInfo rza1h_scif_info = {
    .name          = TYPE_RZA1H_SCIF,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HScifState),
    .instance_init = rza1h_scif_init,
    .class_init    = rza1h_scif_class_init,
};

static void rza1h_scif_register_types(void)
{
    type_register_static(&rza1h_scif_info);
}

type_init(rza1h_scif_register_types)
