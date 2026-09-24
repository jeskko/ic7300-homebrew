/*
 * RZ/A1H OSTM (one-shot/free-running timer) -- real QEMU timer + real IRQ.
 *
 * This is the one genuinely new capability the QEMU migration exists to
 * prove: emu/peripherals/ostm.py's Python model has a counter that jumps on
 * read but asserts nothing, because Unicorn has no built-in async interrupt
 * delivery (see emu/README.md's Status section for the full story of why
 * this project moved to QEMU). This device instead backs OSTM0 with a real
 * `ptimer` and raises a genuine GIC interrupt line on expiry.
 *
 * Register offsets confirmed against ~/Downloads/rza1.svd (the same SVD
 * emu/peripherals/ostm.py's docstring cites): CMP=+0x0, CNT=+0x4, TE=+0x10,
 * TS=+0x14, TT=+0x18, CTL=+0x20. The real interrupt ID this device's IRQ
 * output is wired to in rz_a1h.c -- 134 for OSTM0 -- comes from Renesas's own
 * reference HAL (scratch/r01an5093ej0170-rza1-swpkg/.../inc/r_intc.h:
 * `INTC_ID_OSTM0TINT = 134`), confirmed to be an absolute GIC interrupt ID
 * (not an offset needing +32) by cross-checking that same header's own
 * ICDICFR-table comments against the real GIC register layout.
 *
 * Deliberately not a faithful OSTM, and this module's CNT semantics changed
 * once against a real, concrete counter-example: `CNT` is *not* modeled as
 * bounded by `CMP` (an "elapsed fraction of the current period" reading, the
 * first version of this file's approach) -- found the hard way, 2026-09-08,
 * booting real firmware on this exact machine: `body.bin`'s own GPIO-pin-
 * settle routine (the same `FUN_2002b878` already traced on the Unicorn
 * side, see emu/peripherals/gpio.py's docstring) starts OSTM1 without ever
 * writing `CMP` (so it stayed at its reset value, 0) and then polls `CNT`
 * against a large, unrelated software threshold (~22.3M, `0x0155cc00`) as a
 * plain timeout guard -- a bounded-by-CMP `CNT` can never grow past that
 * small limit, so the poll span forever (confirmed via GDB single-stepping:
 * PC cycled through the same 6 instructions indefinitely). `CNT` here is
 * instead a genuinely unbounded, monotonically increasing count of elapsed
 * virtual nanoseconds since the last `TS` write, scaled to `OSTM_FREQ_HZ` --
 * matching what this call site actually needs, and structurally the same
 * choice `emu/peripherals/ostm.py`'s own Python model already made (a
 * counter that only ever goes up, not tied to `CMP` at all).
 *
 * The real GIC IRQ this device raises (the actual point of this file) is
 * kept as a *separate* concern: a `ptimer` with period `CMP`, auto-reload,
 * started by the same `TS` write -- real hardware almost certainly ties
 * both to the same underlying counter more tightly than this does, but
 * decoupling them is enough to validate this project's actual open
 * question (does a real, correctly-delivered periodic IRQ work at all)
 * without needing to resolve OSTM's real interval-vs-free-running mode
 * semantics (selected via CTL, not disambiguated here) first.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HOstmState, RZA1H_OSTM)

struct RZA1HOstmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;

    uint32_t cmp;
    uint32_t ctl;
    int64_t start_ns; /* QEMU_CLOCK_VIRTUAL time of the last TS write */
};

/* EXPERIMENT, 2026-09-09 -- was 500MHz (chosen only so a large software
 * timeout threshold, like the ~22.3M-count one traced in this file's own
 * comment above, resolves in a reasonable wall-clock testing time; not
 * real-clock-accurate). Now the real, hardware-confirmed value: the IC-7300's
 * own schematic shows X301 (48.000MHz) on the main CPU's USB_X1/USB_X2 pins
 * (108/109) -- an exact match for the RZ/A1H manual's clock mode 1 (48MHz on
 * USB_X1, PLL x32), which the manual (Section 11.3.2 + Table 6.3) documents
 * as giving a fixed P0-phi = 32.00MHz, OSTM's real count clock. Elegant
 * cross-check: CMP=32000 at this frequency is exactly 1.000ms, a clean RTOS
 * tick period -- strong independent confirmation this is the right number.
 * Testing live whether this alone helps or worsens qemu-machine/README-
 * history.md's newest job-ring-overflow thread (without QEMU -icount, this
 * makes OSTM's real GIC IRQ ~15.6x less frequent in wall-clock terms while
 * TCG keeps executing guest code at unthrottled host speed in between --
 * reasoned live to plausibly make a burst-vs-drain-rate mismatch *worse*,
 * not better; not yet confirmed either way). See README-history.md for the
 * result once tested -- revert to 500000000 if this doesn't hold up. */
#define OSTM_FREQ_HZ 32000000

#define OSTM_CTL_MD1 0x02                 /* 1 = free-running compare mode */
#define OSTM_FREERUN_WRAP (1ULL << 32)    /* counts per CNT wrap */

static uint32_t rza1h_ostm_cnt(RZA1HOstmState *s)
{
    int64_t elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns;

    return (uint32_t)muldiv64(elapsed_ns, OSTM_FREQ_HZ, NANOSECONDS_PER_SECOND);
}

static uint64_t rza1h_ostm_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HOstmState *s = RZA1H_OSTM(opaque);

    switch (offset) {
    case 0x00: /* CMP */
        return s->cmp;
    case 0x04: /* CNT -- see file comment for why this is unbounded, not
                * "elapsed fraction of the current period towards CMP". */
        return rza1h_ostm_cnt(s);
    case 0x10: /* TE -- bit 0: timer running */
        return ptimer_get_limit(s->timer) != 0 ? 1 : 0;
    case 0x20: /* CTL */
        return s->ctl;
    default:
        return 0; /* TS/TT are conventionally write-only */
    }
}

static void rza1h_ostm_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HOstmState *s = RZA1H_OSTM(opaque);

    switch (offset) {
    case 0x00: /* CMP */
        s->cmp = value;
        break;
    case 0x14: /* TS -- start */
        rza1h_debug("ostm", "TS: started, cmp=%u ctl=0x%x (periodic -- own ticks not logged, "
                   "see rza1h_debug.h's own \"boundary events, not noise\" design)",
                   s->cmp, s->ctl);
        s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ptimer_transaction_begin(s->timer);
        if (s->ctl & OSTM_CTL_MD1) {
            /* Free-running compare mode: CNT counts up from 0 and matches CMP once per 2^32
             * wrap. The firmware runs OSTM1 this way with CMP=0 purely as a counter for its
             * busy-wait delays (ostm1_busywait_delay_us). Until 2026-09-24 this used a
             * 1-count period for CMP=0 -- a 32 MHz ptimer, every tick a vCPU/main-loop
             * handoff under -icount, which ran boot's early delay loops at ~0.01x. */
            ptimer_set_limit(s->timer, s->cmp ? s->cmp : OSTM_FREERUN_WRAP, 1);
        } else {
            ptimer_set_limit(s->timer, s->cmp ? s->cmp : 1, 1);
        }
        ptimer_run(s->timer, 0);
        ptimer_transaction_commit(s->timer);
        break;
    case 0x18: /* TT -- stop */
        rza1h_debug("ostm", "TT: stopped");
        ptimer_transaction_begin(s->timer);
        ptimer_stop(s->timer);
        ptimer_transaction_commit(s->timer);
        break;
    case 0x20: /* CTL */
        s->ctl = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rza1h_ostm_ops = {
    .read = rza1h_ostm_read,
    .write = rza1h_ostm_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_ostm_tick(void *opaque)
{
    RZA1HOstmState *s = RZA1H_OSTM(opaque);

    /* Real OSTM interrupts are edge events cleared via the GIC's own EOI
     * (already handled correctly by QEMU's real arm_gic device) -- there's
     * no separate "clear pending" register in this device to model. */
    qemu_irq_pulse(s->irq);
    if (s->ctl & OSTM_CTL_MD1) {
        /* free-running: the next match is one full wrap later (inside ptimer_tick's
         * transaction, so the new period takes effect for the next expiry) */
        ptimer_set_limit(s->timer, OSTM_FREERUN_WRAP, 1);
    }
}

static void rza1h_ostm_reset(DeviceState *dev)
{
    RZA1HOstmState *s = RZA1H_OSTM(dev);

    s->cmp = 0;
    s->ctl = 0;
    s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_set_limit(s->timer, 0, 1);
    ptimer_transaction_commit(s->timer);
}

static void rza1h_ostm_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HOstmState *s = RZA1H_OSTM(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_ostm_ops, s,
                          TYPE_RZA1H_OSTM, 0x24);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void rza1h_ostm_realize(DeviceState *dev, Error **errp)
{
    RZA1HOstmState *s = RZA1H_OSTM(dev);

    s->timer = ptimer_init(rza1h_ostm_tick, s,
                           PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
                           PTIMER_POLICY_CONTINUOUS_TRIGGER |
                           PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, OSTM_FREQ_HZ);
    ptimer_transaction_commit(s->timer);
}

static void rza1h_ostm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_ostm_realize;
    device_class_set_legacy_reset(dc, rza1h_ostm_reset);
}

static const TypeInfo rza1h_ostm_info = {
    .name          = TYPE_RZA1H_OSTM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HOstmState),
    .instance_init = rza1h_ostm_init,
    .class_init    = rza1h_ostm_class_init,
};

static void rza1h_ostm_register_types(void)
{
    type_register_static(&rza1h_ostm_info);
}

type_init(rza1h_ostm_register_types)
