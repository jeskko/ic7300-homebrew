/*
 * RZ/A1H RSPI (Renesas Serial Peripheral Interface) channel 2 -- minimal,
 * matching this project's own established permissive-peripheral
 * philosophy (mmc.c's virtual SD card, riic.c's virtual EEPROM, scif.c's
 * TX-always-logged-never-blocking model).
 *
 * 2026-09-09 (DSP-comms session, continued): found needing this the same
 * way every prior blocker this session was -- a 90-second free-run poll
 * (this project's own established discipline) showed PC genuinely parked
 * inside `rspi2_transmit` (body.bin, `0x200b6c50` -- already fully
 * confirmed and documented by an earlier session, 2026-08-29, including
 * every register address cited below, not re-derived here), busy-waiting
 * on `SPSR2`'s TX-ready bit (bit 6). Reached via `shared_job_ring_
 * dispatch`'s own case 3 (a real, previously-untriggered job-type-3
 * "RSPI2 transmit" ring entry), itself only reachable after `mtu2.c`'s
 * own `scif5_cmd_transmit_now` rate-limiter fix unblocked the shared
 * DSP-comms ring far enough to drain past it -- see mtu2.c's and scif.c's
 * own file comments for that chain. `SPSR2` currently falls in the
 * "io-e8000000" plain-storage catch-all (rz_a1h.c), which always reads 0
 * -- the same "armed but the flag never comes" shape every earlier
 * blocker this session had, confirmed the same way (free-run poll,
 * PC/LR read via `gdbrsp.py`, not guessed).
 *
 * Real register addresses (confirmed 2026-08-29, unchanged): `SPCR2` =
 * `0xE800D800`, `SPSR2` = `0xE800D803`, `SPDR2` = `0xE800D804`, `SPCMD2` =
 * `0xE800D820`. `rspi2_transmit`'s own real sequence: poll SPSR2 bit 6,
 * toggle SPCMD2 (set then clear bit 7), set SPCR2 = 0x48, write
 * `count + 1` bytes one at a time through SPDR2, then arm a completion
 * deadline and enable GIC ID `0xa2` (162) -- but `shared_job_ring_
 * dispatch`'s own case 3 doesn't itself wait for that completion event
 * (`rspi2_wait_ready`, a separate function, isn't called from the ring
 * dispatcher), so unblocking `rspi2_transmit`'s own internal busy-wait is
 * everything this specific stage needs; no further RSPI2 protocol
 * fidelity, real transaction timing, or the completion IRQ path has been
 * traced as required yet.
 *
 * SPSR2's TX-ready bit (bit 6) always reads set -- same reasoning as
 * `spi_boot.c`'s own file comment and `scif.c`'s FSR (TDFE/TEND always
 * set): sufficient to let the real polling driver proceed, no real SPI
 * transaction timing needed to unblock this boot path. SPDR2 writes are
 * logged only (`qemu_log_mask`, matching scif.c's own FTDR convention) --
 * no real chardev/backend wired, nothing traced needs the transmitted
 * bytes to go anywhere. SPCR2/SPCMD2 are plain stored/read-back state,
 * inert otherwise.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HRspi2State, RZA1H_RSPI2)

struct RZA1HRspi2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t regs[RZA1H_RSPI2_SIZE];

    /* Frame logger (2026-09-24): bytes written since the last SPCMD2 bit-7 toggle (the
     * driver's per-transfer start), logged as one line once the bus has been quiet for
     * RSPI2_FRAME_GAP_NS of emulated time. RZA1H_DEBUG=rspi2 -> frames, rspi2byte -> bytes. */
    uint8_t frame[64];
    int frame_len;
    uint64_t frames;
    QEMUTimer *flush_timer;
};

#define RSPI2_FRAME_GAP_NS 20000

#define RSPI2_SPCR2  0x00
#define RSPI2_SPSR2  0x03
#define RSPI2_SPDR2  0x04
#define RSPI2_SPCMD2 0x20

#define RSPI2_SPSR2_TX_READY (1 << 6)

static void rza1h_rspi2_flush(RZA1HRspi2State *s)
{
    char hex[3 * 64 + 1];
    int n = 0;

    if (!s->frame_len) {
        return;
    }
    for (int i = 0; i < s->frame_len; i++) {
        n += snprintf(hex + n, sizeof(hex) - n, "%s%02x", i ? " " : "", s->frame[i]);
    }
    s->frames++;
    rza1h_debug("rspi2", "frame #%" PRIu64 " (%d bytes): %s", s->frames, s->frame_len, hex);
    s->frame_len = 0;
}

static void rza1h_rspi2_flush_cb(void *opaque)
{
    rza1h_rspi2_flush(RZA1H_RSPI2(opaque));
}

static uint64_t rza1h_rspi2_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HRspi2State *s = RZA1H_RSPI2(opaque);
    uint64_t val = 0;

    if (offset == RSPI2_SPSR2) {
        /* Always ready -- see file comment. */
        return RSPI2_SPSR2_TX_READY;
    }

    memcpy(&val, &s->regs[offset], size);
    return val;
}

static void rza1h_rspi2_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    RZA1HRspi2State *s = RZA1H_RSPI2(opaque);

    if (offset == RSPI2_SPDR2) {
        rza1h_debug("rspi2byte", "TX %02x ('%c')",
                   (uint8_t)value,
                   ((uint8_t)value >= 0x20 && (uint8_t)value < 0x7f)
                       ? (uint8_t)value : '.');
        if (s->frame_len < (int)sizeof(s->frame)) {
            s->frame[s->frame_len++] = value;
        }
        timer_mod(s->flush_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RSPI2_FRAME_GAP_NS);
        return;
    }
    if (offset == RSPI2_SPCMD2 && (value & 0x80) && s->frame_len) {
        rza1h_rspi2_flush(s);     /* a new transfer starts: close the previous frame */
    }

    memcpy(&s->regs[offset], &value, size);
}

static const MemoryRegionOps rza1h_rspi2_ops = {
    .read = rza1h_rspi2_read,
    .write = rza1h_rspi2_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_rspi2_reset(DeviceState *dev)
{
    RZA1HRspi2State *s = RZA1H_RSPI2(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->frame_len = 0;
}

static void rza1h_rspi2_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HRspi2State *s = RZA1H_RSPI2(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_rspi2_ops, s,
                          TYPE_RZA1H_RSPI2, RZA1H_RSPI2_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    s->flush_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rza1h_rspi2_flush_cb, s);
}

static void rza1h_rspi2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rza1h_rspi2_reset);
}

static const TypeInfo rza1h_rspi2_info = {
    .name          = TYPE_RZA1H_RSPI2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HRspi2State),
    .instance_init = rza1h_rspi2_init,
    .class_init    = rza1h_rspi2_class_init,
};

static void rza1h_rspi2_register_types(void)
{
    type_register_static(&rza1h_rspi2_info);
}

type_init(rza1h_rspi2_register_types)
