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
#include "exec/icount.h" /* for the permanent diagnostic instrumentation in
                          * riic_schedule_irq_delay() -- see its own comment; kept deliberately
                          * (2026-09-10, user's own call) rather than reverted, since it directly
                          * found the real QEMU-main-loop-overhead mechanism behind this whole
                          * RIIC2-timing thread -- see README.md's Status section. */
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/nvram/eeprom_at24c.h"
#include "qemu/log.h"
#include "qemu/main-loop.h" /* QEMUBH -- see riic_ti_offer_bh()'s own comment */
#include "qemu/module.h"
#include "qemu/timer.h" /* qemu_clock_get_ns()/QEMU_CLOCK_VIRTUAL -- see
                          * EEPROM_WRITE_CYCLE_NS's own comment */
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

    /* Real EEPROM write-cycle busy time (2026-09-10) -- see EEPROM_WRITE_CYCLE_NS's own comment.
     * `eeprom_write_pending` tracks whether *this* transaction has sent at least one real
     * write-data byte (RIIC_WAIT_RESTART's own DRT-write case) by the time STOP is seen -- a
     * bare "write the address, then STOP" transaction (no data) doesn't trigger a real EEPROM
     * write cycle at all, so shouldn't arm any busy time. `eeprom_write_busy_until_ns` is the
     * `QEMU_CLOCK_VIRTUAL` deadline before which a new START's own address byte gets NACKed,
     * exactly matching real serial-EEPROM "ACK polling" behavior. */
    bool eeprom_write_pending;
    int64_t eeprom_write_busy_until_ns;

    char *image_path;   /* QOM property, unchanged CLI usage (-global rza1h-riic.image=...) --
                          * read once, at realize() time, only to seed the real EEPROM slave's
                          * initial content (see rza1h_riic_realize()); no longer kept open or
                          * read from directly at runtime. */
    I2CBus *eeprom_bus; /* 2026-09-10: a real generic I2C bus + a real hw/nvram/eeprom_at24c.c
                          * slave device replace this file's own former hand-rolled byte-array
                          * EEPROM model -- see README.md's Status section (the differential A/B
                          * test that found the old model silently dropped every write and wrapped
                          * at the wrong address-space size, 16-bit instead of the real 0x4000).
                          * This device's own CR2/SR2/DRT/DRR/STI/TI/TEI/RI/SPI protocol state
                          * machine below (unchanged in spirit -- still Renesas-specific, no
                          * generic QEMU equivalent) now drives this real bus via
                          * i2c_start_transfer()/i2c_send()/i2c_recv()/i2c_end_transfer() instead
                          * of touching a private array directly. */

    /* Real I2C bus-speed pacing (2026-09-10) -- see riic_byte_time_ns()'s own comment. Every
     * `qemu_irq_raise()` above that represents a genuinely new real hardware event (STI/TI/TEI/
     * RI/SPI becoming ready) goes through riic_schedule_irq() instead, which defers the actual
     * raise by one real byte time. The state-machine bookkeeping (`phase`, `mem_addr`, the
     * immediate `qemu_irq_lower()` calls) stays synchronous -- those are internal/superseded-
     * signal transitions, not new events a real driver would need real time to observe. */
    ptimer_state *event_timer;
    QEMUBH *ti_offer_bh; /* see riic_ti_offer_bh()'s own comment */
    int pending_irq; /* -1 = idle */
    bool pending_irq_conditional; /* see riic_schedule_irq_conditional()'s own comment -- governs
                                    * the write-data-loop's TEI-then-conditional-TI chaining in
                                    * riic_event_fire() (2026-09-10, the real write-data-loop
                                    * protocol this project's own EEPROM differential test found
                                    * was never implemented -- see README.md's Status section and
                                    * the decompiled FUN_2001dcc4/FUN_2001da80/FUN_2001db50 trace
                                    * behind this design). */
};

/* Real RZ/A1H RIIC bit-rate-generator formula (2026-09-10, corrected -- confirmed against the
 * real RZ/A1H hardware manual, R01UH0403EJ0600 section 18.3.12/18.3.13, and the real
 * firmware-programmed register values read directly from `riic2_driver_init`, 0x2001e120):
 * SCL low/high periods are NOT simply (BRL[4:0]+1)/IICφ and (BRH[4:0]+1)/IICφ as this file
 * previously, incorrectly, always assumed -- that's only formula (1) below, and only applies
 * when FER.SCLE=0. The manual gives 5 variants depending on FER.SCLE/NFE and MR1.CKS:
 *   (1) SCLE=0:                extra = 1
 *   (2) SCLE=1,NFE=0,CKS=000:  extra = 3
 *   (3) SCLE=1,NFE=1,CKS=000:  extra = 3 + nf
 *   (4) SCLE=1,NFE=0,CKS!=000: extra = 2
 *   (5) SCLE=1,NFE=1,CKS!=000: extra = 2 + nf
 * with period = (BR[4:0] + extra) / IICφ per side, `nf` = MR3.NF[1:0]'s noise-filter stage count
 * (0->1, 1->2, 2->3, 3->4), and IICφ = PCLK / 2^CKS (CKS = MR1.CKS[2:0], bits 6-4 -- confirmed
 * against the manual, not a guess). PCLK is P0φ = 32MHz, the same real clock established
 * project-wide. This matters in practice, not just on paper: `riic2_driver_init` never writes
 * FER at all, leaving it at its real hardware power-on-reset default -- **SCLE=1, NFE=1** (see
 * `rza1h_riic_reset()`'s own comment) -- and programs MR1.CKS=1, MR3.NF[1:0]=0, so this project's
 * own traced RIIC2 boot path lands on formula (5), not (1): real byte time ~26.4us, not the
 * ~12.1us the old (1)-only formula computed -- about 2.2x too fast. tr/tf (SCL rise/fall time,
 * bus-capacitance-dependent) are omitted, same simplification level as every other parasitic
 * this project has consistently left out (e.g. scif.c's own transceiver-propagation omission).
 * 9 SCL cycles per byte (8 data bits + 1 ACK, MR1.BC[2:0]'s own default) -- this is what's
 * actually being paced: every phase transition below (STI/TI/TEI/RI/SPI) represents one such
 * byte-shaped step in the real protocol. */
#define RIIC_PCLK_HZ 32000000

/* Real GT24C128B parameters (2026-09-10) -- 16KB, 2-byte addressing (via
 * hw/nvram/eeprom_at24c.c's own asize>256-bytes rule), at the real 7-bit I2C address firmware
 * always uses: decompiled `FUN_2001d9bc` (the real STI handler) writes DRT=0xa0 (write) or 0xa1
 * (read) -- 0x50<<1 | R/W -- confirmed directly, not assumed from a datasheet default. */
#define RIIC2_EEPROM_I2C_ADDR 0x50
#define RIIC2_EEPROM_ROM_SIZE 16384

/* Computes the real SCL low/high periods (RIICnBRL/RIICnBRH's own real-time meaning) in ns,
 * shared by riic_byte_time_ns() and riic_condition_time_ns() below -- both need the same
 * low_ns/high_ns, just combine them differently for a full byte transfer vs. a bare condition. */
static void riic_scl_periods_ns(RZA1HRiicState *s, uint64_t *low_ns, uint64_t *high_ns)
{
    bool scle = (s->fer >> 6) & 1;
    bool nfe = (s->fer >> 5) & 1;
    unsigned cks = (s->mr1 >> 4) & 0x7;
    unsigned nf = (s->mr3 & 0x3) + 1; /* NF[1:0]: 0->1 stage, 1->2, 2->3, 3->4 */
    uint64_t iicphi_hz = (uint64_t)RIIC_PCLK_HZ >> cks;
    unsigned extra;

    if (!scle) {
        extra = 1;
    } else {
        extra = (cks == 0) ? 3 : 2;
        if (nfe) {
            extra += nf;
        }
    }

    *low_ns = ((s->brl & 0x1f) + extra) * 1000000000ULL / iicphi_hz;
    *high_ns = ((s->brh & 0x1f) + extra) * 1000000000ULL / iicphi_hz;
}

static uint64_t riic_byte_time_ns(RZA1HRiicState *s)
{
    uint64_t low_ns, high_ns;

    riic_scl_periods_ns(s, &low_ns, &high_ns);
    return (low_ns + high_ns) * 9;
}

/* Real START/RESTART/STOP condition timing (2026-09-10, added -- confirmed against the manual's
 * own §18.12 "Start Condition/Restart Condition/Stop Condition Issuing Function" timing
 * diagrams, Figures 18.37/18.38). Each condition is a short, bounded sequence of SCL/SDA edges
 * gated by RIICnBRL/RIICnBRH directly -- NOT a full 9-cycle byte time, unlike TI/TEI/RI which
 * genuinely clock a real byte + ACK. Before this fix, every phase transition in this file
 * (including these three) was charged one full riic_byte_time_ns(), a real, distinct overcount
 * (roughly 4-9x too long for these specifically) independent of (and compounding) the bit-rate
 * *value* bug fixed earlier the same session. Cycle counts below are read directly off the
 * manual's own labeled BRL/BRH segments per condition (its separately-named "setup"/"hold"/
 * "bus free" times are not given as independent numeric specs beyond "at least one BRL/BRH
 * period", so each such segment is counted as one more BRL/BRH period, not an extra unmodeled
 * margin):
 *   - START:   1 x high (BRH) + 1 x low (BRL)                    -- Figure 18.37, left half
 *   - RESTART: 1 x low + 1 x low(setup) + 1 x high(hold) + 1 x low = 3 x low + 1 x high
 *                                                                  -- Figure 18.37, right half
 *   - STOP:    1 x low + 1 x high(setup) + 1 x low(bus-free) = 2 x low + 1 x high
 *                                                                  -- Figure 18.38 */
enum {
    RIIC_COND_START,
    RIIC_COND_RESTART,
    RIIC_COND_STOP,
};

static uint64_t riic_condition_time_ns(RZA1HRiicState *s, int kind)
{
    uint64_t low_ns, high_ns;

    riic_scl_periods_ns(s, &low_ns, &high_ns);
    switch (kind) {
    case RIIC_COND_START:
        return high_ns + low_ns;
    case RIIC_COND_RESTART:
        return 3 * low_ns + high_ns;
    case RIIC_COND_STOP:
        return 2 * low_ns + high_ns;
    default:
        g_assert_not_reached();
    }
}

/* Real EEPROM write-cycle busy time (2026-09-10) -- added per the user's own follow-up on the
 * ring-overflow thread ("let's add the delays and re-test"), see README-history.md's newest
 * section for the full derivation: a live no-icount trace found the overflow driven by a ~36ms
 * *burst* of real RIIC2 write-completion/restart interrupts, and QEMU's own `hw/nvram/
 * eeprom_at24c.c` (the real EEPROM slave model this device drives, see the struct's own
 * `eeprom_bus` comment) was found to model zero write-cycle busy time at all -- writes complete
 * instantly from the bus's own perspective, paced only by this file's already-real per-bit clock
 * timing. Every real serial EEPROM (this family included -- GT24C128B, see notes/
 * diode-matrix.md) needs several real milliseconds after each write for its own internal
 * cell-programming cycle, during which it NAKs any new START addressed to it -- the textbook
 * reason real EEPROM drivers poll/retry ("ACK polling") before their next transaction. 5ms is
 * the standard, near-universal "Write Cycle Time (byte or page)" spec across the whole 24Cxx
 * family's datasheets (Atmel/ST/Microchip agree on this figure for this device class) -- not
 * independently re-derived from a GT24C128B-specific datasheet page this session, but not an
 * arbitrary placeholder either. See rza1h_riic_write()'s RIIC_WAIT_ADDR case for where this gets
 * enforced (a synthetic NACK, not a real i2c_start_transfer() rejection -- eeprom_at24c.c itself
 * has no concept of this busy state). */
#define EEPROM_WRITE_CYCLE_NS 5000000

/* Defers the actual qemu_irq_raise() for `irq_idx` by `delay_ns` -- see this file's own struct
 * comment. Only one such event is ever in flight at a time in this strictly-sequential state
 * machine; a fresh request simply restarts the timer for whatever's newest. */
static void riic_schedule_irq_delay(RZA1HRiicState *s, int irq_idx, uint64_t delay_ns)
{
    /* Diagnostic instrumentation (2026-09-10) -- kept permanently (deliberate, not an oversight:
     * this exact log line is what found the real QEMU-main-loop-overhead mechanism dominating
     * this whole scan's real duration, see README.md's Status section). Logs, per phase
     * transition, host-side, GDB-free, zero perturbation:
     *   - icount_get_raw() -- the real raw guest instruction count, independent of
     *     icount_time_shift. Originally built to check whether the real busy-wait loop
     *     firmware uses between phase transitions (FUN_2001dcc4/FUN_2001dd58's
     *     `while (*pcVar1 != 0) FUN_20062c1c();`) is a genuine cheap RTOS block/wake or a real
     *     spin -- confirmed cheap (median ~65 real instructions/transition), ruling out "the
     *     guest is doing a lot of real work" as an explanation for the observed real-time cost.
     *   - g_get_monotonic_time(), raw microseconds (rza1h_debug()'s own timestamp is only
     *     millisecond-precision, too coarse here) -- correlating this against the *requested*
     *     ptimer delay (`delay_ns`) directly caught the real mechanism: STI's real median cost
     *     (~67us) is nearly identical to TI/TEI/RI's (~65-90us) despite STI's own requested
     *     delay being ~4x shorter (~6.2us vs ~26.4us, after this same session's condition-timing
     *     fix) -- a real, ~65-90us-ish cost per scheduled event that's largely *independent* of
     *     the nominal ptimer delay value, consistent with a fixed per-event QEMU round-robin
     *     main-loop overhead (exiting/re-entering cpu_exec(), BQL reacquisition, icount
     *     bookkeeping, GIC IRQ delivery) rather than anything about this device model's own
     *     timing values. `TCG_KICK_PERIOD` (100ms) was checked and ruled out as the cause -- far
     *     too coarse to matter at this event rate. Not yet pinned to one exact QEMU function;
     *     the *magnitude and event-type-independence* of the effect is what's confirmed.
     *
     * Gated explicitly (2026-09-10 follow-up), not left as a bare rza1h_debug() call like every
     * other site in this file -- rza1h_debug() only gates its own *body* (the fprintf), but C
     * always evaluates a function's arguments before the call, so icount_get_raw()/
     * g_get_monotonic_time() were being paid on *every* schedule (potentially thousands per dense
     * scan) even with RZA1H_DEBUG unset, unlike this file's other call sites (whose arguments are
     * plain field reads, not function calls -- not worth gating the same way). This is a real,
     * always-on hot-path cost, not a hypothetical one; explicitly checking rza1h_debug_enabled()
     * first restores the "silent unless asked" cost this header's own comment promises. Evaluated
     * per the ring-overflow/round-robin-overhead thread's own handoff (README.md's Status
     * section) as a cheap, concrete overhead cut to try after concluding the originally-proposed
     * read-batching design can't reduce round-robin *dispatch count* (N real per-byte guest ISR
     * entries are unavoidable, batching only affects how many bytes are precomputed, not how many
     * times the guest is woken) -- this doesn't touch dispatch count either, so expected to be
     * marginal, but it's a real cost that was actually being paid, not a speculative one. */
    if (rza1h_debug_enabled("riic")) {
        rza1h_debug("riic", "riic%u: schedule irq=%d delay_ns=%" PRIu64 " icount_raw=%" PRId64
                    " host_us=%" PRId64,
                    s->channel, irq_idx, delay_ns, icount_get_raw(), g_get_monotonic_time());
    }

    s->pending_irq = irq_idx;
    s->pending_irq_conditional = false; /* any plain (non-conditional) schedule call clears a
                                          * stale conditional flag from an earlier chain -- see
                                          * riic_schedule_irq_conditional()'s own comment. */
    ptimer_transaction_begin(s->event_timer);
    ptimer_set_count(s->event_timer, delay_ns);
    ptimer_run(s->event_timer, 1); /* oneshot */
    ptimer_transaction_commit(s->event_timer);
}

/* The common case: a real byte time (TI/TEI/RI -- an actual byte + ACK genuinely being clocked
 * over SCL). START/RESTART/STOP go through riic_schedule_irq_delay() directly with
 * riic_condition_time_ns() instead -- see that function's own comment for why they're shorter. */
static void riic_schedule_irq(RZA1HRiicState *s, int irq_idx)
{
    riic_schedule_irq_delay(s, irq_idx, riic_byte_time_ns(s));
}

/* Schedules `irq_idx` exactly like riic_schedule_irq(), but marks it "conditional": when the
 * timer fires, riic_event_fire() only actually raises it if the channel is still in
 * RIIC_WAIT_RESTART at that moment. This is the real hardware TDRE/TEND relationship, discretely
 * approximated (2026-09-10, decompiled from FUN_2001dcc4/FUN_2001da80/FUN_2001db50 -- the real
 * write-data-loop protocol this project's own EEPROM differential test found was never
 * implemented, see README.md's Status section): after any byte finishes clocking out with
 * nothing new queued, real hardware asserts both "transmit ended" (TEI) and, an instant later,
 * "ready for a new byte" (TI) again -- offering one more chance to continue. A real read has
 * already (synchronously, in guest virtual time -- essentially instant relative to this
 * real-time-paced schedule) written CR2=RS by the time this fires, moving `phase` away from
 * RIIC_WAIT_RESTART, so the conditional check safely suppresses this offer for reads. A real
 * write responds by writing DRT again (see the RIIC_WAIT_RESTART case in rza1h_riic_write()),
 * which re-arms the exact same TEI-then-conditional-TI chain for the next byte -- this is what
 * lets a write of any length (0 or more data bytes) drain correctly without this device needing
 * to know the real byte count firmware itself is tracking. */
static void riic_schedule_irq_conditional(RZA1HRiicState *s, int irq_idx)
{
    riic_schedule_irq(s, irq_idx);
    s->pending_irq_conditional = true;
}

/* Bottom half that actually arms the conditional-TI follow-up (see
 * riic_schedule_irq_conditional()'s own comment). Required, not just style: riic_event_fire()
 * (below) is itself `s->event_timer`'s ptimer callback, invoked by ptimer.c's own internals
 * while that ptimer is already mid-transaction (confirmed live, 2026-09-10 -- calling
 * riic_schedule_irq_conditional() directly from riic_event_fire() hit
 * `ptimer_transaction_begin: Assertion '!s->in_transaction' failed` immediately on a real boot).
 * ptimer.c's own `ptimer_trigger()` comment says exactly this: "Use a bottom-half routine to
 * avoid reentrancy issues." Scheduling here instead runs the follow-up after the current
 * callback (and its transaction) has fully unwound. */
static void riic_ti_offer_bh(void *opaque)
{
    RZA1HRiicState *s = RZA1H_RIIC(opaque);

    /* Real, live-confirmed bug (2026-09-10): a BH runs asynchronously, at some indeterminate
     * later point relative to the guest -- by the time this actually executes, the guest may
     * have ALREADY responded to the TEI that scheduled it (e.g. a genuine CR2=RS for a real
     * read, which itself calls riic_schedule_irq_delay() for the real restart's own STI). Since
     * this device has only one pending-timer slot in flight at a time (by design, see
     * riic_schedule_irq_delay()'s own comment), an unconditional schedule here would silently
     * clobber that already-in-flight, now-load-bearing STI request with a stale, no-longer-
     * relevant TI offer -- confirmed live: this exact race stalled every read transaction's own
     * restart, parking the whole channel (and, transitively, the boot the ring-overflow trap
     * itself depends on) in an idle WFE loop within under a second of dense scan activity,
     * instead of the many further seconds of normal traffic every prior session observed. Fixed
     * by re-checking phase here too, not just at riic_event_fire()'s own raise-time check. */
    if (s->phase != RIIC_WAIT_RESTART) {
        return;
    }
    riic_schedule_irq_conditional(s, IRQ_TI);
}

static void riic_event_fire(void *opaque)
{
    RZA1HRiicState *s = RZA1H_RIIC(opaque);
    int idx = s->pending_irq;
    bool conditional = s->pending_irq_conditional;

    s->pending_irq = -1;
    s->pending_irq_conditional = false;
    if (idx < 0) {
        return;
    }
    if (conditional && s->phase != RIIC_WAIT_RESTART) {
        return; /* the offer this event represents is moot -- the guest has already moved the
                 * transaction on (a real CR2=RS/SP write), see this function's own doc comment
                 * on riic_schedule_irq_conditional() above. */
    }
    qemu_irq_raise(s->irq[idx]);
    if (idx == IRQ_TEI && s->phase == RIIC_WAIT_RESTART) {
        qemu_bh_schedule(s->ti_offer_bh); /* see riic_ti_offer_bh()'s own comment on why this
                                            * can't be a direct call from here. */
    }
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
            uint8_t byte = i2c_recv(s->eeprom_bus); /* real device's own address-counter/
                                                       * wraparound/dummy-read handling -- see
                                                       * the struct's own comment on why this
                                                       * replaced riic_eeprom_read(). */

            qemu_irq_lower(s->irq[IRQ_RI]); /* real hardware: reading DRR
                                              * auto-clears RDRF */
            if (s->sp_pending) {
                s->sp_pending = false;
                s->phase = RIIC_IDLE;
                s->sr2 |= SR2_STOP;
                i2c_end_transfer(s->eeprom_bus);
                riic_schedule_irq_delay(s, IRQ_SPI,
                                        riic_condition_time_ns(s, RIIC_COND_STOP));
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
            s->eeprom_write_pending = false; /* fresh transaction -- see EEPROM_WRITE_CYCLE_NS's
                                                * own comment */
            s->sr2 |= SR2_START;
            riic_schedule_irq_delay(s, IRQ_STI,
                                    riic_condition_time_ns(s, RIIC_COND_START));
        } else if ((value & CR2_RS) && s->phase == RIIC_WAIT_RESTART) {
            rza1h_debug("riic", "riic%u: RESTART condition", s->channel);
            qemu_irq_lower(s->irq[IRQ_TEI]); /* real hardware: the next
                                               * CR2 write after TEND
                                               * auto-clears it */
            qemu_irq_lower(s->irq[IRQ_TI]); /* 2026-09-10: a conditional TI offer (see
                                              * riic_schedule_irq_conditional()) may currently be
                                              * asserted too -- the guest chose to respond with
                                              * this restart instead of another DRT write, so
                                              * clear it defensively; riic_event_fire()'s own
                                              * phase check would suppress the *next* one anyway,
                                              * but the line itself needs an explicit lower like
                                              * every other source in this file. */
            s->phase = RIIC_WAIT_READ_ADDR;
            s->sr2 |= SR2_START;
            riic_schedule_irq_delay(s, IRQ_STI, /* restart re-triggers the
                                             * same start-condition
                                             * source as the initial
                                             * start, per real hardware,
                                             * but with its own (longer)
                                             * real timing -- see
                                             * riic_condition_time_ns()'s
                                             * own comment */
                                    riic_condition_time_ns(s, RIIC_COND_RESTART));
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
                /* 2026-09-10: reached directly from RIIC_WAIT_RESTART now, for both a genuine
                 * zero-data "just set the address" stop and (new) a real write's own completion
                 * stop after 1+ data bytes -- see the RIIC_WAIT_RESTART DRT-write case above.
                 * Real hardware: this STOP also auto-clears TDRE/TEND, same reasoning as the
                 * CR2=RS branch above; ends the real bus transaction so the connected EEPROM
                 * slave sees its own real I2C_FINISH event (hw/nvram/eeprom_at24c.c uses this to
                 * decide whether to flush to a backing store -- inert here since this device has
                 * none, but the correct real-protocol action regardless). */
                qemu_irq_lower(s->irq[IRQ_TI]);
                qemu_irq_lower(s->irq[IRQ_TEI]);
                i2c_end_transfer(s->eeprom_bus);
                s->phase = RIIC_IDLE;
                s->sr2 |= SR2_STOP;
                if (s->eeprom_write_pending) {
                    /* Real EEPROM write-cycle busy time (2026-09-10) -- only a STOP following
                     * 1+ real write-data bytes triggers a real internal cell-programming cycle;
                     * see EEPROM_WRITE_CYCLE_NS's own comment. A bare "write the address, then
                     * STOP" (no data) doesn't. */
                    s->eeprom_write_busy_until_ns =
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + EEPROM_WRITE_CYCLE_NS;
                    s->eeprom_write_pending = false;
                }
                riic_schedule_irq_delay(s, IRQ_SPI,
                                        riic_condition_time_ns(s, RIIC_COND_STOP));
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
            /* Real EEPROM write-cycle busy check (2026-09-10) -- see EEPROM_WRITE_CYCLE_NS's own
             * comment. A real, still-busy-programming EEPROM NAKs *any* address byte addressed
             * to it, read or write -- checked before ever touching the real i2c core (there's
             * nothing for eeprom_at24c.c itself to reject; it has no concept of this state), so
             * this is a synthetic NACK, not a real i2c_start_transfer() rejection. Ends the
             * transaction the same way a real NACK would (nothing here models a master retrying
             * automatically -- that's real firmware's own job, exactly as on real hardware). */
            if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->eeprom_write_busy_until_ns) {
                rza1h_debug("riic", "riic%u: address NACKed -- EEPROM still in its real write "
                           "cycle", s->channel);
                s->sr2 |= SR2_NACK;
                s->phase = RIIC_IDLE;
                riic_schedule_irq(s, IRQ_NAKI);
                break;
            }
            /* Real hardware: bit0 of the address byte is the R/W
             * direction, exactly matching what this device itself just
             * told the driver to send via the STI handler (see file
             * comment). STI itself is already lowered by then -- the
             * real STI handler clears SR2's START flag (see the SR2
             * write case above) as its very first action, before this
             * DRT write. This is also the real START condition on the
             * bus (2026-09-10) -- tell the real I2C core so the connected
             * EEPROM slave actually matches/ACKs it; this project's own
             * virtual EEPROM is the only device on this bus at the fixed
             * address firmware always sends (RIIC2_EEPROM_I2C_ADDR,
             * confirmed via decompile, see that macro's own comment), so
             * this is not expected to ever fail in practice -- logged,
             * not raised as NAKI, to keep this change bounded to what's
             * actually been observed live. */
            if (i2c_start_transfer(s->eeprom_bus, value >> 1, (value & 1) != 0)) {
                rza1h_debug("riic", "riic%u: i2c_start_transfer(%#x) unexpectedly NACKed",
                           s->channel, (unsigned)(value >> 1));
            }
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
            i2c_send(s->eeprom_bus, value); /* address byte 1/2 -- the real EEPROM slave's own
                                              * send() accumulates this into its address counter
                                              * (see hw/nvram/eeprom_at24c.c's own haveaddr/asize
                                              * logic), not our own mem_addr (kept only for the
                                              * debug log below -- see the struct comment). */
            s->phase = RIIC_WAIT_MEM_LO;
            riic_schedule_irq(s, IRQ_TI);
            break;
        case RIIC_WAIT_MEM_LO:
            qemu_irq_lower(s->irq[IRQ_TI]);
            s->mem_addr |= (uint8_t)value;
            i2c_send(s->eeprom_bus, value); /* address byte 2/2 -- completes the real slave's own
                                              * address counter. */
            s->phase = RIIC_WAIT_RESTART;
            /* Log the resolved EEPROM address once both bytes are known (2026-09-10) -- a real
             * boundary event (this project's own protocol-handler log lines above only ever see
             * one address byte at a time), added specifically to let host-side, GDB-free log
             * correlation distinguish *which* address a given transaction targets -- see
             * README.md's Status section (tracing the EEPROM-scan caller). `mem_addr` is now
             * logging-only -- see the struct comment on why the real slave's own address counter
             * (not this field) is authoritative for what's actually served. */
            rza1h_debug("riic", "riic%u: EEPROM addr=%#06x resolved", s->channel, s->mem_addr);
            /* Real hardware genuinely asserts TEND (TEI) here for both reads and writes -- a
             * read's own TEI handler acts on it (issues CR2=RS, unchanged, existing behavior);
             * a write's own TEI handler is a real no-op at this exact point (no data sent yet),
             * but riic_event_fire() above always chains a conditional TI right after any TEI
             * fired from this phase -- that's what lets a genuine write see its own first
             * "ready for data" offer without this device needing to know in advance whether a
             * restart or write-data is coming next (2026-09-10 addition -- see
             * riic_schedule_irq_conditional()'s own comment for the full derivation). */
            riic_schedule_irq(s, IRQ_TEI);
            break;
        case RIIC_WAIT_RESTART:
            /* A further DRT write here (not CR2=RS/SP) is a genuine write-data byte -- see
             * riic_schedule_irq_conditional()'s own comment and README.md's Status section
             * (decompiled FUN_2001dcc4/FUN_2001da80/FUN_2001db50, 2026-09-10: the real write-
             * data-loop protocol this project's own EEPROM differential test found was never
             * implemented -- every write silently dropped its data before this). Lower both TI
             * and TEI defensively (real hardware: writing DRT auto-clears TDRE, and starting a
             * new byte's shift naturally supersedes any still-pending TEND from the previous
             * one) -- either or both may currently be asserted depending on which offer the
             * guest actually responded to. */
            qemu_irq_lower(s->irq[IRQ_TI]);
            qemu_irq_lower(s->irq[IRQ_TEI]);
            i2c_send(s->eeprom_bus, value);
            s->eeprom_write_pending = true; /* a real write-data byte was sent -- see
                                              * EEPROM_WRITE_CYCLE_NS's own comment */
            riic_schedule_irq(s, IRQ_TEI); /* re-arm the same TEI-then-conditional-TI chain for
                                             * the next byte (or, if the guest is actually done,
                                             * this offer is simply never acted on -- see the
                                             * CR2=RS/SP write handlers' own explicit lowers). */
            break;
        case RIIC_WAIT_READ_ADDR:
            /* STI already lowered via the SR2 START-flag clear, same as
             * the RIIC_WAIT_ADDR case above. This is also the real REPEATED START condition on
             * the bus (2026-09-10) -- i2c_start_transfer() on an already-busy bus (current_devs
             * non-empty) is explicitly documented (hw/i2c/core.c) to skip re-scanning and just
             * re-fire the start event on the same already-matched device, exactly matching real
             * repeated-start semantics. */
            i2c_start_transfer(s->eeprom_bus, value >> 1, true);
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

    /* image_path/eeprom_bus deliberately untouched, same reasoning as
     * mmc.c's own reset -- realize()'s job, not a per-reset concern. Real hardware's own EEPROM
     * content likewise survives an RIIC channel reset -- it's a separate physical chip. */
    s->cr1 = s->mr1 = s->mr2 = s->mr3 = s->ser = s->ier = 0;
    s->sr1 = s->sar0 = s->sar1 = s->sar2 = 0;
    /* FER/BRL/BRH/MR1's real hardware power-on-reset defaults (2026-09-10, corrected -- per the
     * manual, section 18.3.3/18.3.6/18.3.12/18.3.13): FER=0x72 (SCLE=1, NFE=1, NACKE=1, MALE=1),
     * BRL=BRH=0xFF (bits 7-5 reserved-as-1, BR[4:0]=0x1F), MR1=0x08 (BCWP=1, CKS=0, BC=0/9-bit).
     * This isn't cosmetic for FER specifically: `riic2_driver_init` never writes it at all, so
     * whatever this reset leaves it at is what `riic_byte_time_ns()` sees for the entire traced
     * boot -- getting SCLE/NFE wrong here (previously: both defaulted to 0) silently collapsed
     * the corrected formula above back to its SCLE=0 case regardless of what real hardware
     * actually does. BRL/BRH/MR1 get fully reprogrammed by `riic2_driver_init` before any real
     * transaction on the one channel this project traces, so their own reset values are inert in
     * practice today -- fixed anyway so an untraced channel/boot path doesn't inherit a latent,
     * invisible mismatch later. */
    s->fer = 0x72;
    s->brl = s->brh = 0xff;
    s->mr1 = 0x08;
    s->cr2 = s->sr2 = s->drt = 0;
    s->phase = RIIC_IDLE;
    s->mem_addr = 0;
    s->sp_pending = false;
    s->eeprom_write_pending = false;
    s->eeprom_write_busy_until_ns = 0;
    s->pending_irq = -1;
    s->pending_irq_conditional = false;
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
    g_autofree uint8_t *rom = NULL;
    gsize rom_len = 0;

    /* 2026-09-10: read the image once, here, only to seed the real EEPROM slave's initial
     * content below -- replaces the former open()+pread()-per-access approach entirely (see the
     * struct's own comment on why: the old approach was read-only by construction and wrapped
     * at the wrong address-space size, both confirmed by this project's own EEPROM differential
     * test, see README.md's Status section). */
    if (s->image_path && s->image_path[0]) {
        GError *gerr = NULL;

        if (!g_file_get_contents(s->image_path, (gchar **)&rom, &rom_len, &gerr)) {
            qemu_log_mask(LOG_UNIMP,
                         "rza1h-riic%u: could not read image '%s' (%s) -- "
                         "virtual EEPROM will start blank\n",
                         s->channel, s->image_path,
                         gerr ? gerr->message : "unknown error");
            g_clear_error(&gerr);
            rom = NULL;
            rom_len = 0;
        }
    }

    s->eeprom_bus = i2c_init_bus(dev, "eeprom");
    /* Real GT24C128B (16KB, 2-byte addressing) at the real 7-bit address firmware always uses
     * (RIIC2_EEPROM_I2C_ADDR, confirmed via decompile -- see that macro's own comment). No
     * backing `-drive` is given, so `writable` defaults true in-memory-only (see
     * at24c_eeprom_realize()/at24c_eeprom_props in qemu-src/hw/nvram/eeprom_at24c.c) -- matches
     * this project's existing, non-cross-boot-persistent usage exactly; writes now genuinely
     * take effect for the rest of this run instead of being silently dropped. */
    at24c_eeprom_init_rom(s->eeprom_bus, RIIC2_EEPROM_I2C_ADDR, RIIC2_EEPROM_ROM_SIZE,
                          rom, (uint32_t)rom_len);

    s->ti_offer_bh = qemu_bh_new(riic_ti_offer_bh, s);

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
