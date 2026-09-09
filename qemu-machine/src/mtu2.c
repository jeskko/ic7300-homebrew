/*
 * RZ/A1H MTU2 (Multi-Function Timer Pulse Unit 2) -- real timer + real IRQ
 * for three specific compare-match events: channel 3's TGI3A, channel 4's
 * TGI4A, and channel 4's TGI4C. Every other register/channel/event in the
 * module's 0x400-byte window keeps this project's prior plain-storage
 * behavior (`add_plain_ram_region()`, see rz_a1h.c's Confirmed-peripherals
 * table before this file existed) -- nothing traced through any boot path
 * needs more yet.
 *
 * Channel 3 / TGI3A (2026-09-09, first pass): cold_boot_hw_init's own
 * task-readiness busy-wait is driven by a generic system-tick handler
 * (FUN_200b7910) that only ever runs as the registered ISR for GIC ID 154
 * -- confirmed (via the same "ICDISRn register-index*32+bit" SVD formula
 * already used for OSTM0=134 and SCIF3's TXI/RXI=236/235) to be MTU2
 * channel 3's TGI3A (TGRA compare-match A) interrupt. Register offsets
 * confirmed against ~/Downloads/rza1.svd's own MTU2 peripheral block (base
 * 0xFCFF0000, matching RZA1H_MTU2_BASE): TCR_3=0x200, TMDR_3=0x202,
 * TIORH_3/TIORL_3=0x204/0x205, TIER_3=0x208, TCNT_3=0x210, TGRA_3=0x218,
 * TSR_3=0x22c. Cross-checked live: TGRA_3 reads back 0x1f40 (8000, an
 * exact match for the value FUN_20005c08's own decompile shows being
 * written), TIER_3 reads back 0x0d (TGIEA/TGIEC/TGIED set -- TGIEA, bit 0,
 * is the one this device gates on).
 *
 * Channel 4 / TGI4A and TGI4C (2026-09-09, second and third passes, same
 * session): right after channel 3 unblocks cold_boot_hw_init's first
 * task-readiness wait and dmac.c's channel 0 unblocks the one right after
 * that, boot reaches `dsp_boot_handshake` (SCIF5 DSP-link bring-up) --
 * which busy-waits on a flag only `scif5_ring_underrun_handler` clears,
 * registered as GIC ID 159's ISR. Same SVD formula (register index 4, bit
 * 31): **TGI4A**. Channel 4's register offsets, same SVD block: TCR_4=
 * 0x201, TIER_4=0x209, TCNT_4=0x212, TGRA_4=0x21c, TGRC_4=0x224,
 * TSR_4=0x22d -- interleaved on odd byte offsets against channel 3's even
 * ones, a real, documented MTU2 hardware quirk (channels 3/4 share a
 * byte-interleaved register bank), not an inconsistency.
 *
 * `scif5_dsp_link_driver_init`'s own pre-existing file comment (written
 * 2026-08-29, before this session) already documents the wider mechanism
 * both channel-4 events tap into: a ring buffer (shared with a more
 * general job-dispatch mechanism, `shared_job_ring_dispatch`) drained via
 * MTU2 ch.4 compare-match-C (GIC ID 161, **TGI4C**, register index 5 bit
 * 1 -- same SVD formula again -- `scif5_ring_pop_and_send`), with
 * compare-match-A (GIC 159) as `scif5_ring_underrun_handler`'s own "ring
 * now empty" signal. Confirmed live that TIER_4 already has TGIEA/TGIEB/
 * TGIEC (bits 0-2) all enabled, matching that description. **TGI4C had to
 * be modeled too, not left as a scope note**: without it, nothing ever
 * drains the ring `dsp_param_sync_tick` (called from `dsp_cmd_table_init`,
 * itself right after `dsp_boot_handshake`) pushes ~23 parameter words
 * onto in one go, and a fixed-capacity generic ring-push helper
 * (`FUN_201877e4`/`FUN_20187bb4`, used well beyond just this one ring)
 * hits real, unrecoverable-by-design overflow protection (`FUN_200b93fc`,
 * an unconditional infinite loop, called with an error code as its
 * argument in `r0`) once the ring fills up -- confirmed live via a stable,
 * reproducible (not timing-noise) parked PC once TGI4A alone was modeled.
 *
 * Both channel-4 events share the same underlying counter/prescaler
 * (`TCR_4`) but arm completely differently, confirmed live, not assumed:
 * **TGI4A is armed only by an explicit `TGRA_4`/`TCR_4` write** while
 * `CST4` is already set (the real trigger `scif5_bitrev_transmit_word`/
 * `scif5_ring_underrun_handler` each issue) -- never by the `TSTR`
 * transition itself. Live testing found `CST4` goes high early in boot,
 * well before `TGRA_4` is ever meaningfully written; auto-arming TGI4A on
 * that transition (this file's first channel-4 attempt) fired it far too
 * early, using whatever stale `TGRA_4` the reset state happened to hold,
 * and left `TSR_4` spuriously set long before this channel's real user
 * ever begins. **TGI4C, by contrast, genuinely is armed by the `TSTR`
 * transition** the same way channel 3's `TGI3A` is -- confirmed live that
 * `TGRC_4` stays at its reset value (`0x0000`) through the whole traced
 * boot path (no code ever writes it before `TSTR`'s `CST4` bit is already
 * set), so a real, correctly-configured periodic drain tick can only be
 * coming from the free-running-wraparound case (`TCR_4`'s CCLR field
 * clear, matching channel 3's own confirmed configuration) -- not from
 * any explicit compare-value write. `scif5_ring_underrun_handler` does
 * later write `TGRC_4` explicitly (a real, live-observed
 * `TGRC_4 = TCNT_4 + 0x280`) -- handled too (rearms TGI4C using this
 * device's own generic period logic, see Simplifications below), but is
 * not what gets the very first drain tick going.
 *
 * Channel 3 (`TSTR` transition arms `TGI3A`) and channel 4 (`TSTR`
 * transition arms only `TGI4C`, never `TGI4A`) therefore need genuinely
 * different arming policies per event, not just per channel -- this file
 * tracks that per-event via an explicit `periodic` flag rather than
 * hardcoding channel-shaped logic, so a future fourth event doesn't need
 * another special case threaded through every function.
 *
 * `TSTR` (the module-shared start register) is at 0x280, with CST3 at
 * *bit 6* and CST4 at *bit 7* -- not bits 3/4 -- channels 3/4 share bits
 * 6/7, a real, documented MTU2 quirk confirmed directly against the SVD's
 * own TSTR field layout, not assumed; live-confirmed TSTR reads back 0xc1
 * (CST0/CST3/CST4 all running).
 *
 * Trigger mode confirmed live for all three, not assumed (this project's
 * own established discipline after the SCIF3 TXI bugs): GICD_ICFGR9 bits
 * 20-21 (ID 154), GICD_ICFGR9 bits 30-31 (ID 159), and GICD_ICFGR10 bits
 * 2-3 (ID 161) all read back `0b00` = *level*-triggered. Each is a genuine
 * level interrupt driven by a status flag the real hardware itself never
 * auto-clears (each event's own TSR flag bit) -- unlike ostm.c's
 * edge-style qemu_irq_pulse() (confirmed safe there only after checking,
 * not by default), this device must raise-and-hold while the flag is set
 * and the matching TIER enable bit is set, then lower only once the
 * guest's own ISR clears the flag -- the standard Renesas MTU2
 * write-0-to-clear / write-1-to-preserve protocol for TSR (a naive plain
 * store here would let software "set" a flag by writing 1, wedging the
 * level line permanently high the first time its enable bit is also set).
 *
 * Simplifications, deliberate (same philosophy as ostm.c's own file
 * comment -- not a faithful MTU2, just real enough for what this boot
 * path's own ISRs need): TCNT is plain stored/read-back state for both
 * channels, not a live-computed value the way OSTM's CNT is -- channel 4's
 * own arm sequences read TCNT_4 to compute *relative* compare targets
 * (`TGRA_4 = TCNT_4 + 0x200`, `TGRC_4 = TCNT_4 + 0x280`), but since
 * nothing traced needs those specific offsets to be real-time-accurate
 * (only that each event eventually fires once armed), TGI4A arms a
 * fixed-period one-shot on every `TGRA_4`/`TCR_4` write instead of
 * computing the guest's real relative delay, and TGI4C reuses this
 * device's own generic period logic (free-running-to-wraparound vs.
 * clear-on-compare, the same binary CCLR simplification channel 3's
 * TGI3A already made) rather than the guest's live `TCNT_4` value either
 * -- simpler, and indistinguishable from the guest's own perspective for
 * what's traced. TIOR/TMDR/TGRB/TGRD are stored but inert (no real
 * output-pin or buffer-mode behavior modeled).
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

/* One compare-match event this device actually models: which TCR/TIER/
 * TGRx/TSR registers and TSTR bit it belongs to, which TIER/TSR bit is
 * its own flag, whether it arms on the TSTR 0->1 transition (like TGI3A/
 * TGI4C) or only on an explicit TGRx/TCR write while already running
 * (like TGI4A) -- see file comment for why this varies per event, not
 * per channel. */
typedef struct RZA1HMtu2Event {
    hwaddr tcr_off;
    hwaddr tier_off;
    hwaddr tgr_off;   /* 16-bit compare register for this event */
    hwaddr tsr_off;
    uint8_t bit;          /* bit within TIER/TSR: 0 for an A-event, 2 for a C-event */
    uint8_t tstr_cst_bit;
    bool arms_on_tstr;    /* true: TGI3A/TGI4C-style; false: TGI4A-style */
    qemu_irq irq;
    ptimer_state *timer;
} RZA1HMtu2Event;

struct RZA1HMtu2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    RZA1HMtu2Event ch3a; /* TGI3A, GIC ID 154 */
    RZA1HMtu2Event ch4a; /* TGI4A, GIC ID 159 */
    RZA1HMtu2Event ch4c; /* TGI4C, GIC ID 161 */

    uint8_t regs[RZA1H_MTU2_SIZE]; /* plain backing store for every offset
                                     * this device doesn't special-case --
                                     * matches the add_plain_ram_region()
                                     * behavior this device replaces */
};

#define MTU2_TCR_3   0x200
#define MTU2_TIER_3  0x208
#define MTU2_TGRA_3  0x218
#define MTU2_TSR_3   0x22c

#define MTU2_TCR_4   0x201
#define MTU2_TIER_4  0x209
#define MTU2_TGRA_4  0x21c
#define MTU2_TGRC_4  0x224
#define MTU2_TSR_4   0x22d

#define MTU2_TSTR    0x280
#define MTU2_TSTR_CST3   (1 << 6)
#define MTU2_TSTR_CST4   (1 << 7)

/* 25 MHz -- deliberately NOT ostm.c's own 500 MHz "fast for testing"
 * constant, and not arbitrary: lowered from an original 500 MHz choice,
 * 2026-09-09, after live testing traced a real, reproducible RTOS
 * job-queue overflow (see README-history.md's "MTU2 channel 4 built; a
 * generic RTOS job-queue overflow" section) to this device's own tick
 * rate outrunning a small, fixed-capacity software-timer-expiry queue
 * elsewhere in the firmware -- confirmed via a breakpoint-based hit trace
 * showing a single, steady, always-identical producer (never a genuine
 * multi-source burst), and confirmed further that pausing execution on
 * every one of that producer's calls (a breakpoint's own overhead) was
 * enough to avoid the overflow entirely, pointing at a rate mismatch, not
 * a scheduling bug. This project's own initial hypothesis after finding
 * the overflow -- that it exposed a genuine emulator context-switch/
 * multi-tasking defect -- was wrong; live testing corrected it before any
 * code changed to "fix" a nonexistent scheduler bug. 25 MHz was chosen
 * empirically (confirmed via 6 independent trials, up to 90s each, zero
 * recurrences) as slow enough to avoid the overflow while still much
 * faster than the SoC's real clock, matching this project's standing
 * "not real-clock-accurate, just fast enough for practical testing"
 * philosophy (same rationale as ostm.c's own constant) -- not a claim
 * that 25 MHz is RZ/A1H's real Pφ. */
#define MTU2_FREQ_HZ 25000000

/* TGI4A's fixed one-shot period (in MTU2_FREQ_HZ counts) -- see file
 * comment's Simplifications paragraph for why this doesn't compute the
 * guest's own real relative-to-TCNT delay. */
#define MTU2_CH4A_ONESHOT_COUNTS 0x200

static uint32_t rza1h_mtu2_period_counts(RZA1HMtu2State *s, RZA1HMtu2Event *ev)
{
    uint8_t tcr = s->regs[ev->tcr_off];

    /* CCLR (bits 7:5 of TCR) selects TCNT's clear source on real
     * hardware; this project's only traced configuration (TCR == 0, see
     * file comment) leaves it free-running, so the real compare-match
     * event only recurs once per full 16-bit wraparound, not once every
     * TGRx counts -- see file comment for why only this binary
     * distinction is modeled. */
    if ((tcr & 0xe0) != 0) {
        uint16_t tgr = s->regs[ev->tgr_off] | (s->regs[ev->tgr_off + 1] << 8);
        return tgr ? tgr : 1;
    }
    return 0x10000;
}

static void rza1h_mtu2_update_irq(RZA1HMtu2State *s, RZA1HMtu2Event *ev)
{
    bool tgf = (s->regs[ev->tsr_off] & (1 << ev->bit)) != 0;
    bool tgie = (s->regs[ev->tier_off] & (1 << ev->bit)) != 0;

    /* Level, not edge -- see file comment. Stays asserted for as long as
     * both bits read true; qemu_set_irq() is idempotent when called
     * repeatedly with the same level, so re-evaluating on every relevant
     * register write (rather than tracking an explicit prior-state flag)
     * is simplest and always correct. */
    qemu_set_irq(ev->irq, tgf && tgie);
}

static void rza1h_mtu2_rearm(RZA1HMtu2State *s, RZA1HMtu2Event *ev)
{
    uint32_t counts = ev->arms_on_tstr ? rza1h_mtu2_period_counts(s, ev)
                                        : MTU2_CH4A_ONESHOT_COUNTS;

    ptimer_transaction_begin(ev->timer);
    ptimer_set_limit(ev->timer, counts, 1);
    ptimer_run(ev->timer, 0);
    ptimer_transaction_commit(ev->timer);
}

static void rza1h_mtu2_stop(RZA1HMtu2Event *ev)
{
    ptimer_transaction_begin(ev->timer);
    ptimer_stop(ev->timer);
    ptimer_transaction_commit(ev->timer);
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
    RZA1HMtu2Event *ev;

    switch (offset) {
    case MTU2_TSR_3:
        /* Write-0-to-clear / write-1-to-preserve, the real MTU2 TSR
         * protocol -- see file comment for why a plain store is wrong. */
        s->regs[offset] &= (uint8_t)value;
        rza1h_mtu2_update_irq(s, &s->ch3a);
        return;
    case MTU2_TSR_4:
        /* Shared by both channel-4 events -- clearing affects whichever
         * bit(s) the write actually targets; re-evaluate both. */
        s->regs[offset] &= (uint8_t)value;
        rza1h_mtu2_update_irq(s, &s->ch4a);
        rza1h_mtu2_update_irq(s, &s->ch4c);
        return;
    case MTU2_TIER_3:
        s->regs[offset] = (uint8_t)value;
        rza1h_mtu2_update_irq(s, &s->ch3a);
        return;
    case MTU2_TIER_4:
        s->regs[offset] = (uint8_t)value;
        rza1h_mtu2_update_irq(s, &s->ch4a);
        rza1h_mtu2_update_irq(s, &s->ch4c);
        return;
    case MTU2_TSTR: {
        uint8_t old = s->regs[offset];
        uint8_t now = (uint8_t)value;

        s->regs[offset] = now;
        if ((now & MTU2_TSTR_CST3) && !(old & MTU2_TSTR_CST3)) {
            rza1h_mtu2_rearm(s, &s->ch3a);
        } else if (!(now & MTU2_TSTR_CST3) && (old & MTU2_TSTR_CST3)) {
            rza1h_mtu2_stop(&s->ch3a);
        }
        if ((now & MTU2_TSTR_CST4) && !(old & MTU2_TSTR_CST4)) {
            /* Only TGI4C arms here -- TGI4A deliberately does not, see
             * file comment (live testing found CST4 goes high long
             * before TGRA_4 is ever meaningfully written, so auto-arming
             * TGI4A here fired it far too early). */
            rza1h_mtu2_rearm(s, &s->ch4c);
        } else if (!(now & MTU2_TSTR_CST4) && (old & MTU2_TSTR_CST4)) {
            rza1h_mtu2_stop(&s->ch4a);
            rza1h_mtu2_stop(&s->ch4c);
        }
        return;
    }
    case MTU2_TCR_3:
    case MTU2_TGRA_3:
        memcpy(&s->regs[offset], &value, size);
        if (s->regs[MTU2_TSTR] & s->ch3a.tstr_cst_bit) {
            /* Channel already running -- a live reprogram takes effect
             * immediately. Handles either write ordering (TSTR-then-
             * configure or configure-then-TSTR) without assuming which
             * one this firmware build uses. */
            rza1h_mtu2_rearm(s, &s->ch3a);
        }
        return;
    case MTU2_TCR_4:
        /* Shared prescaler/CCLR for both channel-4 events -- a live
         * reprogram could in principle affect either's real period, so
         * rearm both if channel 4 is already running. */
        memcpy(&s->regs[offset], &value, size);
        if (s->regs[MTU2_TSTR] & MTU2_TSTR_CST4) {
            rza1h_mtu2_rearm(s, &s->ch4a);
            rza1h_mtu2_rearm(s, &s->ch4c);
        }
        return;
    case MTU2_TGRA_4:
        ev = &s->ch4a;
        goto ch4_configure;
    case MTU2_TGRC_4:
        ev = &s->ch4c;
ch4_configure:
        memcpy(&s->regs[offset], &value, size);
        if (s->regs[MTU2_TSTR] & MTU2_TSTR_CST4) {
            rza1h_mtu2_rearm(s, ev);
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

static void rza1h_mtu2_ch3a_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    s->regs[MTU2_TSR_3] |= (1 << s->ch3a.bit);
    rza1h_mtu2_update_irq(s, &s->ch3a);
}

static void rza1h_mtu2_ch4a_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    s->regs[MTU2_TSR_4] |= (1 << s->ch4a.bit);
    rza1h_mtu2_update_irq(s, &s->ch4a);
}

static void rza1h_mtu2_ch4c_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    s->regs[MTU2_TSR_4] |= (1 << s->ch4c.bit);
    rza1h_mtu2_update_irq(s, &s->ch4c);
}

static void rza1h_mtu2_reset(DeviceState *dev)
{
    RZA1HMtu2State *s = RZA1H_MTU2(dev);

    memset(s->regs, 0, sizeof(s->regs));
    rza1h_mtu2_stop(&s->ch3a);
    rza1h_mtu2_stop(&s->ch4a);
    rza1h_mtu2_stop(&s->ch4c);
}

static void rza1h_mtu2_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HMtu2State *s = RZA1H_MTU2(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_mtu2_ops, s,
                          TYPE_RZA1H_MTU2, RZA1H_MTU2_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    s->ch3a.tcr_off = MTU2_TCR_3;
    s->ch3a.tier_off = MTU2_TIER_3;
    s->ch3a.tgr_off = MTU2_TGRA_3;
    s->ch3a.tsr_off = MTU2_TSR_3;
    s->ch3a.bit = 0;
    s->ch3a.tstr_cst_bit = MTU2_TSTR_CST3;
    s->ch3a.arms_on_tstr = true;
    sysbus_init_irq(sbd, &s->ch3a.irq);

    s->ch4a.tcr_off = MTU2_TCR_4;
    s->ch4a.tier_off = MTU2_TIER_4;
    s->ch4a.tgr_off = MTU2_TGRA_4;
    s->ch4a.tsr_off = MTU2_TSR_4;
    s->ch4a.bit = 0;
    s->ch4a.tstr_cst_bit = MTU2_TSTR_CST4;
    s->ch4a.arms_on_tstr = false;
    sysbus_init_irq(sbd, &s->ch4a.irq);

    s->ch4c.tcr_off = MTU2_TCR_4;
    s->ch4c.tier_off = MTU2_TIER_4;
    s->ch4c.tgr_off = MTU2_TGRC_4;
    s->ch4c.tsr_off = MTU2_TSR_4;
    s->ch4c.bit = 2;
    s->ch4c.tstr_cst_bit = MTU2_TSTR_CST4;
    s->ch4c.arms_on_tstr = true;
    sysbus_init_irq(sbd, &s->ch4c.irq);
}

static void rza1h_mtu2_realize(DeviceState *dev, Error **errp)
{
    RZA1HMtu2State *s = RZA1H_MTU2(dev);
    RZA1HMtu2Event *events[] = { &s->ch3a, &s->ch4a, &s->ch4c };
    ptimer_cb callbacks[] = { rza1h_mtu2_ch3a_tick, rza1h_mtu2_ch4a_tick,
                             rza1h_mtu2_ch4c_tick };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(events); i++) {
        events[i]->timer = ptimer_init(callbacks[i], s,
                                       PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
                                       PTIMER_POLICY_CONTINUOUS_TRIGGER |
                                       PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
        ptimer_transaction_begin(events[i]->timer);
        ptimer_set_freq(events[i]->timer, MTU2_FREQ_HZ);
        ptimer_transaction_commit(events[i]->timer);
    }
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
