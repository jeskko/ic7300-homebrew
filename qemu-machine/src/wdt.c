/*
 * RZ/A1H watchdog timer (WDT) at 0xFCFE0000 -- the firmware's restart primitive.
 *
 * Added 2026-09-25 so the SD-card updater's "will automatically restart" actually restarts.
 * body.bin restarts the system by arming the watchdog and spinning (FUN_20052bd0, see
 * notes/firmware-update-history.md "The system restart mechanism"):
 *   WRCSR = 0x5a5f (RSTE), WTCNT = 0x5afe, WTCSR = 0xa57f (WT/IT watchdog mode, TME, CKS=7).
 * Manual chapter 12: WTCSR +0 (write 0xA5xx), WTCNT +2 (write 0x5Axx), WRCSR +4 (write 0x5Axx
 * sets RSTE, 0xA500 clears WOVF); all read as bytes. WTCNT counts up at P0phi (33.33 MHz) / 1,
 * 64, 128, ... 16384 (CKS); on overflow in watchdog mode WOVF is set and, with RSTE, the chip
 * resets. WRCSR survives that reset (it's only cleared by power-on), so the firmware can see
 * WOVF on the next boot. Interval-timer mode (IOVF, no IRQ wiring) is only stored: nothing
 * traced uses it, and no boot touches the WDT at all.
 */

#include "qemu/osdep.h"
#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/runstate.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HWdtState, RZA1H_WDT)

#define WTCSR 0x0
#define WTCNT 0x2
#define WRCSR 0x4

#define WTCSR_WTIT  (1 << 6)
#define WTCSR_TME   (1 << 5)
#define WRCSR_WOVF  (1 << 7)
#define WRCSR_RSTE  (1 << 6)
#define WDT_P0_HZ   33333333

struct RZA1HWdtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    ptimer_state *timer;
    uint8_t wtcsr, wtcnt, wrcsr;
};

static const uint8_t wdt_cks_shift[8] = { 0, 6, 7, 8, 9, 10, 12, 14 };

static void wdt_update(RZA1HWdtState *s)
{
    ptimer_transaction_begin(s->timer);
    if (s->wtcsr & WTCSR_TME) {
        ptimer_set_freq(s->timer, WDT_P0_HZ >> wdt_cks_shift[s->wtcsr & 7]);
        ptimer_set_count(s->timer, 256 - s->wtcnt);
        ptimer_run(s->timer, 1);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void wdt_overflow(void *opaque)
{
    RZA1HWdtState *s = RZA1H_WDT(opaque);

    s->wtcnt = 0;
    if (!(s->wtcsr & WTCSR_WTIT)) {
        s->wtcsr |= 0x80;           /* IOVF; interval mode keeps counting */
        ptimer_set_count(s->timer, 256);
        ptimer_run(s->timer, 1);
        return;
    }
    s->wrcsr |= WRCSR_WOVF;
    if (s->wrcsr & WRCSR_RSTE) {
        rza1h_debug("wdt", "overflow: system reset");
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else {
        s->wtcsr &= ~WTCSR_TME;     /* no LSI reset: only WTCNT/WTCSR reset */
    }
}

static uint64_t wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HWdtState *s = RZA1H_WDT(opaque);

    switch (offset) {
    case WTCSR:
        return s->wtcsr | 0x18;
    case WTCNT:
        if (s->wtcsr & WTCSR_TME) {
            return 256 - ptimer_get_count(s->timer);
        }
        return s->wtcnt;
    case WRCSR:
        return s->wrcsr | 0x1f;
    default:
        return 0;
    }
}

static void wdt_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    RZA1HWdtState *s = RZA1H_WDT(opaque);
    uint8_t key = value >> 8, v = value;

    if (size != 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "rza1h-wdt: %u-byte write to +%" HWADDR_PRIx
                      " ignored (needs a keyed 16-bit write)\n", size, offset);
        return;
    }
    switch (offset) {
    case WTCSR:
        if (key == 0xa5) {
            s->wtcsr = (v & 0x67) | (s->wtcsr & v & 0x80);   /* IOVF: write-0 clears */
            rza1h_debug("wdt", "WTCSR=%02x WTCNT=%02x WRCSR=%02x", s->wtcsr, s->wtcnt, s->wrcsr);
            wdt_update(s);
        }
        break;
    case WTCNT:
        if (key == 0x5a) {
            s->wtcnt = v;
            if (s->wtcsr & WTCSR_TME) {
                wdt_update(s);
            }
        }
        break;
    case WRCSR:
        if (key == 0x5a) {
            s->wrcsr = (s->wrcsr & WRCSR_WOVF) | (v & WRCSR_RSTE);
        } else if (key == 0xa5 && !(v & WRCSR_WOVF)) {
            s->wrcsr &= ~WRCSR_WOVF;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps wdt_ops = {
    .read = wdt_read,
    .write = wdt_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void wdt_reset(DeviceState *dev)
{
    RZA1HWdtState *s = RZA1H_WDT(dev);

    s->wtcsr = 0;
    s->wtcnt = 0;
    /* WRCSR is kept: a watchdog reset doesn't clear it (QEMU's first reset is the power-on
     * one, when it is still 0 anyway). */
    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_transaction_commit(s->timer);
}

static void wdt_init(Object *obj)
{
    RZA1HWdtState *s = RZA1H_WDT(obj);

    memory_region_init_io(&s->iomem, obj, &wdt_ops, s, TYPE_RZA1H_WDT, 6);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->timer = ptimer_init(wdt_overflow, s, PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                            PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
}

static void wdt_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), wdt_reset);
}

static const TypeInfo wdt_info = {
    .name          = TYPE_RZA1H_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HWdtState),
    .instance_init = wdt_init,
    .class_init    = wdt_class_init,
};

static void wdt_register_types(void)
{
    type_register_static(&wdt_info);
}

type_init(wdt_register_types)
