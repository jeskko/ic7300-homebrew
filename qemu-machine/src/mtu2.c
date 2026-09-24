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
 *
 * TGI4B, GIC ID 160 (2026-09-20, ring-overflow-fix follow-on session):
 * once `riic.c`'s own `SR2_TEND` fix let boot past the RIIC2 ring-overflow
 * trap for the first time, it parked instead on a bounded busy-wait inside
 * `FUN_200b3c5c` (the internal tuner relay-network's own cold-boot init) --
 * traced to `tuner_relay_serial_bus_init`'s registration of
 * `tuner_relay_tstb_strobe_dispatch` as event `0xa0` = **160 decimal =
 * TGI4B** (confirmed against `scratch/r01an5093ej0170-rza1-swpkg`'s
 * `INTC_ID_TGI4B`), never modeled before now. `FUN_200b3844` (the
 * relay-shift-out helper this same cluster calls) arms it with
 * `TGRB_4 = TCNT_4 + 0x480`, the exact same "explicit TGRx/TCR write while
 * already running" arming style as TGI4A (not TSTR-transition-triggered
 * like TGI4C) -- modeled identically to TGI4A's own event, just its own
 * TGRB_4/bit-1 pair, sharing channel 4's TCR/TIER/TSR with TGI4A/TGI4C as
 * real hardware does. `TGRB_4`'s own offset (`0x21e`) wasn't previously
 * documented in this file -- derived from `FUN_200b3844`'s own decompiled
 * pointer arithmetic (`DAT_200b40b0+0x1e`, where `DAT_200b40b0` is
 * channel 3's own confirmed base `0xFCFF0200`), landing exactly 2 bytes
 * past the already-confirmed `TGRA_4` (`0x21c`) -- the expected real MTU2
 * layout (A/B adjacent, C/D in a separate bank further out, matching
 * `TGRC_4`'s already-confirmed `0x224`).
 *
 * TGI4D, GIC ID 162 (2026-09-21, icom-main-idle-loop-not-reached thread): once the
 * TGI4B fix above let boot past the tuner-relay-network's own cold-boot init,
 * `main_idle_loop` was *still* unreachable -- a live QMP-only poll (no gdbstub
 * involvement at all, this project's own established "cross-check any breakpoint-
 * derived stuck-forever claim" discipline) of `shared_job_ring_dispatch`'s own ring
 * state showed `read_idx` catching up to `write_idx` (ring genuinely empty, last
 * queued job was job_type 3, `rspi2_transmit`) within the first ~10s of boot, yet
 * `main_idle_loop`'s own gating flag (`0x203906ed`, `shared_job_ring_dispatch`'s
 * `DAT_200b1cac`) stayed stuck at 1 for a full 300s capture regardless --
 * `shared_job_ring_dispatch`'s own top-of-function "ring already empty -> clear the
 * flag and return" check is real (confirmed from its decompile) but is only ever
 * evaluated when something calls the dispatcher *again*; nothing was doing that once
 * the ring drained. Traced `rspi2_transmit`'s own event-0xa2 registration
 * (`rspi2_driver_init`) to its handler, `rspi2_wait_ready` (`0x200b6444`) -- which,
 * after its own real-hardware busy-wait on RSPI2's `SPSR2` (already confirmed
 * working, unrelated to this bug), tail-calls `shared_job_ring_dispatch(0)` itself.
 * That call is the *only* place left in the whole traced chain that ever re-checks
 * the ring once it empties -- and it can only ever run if event 0xa2 (= 162 decimal
 * = **TGI4D**, confirmed against `scratch/r01an5093ej0170-rza1-swpkg`'s own
 * `TGI4D_IRQn = 162`) actually fires, which nothing in this device modeled before
 * now. `rspi2_transmit`'s own arm sequence (`*(short*)(DAT_200b7370+0x2a) =
 * *(short*)(DAT_200b7370+0x12) + (count+1)*0xa0`, with `DAT_200b7370` confirmed live
 * (raw `body.bin` memory read) to hold the identical `0xFCFF0200` channel-3/4 base
 * every other channel-4 event already uses) is modeled exactly like TGI4B: an
 * explicit `TGRD_4`/`TCR_4` write while `CST4` is already running, not a `TSTR`-
 * transition arm (same `arms_on_tstr = false` style as TGI4A/TGI4B). `TGRD_4`'s own
 * offset (`0x2a` from the channel-3/4 base) lands 6 bytes past `TGRC_4` (`0x224`,
 * already confirmed) -- not immediately adjacent the way `TGRA_4`/`TGRB_4` are, but
 * this project's own established policy is to trust a live-derived pointer-
 * arithmetic offset over an assumed adjacency pattern, so `0x22a` is used as found,
 * not "corrected" to `0x226`. `TSR_4`/`TIER_4` bit 3 (the next free bit after
 * TGI4C's bit 2) confirmed the same way: `rspi2_transmit` and `rspi2_wait_ready`
 * both clear `TSR_4` bit 3 (`FUN_20360adc(DAT_200b7310, 0, 3, 8)`, mask `0x08` = bit
 * 3) around their own arm/wait sequences, the same "A/B/C/D = bits 0/1/2/3"
 * convention already established for A-C.
 *
 * A fourth event, `0x30c`/`0x305` bit 2 (2026-09-09, DSP-comms session,
 * continued -- see MTU2_DSP_PACE_TARGET/STATUS below): `scif5_cmd_
 * transmit_now`'s own real busy-wait after unblocking `scif5_send_and_
 * wait_reply`'s reply-ready flag. Confirmed live via `gdbrsp.py`'s
 * `set_watchpoint` (a write watchpoint on `0xFCFF0305` across a full
 * 90-second free-run boot, zero hits) that nothing ever writes this
 * status bit at all -- not a wrong value, a genuinely unmodeled real
 * compare-match, same shape as TGI3A/TGI4A/TGI4C each were before this
 * device existed. `0xFCFF0306` (the free-running counter this compare
 * reads to compute its own relative target, `TCNT + 0x280` in the one
 * caller found, `scif5_cmd_transmit_now`'s own direct-send rate-limiter)
 * has many more read sites across unrelated subsystems (not just SCIF5),
 * so this is very likely a real, widely-shared system timer, not a
 * SCIF5-private one -- only this one compare/status pair is modeled,
 * matching this file's own established narrow-scope philosophy. Modeled
 * as a host-wall-clock deadline rather than a live 16-bit counter plus
 * comparison (sidesteps 16-bit wraparound math entirely) -- a further,
 * consistent application of the same "fixed one-shot period, not the
 * guest's real relative delay" simplification already established for
 * TGI4A above.
 *
 * A fifth event, `0x308`/`0x305` bit 0 (2026-09-09, continuing straight
 * on): confirmed live via a free-run poll the same way the fourth event
 * was -- once that fix let `scif5_cmd_transmit_now`'s and `rspi2_
 * transmit`'s own busy-waits (the latter needing `rspi2.c`, a genuinely
 * new peripheral) resolve, boot reached previously entirely unanalyzed
 * firmware (no Ghidra function symbol existed yet at its entry) and
 * parked on a *different* bit of `MTU2_DSP_PACE_STATUS`'s own byte,
 * paired with a *different* target register (`0x308`, not `0x30c`) --
 * confirmed by reading the raw disassembly's actual branch condition
 * (one of three near-identical-looking wait blocks in the new code
 * turned out to use the *opposite* branch sense from the other two, a
 * real, meaningful difference easy to miss skimming rather than reading
 * closely). Two independent callers arm it (both via a shared helper,
 * `references_to` confirmed only these two), both with the identical
 * literal period `0x7d00`/32000 -- unlike the fourth event, this one's
 * real requested period genuinely is known, so it's honored exactly
 * (`MTU2_SWTIMER_B_ONESHOT_NS`) rather than approximated. This caller
 * isn't SCIF5/DSP-comms related (reached via a DMA-descriptor-setup
 * routine, very plausibly graphics/display given `ui_graphics_lifecycle_
 * task`'s already-confirmed EGL work) -- confirming this is a genuinely
 * shared, multi-subsystem software-timeout facility (one counter, several
 * independent compare/status pairs), not something SCIF5-private that
 * happened to get reused.
 *
 * A sixth event, `0x30e`/`0x305` bit 3 -- TGRD_0/TGFD_0, both named
 * straight out of the manual's own channel-0 register table (2026-09-21,
 * icom-main-idle-loop-not-reached thread). Same one-counter/many-compare-
 * pairs facility again, and the pattern this file predicted: a third
 * compare/status pair on the identical channel-0 counter. Found once
 * `openvg.c` (new this session) unblocked the OpenVG graphics bring-up
 * that had been deadlocking the whole boot -- past that point a free-run
 * poll showed the PC newly parked in `FUN_200b7d64`'s busy-wait at
 * `0x200b7f6c`, spinning on this exact bit. Unlike the fourth and fifth
 * events, both ends are fully identified here: `FUN_200b7b40` arms it
 * (`TGRD_0 = TCNT_0 + delta`, then clears the flag) and `FUN_200b7d64`
 * consumes it, so the model honors the written delta exactly rather than
 * approximating a period. See `MTU2_SWTIMER_C_TARGET` and the
 * `swtimer_c_deadline_ns` field for the verbatim firmware and the exact
 * reasoning.
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
#include "rza1h_debug.h"

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
    int64_t tcnt3_base_ns; /* virtual time at which the free-running TCNT_3 read 0 */
    RZA1HMtu2Event ch4a; /* TGI4A, GIC ID 159 */
    RZA1HMtu2Event ch4b; /* TGI4B, GIC ID 160 -- see file comment */
    RZA1HMtu2Event ch4c; /* TGI4C, GIC ID 161 */
    RZA1HMtu2Event ch4d; /* TGI4D, GIC ID 162 -- see file comment's TGI4D paragraph */

    /* scif5_cmd_transmit_now's own rate-limiter compare-match -- purely
     * polled (no IRQ registration found anywhere near it), see file
     * comment's Simplifications paragraph. INT64_MAX = never armed /
     * always "not yet expired". */
    int64_t dsp_pace_deadline_ns;

    /* A second, independent compare-match sharing the same status byte
     * (`0xFCFF0305`, a different bit) and the same underlying counter
     * (`0xFCFF0306`) as the one above -- confirmed live 2026-09-09,
     * reached from a DMA-descriptor-setup routine, not SCIF5 (see file
     * comment's "A fifth event" paragraph for the derivation and why the
     * caller's real purpose isn't claimed further than that). */
    int64_t swtimer_b_deadline_ns;

    /* A third compare-match on the same channel-0 status byte, added
     * 2026-09-21 (icom-main-idle-loop-not-reached thread): TGRD_0 ->
     * TSR_0 bit 3 (TGFD_0). Unlike the two above this one's arming code
     * is fully identified: FUN_200b7b40 (body.bin 0x200b7b40) does,
     * verbatim,
     *     *(short *)(0xFCFF0300 + 0xe) = delta + *(short *)(0xFCFF0300 + 6);
     *     FUN_20360adc(0xFCFF0305, 0, 3, 8);   // clear TSR_0 bit 3
     * i.e. TGRD_0 = TCNT_0 + delta, then clear the flag -- the exact same
     * idiom the two events above already use for TGRC_0/TGRA_0. Its one
     * consumer is FUN_200b7d64's own busy-wait at 0x200b7f6c:
     *     do { if (*0x20390763 == 0) break;
     *     } while (FUN_20360b24(0xFCFF0305, 3, 8) == 0);
     * which spins forever while the flag never sets. That busy-wait is
     * only reachable once openvg.c unblocked the graphics bring-up (see
     * that file's comment) -- before then boot deadlocked earlier and this
     * code was never reached at all, which is why this gap only surfaced
     * now. */
    int64_t swtimer_c_deadline_ns;

    uint8_t regs[RZA1H_MTU2_SIZE]; /* plain backing store for every offset
                                     * this device doesn't special-case --
                                     * matches the add_plain_ram_region()
                                     * behavior this device replaces */
};

#define MTU2_TCR_3   0x200
#define MTU2_TIER_3  0x208
#define MTU2_TGRA_3  0x218
#define MTU2_TCNT_3  0x210
#define MTU2_TSR_3   0x22c

#define MTU2_TCR_4   0x201
#define MTU2_TIER_4  0x209
#define MTU2_TGRA_4  0x21c
#define MTU2_TGRB_4  0x21e /* see file comment's TGI4B paragraph for the derivation */
#define MTU2_TGRC_4  0x224
#define MTU2_TGRD_4  0x22a /* see file comment's TGI4D paragraph for the derivation
                             * (rspi2_transmit's own `DAT_200b7370+0x2a` pointer
                             * arithmetic, DAT_200b7370 confirmed == the same
                             * 0xFCFF0200 channel-3/4 base every other channel-4
                             * event already uses) -- not adjacent to TGRC_4 the
                             * way TGRA_4/TGRB_4 are, used as found, not "corrected" */
#define MTU2_TSR_4   0x22d

#define MTU2_TSTR    0x280
#define MTU2_TSTR_CST3   (1 << 6)
#define MTU2_TSTR_CST4   (1 << 7)

/* scif5_cmd_transmit_now's own rate-limiter compare-match -- see file
 * comment's Simplifications paragraph (the "A fourth event" note). */
#define MTU2_DSP_PACE_TARGET 0x30c /* 16-bit; writing this arms the deadline */
#define MTU2_DSP_PACE_STATUS 0x305 /* byte; bit 2 is this event's own flag */
#define MTU2_DSP_PACE_STATUS_BIT (1 << 2)
/* 50us -- small, real, and well under this project's own established
 * multi-second free-run poll intervals; see file comment for why exact
 * tick-for-tick fidelity isn't needed here (same rationale as TGI4A's own
 * MTU2_CH4A_ONESHOT_COUNTS). */
#define MTU2_DSP_PACE_ONESHOT_NS 50000

/* A second, independent compare-match sharing MTU2_DSP_PACE_STATUS's own
 * byte (a different bit) -- see file comment's "A fifth event" paragraph. */
#define MTU2_SWTIMER_B_TARGET 0x308 /* 16-bit; writing this arms the deadline */
#define MTU2_SWTIMER_B_STATUS_BIT (1 << 0) /* same status byte, MTU2_DSP_PACE_STATUS */
/* Unlike MTU2_DSP_PACE_ONESHOT_NS, this one's real requested period is
 * actually known (both call sites request the same literal 0x7d00/32000
 * -- confirmed via `references_to`, not assumed) -- honored exactly
 * rather than guessed, at this device's own already-established
 * MTU2_FREQ_HZ: 32000 / 32000000 = 1.000ms. */
#define MTU2_SWTIMER_B_ONESHOT_NS ((int64_t)32000 * NANOSECONDS_PER_SECOND \
                                  / MTU2_FREQ_HZ)

/* A third compare-match on the same channel-0 status byte -- TGRD_0 ->
 * TSR_0 bit 3 (TGFD_0), both straight out of the RZ/A1H manual's own
 * channel-0 register table (TGRD_0 = H'FCFF030E, TSR_0 = H'FCFF0305). See
 * the swtimer_c_deadline_ns field comment for the firmware side. The
 * requested interval is honored exactly rather than fixed: the arming
 * write is TGRD_0 = TCNT_0 + delta, and since this model never advances
 * TCNT_0 (it reads back the 0 the channel-0 init at body.bin 0x20005adc
 * left there), the value written IS the delta in MTU2_FREQ_HZ counts. The
 * one traced caller passes 0x200, i.e. 512/32MHz = 16us. */
#define MTU2_SWTIMER_C_TARGET 0x30e /* 16-bit; writing this arms the deadline */
#define MTU2_SWTIMER_C_STATUS_BIT (1 << 3) /* same status byte, MTU2_DSP_PACE_STATUS */

/* EXPERIMENT, 2026-09-09 -- was 25MHz, chosen empirically (see git history/
 * README-history.md's "MTU2 channel 4 built; a generic RTOS job-queue
 * overflow" section for the full derivation of that number, and why an
 * initial "genuine emulator scheduler bug" hypothesis was wrong) to dodge a
 * real RTOS job-queue overflow while still much faster than the SoC's real
 * clock -- explicitly NOT a claim that 25MHz is RZ/A1H's real Pφ.
 *
 * Now the real, derived value, found the same way OSTM's own real
 * OSTM_FREQ_HZ was (see ostm.c's own comment): channels 3 and 4's real TCR
 * register (confirmed via direct disassembly of this device's own already-
 * identified init functions, FUN_20005c08/FUN_20005cac) is written as a
 * literal 0x00 for both channels -- decoded against the RZ/A1H manual's
 * Table 10.9 (channel-3/4-specific TPSC encoding, distinct from channels
 * 0-2's own table), TPSC=000 means "count on P0φ/1", i.e. no division at
 * all. Combined with P0φ's own real, schematic-confirmed value (32.00MHz,
 * see ostm.c's file comment for the X301/USB_X1 derivation), both
 * channels' real rate is exactly 32,000,000 Hz -- the identical clean
 * 1.000ms-period base OSTM0 itself already uses, not a coincidence: both
 * peripherals sit on the same real P0φ domain.
 *
 * Testing live whether restoring this real value (now that ostm.c pairs
 * its own real clock with QEMU -icount, fixing the job-ring overflow this
 * constant was originally detuned to dodge) still holds up -- the original
 * queue-overflow class of bug this constant was created to avoid could in
 * principle resurface at a different rate; not yet confirmed either way.
 * See README-history.md for the result once tested -- revert to 25000000
 * if this doesn't hold up. */
#define MTU2_FREQ_HZ 32000000

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

/* Channel 3's TCNT as a live free-running 16-bit counter (2026-09-24). The system tick
 * ISR (0x20005b98) re-arms TGI3A with `TGRA_3 += 8000` -- a compare match 8000 counts (250 us)
 * after the previous one on a free-running counter. Modelling it as "one match per 16-bit
 * wrap" (the earlier simplification) made the tick 2.048 ms, 8.2x slow: the main loop, the
 * 500 us tick (FUN_200b7910) and the SSIF audio pumps all ran 8x too rarely (the audio pumps
 * missed ~63% of DMA buffers). Now each TGRA_3 write schedules the match at
 * (TGRA_3 - TCNT_3) mod 65536 counts from now, exactly like the hardware. */
static uint16_t rza1h_mtu2_tcnt3(RZA1HMtu2State *s)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->tcnt3_base_ns;

    return (uint16_t)muldiv64(ns, MTU2_FREQ_HZ, NANOSECONDS_PER_SECOND);
}

static void rza1h_mtu2_ch3a_arm(RZA1HMtu2State *s)
{
    uint16_t tgr = s->regs[MTU2_TGRA_3] | (s->regs[MTU2_TGRA_3 + 1] << 8);
    uint32_t delta = (uint16_t)(tgr - rza1h_mtu2_tcnt3(s));

    ptimer_transaction_begin(s->ch3a.timer);
    ptimer_set_limit(s->ch3a.timer, delta ? delta : 0x10000, 1);
    ptimer_run(s->ch3a.timer, 0);
    ptimer_transaction_commit(s->ch3a.timer);
}

static void rza1h_mtu2_rearm(RZA1HMtu2State *s, RZA1HMtu2Event *ev)
{
    if (ev == &s->ch3a) {
        rza1h_mtu2_ch3a_arm(s);
        return;
    }
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

    if (offset == MTU2_TCNT_3 && size == 2) {
        return rza1h_mtu2_tcnt3(s);
    }

    if (offset == MTU2_DSP_PACE_STATUS && size == 1) {
        /* Computed on read, not maintained by a ptimer callback --
         * sidesteps needing a live 16-bit counter/wraparound at all, see
         * file comment. Persisted into regs[] once observed expired so a
         * plain memcpy elsewhere (e.g. a wider read spanning this byte)
         * stays consistent with what this exact read already saw. Two
         * independent bits, two independent deadlines, one shared byte. */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (s->dsp_pace_deadline_ns != INT64_MAX &&
            now >= s->dsp_pace_deadline_ns &&
            !(s->regs[offset] & MTU2_DSP_PACE_STATUS_BIT)) {
            s->regs[offset] |= MTU2_DSP_PACE_STATUS_BIT;
            rza1h_debug("mtu2", "dsp_pace expired (0x305 bit 2 now set)");
        }
        if (s->swtimer_b_deadline_ns != INT64_MAX &&
            now >= s->swtimer_b_deadline_ns &&
            !(s->regs[offset] & MTU2_SWTIMER_B_STATUS_BIT)) {
            s->regs[offset] |= MTU2_SWTIMER_B_STATUS_BIT;
            rza1h_debug("mtu2", "swtimer_b expired (0x305 bit 0 now set)");
        }
        if (s->swtimer_c_deadline_ns != INT64_MAX &&
            now >= s->swtimer_c_deadline_ns &&
            !(s->regs[offset] & MTU2_SWTIMER_C_STATUS_BIT)) {
            s->regs[offset] |= MTU2_SWTIMER_C_STATUS_BIT;
            rza1h_debug("mtu2", "swtimer_c expired (0x305 bit 3 now set)");
        }
    }

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
        /* Shared by all four channel-4 events -- clearing affects
         * whichever bit(s) the write actually targets; re-evaluate all. */
        s->regs[offset] &= (uint8_t)value;
        rza1h_mtu2_update_irq(s, &s->ch4a);
        rza1h_mtu2_update_irq(s, &s->ch4b);
        rza1h_mtu2_update_irq(s, &s->ch4c);
        rza1h_mtu2_update_irq(s, &s->ch4d);
        return;
    case MTU2_TIER_3:
        s->regs[offset] = (uint8_t)value;
        rza1h_mtu2_update_irq(s, &s->ch3a);
        return;
    case MTU2_TIER_4:
        s->regs[offset] = (uint8_t)value;
        rza1h_mtu2_update_irq(s, &s->ch4a);
        rza1h_mtu2_update_irq(s, &s->ch4b);
        rza1h_mtu2_update_irq(s, &s->ch4c);
        rza1h_mtu2_update_irq(s, &s->ch4d);
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
    case MTU2_TCNT_3:
        s->tcnt3_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                           muldiv64((uint16_t)value, NANOSECONDS_PER_SECOND, MTU2_FREQ_HZ);
        memcpy(&s->regs[offset], &value, size);
        return;
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
        /* Shared prescaler/CCLR for all four channel-4 events -- a live
         * reprogram could in principle affect any of their real periods,
         * so rearm all four if channel 4 is already running. */
        memcpy(&s->regs[offset], &value, size);
        if (s->regs[MTU2_TSTR] & MTU2_TSTR_CST4) {
            rza1h_mtu2_rearm(s, &s->ch4a);
            rza1h_mtu2_rearm(s, &s->ch4b);
            rza1h_mtu2_rearm(s, &s->ch4c);
            rza1h_mtu2_rearm(s, &s->ch4d);
        }
        return;
    case MTU2_TGRA_4:
        ev = &s->ch4a;
        goto ch4_configure;
    case MTU2_TGRB_4:
        ev = &s->ch4b;
        goto ch4_configure;
    case MTU2_TGRC_4:
        ev = &s->ch4c;
        goto ch4_configure;
    case MTU2_TGRD_4:
        ev = &s->ch4d;
ch4_configure:
        memcpy(&s->regs[offset], &value, size);
        if (s->regs[MTU2_TSTR] & MTU2_TSTR_CST4) {
            rza1h_mtu2_rearm(s, ev);
        }
        return;
    case MTU2_DSP_PACE_TARGET:
        memcpy(&s->regs[offset], &value, size);
        s->regs[MTU2_DSP_PACE_STATUS] &= ~MTU2_DSP_PACE_STATUS_BIT;
        s->dsp_pace_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                  MTU2_DSP_PACE_ONESHOT_NS;
        rza1h_debug("mtu2", "dsp_pace armed (0x305 bit 2), deadline +%d ns",
                   MTU2_DSP_PACE_ONESHOT_NS);
        return;
    case MTU2_SWTIMER_B_TARGET:
        memcpy(&s->regs[offset], &value, size);
        s->regs[MTU2_DSP_PACE_STATUS] &= ~MTU2_SWTIMER_B_STATUS_BIT;
        s->swtimer_b_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                   MTU2_SWTIMER_B_ONESHOT_NS;
        rza1h_debug("mtu2", "swtimer_b armed (0x305 bit 0), deadline +%lld ns",
                   (long long)MTU2_SWTIMER_B_ONESHOT_NS);
        return;
    case MTU2_SWTIMER_C_TARGET: {
        /* TGRD_0 = TCNT_0 + delta; TCNT_0 is never advanced by this model,
         * so the stored value is the delta in MTU2_FREQ_HZ counts. */
        int64_t ns = (int64_t)(value & 0xffff) * NANOSECONDS_PER_SECOND
                     / MTU2_FREQ_HZ;

        memcpy(&s->regs[offset], &value, size);
        s->regs[MTU2_DSP_PACE_STATUS] &= ~MTU2_SWTIMER_C_STATUS_BIT;
        s->swtimer_c_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns;
        rza1h_debug("mtu2", "swtimer_c armed (0x305 bit 3), deadline +%lld ns",
                   (long long)ns);
        return;
    }
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

/* After the first compare match of an explicitly armed (TGI4A-style) event, the real
 * free-running TCNT only matches TGRx again once per full 16-bit wrap: 0x10000 counts =
 * 2.048 ms at MTU2_FREQ_HZ. Until 2026-09-24 rearm's periodic ptimer kept re-firing at the
 * 16 us MTU2_CH4A_ONESHOT_COUNTS delay instead -- ~62.5 kHz each for TGI4A/4B/4D, forever,
 * and every expiry is a vCPU <-> main-loop handoff under -icount. That storm was the
 * emulator's single biggest cost (tools/bench_boot.py; ~190k callbacks per emulated
 * second). Called from inside ptimer_tick's own transaction. */
static void rza1h_mtu2_after_fire(RZA1HMtu2Event *ev)
{
    if (!ev->arms_on_tstr) {
        ptimer_set_limit(ev->timer, 0x10000, 1);
    }
}

static void rza1h_mtu2_ch3a_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    rza1h_debug("mtu2", "ch3 TGI3A compare-match (GIC 154)");
    ptimer_set_limit(s->ch3a.timer, 0x10000, 1);   /* next match: a full wrap later */
    s->regs[MTU2_TSR_3] |= (1 << s->ch3a.bit);
    rza1h_mtu2_update_irq(s, &s->ch3a);
}

static void rza1h_mtu2_ch4a_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    rza1h_debug("mtu2", "ch4 TGI4A compare-match (GIC 159)");
    s->regs[MTU2_TSR_4] |= (1 << s->ch4a.bit);
    rza1h_mtu2_update_irq(s, &s->ch4a);
    rza1h_mtu2_after_fire(&s->ch4a);
}

static void rza1h_mtu2_ch4b_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    rza1h_debug("mtu2", "ch4 TGI4B compare-match (GIC 160)");
    s->regs[MTU2_TSR_4] |= (1 << s->ch4b.bit);
    rza1h_mtu2_update_irq(s, &s->ch4b);
    rza1h_mtu2_after_fire(&s->ch4b);
}

static void rza1h_mtu2_ch4c_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    rza1h_debug("mtu2", "ch4 TGI4C compare-match (GIC 161)");
    s->regs[MTU2_TSR_4] |= (1 << s->ch4c.bit);
    rza1h_mtu2_update_irq(s, &s->ch4c);
}

static void rza1h_mtu2_ch4d_tick(void *opaque)
{
    RZA1HMtu2State *s = RZA1H_MTU2(opaque);

    rza1h_debug("mtu2", "ch4 TGI4D compare-match (GIC 162)");
    s->regs[MTU2_TSR_4] |= (1 << s->ch4d.bit);
    rza1h_mtu2_update_irq(s, &s->ch4d);
    rza1h_mtu2_after_fire(&s->ch4d);
}

static void rza1h_mtu2_reset(DeviceState *dev)
{
    RZA1HMtu2State *s = RZA1H_MTU2(dev);
    s->tcnt3_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    memset(s->regs, 0, sizeof(s->regs));
    rza1h_mtu2_stop(&s->ch3a);
    rza1h_mtu2_stop(&s->ch4a);
    rza1h_mtu2_stop(&s->ch4b);
    rza1h_mtu2_stop(&s->ch4c);
    rza1h_mtu2_stop(&s->ch4d);
    s->dsp_pace_deadline_ns = INT64_MAX;
    s->swtimer_b_deadline_ns = INT64_MAX;
    s->swtimer_c_deadline_ns = INT64_MAX;
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

    s->ch4b.tcr_off = MTU2_TCR_4;
    s->ch4b.tier_off = MTU2_TIER_4;
    s->ch4b.tgr_off = MTU2_TGRB_4;
    s->ch4b.tsr_off = MTU2_TSR_4;
    s->ch4b.bit = 1;
    s->ch4b.tstr_cst_bit = MTU2_TSTR_CST4;
    s->ch4b.arms_on_tstr = false; /* TGI4A-style -- see file comment's TGI4B paragraph */
    sysbus_init_irq(sbd, &s->ch4b.irq);

    s->ch4c.tcr_off = MTU2_TCR_4;
    s->ch4c.tier_off = MTU2_TIER_4;
    s->ch4c.tgr_off = MTU2_TGRC_4;
    s->ch4c.tsr_off = MTU2_TSR_4;
    s->ch4c.bit = 2;
    s->ch4c.tstr_cst_bit = MTU2_TSTR_CST4;
    s->ch4c.arms_on_tstr = true;
    sysbus_init_irq(sbd, &s->ch4c.irq);

    s->ch4d.tcr_off = MTU2_TCR_4;
    s->ch4d.tier_off = MTU2_TIER_4;
    s->ch4d.tgr_off = MTU2_TGRD_4;
    s->ch4d.tsr_off = MTU2_TSR_4;
    s->ch4d.bit = 3;
    s->ch4d.tstr_cst_bit = MTU2_TSTR_CST4;
    s->ch4d.arms_on_tstr = false; /* TGI4A/TGI4B-style -- see file comment's TGI4D paragraph */
    sysbus_init_irq(sbd, &s->ch4d.irq);
}

static void rza1h_mtu2_realize(DeviceState *dev, Error **errp)
{
    RZA1HMtu2State *s = RZA1H_MTU2(dev);
    RZA1HMtu2Event *events[] = { &s->ch3a, &s->ch4a, &s->ch4b, &s->ch4c, &s->ch4d };
    ptimer_cb callbacks[] = { rza1h_mtu2_ch3a_tick, rza1h_mtu2_ch4a_tick,
                             rza1h_mtu2_ch4b_tick, rza1h_mtu2_ch4c_tick,
                             rza1h_mtu2_ch4d_tick };
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
