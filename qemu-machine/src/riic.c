/*
 * RZ/A1H RIIC (I2C Bus Interface) -- real enough CR2/SR2/DRT/DRR protocol
 * sequencing to unblock body.bin's own cold-boot RIIC2 read, which this
 * project found busy-waiting forever on a completion interrupt once the
 * channel was still a bare RAM region (see qemu-machine/README.md's "The
 * above resolved" section, 2026-09-08 second-pass session). Register
 * offsets confirmed against ~/Downloads/rza1.svd: CR1=+0x00 CR2=+0x04
 * MR1=+0x08 MR2=+0x0c MR3=+0x10 FER=+0x14 SER=+0x18 IER=+0x1c SR1=+0x20
 * SR2=+0x24 SAR0=+0x28 SAR1=+0x2c SAR2=+0x30 BRL=+0x34 BRH=+0x38
 * DRT=+0x3c DRR=+0x40 (the SVD lists names/offsets only, zero bit fields,
 * same limitation mmc.c's own comment already noted for MMCIF).
 *
 * The real protocol sequence modeled here -- and which of the 6 real
 * interrupt sources (INTIICTEI/RI/TI/SPI/STI/NAKI, per
 * scratch/r01an5093ej0170-rza1-swpkg's r_intc.h, IDs 189+8*channel..
 * +5 -- the same absolute-GIC-ID convention already established for
 * OSTM0's 134, see ostm.c) does what -- was derived by decompiling all 6
 * handlers `riic2_driver_init` (body.bin, 0x2001e120) registers via
 * `register_event_handler`, not guessed:
 *
 *   STI (start/restart detected): if the software state this channel's
 *     driver keeps says "fresh start" -> write DRT = own-address<<1 |
 *     WRITE(0). Otherwise (mid-transaction restart) -> DRT =
 *     own-address<<1 | READ(1). (FUN_2001d9bc)
 *   TI  (transmit-data-empty): feeds the next TX byte to DRT -- the
 *     16-bit memory address' high then low byte, for a 2-byte-addressed
 *     read. (FUN_2001da80)
 *   TEI (transmit-end, last shifted byte fully out): once the address
 *     phase is done, writes CR2 = RS (issue repeated start) to switch to
 *     read direction. (FUN_2001db50)
 *   RI  (receive-data-full): reads DRR into the destination buffer. Its
 *     very first firing (state != 5) reads DRR too, per the real
 *     disassembly (`ldrb r0,[r1,#0]` at `0x2001dbf4`) -- but *discards*
 *     the byte (immediately overwritten by `mov r0,#5`), a dead store
 *     Ghidra's decompiler dropped entirely, hiding this from the
 *     decompiled C. This is a real, deliberate "dummy read after
 *     switching to receive mode" pattern (a documented quirk of many
 *     I2C-master IP blocks, RIIC included) -- **not a bug in Icom's
 *     firmware, and not modeled specially here**: this device already
 *     serves it like any other DRR read (delivers whatever's at
 *     `mem_addr`, increments, re-arms). The real, practical consequence
 *     for anyone building a backing image: the byte actually *kept* by
 *     the driver for its destination buffer's position 0 is at
 *     `mem_addr + 1`, not `mem_addr` -- confirmed the hard way (built a
 *     test image with a real ROM-sourced 16-byte reference string at
 *     the nominal offset, watched it arrive shifted left by one with a
 *     trailing garbage byte, found this exact dummy read explains it
 *     exactly, shifted the image by one byte, got a byte-for-byte match
 *     against the same live comparison). On the last expected *real*
 *     byte the driver writes CR2 = SP (issue stop) *before* reading DRR
 *     -- this device raises SPI only after that DRR read completes,
 *     matching real hardware (the stop condition physically follows the
 *     ACK/NACK phase, which follows the CPU's own register read).
 *     (FUN_2001dbcc)
 *   SPI (stop detected): the real completion signal -- clears the
 *     driver's own busy-wait flag. (FUN_2001da20)
 *   NAKI (NACK received): retry-on-error path, not exercised by the one
 *     concrete transaction this device was derived from (a permissive
 *     virtual EEPROM that always ACKs never needs it) -- never raised
 *     by this model.
 *
 * Real bug found and fixed getting this far, worth remembering generally
 * (not RIIC- or IC-7300-specific): the first version of this device used
 * `qemu_irq_pulse()` for every step, copying ostm.c's own pattern -- but
 * unlike OSTM0's interrupt (edge-triggered, confirmed via its own
 * GICD_ICFGR bits), body.bin configures RIIC2's interrupts (all 6, read
 * back live) as *level*-triggered. QEMU's arm_gic only latches a
 * level-sensitive SPI as pending while the line actually reads high; a
 * pulse (raise then immediately lower, synchronously, within one host
 * call) never gives the CPU a chance to sample it, so the interrupt was
 * silently dropped every time -- confirmed by tracing a real natural-boot
 * CR2=ST write reaching this device (logged), the timer firing and
 * calling qemu_irq_pulse (logged), and then *nothing* -- no ISR ever ran,
 * no further register access followed. Fixed by switching to real
 * level-sensitive semantics throughout: `qemu_irq_raise()` a line and
 * leave it asserted until the specific register access that represents
 * the real hardware auto-clearing (or the driver's own explicit SR2
 * write-0) actually happens, matching each source's own real behavior
 * (see the per-source comments on each `qemu_irq_lower()` call below).
 * The exact same class of gotcha this project already hit once with
 * OSTM0's own ICFGR (see rz_a1h.c's own comment on that) -- always check
 * a real interrupt's actual configured trigger mode before assuming
 * pulse is safe, don't just copy the pattern from a working device.
 *
 * Backed by a flat byte-addressable image via the "image" property (same
 * convention as mmc.c's own virtual SD card) -- reads with no image
 * configured, or past its end, return 0x00 (a real, valid, already-
 * documented value: diode-matrix region_code 0 is USA/JAP, see
 * notes/diode-matrix.md) rather than faulting. Device-side address
 * validation against SAR0 is deliberately not modeled -- this device
 * always "answers" whatever 7-bit address the driver addresses, the same
 * permissive-card philosophy mmc.c's own comment already states for the
 * SD side.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HRiicState, RZA1H_RIIC)

#define RIIC_REG_CR1  0x00
#define RIIC_REG_CR2  0x04
#define RIIC_REG_MR1  0x08
#define RIIC_REG_MR2  0x0c
#define RIIC_REG_MR3  0x10
#define RIIC_REG_FER  0x14
#define RIIC_REG_SER  0x18
#define RIIC_REG_IER  0x1c
#define RIIC_REG_SR1  0x20
#define RIIC_REG_SR2  0x24
#define RIIC_REG_SAR0 0x28
#define RIIC_REG_SAR1 0x2c
#define RIIC_REG_SAR2 0x30
#define RIIC_REG_BRL  0x34
#define RIIC_REG_BRH  0x38
#define RIIC_REG_DRT  0x3c
#define RIIC_REG_DRR  0x40

#define CR2_ST  0x02
#define CR2_RS  0x04
#define CR2_SP  0x08
#define CR2_BBSY 0x80

/* SR2 flag bits this device actually sets/clears -- the 3 sources whose
 * own ISR handlers explicitly clear a status flag (confirmed via
 * decompile: FUN_2001d9bc/FUN_2001da20/FUN_2001dc8c AND SR2 with
 * ~0x04/~0x08/~0x10 respectively). TI/TEI/RI never touch SR2 at all in
 * their own handlers -- real hardware auto-clears those via the
 * matching data/control register access instead, modeled below via
 * qemu_irq_lower() at that exact access rather than an SR2 bit. */
#define SR2_START 0x04
#define SR2_STOP  0x08
#define SR2_NACK  0x10

/* IRQ array indices -- consecutive real absolute IDs, see rz_a1h.h's
 * RZA1H_RIIC_IRQ_BASE0/STRIDE and this file's own comment above. */
enum {
    IRQ_TEI = 0,
    IRQ_RI  = 1,
    IRQ_TI  = 2,
    IRQ_SPI = 3,
    IRQ_STI = 4,
    IRQ_NAKI = 5,
    RIIC_NUM_IRQ = 6,
};

typedef enum {
    RIIC_IDLE = 0,
    RIIC_WAIT_ADDR,       /* STI raised; expect DRT = own-addr<<1|R/W */
    RIIC_WAIT_MEM_HI,     /* TI raised; expect DRT = mem-addr high byte */
    RIIC_WAIT_MEM_LO,     /* TI raised; expect DRT = mem-addr low byte */
    RIIC_WAIT_RESTART,    /* TEI raised; expect CR2 = RS */
    RIIC_WAIT_READ_ADDR,  /* STI (restart) raised; expect DRT=addr<<1|1 */
    RIIC_READING,         /* RI raised (arm or real); expect DRR read */
} RiicPhase;

struct RZA1HRiicState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[RIIC_NUM_IRQ];
    uint32_t channel;

    /* Plain-storage registers -- no side effects modeled beyond what's
     * described in the file comment. */
    uint8_t cr1, mr1, mr2, mr3, fer, ser, ier, sr1, sar0, sar1, sar2, brl, brh;
    uint8_t cr2;   /* live bits only, BBSY is synthesized on read */
    uint8_t sr2;
    uint8_t drt;

    /* Protocol-sequencing state, driven by real register accesses (see
     * file comment) -- not read from or written into guest RAM. */
    RiicPhase phase;
    uint16_t mem_addr;
    bool sp_pending;    /* CR2=SP seen; raise SPI once the in-flight DRR
                          * read (the byte it was requested ahead of)
                          * completes, not immediately -- see file
                          * comment's RI/SPI ordering note. */

    char *image_path;
    int image_fd; /* -1 if no image given / failed to open */

    /* Real I2C bus-speed pacing (2026-09-10) -- see riic_byte_time_ns()'s own comment. Every
     * `qemu_irq_raise()` above that represents a genuinely new real hardware event (STI/TI/TEI/
     * RI/SPI becoming ready) goes through riic_schedule_irq() instead, which defers the actual
     * raise by one real byte time. The state-machine bookkeeping (`phase`, `mem_addr`, the
     * immediate `qemu_irq_lower()` calls) stays synchronous -- those are internal/superseded-
     * signal transitions, not new events a real driver would need real time to observe. */
    ptimer_state *event_timer;
    int pending_irq; /* -1 = idle */
};

/* Real RZ/A1H RIIC bit-rate-generator formula: SCL low/high widths are (BRL[4:0]+1)/IICφ and
 * (BRH[4:0]+1)/IICφ respectively (standard Renesas RIIC design, shared across the RX/RZ family).
 * IICφ = PCLK / 2^CKS, where CKS is a prescaler field in MR1 -- its exact bit position isn't
 * independently confirmed against this project's own SVD copy within this pass's scope, so it's
 * treated as CKS=0 (IICφ=PCLK) here, an honestly-flagged simplification in the same spirit as
 * scif.c's own "PCLK not independently re-derived for SCIF specifically" caveat. PCLK is P0φ =
 * 32MHz, the same real clock already established project-wide. 9 SCL cycles per byte (8 data
 * bits + 1 ACK) -- this is what's actually being paced: every phase transition below
 * (STI/TI/TEI/RI/SPI) represents one such byte-shaped step in the real protocol. */
#define RIIC_PCLK_HZ 32000000

static uint64_t riic_byte_time_ns(RZA1HRiicState *s)
{
    uint64_t low_ns = ((s->brl & 0x1f) + 1) * 1000000000ULL / RIIC_PCLK_HZ;
    uint64_t high_ns = ((s->brh & 0x1f) + 1) * 1000000000ULL / RIIC_PCLK_HZ;

    return (low_ns + high_ns) * 9;
}

/* Defers the actual qemu_irq_raise() for `irq_idx` by one real byte time -- see this file's own
 * struct comment. Only one such event is ever in flight at a time in this strictly-sequential
 * state machine; a fresh request simply restarts the timer for whatever's newest. */
static void riic_schedule_irq(RZA1HRiicState *s, int irq_idx)
{
    s->pending_irq = irq_idx;
    ptimer_transaction_begin(s->event_timer);
    ptimer_set_count(s->event_timer, riic_byte_time_ns(s));
    ptimer_run(s->event_timer, 1); /* oneshot */
    ptimer_transaction_commit(s->event_timer);
}

static void riic_event_fire(void *opaque)
{
    RZA1HRiicState *s = RZA1H_RIIC(opaque);
    int idx = s->pending_irq;

    s->pending_irq = -1;
    if (idx >= 0) {
        qemu_irq_raise(s->irq[idx]);
    }
}

static uint8_t riic_eeprom_read(RZA1HRiicState *s, uint16_t addr)
{
    uint8_t byte = 0;

    if (s->image_fd >= 0) {
        ssize_t n = pread(s->image_fd, &byte, 1, addr);
        if (n == 1) {
            return byte;
        }
    }
    return 0; /* no image, short/failed read, or past end -- see file
               * comment on why 0x00 is a safe, real, documented value. */
}

static uint64_t rza1h_riic_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HRiicState *s = RZA1H_RIIC(opaque);

    switch (offset) {
    case RIIC_REG_CR1: return s->cr1;
    case RIIC_REG_CR2: return s->cr2 | (s->phase != RIIC_IDLE ? CR2_BBSY : 0);
    case RIIC_REG_MR1: return s->mr1;
    case RIIC_REG_MR2: return s->mr2;
    case RIIC_REG_MR3: return s->mr3;
    case RIIC_REG_FER: return s->fer;
    case RIIC_REG_SER: return s->ser;
    case RIIC_REG_IER: return s->ier;
    case RIIC_REG_SR1: return s->sr1;
    case RIIC_REG_SR2: return s->sr2;
    case RIIC_REG_SAR0: return s->sar0;
    case RIIC_REG_SAR1: return s->sar1;
    case RIIC_REG_SAR2: return s->sar2;
    case RIIC_REG_BRL: return s->brl;
    case RIIC_REG_BRH: return s->brh;
    case RIIC_REG_DRT: return s->drt; /* write-only on real hardware; harmless */
    case RIIC_REG_DRR:
        if (s->phase == RIIC_READING) {
            uint8_t byte = riic_eeprom_read(s, s->mem_addr);

            s->mem_addr++;
            qemu_irq_lower(s->irq[IRQ_RI]); /* real hardware: reading DRR
                                              * auto-clears RDRF */
            if (s->sp_pending) {
                s->sp_pending = false;
                s->phase = RIIC_IDLE;
                s->sr2 |= SR2_STOP;
                riic_schedule_irq(s, IRQ_SPI);
            } else {
                riic_schedule_irq(s, IRQ_RI); /* next byte */
            }
            return byte;
        }
        return 0;
    default:
        return 0;
    }
}

static void rza1h_riic_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HRiicState *s = RZA1H_RIIC(opaque);

    switch (offset) {
    case RIIC_REG_CR1: s->cr1 = value; break;
    case RIIC_REG_CR2:
        s->cr2 = value & ~CR2_BBSY; /* BBSY is synthesized, not stored */
        if ((value & CR2_ST) && s->phase == RIIC_IDLE) {
            rza1h_debug("riic", "riic%u: START condition", s->channel);
            s->phase = RIIC_WAIT_ADDR;
            s->sp_pending = false;
            s->sr2 |= SR2_START;
            riic_schedule_irq(s, IRQ_STI);
        } else if ((value & CR2_RS) && s->phase == RIIC_WAIT_RESTART) {
            rza1h_debug("riic", "riic%u: RESTART condition", s->channel);
            qemu_irq_lower(s->irq[IRQ_TEI]); /* real hardware: the next
                                               * CR2 write after TEND
                                               * auto-clears it */
            s->phase = RIIC_WAIT_READ_ADDR;
            s->sr2 |= SR2_START;
            riic_schedule_irq(s, IRQ_STI); /* restart re-triggers the
                                             * same start-condition
                                             * source as the initial
                                             * start, per real hardware */
        } else if (value & CR2_SP) {
            rza1h_debug("riic", "riic%u: STOP requested (phase %u)",
                       s->channel, (unsigned)s->phase);
            /* Real STOP request -- see file comment on why this doesn't
             * raise SPI immediately (it follows the in-flight DRR read
             * the driver always issues right after, per the traced
             * sequence). If no read is in flight (SP with the channel
             * otherwise idle), raise SPI right away instead of stalling
             * forever waiting for a read that will never come. */
            if (s->phase == RIIC_READING) {
                s->sp_pending = true;
            } else {
                s->phase = RIIC_IDLE;
                s->sr2 |= SR2_STOP;
                riic_schedule_irq(s, IRQ_SPI);
            }
        }
        break;
    case RIIC_REG_MR1: s->mr1 = value; break;
    case RIIC_REG_MR2: s->mr2 = value; break;
    case RIIC_REG_MR3: s->mr3 = value; break;
    case RIIC_REG_FER: s->fer = value; break;
    case RIIC_REG_SER: s->ser = value; break;
    case RIIC_REG_IER: s->ier = value; break;
    case RIIC_REG_SR1: s->sr1 = value; break;
    case RIIC_REG_SR2: {
        uint8_t cleared = s->sr2 & ~value; /* real hardware: write 0 to a
                                             * bit clears that flag, write
                                             * 1 is a no-op -- AND is the
                                             * correct real semantics. */
        s->sr2 &= value;
        if (cleared & SR2_START) {
            qemu_irq_lower(s->irq[IRQ_STI]);
        }
        if (cleared & SR2_STOP) {
            qemu_irq_lower(s->irq[IRQ_SPI]);
        }
        if (cleared & SR2_NACK) {
            qemu_irq_lower(s->irq[IRQ_NAKI]);
        }
        break;
    }
    case RIIC_REG_SAR0: s->sar0 = value; break;
    case RIIC_REG_SAR1: s->sar1 = value; break;
    case RIIC_REG_SAR2: s->sar2 = value; break;
    case RIIC_REG_BRL: s->brl = value; break;
    case RIIC_REG_BRH: s->brh = value; break;
    case RIIC_REG_DRT: {
        unsigned old_phase = s->phase;

        s->drt = value;
        switch (s->phase) {
        case RIIC_WAIT_ADDR:
            /* Real hardware: bit0 of the address byte is the R/W
             * direction, exactly matching what this device itself just
             * told the driver to send via the STI handler (see file
             * comment). STI itself is already lowered by then -- the
             * real STI handler clears SR2's START flag (see the SR2
             * write case above) as its very first action, before this
             * DRT write. */
            if (value & 1) {
                s->phase = RIIC_READING; /* shouldn't normally happen on
                                           * the very first address byte
                                           * (real transactions go
                                           * through a write-address +
                                           * restart first), but handle
                                           * it rather than wedge */
                riic_schedule_irq(s, IRQ_RI);
            } else {
                s->phase = RIIC_WAIT_MEM_HI;
                riic_schedule_irq(s, IRQ_TI);
            }
            break;
        case RIIC_WAIT_MEM_HI:
            qemu_irq_lower(s->irq[IRQ_TI]); /* real hardware: writing DRT
                                              * auto-clears TDRE */
            s->mem_addr = (uint16_t)(value << 8);
            s->phase = RIIC_WAIT_MEM_LO;
            riic_schedule_irq(s, IRQ_TI);
            break;
        case RIIC_WAIT_MEM_LO:
            qemu_irq_lower(s->irq[IRQ_TI]);
            s->mem_addr |= (uint8_t)value;
            s->phase = RIIC_WAIT_RESTART;
            /* Log the resolved EEPROM address once both bytes are known (2026-09-10) -- a real
             * boundary event (this project's own protocol-handler log lines above only ever see
             * one address byte at a time), added specifically to let host-side, GDB-free log
             * correlation distinguish *which* address a given transaction targets -- see
             * README.md's Status section (tracing the EEPROM-scan caller). */
            rza1h_debug("riic", "riic%u: EEPROM addr=%#06x resolved", s->channel, s->mem_addr);
            riic_schedule_irq(s, IRQ_TEI);
            break;
        case RIIC_WAIT_READ_ADDR:
            /* STI already lowered via the SR2 START-flag clear, same as
             * the RIIC_WAIT_ADDR case above. */
            s->phase = RIIC_READING;
            riic_schedule_irq(s, IRQ_RI); /* arm step -- see file
                                            * comment on why this
                                            * naturally re-triggers a
                                            * second time on its own,
                                            * no explicit chaining
                                            * needed for a level line */
            break;
        default:
            rza1h_debug("riic", "riic%u: unexpected DRT write %#x in phase %u",
                       s->channel, (unsigned)value, old_phase);
            break;
        }
        rza1h_debug("riic", "riic%u: DRT=%#x, phase %u -> %u",
                   s->channel, (unsigned)value, old_phase, (unsigned)s->phase);
        break;
    }
    case RIIC_REG_DRR:
        break; /* real hardware: read-only */
    default:
        break;
    }
}

static const MemoryRegionOps rza1h_riic_ops = {
    .read = rza1h_riic_read,
    .write = rza1h_riic_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_riic_reset(DeviceState *dev)
{
    RZA1HRiicState *s = RZA1H_RIIC(dev);
    int i;

    /* image_path/image_fd deliberately untouched, same reasoning as
     * mmc.c's own reset -- realize()'s job, not a per-reset concern. */
    s->cr1 = s->mr1 = s->mr2 = s->mr3 = s->fer = s->ser = s->ier = 0;
    s->sr1 = s->sar0 = s->sar1 = s->sar2 = s->brl = s->brh = 0;
    s->cr2 = s->sr2 = s->drt = 0;
    s->phase = RIIC_IDLE;
    s->mem_addr = 0;
    s->sp_pending = false;
    s->pending_irq = -1;
    ptimer_transaction_begin(s->event_timer);
    ptimer_stop(s->event_timer);
    ptimer_transaction_commit(s->event_timer);
    for (i = 0; i < RIIC_NUM_IRQ; i++) {
        qemu_irq_lower(s->irq[i]);
    }
}

static void rza1h_riic_realize(DeviceState *dev, Error **errp)
{
    RZA1HRiicState *s = RZA1H_RIIC(dev);

    s->image_fd = -1;
    if (s->image_path && s->image_path[0]) {
        s->image_fd = open(s->image_path, O_RDONLY);
        if (s->image_fd < 0) {
            qemu_log_mask(LOG_UNIMP,
                         "rza1h-riic%u: could not open image '%s' (%s) -- "
                         "reads will synthesize 0x00 bytes\n",
                         s->channel, s->image_path, strerror(errno));
        }
    }

    s->event_timer = ptimer_init(riic_event_fire, s,
                                 PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                 PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->event_timer);
    ptimer_set_freq(s->event_timer, 1000000000); /* 1 tick = 1ns */
    ptimer_transaction_commit(s->event_timer);
}

static void rza1h_riic_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HRiicState *s = RZA1H_RIIC(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &rza1h_riic_ops, s,
                          TYPE_RZA1H_RIIC, RZA1H_RIIC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (i = 0; i < RIIC_NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static const Property rza1h_riic_properties[] = {
    DEFINE_PROP_UINT32("channel", RZA1HRiicState, channel, 0),
    DEFINE_PROP_STRING("image", RZA1HRiicState, image_path),
};

static void rza1h_riic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_riic_realize;
    device_class_set_legacy_reset(dc, rza1h_riic_reset);
    device_class_set_props(dc, rza1h_riic_properties);
}

static const TypeInfo rza1h_riic_info = {
    .name          = TYPE_RZA1H_RIIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HRiicState),
    .instance_init = rza1h_riic_init,
    .class_init    = rza1h_riic_class_init,
};

static void rza1h_riic_register_types(void)
{
    type_register_static(&rza1h_riic_info);
}

type_init(rza1h_riic_register_types)
