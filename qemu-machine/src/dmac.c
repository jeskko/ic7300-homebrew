/*
 * RZ/A1H DMAC (Direct Memory Access Controller) -- channel 0 real transfer +
 * real IRQ. Every other register/channel in this device's window keeps this
 * project's usual plain-storage fallback (`regs[]` passthrough, same
 * approach as mtu2.c's untouched channels) -- nothing traced needs more.
 *
 * Why channel 0, and the derivation behind every constant below: qemu-
 * machine/README.md's Status section and README-history.md's newest
 * section, 2026-09-09. Short version: right after mtu2.c's channel 3
 * unblocks `cold_boot_hw_init`'s first task-readiness wait, the very next
 * call in the same init chain (`FUN_200b5be0(); FUN_200b5ea4();`) hits a
 * second busy-wait -- gated on GIC ID 41, confirmed (the same "ICDISRn
 * register-index*32+bit" SVD formula used throughout this project,
 * register index 1, bit 9) to be `DMAINT0`, DMA controller channel 0's
 * completion interrupt.
 *
 * Register offsets confirmed against ~/Downloads/rza1.svd's own DMAC
 * peripheral block (base 0xE8200000, matching RZA1H_DMAC_BASE): `N0SA_0`
 * (source address) = 0x00, `N0DA_0` (dest address) = 0x04, `N0TB_0`
 * (transfer byte count) = 0x08, `CHSTAT_0` = 0x24, `CHCTRL_0` = 0x28,
 * `CHCFG_0` = 0x2c, `CHITVL_0` = 0x30, `CHEXT_0` = 0x34, `DCTRL_0_7`
 * (shared, channels 0-7) = 0x300 -- all cross-checked directly against a
 * real natural boot's own writes (`FUN_200b5be0`/`FUN_200b5dc0`, full
 * disassembly trace in README-history.md): `CHCTRL_0` gets `|= 0x62`
 * (channel enable, real firmware bits, not interpreted further here) and
 * `CHCFG_0` gets a real, firmware-chosen configuration word (0x00222160,
 * likewise stored but not interpreted); the transfer itself is armed by
 * writing `N0SA_0`/`N0DA_0`/`N0TB_0` in that exact order, confirmed via
 * live register reads to be a real RAM buffer -> a GPIO-region destination
 * address, not guessed.
 *
 * GIC ID 41 confirmed live to be edge-triggered (`GICD_ICFGR2` bits
 * 18-19), unlike MTU2's level-triggered `TGI3A` -- so this device uses
 * `ostm.c`'s simpler `qemu_irq_pulse()` pattern, not `mtu2.c`'s
 * raise-and-hold one. Checked live before assuming this, per this
 * project's own established discipline (see `mtu2.c`'s own comment for
 * why that discipline exists).
 *
 * Simplification, deliberate (same philosophy as `ostm.c`/`mtu2.c`'s own
 * file comments -- not a faithful DMAC, just real enough for what this
 * boot path's own ISR needs): the real transfer is performed synchronously
 * via `address_space_read()`/`address_space_write()` (the same real-
 * firmware-pointer-values approach `scif.c`'s virtual front-panel
 * responder already established) as soon as `N0TB_0` -- the last of the
 * three "arm" registers this firmware always writes, confirmed via the
 * live disassembly order -- is written, with the completion IRQ fired
 * from a short one-shot delay rather than instantly: real DMA is
 * asynchronous, and nothing traced needs the transfer to take any
 * particular real time, just to not complete synchronously from inside
 * the triggering write itself. `CHCTRL_0`/`CHCFG_0`/`CHITVL_0`/`CHEXT_0`/
 * `CHSTAT_0`/`DCTRL_0_7` and every channel but 0 are plain stored/
 * read-back state, not interpreted -- nothing traced reads them back in a
 * way that depends on real bit-level DMAC semantics (in particular,
 * `CHSTAT_0`'s real transfer-end/error bits are not modeled -- the ISR
 * this project traced, `FUN_200b5b90`, doesn't consult them either, only
 * this project's own busy-wait flag).
 *
 * FIXED, 2026-09-09 -- a real `-icount` bug, not a value tuning issue: this
 * device originally used a raw `QEMUTimer` (`timer_new_ns`/`timer_mod`
 * against `QEMU_CLOCK_VIRTUAL`) for the completion delay below, unlike
 * `ostm.c`/`mtu2.c`'s `ptimer`-based devices. Confirmed live (a longer
 * boot trial once the job-ring-overflow fix was in, see qemu-machine/
 * README-history.md's newest sections) that this specific busy-wait
 * (`FUN_200b5ea4`, this device's own real completion IRQ target) genuinely
 * stalls forever under `-icount shift=auto` -- the guest CPU spinning at
 * ~91% real host CPU the whole time (confirmed via `ps`, not idle/WFE),
 * meaning the raw `QEMUTimer`'s callback never fired even though the guest
 * kept executing plenty of real instructions in its own busy-wait. `ostm.c`/
 * `mtu2.c`'s `ptimer`-based devices don't have this problem (confirmed --
 * OSTM's own real GIC IRQ is what the whole ring-overflow fix depends on
 * and demonstrably keeps working under icount). Ported to `ptimer` the same
 * way, as a one-shot delay line (a fixed 1GHz internal tick rate so the
 * limit value is directly the delay in nanoseconds -- `ptimer_run(timer,
 * 1)`'s own `oneshot` argument handles "fire once per arm", not a policy
 * flag).
 *
 * CORRECTED, 2026-09-10 -- the `ptimer` port above genuinely fixed the
 * "never fires" symptom, but a session that concluded from that fix (plus
 * host-side dmac.c print correlation and one GDB breakpoint at the ISR's
 * own entry) that the whole busy-wait "genuinely completes in under 1ms,
 * 8/8 trials" and filed the remaining mystery as a pure QEMU gdbstub
 * reliability artifact was WRONG -- it never independently checked the
 * actual RAM flag firmware itself reads, only that the device model's own
 * internal chain (arm -> ptimer -> IRQ -> ISR entry) runs fast. A fully
 * GDB-free direct read of that flag (0x203906EE, via QMP's
 * `human-monitor-command` -> `xp`, zero gdbstub involvement at all) on a
 * real natural boot showed it permanently stuck non-zero, confirmed across
 * multiple independent trials -- `FUN_200b5ea4` really does hang forever,
 * and (since it's `cold_boot_hw_init`'s very next call after DMAC and
 * shares the identical control struct) so does its sibling `FUN_200b5f38`,
 * which is never even reached.
 *
 * The real bug: `FUN_200b5dc0` (this firmware's own low-level "arm"
 * routine, shared by all 6 real callers of channel 0) writes `N0TB_0` --
 * the exact write this model treats as "start the completion ptimer" --
 * and only ONE instruction later marks its own struct busy (`strb r0,
 * [r2,#2]`, i.e. sets the very flag `FUN_200b5ea4`/`FUN_200b5f38` poll).
 * With the delay short enough (the original 1000ns, under `-icount`'s own
 * virtual-time pacing), the ptimer callback -- and the guest ISR it
 * triggers, which correctly clears that same flag -- can run to completion
 * in the single-instruction gap between the arm write and the firmware's
 * own busy=1 write. The firmware's delayed busy=1 then silently overwrites
 * an already-correct completion, permanently (the transfer is done, so
 * nothing will ever clear it again). Confirmed directly: raising
 * `DMAC_COMPLETE_DELAY_NS` alone (still 100% GDB-free QMP reads, no
 * breakpoint anywhere near the race) took the flag from permanently stuck
 * at 0x01 to reliably 0x00, 5/5 trials, at both 10ms and 100us -- real DMA
 * can't outrun the CPU's own very next instruction the way a sub-microsecond
 * model can, so this was always a self-inflicted emulation race, not a real
 * hardware race and not a GDB artifact. `DMAC_COMPLETE_DELAY_NS` raised to
 * 100us below (still an arbitrary, not-real-clock-accurate value, same
 * rationale as before -- see the manual-clock-domain research two
 * paragraphs up -- just comfortably clear of this specific race instead of
 * landing right inside its danger zone). With the fix in, boot progresses
 * well past this point for the first time since the `ptimer` port, straight
 * into the already-known `0x200b93fc` job-ring-overflow trap (reproduced
 * 2/2 fresh trials, ~20-45s in) -- see qemu-machine/README.md's Status
 * section for why that trap's own prior "confirmed clean under -icount"
 * finding needs a fresh look given it was never actually tested together
 * with a genuinely working DMAC channel 0 before now.
 *
 * Channels 1-7 (2026-09-24, fake FPGA, notes/fpga-link.md): the band scope's
 * sweep read uses ch1 (RSPI2 SPDR -> RAM, 475 bytes) and ch2 (a fixed 0x00 ->
 * SPDR, the clocks) together. Unlike ch0 these start on CHCTRL_n.SETEN (bit 0),
 * after N0SA/N0DA/N0TB are set. CHCTRL_n is write-only command bits here
 * (reads 0): the DMA-end ISRs do CHCTRL |= 0x62 by read-modify-write, and a
 * stored SETEN would restart the transfer. Only channels with SPDR2 on one
 * side are performed; any other SETEN on ch1-7 is logged and ignored, which
 * keeps whatever those channels did before (nothing). Transfers run from one
 * ptimer, RSPI2_BYTE_NS per byte after the SETEN, peripheral-bound (fixed
 * destination) channels first, so ch2's writes clock the FPGA's bytes into
 * rspi2.c's RX queue before ch1 reads them. Then DMAINTn (GIC 41+n) pulses.
 *
 * Streaming channels (2026-09-24, SSIF audio link, notes/dsp-protocol.md): a ch1-7 SETEN whose
 * CHCFG has REN (bit 30, continuous) and SSIF0/1 FIFO data on one side runs forever,
 * alternating register sets N0/N1 (RSW), one set per DMAC_STREAM_PERIOD (72 frames of the
 * 96 kHz I2S clock = the firmware's 0x240-byte buffers). Each completion sets CHSTAT END
 * (bit 6) and SR (bit 7) = the set now selected (1 = N1, i.e. N0 just finished). The firmware
 * polls CHSTAT from its 250 us tick, takes the finished buffer and clears END with CHCTRL CLREND.
 * CHSTAT EN (bit 0) reads 1 while streaming; the pumps redo the whole SSIF bring-up if it
 * doesn't. DMAINTn is not pulsed when CHCFG DEM (bit 24) masks it (the firmware sets DEM). One
 * ptimer serves every stream, so it's one expiry per 750 us, not one per word. */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HDmacState, RZA1H_DMAC)

struct RZA1HDmacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq0;              /* channel 0's DMAINT0, GIC ID 41 */
    ptimer_state *complete_timer;
    qemu_irq irq[RZA1H_DMAC_CHANNELS];  /* ch1..7 -> DMAINT1..7; [0] unused */
    ptimer_state *periph_timer;
    uint8_t periph_pending;     /* channels armed by SETEN, bit n = channel n */
    ptimer_state *stream_timer;
    uint8_t stream_active;      /* streaming channels, bit n = channel n */
    bool stream_running;        /* stream_timer is running */
    uint8_t stream_set[RZA1H_DMAC_CHANNELS];   /* register set the next completion uses */
    uint32_t chstat[RZA1H_DMAC_CHANNELS];      /* ch1-7 CHSTAT (EN, END, SR) */
    uint64_t stream_blocks[RZA1H_DMAC_CHANNELS];

    uint8_t regs[RZA1H_DMAC_SIZE]; /* plain backing store for every offset
                                     * this device doesn't special-case */
};

#define DMAC_N0SA_0  0x00
#define DMAC_N0DA_0  0x04
#define DMAC_N0TB_0  0x08
#define DMAC_CHCFG_0 0x2c
#define DMAC_CHCFG_SAD (1u << 20)  /* source address fixed */
#define DMAC_CHCFG_DAD (1u << 21)  /* destination address fixed */
#define DMAC_CH_STRIDE 0x40
#define DMAC_CHCTRL    0x28
#define DMAC_CHCTRL_SETEN 1u
/* ch1+ transfers are paced by their peripheral; the only one modelled is RSPI2 at
 * ~2 Mbit/s (see rspi2.c). */
#define DMAC_PERIPH_BYTE_NS 4000
#define DMAC_N1SA 0x0c
#define DMAC_N1DA 0x10
#define DMAC_N1TB 0x14
#define DMAC_CHSTAT 0x24
#define DMAC_CHSTAT_EN  (1u << 0)
#define DMAC_CHSTAT_END (1u << 6)
#define DMAC_CHSTAT_SR  (1u << 7)
#define DMAC_CHCTRL_CLREN  (1u << 1)
#define DMAC_CHCTRL_SWRST  (1u << 3)
#define DMAC_CHCTRL_CLREND (1u << 5)
#define DMAC_CHCFG_DEM (1u << 24)
#define DMAC_CHCFG_REN (1u << 30)
/* 72 frames of 96 kHz = one 0x240-byte SSIF buffer (see file comment). */
#define DMAC_STREAM_PERIOD_NS 750000

/* Real DMA is asynchronous -- an arbitrary short delay, same rationale as
 * ostm.c/mtu2.c's own frequency constants: not real-clock-accurate, just
 * enough that this doesn't look like a synchronous same-instruction
 * completion, and short enough that a boot-time busy-wait resolves in a
 * reasonable wall-clock testing time. See the FIXED note above for why
 * this is now delivered via `ptimer` at a fixed 1GHz tick rate (1 tick =
 * 1ns) rather than a raw `QEMUTimer`. Raised from the original 1000ns to
 * 100us, 2026-09-10 (see the CORRECTED note above) -- 1000ns was short
 * enough to race the firmware's own next instruction under `-icount`;
 * 100us confirmed clear of that race, 5/5 trials (also confirmed at 10ms,
 * kept at 100us as the smaller value that still tested clean). */
#define DMAC_COMPLETE_DELAY_NS 100000
#define DMAC_TIMER_FREQ_HZ 1000000000

static uint64_t rza1h_dmac_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HDmacState *s = RZA1H_DMAC(opaque);
    int ch = offset / DMAC_CH_STRIDE;
    uint64_t val = 0;

    if (ch >= 1 && ch < RZA1H_DMAC_CHANNELS &&
        offset % DMAC_CH_STRIDE == DMAC_CHSTAT && size == 4) {
        return s->chstat[ch];
    }
    memcpy(&val, &s->regs[offset], size);
    return val;
}

static uint32_t dmac_reg32(RZA1HDmacState *s, hwaddr off)
{
    uint32_t v;

    memcpy(&v, &s->regs[off], 4);
    return v;
}

/* Move channel ch's register set `set` (N0 or N1): TB bytes from SA to DA as its CHCFG says. */
static void rza1h_dmac_transfer_set(RZA1HDmacState *s, int ch, int set, bool quiet)
{
    AddressSpace *as = &address_space_memory;
    hwaddr base = ch * DMAC_CH_STRIDE + (set ? DMAC_N1SA : DMAC_N0SA_0);
    uint32_t src = dmac_reg32(s, base);
    uint32_t dst = dmac_reg32(s, base + 4);
    uint32_t count = dmac_reg32(s, base + 8);
    uint32_t cfg = dmac_reg32(s, ch * DMAC_CH_STRIDE + DMAC_CHCFG_0);

    if (!quiet) {
        rza1h_debug("dmac", "ch%d complete: src=%#x dst=%#x count=%u cfg=%08x, "
                   "pulsing DMAINT%d", ch, src, dst, count, cfg, ch);
    }

    if (count > 0 && (cfg & (DMAC_CHCFG_SAD | DMAC_CHCFG_DAD))) {
        /* A fixed peripheral address on either side (2026-09-23): the
         * band-switch shift-register driver (FUN_200b5dc0) streams a buffer
         * of PSR2 words into 0xFCFE3108, one 32-bit word per MTU2 ch2
         * request. Moving it as one block used to spill the words into
         * PSR3.. instead. Unit = the fixed side's size code (DDS/SDS). */
        int code = (cfg & DMAC_CHCFG_DAD) ? (cfg >> 16) & 0xf : (cfg >> 12) & 0xf;
        uint32_t unit = code <= 2 ? 1u << code : 4;
        uint8_t w[4];

        for (uint32_t i = 0; i + unit <= count; i += unit) {
            hwaddr sa = src + ((cfg & DMAC_CHCFG_SAD) ? 0 : i);
            hwaddr da = dst + ((cfg & DMAC_CHCFG_DAD) ? 0 : i);
            address_space_read(as, sa, MEMTXATTRS_UNSPECIFIED, w, unit);
            address_space_write(as, da, MEMTXATTRS_UNSPECIFIED, w, unit);
        }
    } else if (count > 0) {
        g_autofree uint8_t *buf = g_malloc(count);

        if (address_space_read(as, src, MEMTXATTRS_UNSPECIFIED,
                               buf, count) == MEMTX_OK) {
            address_space_write(as, dst, MEMTXATTRS_UNSPECIFIED, buf, count);
        }
    }
}

static void rza1h_dmac_transfer(RZA1HDmacState *s, int ch)
{
    rza1h_dmac_transfer_set(s, ch, 0, false);
}

static bool dmac_is_ssif_data(uint32_t a)
{
    return a == RZA1H_SSIF0_TDR || a == RZA1H_SSIF0_RDR ||
           a == RZA1H_SSIF1_TDR || a == RZA1H_SSIF1_RDR;
}

static void rza1h_dmac_stream_tick(void *opaque)
{
    RZA1HDmacState *s = RZA1H_DMAC(opaque);

    for (int ch = 1; ch < RZA1H_DMAC_CHANNELS; ch++) {
        int set = s->stream_set[ch];

        if (!(s->stream_active & (1u << ch))) {
            continue;
        }
        rza1h_dmac_transfer_set(s, ch, set, true);
        s->stream_set[ch] = !set;
        s->chstat[ch] = DMAC_CHSTAT_EN | DMAC_CHSTAT_END | (set ? 0 : DMAC_CHSTAT_SR);
        if (++s->stream_blocks[ch] % 4000 == 1) {
            rza1h_debug("dmac", "ch%d stream: block %" PRIu64 " (set N%d)", ch,
                       s->stream_blocks[ch], set);
        }
        if (!(dmac_reg32(s, ch * DMAC_CH_STRIDE + DMAC_CHCFG_0) & DMAC_CHCFG_DEM)) {
            qemu_irq_pulse(s->irq[ch]);
        }
    }
}

static void rza1h_dmac_stream_update(RZA1HDmacState *s)
{
    ptimer_transaction_begin(s->stream_timer);
    if (s->stream_active && !s->stream_running) {
        ptimer_set_limit(s->stream_timer, DMAC_STREAM_PERIOD_NS, 1);
        ptimer_run(s->stream_timer, 0);   /* periodic */
        s->stream_running = true;
    } else if (!s->stream_active && s->stream_running) {
        ptimer_stop(s->stream_timer);
        s->stream_running = false;
    }
    ptimer_transaction_commit(s->stream_timer);
}

static void rza1h_dmac_ch0_complete(void *opaque)
{
    RZA1HDmacState *s = RZA1H_DMAC(opaque);

    rza1h_dmac_transfer(s, 0);
    /* Edge, not level -- see file comment. */
    qemu_irq_pulse(s->irq0);
}

static void rza1h_dmac_periph_complete(void *opaque)
{
    RZA1HDmacState *s = RZA1H_DMAC(opaque);
    uint8_t pending = s->periph_pending;

    s->periph_pending = 0;
    /* Writes to a peripheral first (they produce what the reads collect). */
    for (int pass = 0; pass < 2; pass++) {
        for (int ch = 1; ch < RZA1H_DMAC_CHANNELS; ch++) {
            bool to_periph = dmac_reg32(s, ch * DMAC_CH_STRIDE + DMAC_CHCFG_0) &
                             DMAC_CHCFG_DAD;

            if ((pending & (1u << ch)) && to_periph == (pass == 0)) {
                rza1h_dmac_transfer(s, ch);
                qemu_irq_pulse(s->irq[ch]);
            }
        }
    }
}

static void rza1h_dmac_seten(RZA1HDmacState *s, int ch)
{
    hwaddr base = ch * DMAC_CH_STRIDE;
    uint32_t src = dmac_reg32(s, base + DMAC_N0SA_0);
    uint32_t dst = dmac_reg32(s, base + DMAC_N0DA_0);
    uint32_t count = dmac_reg32(s, base + DMAC_N0TB_0);

    if ((dmac_reg32(s, base + DMAC_CHCFG_0) & DMAC_CHCFG_REN) &&
        (dmac_is_ssif_data(src) || dmac_is_ssif_data(dst))) {
        if (!(s->stream_active & (1u << ch))) {
            rza1h_debug("dmac", "ch%d stream start: src=%#x dst=%#x count=%u", ch, src,
                       dst, count);
        }
        s->stream_active |= 1u << ch;
        s->stream_set[ch] = 0;
        s->chstat[ch] = DMAC_CHSTAT_EN;
        rza1h_dmac_stream_update(s);
        return;
    }
    if (src != RZA1H_RSPI2_SPDR && dst != RZA1H_RSPI2_SPDR) {
        rza1h_debug("dmac", "ch%d SETEN ignored (src=%#x dst=%#x count=%u): "
                   "not an RSPI2 transfer", ch, src, dst, count);
        return;
    }
    s->periph_pending |= 1u << ch;
    ptimer_transaction_begin(s->periph_timer);
    ptimer_set_count(s->periph_timer, MAX(count, 1u) * DMAC_PERIPH_BYTE_NS);
    ptimer_run(s->periph_timer, 1);
    ptimer_transaction_commit(s->periph_timer);
}

static void rza1h_dmac_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HDmacState *s = RZA1H_DMAC(opaque);
    int ch = offset / DMAC_CH_STRIDE;

    if (ch >= 1 && ch < RZA1H_DMAC_CHANNELS &&
        offset % DMAC_CH_STRIDE == DMAC_CHCTRL) {
        if (value & DMAC_CHCTRL_SWRST) {
            s->chstat[ch] &= DMAC_CHSTAT_EN;
        }
        if (value & DMAC_CHCTRL_CLREND) {
            s->chstat[ch] &= ~DMAC_CHSTAT_END;
        }
        if (value & DMAC_CHCTRL_CLREN) {
            if (s->stream_active & (1u << ch)) {
                rza1h_debug("dmac", "ch%d stream stop", ch);
            }
            s->stream_active &= ~(1u << ch);
            s->chstat[ch] &= ~DMAC_CHSTAT_EN;
            rza1h_dmac_stream_update(s);
        }
        if (value & DMAC_CHCTRL_SETEN) {
            rza1h_dmac_seten(s, ch);
        }
        return;     /* command bits, not stored -- see file comment */
    }
    memcpy(&s->regs[offset], &value, size);
    if (offset == DMAC_N0TB_0) {
        rza1h_debug("dmac", "ch0 armed: N0TB_0 write count=%u -- ptimer set for %d ns",
                   (unsigned)value, DMAC_COMPLETE_DELAY_NS);
        ptimer_transaction_begin(s->complete_timer);
        ptimer_set_count(s->complete_timer, DMAC_COMPLETE_DELAY_NS);
        ptimer_run(s->complete_timer, 1); /* oneshot */
        ptimer_transaction_commit(s->complete_timer);
    }
}

static const MemoryRegionOps rza1h_dmac_ops = {
    .read = rza1h_dmac_read,
    .write = rza1h_dmac_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_dmac_reset(DeviceState *dev)
{
    RZA1HDmacState *s = RZA1H_DMAC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    ptimer_transaction_begin(s->complete_timer);
    ptimer_stop(s->complete_timer);
    ptimer_transaction_commit(s->complete_timer);
    s->periph_pending = 0;
    s->stream_active = 0;
    s->stream_running = false;
    memset(s->chstat, 0, sizeof(s->chstat));
    ptimer_transaction_begin(s->stream_timer);
    ptimer_stop(s->stream_timer);
    ptimer_transaction_commit(s->stream_timer);
    ptimer_transaction_begin(s->periph_timer);
    ptimer_stop(s->periph_timer);
    ptimer_transaction_commit(s->periph_timer);
}

static void rza1h_dmac_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HDmacState *s = RZA1H_DMAC(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_dmac_ops, s,
                          TYPE_RZA1H_DMAC, RZA1H_DMAC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq0);
    for (int ch = 1; ch < RZA1H_DMAC_CHANNELS; ch++) {
        sysbus_init_irq(sbd, &s->irq[ch]);   /* sysbus IRQ index ch */
    }
}

static void rza1h_dmac_realize(DeviceState *dev, Error **errp)
{
    RZA1HDmacState *s = RZA1H_DMAC(dev);

    s->complete_timer = ptimer_init(rza1h_dmac_ch0_complete, s,
                                    PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                    PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->complete_timer);
    ptimer_set_freq(s->complete_timer, DMAC_TIMER_FREQ_HZ);
    ptimer_transaction_commit(s->complete_timer);

    s->periph_timer = ptimer_init(rza1h_dmac_periph_complete, s,
                                  PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                  PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->periph_timer);
    ptimer_set_freq(s->periph_timer, DMAC_TIMER_FREQ_HZ);
    ptimer_transaction_commit(s->periph_timer);

    s->stream_timer = ptimer_init(rza1h_dmac_stream_tick, s,
                                  PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                  PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->stream_timer);
    ptimer_set_freq(s->stream_timer, DMAC_TIMER_FREQ_HZ);
    ptimer_transaction_commit(s->stream_timer);
}

static void rza1h_dmac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_dmac_realize;
    device_class_set_legacy_reset(dc, rza1h_dmac_reset);
}

static const TypeInfo rza1h_dmac_info = {
    .name          = TYPE_RZA1H_DMAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HDmacState),
    .instance_init = rza1h_dmac_init,
    .class_init    = rza1h_dmac_class_init,
};

static void rza1h_dmac_register_types(void)
{
    type_register_static(&rza1h_dmac_info);
}

type_init(rza1h_dmac_register_types)
