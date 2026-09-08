/*
 * RZ/A1H MTU2 (Multi-Function Timer Pulse Unit 2) -- channel 3 real timer +
 * real IRQ. Every other register in the module's 0x400-byte window keeps
 * this project's prior plain-storage behavior (`add_plain_ram_region()`,
 * see rz_a1h.c's Confirmed-peripherals table before this file existed) --
 * nothing traced through any boot path needs more than channel 3 yet.
 *
 * Why channel 3 specifically, and the derivation behind every constant
 * below: qemu-machine/README.md's Status section and README-history.md's
 * "The SLV5 hypothesis retired" section, 2026-09-09. Short version:
 * cold_boot_hw_init's own task-readiness busy-wait is driven by a generic
 * system-tick handler (FUN_200b7910) that only ever runs as the registered
 * ISR for GIC ID 154 -- confirmed (via the same "ICDISRn register-index*32+
 * bit" SVD formula already used for OSTM0=134 and SCIF3's TXI/RXI=236/235)
 * to be MTU2 channel 3's TGI3A (TGRA compare-match A) interrupt. Register
 * offsets below are confirmed against ~/Downloads/rza1.svd's own MTU2
 * peripheral block (base 0xFCFF0000, matching RZA1H_MTU2_BASE):
 * TCR_3=0x200, TMDR_3=0x202, TIORH_3/TIORL_3=0x204/0x205, TIER_3=0x208,
 * TCNT_3=0x210, TGRA_3=0x218, TSR_3=0x22c; TSTR (the module-shared start
 * register) at 0x280, with CST3 at *bit 6* -- not bit 3 -- channels 3/4
 * share bits 6/7, a real, documented MTU2 quirk confirmed directly against
 * the SVD's own TSTR field layout, not assumed. All of the above also
 * cross-checked live against a real natural boot (README-history.md's
 * trace): TGRA_3 reads back 0x1f40 (8000, an exact match for the value
 * this project's own Ghidra decompile of FUN_20005c08 already found being
 * written), TIER_3 reads back 0x0d (TGIEA/TGIEC/TGIED set -- TGIEA, bit 0,
 * is the one this device gates on), and TSTR reads back 0xc1 (CST0/CST3/
 * CST4 all running).
 *
 * Trigger mode confirmed live, not assumed (this project's own established
 * discipline after the SCIF3 TXI bugs -- see that section's own comment
 * in README-history.md): GICD_ICFGR9 bits 20-21 (GIC ID 154's config bits)
 * read back 0b00 = *level*-triggered against a real natural boot. This is
 * a genuine level interrupt driven by a status flag the real hardware
 * itself never auto-clears (TSR_3's TGFA, bit 0) -- unlike ostm.c's
 * edge-style qemu_irq_pulse() (confirmed safe there only after checking,
 * not by default), this device must raise-and-hold while TGFA is set and
 * TGIEA is enabled, then lower only once the guest's own ISR clears TGFA
 * -- the standard Renesas MTU2 write-0-to-clear / write-1-to-preserve
 * protocol for TSR (a naive plain store here would let software "set"
 * TGFA by writing 1, wedging the level line permanently high the first
 * time TGIEA is also enabled).
 *
 * Simplifications, deliberate (same philosophy as ostm.c's own file
 * comment -- not a faithful MTU2, just real enough for what this boot
 * path's own ISR needs): TCNT_3 is plain stored/read-back state, not a
 * live-computed value the way OSTM's CNT is -- no boot path traced so far
 * reads it back, unlike OSTM1's CNT-based timeout guard (see ostm.c),
 * so there is nothing to validate against yet. TCR_3's CCLR field (which
 * selects what clears TCNT on a real MTU2) is only honored down to a
 * binary choice -- "clear on TGRA compare" (any of the field's 3 bits set)
 * vs. free-running to a full 16-bit wraparound (all clear, this project's
 * only traced configuration) -- not the full source-select table; nothing
 * traced needs more. TIOR/TMDR are stored but inert (no real output-pin or
 * buffer-mode behavior modeled).
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#include "rz_a1h.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HMtu2State, RZA1H_MTU2)

struct RZA1HMtu2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq3a;       /* channel 3's TGI3A, GIC ID 154 */
    ptimer_state *timer3;

    uint8_t regs[RZA1H_MTU2_SIZE]; /* plain backing store for every offset
                                     * this device doesn't special-case --
                                     * matches the add_plain_ram_region()
                                     * behavior this device replaces */
};

#define MTU2_TCR_3   0x200
#define MTU2_TIER_3  0x208
#define MTU2_TCNT_3  0x210
#define MTU2_TGRA_3  0x218
#define MTU2_TSR_3   0x22c
#define MTU2_TSTR    0x280

#define MTU2_TSTR_CST3   (1 << 6)
#define MTU2_TIER_TGIEA  (1 << 0)
#define MTU2_TSR_TGFA    (1 << 0)

/* 500 MHz -- chosen only so the derived compare-match period (up to a full
 * 16-bit wraparound, see rza1h_mtu2_ch3_period_counts()) resolves in a
 * reasonable wall-clock testing time, same rationale as ostm.c's own
 * OSTM_FREQ_HZ; not real-clock-accurate regardless. */
#define MTU2_FREQ_HZ 500000000

static uint16_t rza1h_mtu2_ch3_tgra(RZA1HMtu2State *s)
{
    return s->regs[MTU2_TGRA_3] | (s->regs[MTU2_TGRA_3 + 1] << 8);
}

static uint32_t rza1h_mtu2_ch3_period_counts(RZA1HMtu2State *s)
{
    uint8_t tcr = s->regs[MTU2_TCR_3];

    /* CCLR (bits 7:5 of TCR_3) selects TCNT's clear source on real
     * hardware; this project's only traced configuration (TCR_3 == 0, see
     * file comment) leaves it free-running, so the real compare-match
     * event only recurs once per full 16-bit wraparound, not once every
     * TGRA_3 counts -- see file comment for why only this binary
     * distinction is modeled. */
    if ((tcr & 0xe0) != 0) {
        uint16_t tgra = rza1h_mtu2_ch3_tgra(s);
        return tgra ? tgra : 1;
    }
    return 0x10000;
}

static void rza1h_mtu2_update_irq3(RZA1HMtu2State *s)
{
    bool tgfa = (s->regs[MTU2_TSR_3] & MTU2_TSR_TGFA) != 0;
    bool tgiea = (s->regs[MTU2_TIER_3] & MTU2_TIER_TGIEA) != 0;

    /* Level, not edge -- see file comment. Stays asserted for as long as
     * both bits read true; qemu_set_irq() is idempotent when called
     * repeatedly with the same level, so re-evaluating on every relevant
     * register write (rather than tracking an explicit prior-state flag)
     * is simplest and always correct. */
    qemu_set_irq(s->irq3a, tgfa && tgiea);
}

static void rza1h_mtu2_ch3_rearm(RZA1HMtu2State *s)
{
    uint32_t counts = rza1h_mtu2_ch3_period_counts(s);

    ptimer_transaction_begin(s->timer3);
    ptimer_set_limit(s->timer3, counts, 1);
    ptimer_run(s->timer3, 0);
    ptimer_transaction_commit(s->timer3);
}

static void rza1h_mtu2_ch3_stop(RZA1HMtu2State *s)
{
    ptimer_transaction_begin(s->timer3);
    ptimer_stop(s->timer3);
    ptimer_transaction_commit(s->timer3);
}

static uint64_t rza1h_mtu2_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);
    uint64_t val = 0;

    memcpy(&val, &s->regs[offset], size);
    return val;
}

static void rza1h_mtu2_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    switch (offset) {
    case MTU2_TSR_3:
        /* Write-0-to-clear / write-1-to-preserve, the real MTU2 TSR
         * protocol -- see file comment for why a plain store is wrong. */
        s->regs[offset] &= (uint8_t)value;
        rza1h_mtu2_update_irq3(s);
        return;
    case MTU2_TIER_3:
        s->regs[offset] = (uint8_t)value;
        rza1h_mtu2_update_irq3(s);
        return;
    case MTU2_TSTR: {
        bool was_running = (s->regs[offset] & MTU2_TSTR_CST3) != 0;
        bool now_running;

        s->regs[offset] = (uint8_t)value;
        now_running = (s->regs[offset] & MTU2_TSTR_CST3) != 0;
        if (now_running && !was_running) {
            rza1h_mtu2_ch3_rearm(s);
        } else if (!now_running && was_running) {
            rza1h_mtu2_ch3_stop(s);
        }
        return;
    }
    case MTU2_TCR_3:
    case MTU2_TGRA_3:
        memcpy(&s->regs[offset], &value, size);
        if (s->regs[MTU2_TSTR] & MTU2_TSTR_CST3) {
            /* Channel already running -- a live reprogram takes effect
             * immediately. Handles either write ordering (TSTR-then-
             * configure or configure-then-TSTR) without assuming which
             * one this firmware build uses. */
            rza1h_mtu2_ch3_rearm(s);
        }
        return;
    default:
        memcpy(&s->regs[offset], &value, size);
        return;
    }
}

static const MemoryRegionOps rza1h_mtu2_ops = {
    .read = rza1h_mtu2_read,
    .write = rza1h_mtu2_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_mtu2_ch3_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    s->regs[MTU2_TSR_3] |= MTU2_TSR_TGFA;
    rza1h_mtu2_update_irq3(s);
}

static void rza1h_mtu2_reset(DeviceState *dev)
{
    RZA1HMtu2State *s = RZA1H_MTU2(dev);

    memset(s->regs, 0, sizeof(s->regs));
    ptimer_transaction_begin(s->timer3);
    ptimer_stop(s->timer3);
    ptimer_set_limit(s->timer3, 0, 1);
    ptimer_transaction_commit(s->timer3);
}

static void rza1h_mtu2_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HMtu2State *s = RZA1H_MTU2(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_mtu2_ops, s,
                          TYPE_RZA1H_MTU2, RZA1H_MTU2_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq3a);
}

static void rza1h_mtu2_realize(DeviceState *dev, Error **errp)
{
    RZA1HMtu2State *s = RZA1H_MTU2(dev);

    s->timer3 = ptimer_init(rza1h_mtu2_ch3_tick, s,
                            PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
                            PTIMER_POLICY_CONTINUOUS_TRIGGER |
                            PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->timer3);
    ptimer_set_freq(s->timer3, MTU2_FREQ_HZ);
    ptimer_transaction_commit(s->timer3);
}

static void rza1h_mtu2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_mtu2_realize;
    device_class_set_legacy_reset(dc, rza1h_mtu2_reset);
}

static const TypeInfo rza1h_mtu2_info = {
    .name          = TYPE_RZA1H_MTU2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HMtu2State),
    .instance_init = rza1h_mtu2_init,
    .class_init    = rza1h_mtu2_class_init,
};

static void rza1h_mtu2_register_types(void)
{
    type_register_static(&rza1h_mtu2_info);
}

type_init(rza1h_mtu2_register_types)
