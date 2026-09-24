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
  *
 * 2026-09-24 -- receive path + the fake FPGA (notes/fpga-link.md). The band
 * scope's sweep read needs a real MISO side: every SPDR2 write (CPU or DMAC)
 * clocks one byte through fake_fpga_xfer() into an RX queue that SPDR2 reads
 * drain. SPRI2 (GIC 277) is a level interrupt, SPCR.SPRIE && RX queue
 * non-empty, raised one byte time (RSPI2_BYTE_NS) after the write that
 * clocked it, via ptimer (dmac.c's file comment explains why not a raw
 * QEMUTimer under icount). The queue is deeper than the real 8-byte buffer
 * on purpose: dmac.c runs the TX-dummy channel's 475 writes before the RX
 * channel's 475 reads, all in one callback. +0x20 is SPBFCR, not SPCMD2:
 * bit7 TXRST, bit6 RXRST (clears the queue). A transfer boundary for the
 * FPGA is a TXRST pulse (rspi2_transmit) or SPE going 0->1 in SPCR (the
 * sweep-read start, fpga_sweep_read_start).
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"
#include "fake_fpga.h"

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

    /* Receive side (2026-09-24), see file comment. */
    FakeFpga fpga;
    uint8_t rxq[1024];
    unsigned rx_head, rx_count;
    bool rx_visible;            /* the newest byte's clock period has elapsed */
    qemu_irq spri;
    ptimer_state *byte_timer;
};

#define RSPI2_FRAME_GAP_NS 20000
/* One byte at the firmware's SPBR=1/BRDV=3 setting, ~2 Mbit/s. */
#define RSPI2_BYTE_NS 4000
#define RSPI2_TIMER_FREQ_HZ 1000000000

#define RSPI2_SPCR2  0x00
#define RSPI2_SPSR2  0x03
#define RSPI2_SPDR2  0x04
#define RSPI2_SPBFCR 0x20

#define RSPI2_SPCR_SPRIE (1 << 7)
#define RSPI2_SPCR_SPE   (1 << 6)
#define RSPI2_SPSR2_SPRF     (1 << 7)
#define RSPI2_SPSR2_TX_READY (1 << 6)   /* TEND */
#define RSPI2_SPSR2_SPTEF    (1 << 5)
#define RSPI2_SPBFCR_TXRST (1 << 7)
#define RSPI2_SPBFCR_RXRST (1 << 6)

static void rza1h_rspi2_update_irq(RZA1HRspi2State *s)
{
    qemu_set_irq(s->spri, (s->regs[RSPI2_SPCR2] & RSPI2_SPCR_SPRIE) &&
                          s->rx_count && s->rx_visible);
}

static void rza1h_rspi2_byte_done(void *opaque)
{
    RZA1HRspi2State *s = RZA1H_RSPI2(opaque);

    s->rx_visible = true;
    rza1h_rspi2_update_irq(s);
}

/* Clock one byte: MOSI to the FPGA, its MISO into the RX queue. */
static void rza1h_rspi2_clock_byte(RZA1HRspi2State *s, uint8_t mosi)
{
    uint8_t miso = fake_fpga_xfer(&s->fpga, mosi,
                                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    if (s->rx_count < sizeof(s->rxq)) {
        s->rxq[(s->rx_head + s->rx_count++) % sizeof(s->rxq)] = miso;
    }
    s->rx_visible = false;
    rza1h_rspi2_update_irq(s);
    ptimer_transaction_begin(s->byte_timer);
    ptimer_set_count(s->byte_timer, RSPI2_BYTE_NS);
    ptimer_run(s->byte_timer, 1);
    ptimer_transaction_commit(s->byte_timer);
}

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
        /* TX always ready -- see file comment. */
        return RSPI2_SPSR2_TX_READY | RSPI2_SPSR2_SPTEF |
               (s->rx_count ? RSPI2_SPSR2_SPRF : 0);
    }
    if (offset == RSPI2_SPDR2) {
        uint8_t b = s->rxq[s->rx_head];   /* empty: the last byte again */

        if (s->rx_count) {
            s->rx_head = (s->rx_head + 1) % sizeof(s->rxq);
            s->rx_count--;
            rza1h_rspi2_update_irq(s);
        }
        return b;
    }

    memcpy(&val, &s->regs[offset], size);
    return val;
}

static void rza1h_rspi2_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    RZA1HRspi2State *s = RZA1H_RSPI2(opaque);

    if (offset == RSPI2_SPDR2) {
        bool reading = s->fpga.state == FAKE_FPGA_READ_HDR ||
                       s->fpga.state == FAKE_FPGA_READ_DATA ||
                       (s->fpga.state == FAKE_FPGA_IDLE &&
                        (uint8_t)value == FAKE_FPGA_READ_CMD);

        rza1h_rspi2_clock_byte(s, value);
        if (reading) {
            return;     /* sweep reads (2 kHz retries): not logged, see fpga stats */
        }
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
    if (offset == RSPI2_SPBFCR && (value & RSPI2_SPBFCR_TXRST)) {
        if (s->frame_len) {
            rza1h_rspi2_flush(s); /* a new transfer starts: close the previous frame */
        }
        fake_fpga_begin(&s->fpga);
    }
    if (offset == RSPI2_SPBFCR && (value & RSPI2_SPBFCR_RXRST)) {
        s->rx_count = 0;
    }
    if (offset == RSPI2_SPCR2 && (value & RSPI2_SPCR_SPE) &&
        !(s->regs[RSPI2_SPCR2] & RSPI2_SPCR_SPE)) {
        if (s->frame_len) {
            rza1h_rspi2_flush(s);
        }
        fake_fpga_begin(&s->fpga);
    }

    memcpy(&s->regs[offset], &value, size);
    rza1h_rspi2_update_irq(s);
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
    s->rx_head = s->rx_count = 0;
    s->rx_visible = false;
    fake_fpga_reset(&s->fpga);
    ptimer_transaction_begin(s->byte_timer);
    ptimer_stop(s->byte_timer);
    ptimer_transaction_commit(s->byte_timer);
    qemu_set_irq(s->spri, 0);
}

static void rza1h_rspi2_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HRspi2State *s = RZA1H_RSPI2(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_rspi2_ops, s,
                          TYPE_RZA1H_RSPI2, RZA1H_RSPI2_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->spri);
    s->flush_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rza1h_rspi2_flush_cb, s);
    s->byte_timer = ptimer_init(rza1h_rspi2_byte_done, s,
                                PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->byte_timer);
    ptimer_set_freq(s->byte_timer, RSPI2_TIMER_FREQ_HZ);
    ptimer_transaction_commit(s->byte_timer);
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
