/*
 * RZ/A1H SCIF (Serial Communication Interface with FIFO) -- Extension-
 * roadmap item 4. No emu/peripherals/ Python original exists for this one
 * (genuinely new work, not a port) -- register offsets/widths/bit layout
 * confirmed via ~/Downloads/rza1.svd (the same SVD every other peripheral
 * in this directory cites), a real Renesas FIFO-capable SCIF, distinct
 * from the byte-addressed non-FIFO "SCI" QEMU already ships a model for
 * (hw/char/renesas_sci.c, for the RX62N family) -- that device's register
 * map doesn't match this SoC's SCIF at all, though its CharBackend wiring
 * idiom (qemu_chr_fe_write_all() on transmit, qemu_chr_fe_set_handlers()
 * for receive) is the pattern this file follows.
 *
 * Eight real instances exist (SCIF0 at 0xE8007000 through SCIF7 at
 * 0xE800A800, each +0x800 apart) -- notes/ic7300-signal-chain.md has
 * confirmed real roles for several: SCIF0 is the CI-V UART, SCIF1 the
 * service/calibration link, SCIF3 the front-panel link, SCIF5 the DSP
 * link. All eight are wired identically here (see rz_a1h.c) since nothing
 * in this device's own logic depends on which channel it is beyond a
 * `channel` property used only to label log output.
 *
 * Deliberately minimal, matching this roadmap item's own scope ("SCIF UART
 * output", not "a fully interrupt-driven SCIF driver"): no baud-rate-
 * accurate transmit pacing (bytes go out immediately, unlike
 * renesas_sci.c's own timer-paced send_byte()). FSR's TDFE/TEND (transmit-
 * FIFO-empty/transmit-end) bits are always reported set so any
 * polling-based transmit driver never blocks; FDR always reports both
 * FIFOs empty for the same reason.
 *
 * 2026-09-09 (third pass): a real TXI (transmit-complete) IRQ output was
 * added, found needing this by live testing -- body.bin's SCIF3 driver
 * (scif3_driver_pump_tick) sets a software "busy" flag before a send and
 * only an interrupt handler ever clears it (confirmed: nothing else in the
 * whole image touches it), so with no line ever raised, every send after
 * the first permanently wedged the driver. See rz_a1h.h's
 * RZA1H_SCIF_TXI_BASE0 comment for the GIC-ID derivation (independently
 * cross-checked against a real 0xec literal in the firmware's own
 * IRQ-enable call) and this file's own REG_SCR/REG_FTDR/REG_FSR write
 * cases for the three real bugs found getting a *reliable* signal out of
 * it (level- vs. edge-triggered, a redundant-raise-is-a-no-op gotcha, and
 * an emulator-only lost-edge artifact) -- verified end-to-end via a real
 * register-write trace, see qemu-machine/README-history.md's 2026-09-09
 * section.
 *
 * 2026-09-09 (fourth pass, same day): a minimal virtual front-panel
 * responder, gated to channel 3 only (the confirmed real front-panel
 * link, see the roles list above). With TXI now real,
 * scif3_frontpanel_identify_handshake's outbound 0xF0 "identify" frame
 * (0xFE, 0xF0, 0xFD) genuinely transmits, but the handshake's own
 * *reply-wait* still blocks forever -- confirmed via a live hardware
 * watchpoint that its own 75-count timeout fallback counter never gets a
 * single write in 120 real seconds either, so waiting it out isn't a real
 * option; only a genuine reply unblocks it. `scif3_frame_dispatch_by_type`
 * (0x20036bb8, body.bin) needs nothing more than a first-content-byte of
 * 0xF0 or 0xF1 to clear the handshake's own busy flag.
 *
 * Getting a reply recognized took two real designs. The first tried to
 * model the exchange faithfully: recognize the complete outbound frame on
 * TX, then feed a canned 3-byte 0xFE/0xF0/0xFD reply back byte-by-byte
 * through the normal frdr/rx_pending/RXI path, exactly like a real
 * chardev byte would arrive. `scif3_frame_rx_statemachine` (0x20036c68,
 * confirmed via a created Ghidra function + full decompile, not just raw
 * disassembly) polls real FDR/FRDR/FSR/LSR/SCR registers directly (not a
 * software ring buffer as the raw listing alone first suggested) and
 * keeps only the *last* byte read across its own internal drain loop
 * (`while (FDR & 0x1f) { byte = FRDR; }`) -- real hardware never has more
 * than one byte in that FIFO at a time (serial bytes arrive with real,
 * far-longer-than-one-loop-iteration timing gaps), so this is harmless
 * there, but it makes correctly modeling "exactly one byte visible per
 * real RXI" essential here, and repeatedly not reliable: a synchronous
 * next-byte delivery let the drain loop consume all 3 in one pass (only
 * the trailing 0xFD's value was ever examined, with no 0xFE ever seen to
 * mark a frame in progress); switching to a QEMUBH did not fix it either
 * (confirmed via a live register-write trace: the next byte still became
 * visible before the guest's own next FDR poll -- a scheduled BH can
 * still run interleaved within the same guest loop that scheduled it, at
 * least in this single-threaded TCG configuration); a real QEMUTimer
 * (even a 1ns one) *looked* like it fixed the interleaving in the trace,
 * but the very first live single-step trace of an actual exchange still
 * showed all 3 bytes already consumed by the time the first RXI was even
 * serviced -- this project's own TCG build processes pending timers far
 * more eagerly than "the guest's own next few instructions" guarantees,
 * and no delay short of a real, much-larger-than-instruction-count gap
 * reliably avoids it. Not worth chasing further: modeling a UART's real
 * byte-at-a-time timing has never been this project's goal.
 *
 * Second, and final, design: don't try to out-race the drain loop at
 * all. Precompute the *end state* scif3_frame_rx_statemachine would have
 * reached after genuinely processing 0xFE then 0xF0 one at a time --
 * frame buffer byte 0 = 0xF0 (the type byte, confirmed from the decompile
 * to land at `*DAT_20037584`), byte-count field = 2, and the driver's own
 * status "frame in progress" bit cleared -- and write that directly into
 * guest RAM via address_space_write() (using the real firmware pointer
 * *values*, read live via address_space_read(), never hardcoded -- these
 * live in a per-boot dynamically-placed struct at a fixed literal-pool
 * *address*, not a fixed *value*, the same pointer-indirection shape this
 * whole investigation kept tripping over). With that state already in
 * place, delivering *only* the single terminator byte (0xFD) through the
 * completely normal FRDR/RXI path is enough: scif3_frame_rx_statemachine
 * sees byte-count >= 2 and the frame-in-progress bit already clear,
 * exactly as if 0xFE and 0xF0 had each been its own real interrupt, and
 * calls scif3_frame_dispatch_by_type for real. One byte, one RXI, no
 * timing race of any kind -- and the real firmware code still parses its
 * own reply through its own real state machine, just with two of its
 * three inputs delivered by direct memory write instead of by (thoroughly
 * unreliable, in this environment) emulated serial timing. Matches this
 * project's established permissive-peripheral philosophy (mmc.c's virtual
 * SD card, riic.c's virtual EEPROM) -- a plausible canned ACK is enough,
 * no real front-panel-MCU protocol fidelity (an actual version string,
 * real key states, real per-byte timing) is needed just to unblock boot.
 * RXI's GIC ID (235, see RZA1H_SCIF_RXI_BASE0) was already enabled by
 * firmware alongside TXI3, confirmed live -- no extra firmware-side
 * arming needed.
 *
 * 2026-09-09 (fifth pass, same day): with the identify handshake's ACK
 * working, live testing found a real *second* layer needing the same
 * treatment -- the handshake's own second wait loop (right after the
 * first, in scif3_frontpanel_identify_handshake) blocks on a genuinely
 * different signal (status bit 0x04, set only by an *ordinary*
 * type-0x00-0x1F frame's own successful dispatch, confirmed by reading
 * that loop's real disassembly directly), and boot reaches it as soon as
 * the identify ACK lets the first loop through -- confirmed live via a
 * real outbound 33-byte status/data frame (type 0x00) that had never
 * been seen transmitted before this session. `rza1h_scif3_frontpanel_ack`
 * below handles both shapes now. One real bug found extending it, same
 * family as the ping-pong loop above: the identify ACK's own status bits
 * (0x60) include bit 0x40, which `scif3_driver_pump_tick` reads as "send
 * a keepalive ping" -- ACKing *that* resulting 0xF1 frame the same way
 * re-armed the same bit right back, producing a genuine, unbounded
 * fe/f1/fd loop (confirmed: tens of thousands of frames in the first
 * dozen real seconds). Fixed by simply never ACKing type 0xF1 -- real
 * hardware's own ping is presumably paced by something (a real timer, or
 * a front panel that doesn't reply instantly) this project hasn't needed
 * to find yet; not ACKing it sidesteps needing to model that pacing at
 * all, since pump_tick clears the ping-request bit itself before sending
 * and nothing else sets it again.
 *
 * 2026-09-09 (sixth pass, continuing the DSP-comms thread): a second
 * virtual responder, gated to channel 5 (the confirmed DSP link, see the
 * roles list above). scif5_send_and_wait_reply's busy-wait -- the DSP's
 * real synchronous command/reply API, 14 call sites project-wide -- was
 * confirmed live to be genuinely, permanently stuck (a 90-second free-run
 * poll never once saw its reply-ready flag clear). Live-traced the real
 * clearer directly rather than guessing from the decompile alone (which
 * silently dropped the actual clearing instruction -- a real dead-store-
 * elision artifact, caught by comparing the raw disassembly listing
 * against the decompiled pseudocode line by line): scif5_rx_isr, SCIF5's
 * real RX-side ISR, clears it only after collecting 4 real bytes and
 * `rbit`-reversing them into a reply word -- with no virtual DSP ever
 * transmitting a reply, this can never happen.
 *
 * First design tried was the obvious one: recognize a complete outbound
 * 4-byte word on the TX side (REG_FTDR, no framing to track since every
 * SCIF5 command is a bare word) and synthesize the reply right there.
 * Worked in one live trial, then got stuck again in the very next one --
 * a real ordering race, not a fluke: the reply-ready flag isn't actually
 * set busy (=1) until *after* the caller's TX returns, inside
 * scif5_arm_retry_timer (called by scif5_send_and_wait_reply, never by
 * the TX helper itself). Acking synchronously during TX can land *before*
 * that -1 write, so arm_retry_timer's own unconditional "=1" silently
 * overwrites an already-delivered ack, and nothing else ever clears it
 * again for that exchange. This project has hit this exact class of
 * problem before (see the channel-3 responder's own comment) -- a
 * QEMUTimer/QEMUBH delay doesn't reliably fix it either (documented there
 * as running "far more eagerly than the guest's own next few instructions
 * guarantees" in this single-threaded TCG build), so timing the fix by
 * delay isn't an option here either.
 *
 * Real fix: don't trigger off TX at all. scif5_arm_retry_timer's own
 * final step, right after setting the busy flag, is a real 5-write arm
 * sequence to a genuine, previously-unmodeled hardware register at
 * 0xFCFE3120 (reached two independent ways in the firmware -- from a
 * literal `DAT_200b1c98+0x120` base and, in the TX helper's own *separate*
 * arm sequence, from `DAT_200b1c8c-0x2e0` -- same physical address, a real
 * confirmation this is one genuine register, not two). Its last write is
 * always one of two large sentinel constants (0x10000000/0x40000000,
 * selected by scif5_arm_retry_timer's own param) -- cleanly distinct from
 * every other write that lands here (the small <=0x10000 values every
 * write in the sequence but the last uses, and the TX helper's own
 * differently-sentineled 0xa0000000 completion). Since this write is the
 * literal next instruction after the busy-flag set, in the same function,
 * hooking it is race-free by construction -- no scheduling assumption
 * needed, just real instruction order, confirmed from the raw listing.
 * `rza1h_scif5_dsp_retry_arm_write` below (a second, small MMIO region on
 * the channel-5 SCIF instance only -- see rza1h_scif_init) is that hook;
 * rza1h_scif5_dsp_ack itself is unchanged from the first design (still the
 * same precompute-3-then-deliver-1-real-byte technique the channel-3
 * responder established), only *when* it's called changed. **Confirmed
 * load-bearing across 2 independent 90-second free-run trials**: the
 * reply-ready flag never once stuck (every prior trial, TX-triggered
 * design included, stuck permanently within 35 seconds) -- boot now
 * progresses to scif5_cmd_transmit_now's own busy-wait on the shared
 * ring-active flag (DAT_200b1cac) instead, a further, genuinely different
 * stage. See README.md's Status section for the current resume point. */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HScifState, RZA1H_SCIF)

struct RZA1HScifState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion iomem_dsp_retry; /* channel 5 only -- see this file's own
                                    * comment and rza1h_scif_init */
    CharFrontend chr;
    qemu_irq irq_tx; /* TXI (transmit-complete) */
    qemu_irq irq_rx; /* RXI (receive-data-full) -- channel 3 (front-panel
                       * responder) only, see this file's own comment */
    uint32_t channel; /* 0-7, log-label only -- see this file's own comment */

    uint16_t smr;
    uint8_t  brr;
    uint16_t scr;
    uint16_t fcr;
    uint16_t sptr;
    uint16_t lsr;
    uint16_t emr;

    uint8_t  frdr;
    bool     rx_pending;

    /* Virtual front-panel responder state (channel 3 only) -- see this
     * file's own comment for the full derivation. Tracks the outbound
     * frame currently being assembled on the TX side just enough to
     * recognize a complete 0xFE/<type>/.../0xFD frame and capture its
     * type byte; frame *contents* beyond the type byte are not parsed
     * (not needed for the one canned reply this model sends). */
    int      tx_frame_pos;   /* -1 = idle (waiting for 0xFE); 0 = next byte
                               * is the type byte; >0 = mid-frame */
    uint8_t  tx_frame_type;
};

#define REG_SMR  0x00
#define REG_BRR  0x04
#define REG_SCR  0x08
#define REG_FTDR 0x0C
#define REG_FSR  0x10
#define REG_FRDR 0x14
#define REG_FCR  0x18
#define REG_FDR  0x1C
#define REG_SPTR 0x20
#define REG_LSR  0x24
#define REG_EMR  0x28

/* FSR bit positions, confirmed via the SVD (see this file's own comment). */
#define FSR_DR   (1 << 0)
#define FSR_RDF  (1 << 1)
#define FSR_TDFE (1 << 5)
#define FSR_TEND (1 << 6)

/* SCR bit positions (same SVD). */
#define SCR_RE  (1 << 4)
#define SCR_TIE (1 << 7)

/* Fixed literal-pool addresses (not the buffers themselves -- each holds
 * a per-boot dynamically-placed *pointer value*, read live) that
 * body.bin's own SCIF3 driver uses. Confirmed live via GDB against a
 * v1.42 boot; see this file's own comment and README-history.md's
 * 2026-09-09 section for the full derivation. Only used by the virtual
 * front-panel responder below (channel 3). */
#define SCIF3_FRAME_BUF_PTR_ADDR 0x20037584 /* -> RX frame assembly buffer;
                                              * byte 0 is the dispatched
                                              * frame's "type" */
#define SCIF3_DESC_PTR_ADDR      0x2003758c /* -> shared TX/RX descriptor;
                                              * byte 0 is the RX byte count
                                              * scif3_frame_rx_statemachine
                                              * itself maintains */
#define SCIF3_STATUS_PTR_ADDR    0x20037588 /* -> shared driver status
                                              * flags; bit 0 is the RX
                                              * "frame in progress" flag */

/* SCIF5's own shared TX/RX descriptor pointer (channel 5's virtual DSP
 * responder below). Unlike SCIF3_FRAME_BUF_PTR_ADDR and friends, this one
 * is a genuine fixed compile-time literal, not a per-boot dynamically-
 * placed value -- confirmed both statically (body.bin's own image already
 * holds 0x203906b8 here) and live (an identical GDB read against a running
 * boot) -- but it's still read live rather than hardcoded, matching this
 * file's own established discipline for every other struct-pointer-shaped
 * constant. See this file's own comment (2026-09-09, sixth pass) for the
 * struct layout this responder depends on: +0x3 the reply-ready busy flag,
 * +0xc the RX byte count scif5_rx_isr's own collection loop maintains,
 * +0x13 the 4-byte raw RX buffer, +0x18 the decoded (post-rbit) reply
 * word scif5_classify_reply reads. */
#define SCIF5_STRUCT_PTR_ADDR    0x200b1c84

static uint64_t rza1h_scif_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    switch (offset) {
    case REG_SMR:
        return s->smr;
    case REG_BRR:
        return s->brr;
    case REG_SCR:
        return s->scr;
    case REG_FTDR:
        return 0; /* write-only on real hardware */
    case REG_FSR:
        /* TDFE/TEND always set -- see module comment; DR/RDF reflect
         * whether a real received byte (from the chardev backend, or the
         * front-panel responder below) is waiting in frdr. */
        return FSR_TDFE | FSR_TEND | (s->rx_pending ? (FSR_DR | FSR_RDF) : 0);
    case REG_FRDR: {
        uint8_t val = s->frdr;
        s->rx_pending = false; /* reading consumes the byte, real hardware too */
        /* RXI is level-triggered exactly like TXI (real hardware: RIE &
         * RDF) -- lower it now that there's genuinely nothing left
         * pending. Found needing this the hard way while building the
         * front-panel responder below: without an explicit lower here,
         * the line stayed permanently high after its one real byte was
         * consumed (the lower+raise pair elsewhere only ever *re-asserts*
         * it), causing a real interrupt storm -- this same ISR
         * re-entering continuously with nothing left to deliver. REG_FSR
         * writes are still deliberately a no-op (see that case's own
         * comment) -- lowering here, tied to the real "no more data"
         * condition, is the correct place, not there. */
        qemu_irq_lower(s->irq_rx);
        return val;
    }
    case REG_FCR:
        return s->fcr;
    case REG_FDR:
        /* Both FIFOs always empty except the RX count this model tracks
         * for the front-panel responder (see REG_FRDR) -- real hardware's
         * low 5 bits are the RX count, matching FUN_20360b34's `& 0x1f`
         * mask on this exact register in scif3_frame_rx_statemachine. */
        return s->rx_pending ? 1 : 0;
    case REG_SPTR:
        return s->sptr;
    case REG_LSR:
        return s->lsr;
    case REG_EMR:
        return s->emr;
    default:
        return 0;
    }
}

/* Virtual front-panel responder (channel 3 only) -- see this file's own
 * comment for the full derivation of why this precomputes state directly
 * in guest RAM instead of feeding a reply through frdr one byte at a
 * time. Called once a complete outbound frame is seen on the TX side.
 *
 * Two shapes, both ending with the caller delivering one real terminator
 * byte (0xFD) through the normal FRDR/RXI path -- see the REG_FTDR case
 * below:
 *   - type 0xF0 (the boot-time identify request): a bare ACK, no payload
 *     -- matches the real 0xFE/0xF0/0xFD frame this model's own TX side
 *     sends for the same request, and is all `scif3_frame_dispatch_by_type`
 *     needs to clear the handshake's busy flag.
 *   - type 0x00-0x1F (an ordinary status/data frame -- the handshake's
 *     own *second* wait loop, reached once the identify ACK above lets it
 *     continue, blocks on one of these next, confirmed live): a 1-byte
 *     dummy-payload echo of the same type. Real front-panel-MCU protocol
 *     fidelity (an actually meaningful reply payload) isn't needed, only
 *     enough real structure for scif3_frame_dispatch_by_type's own length
 *     check to accept it and set its "real frame received" status bit. */
static void rza1h_scif3_frontpanel_ack(uint8_t type)
{
    AddressSpace *as = &address_space_memory;
    uint32_t frame_buf, desc, status_addr;
    uint8_t byte, status;
    bool identify = (type == 0xF0);

    address_space_read(as, SCIF3_FRAME_BUF_PTR_ADDR, MEMTXATTRS_UNSPECIFIED,
                       &frame_buf, 4);
    address_space_read(as, SCIF3_DESC_PTR_ADDR, MEMTXATTRS_UNSPECIFIED,
                       &desc, 4);
    address_space_read(as, SCIF3_STATUS_PTR_ADDR, MEMTXATTRS_UNSPECIFIED,
                       &status_addr, 4);

    byte = type; /* frame buffer byte 0 = the dispatched frame's type */
    address_space_write(as, frame_buf, MEMTXATTRS_UNSPECIFIED, &byte, 1);
    if (!identify) {
        byte = 0; /* one dummy payload byte, at frame buffer byte 1 */
        address_space_write(as, frame_buf + 1, MEMTXATTRS_UNSPECIFIED,
                            &byte, 1);
    }

    /* desc byte count, as if 0xFE then <type> (then, for the generic
     * case, one payload byte) were each already processed by a real
     * scif3_frame_rx_statemachine call. */
    byte = identify ? 2 : 3;
    address_space_write(as, desc, MEMTXATTRS_UNSPECIFIED, &byte, 1);

    address_space_read(as, status_addr, MEMTXATTRS_UNSPECIFIED, &status, 1);
    status &= 0xFE; /* clear bit 0 -- "frame in progress", set by a real
                      * 0xFE byte's own processing */
    address_space_write(as, status_addr, MEMTXATTRS_UNSPECIFIED, &status, 1);
}

/* Virtual DSP-link responder (channel 5 only) -- see this file's own
 * comment (2026-09-09, sixth pass) for the full live-traced derivation of
 * why this is needed and why it's triggered from
 * rza1h_scif5_dsp_retry_arm_write below rather than from TX: scif5_send_
 * and_wait_reply's busy-wait (*(SCIF5_STRUCT_PTR_ADDR value)+3) is cleared
 * only by scif5_rx_isr, once it has collected 4 real bytes over SCIF5's RX
 * path and bit-reverses them (a real ARM `rbit` on the assembled 32-bit
 * word) into the reply-word field at +0x18 -- with no virtual DSP ever
 * replying, every one of the 14 call sites (and the background command-ack
 * ring shared_job_ring_dispatch's case 1 drains) wedges forever.
 * shared_job_ring_dispatch's own case 1, dsp_cmd_table_init/dsp_param_
 * sync_tick's ring traffic, and scif5_arm_retry_timer/scif5_classify_reply
 * are otherwise untouched -- this only ever supplies the one missing
 * input (a real SCIF5 RX event) they all depend on.
 *
 * Same "precompute the end state, deliver only the genuinely necessary
 * last byte through the real path" technique rza1h_scif3_frontpanel_ack
 * uses, applied here for a different reason: our own FDR/FRDR model can
 * only ever present one byte at a time anyway (no real multi-byte
 * drain-loop race is possible the way SCIF3 hit), but reusing the same
 * shape keeps both responders consistent and needs no new machinery.
 * Precomputes the first 3 of the 4 raw bytes scif5_rx_isr's own collection
 * loop would gather directly in guest RAM, then delivers the 4th through
 * the normal FRDR/RXI path -- scif5_rx_isr's own real code then does the
 * rest (clears the reply-ready flag, computes the real `rbit` reply word,
 * tail-calls shared_job_ring_dispatch(1), which is what actually advances
 * the shared ring's read pointer -- letting real guest code run this
 * instead of hand-replicating its effect is deliberate, matching this
 * function's own "run the real code" philosophy elsewhere in this file).
 *
 * Sends a universal class-2 ("trivial ack, no payload processing" --
 * scif5_classify_reply's own top-nibble dispatch) reply regardless of
 * which command was sent: matches this project's established permissive-
 * peripheral philosophy (mmc.c's virtual SD card, riic.c's virtual
 * EEPROM, rza1h_scif3_frontpanel_ack above) -- a plausible canned ACK is
 * enough to unblock boot, not real DSP protocol fidelity. Some callers
 * (the `0xe0000000`/`0xe0000001` command class, per notes/multi-cpu-
 * images-history.md's "SCIF5 command API" section) separately compare the
 * reply's top nibble against 9 or 0xE and will treat a class-2 reply as a
 * soft failure of their own -- that's a real, known simplification, not a
 * bug: it only affects those specific commands' own retry/error handling,
 * never this busy-wait itself (any resolved reply clears it).
 *
 * The 3 precomputed bytes (0x04, 0x00, 0x00) plus the 4th delivered byte
 * (0x00) assemble to raw_word_LE = 0x00000004; scif5_rx_isr's own `rbit`
 * turns that into 0x20000000 -- top byte 0x20, high nibble 2. Worked out
 * by hand (rbit is self-inverse: rbit32(0x20000000) = 0x00000004) rather
 * than guessed. */
static void rza1h_scif5_dsp_ack(RZA1HScifState *s)
{
    AddressSpace *as = &address_space_memory;
    uint32_t struct_base;
    uint8_t raw012[3] = { 0x04, 0x00, 0x00 };
    uint8_t count = 3;

    address_space_read(as, SCIF5_STRUCT_PTR_ADDR, MEMTXATTRS_UNSPECIFIED,
                       &struct_base, 4);
    if (struct_base == 0) {
        return; /* not yet initialized this boot */
    }

    address_space_write(as, struct_base + 0x13, MEMTXATTRS_UNSPECIFIED,
                        raw012, 3);
    address_space_write(as, struct_base + 0xc, MEMTXATTRS_UNSPECIFIED,
                        &count, 1);

    rza1h_debug("scif", "scif5: DSP-link responder: canned class-2 ack");
    s->frdr = 0x00; /* the one byte actually delivered through the normal
                      * RXI path -- see this function's own comment */
    s->rx_pending = true;
    qemu_irq_lower(s->irq_rx);
    qemu_irq_raise(s->irq_rx);
}

/* MMIO handlers for the second, small region rza1h_scif_init maps at
 * 0xFCFE3120 on the channel-5 SCIF instance only -- see this file's own
 * comment (2026-09-09, sixth pass) for why this specific address and why
 * only these two sentinel values matter. Every other write that lands
 * here (the small <=0x10000 values every arm sequence but its last write
 * uses, and the TX helper's own differently-sentineled 0xa0000000
 * completion) is deliberately ignored -- this models only the one bit of
 * real register behavior this project currently needs from it. */
static uint64_t rza1h_scif5_dsp_retry_arm_read(void *opaque, hwaddr offset,
                                               unsigned size)
{
    return 0;
}

static void rza1h_scif5_dsp_retry_arm_write(void *opaque, hwaddr offset,
                                            uint64_t value, unsigned size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    if (value == 0x10000000 || value == 0x40000000) {
        rza1h_scif5_dsp_ack(s);
    }
}

static const MemoryRegionOps rza1h_scif5_dsp_retry_arm_ops = {
    .read = rza1h_scif5_dsp_retry_arm_read,
    .write = rza1h_scif5_dsp_retry_arm_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_scif_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);
    uint8_t byte;

    switch (offset) {
    case REG_SMR:
        s->smr = value;
        break;
    case REG_BRR:
        s->brr = value;
        break;
    case REG_SCR:
        /* TXI is level-triggered on real hardware (TIE & TDFE), reconfirmed
         * live for this project's own TXI3 (GICD_ICFGR14 bits 8-9 = 0,
         * ISENABLER7 bit12 set) -- the exact same class of gotcha riic.c's
         * own file comment already documents in detail (a pulse gets
         * silently dropped by arm_gic for a level-sensitive SPI). Since
         * TDFE is unconditionally true in this model, enabling TIE alone
         * makes the condition true right away.
         *
         * A second, less obvious bug from the same family found getting
         * this far: a bare qemu_irq_raise() while the line is *already*
         * high is a silent no-op (qemu_set_irq only forwards an actual
         * value change) -- fine for a genuinely one-shot send, but a real
         * multi-byte frame's own ISR (FUN_20036e34) re-primes TIE-gated
         * TXI on every subsequent byte without ever clearing TIE in
         * between, so only the frame's first byte was ever delivered.
         * riic.c's own DRT-write cases already establish the fix for this
         * exact shape (explicit lower-then-raise to force a real
         * transition even when the condition was "already true") --
         * applied the same way here on every TIE-enabling write, not just
         * the rising edge. */
        if (value & SCR_TIE) {
            qemu_irq_lower(s->irq_tx);
            qemu_irq_raise(s->irq_tx);
        } else {
            qemu_irq_lower(s->irq_tx);
        }
        s->scr = value;
        break;
    case REG_FTDR:
        byte = value;
        /* Always surfaced, chardev or not -- the whole point of this
         * roadmap item is *observability*, and most testing won't bother
         * wiring a real -chardev per channel. */
        rza1h_debug("scif", "scif%u: TX %02x ('%c')",
                   s->channel, byte, (byte >= 0x20 && byte < 0x7f) ? byte : '.');
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, &byte, 1);
        }
        /* Covers the other write ordering (TIE already enabled before this
         * byte, as every byte after a frame's first will be) -- explicit
         * lower+raise for the same reason as the REG_SCR case above. */
        if (s->scr & SCR_TIE) {
            qemu_irq_lower(s->irq_tx);
            qemu_irq_raise(s->irq_tx);
        }
        /* Virtual front-panel responder (channel 3 only) -- see this
         * file's own comment. Tracks byte position within the outbound
         * frame just enough to capture the type byte and recognize the
         * terminator; doesn't otherwise parse frame contents (real 0xFF
         * escape sequences in longer, unrelated frame types could confuse
         * this simple tracking, but the only frame this model ever acts
         * on -- the bare 3-byte 0xF0 identify request -- never contains
         * one, so this is a deliberately narrow, sufficient
         * simplification, not a general frame parser).
         *
         * Only 0xF0 (not 0xF1) is ever ACKed -- found needing this
         * restriction the hard way, live: `scif3_frame_dispatch_by_type`'s
         * own 0xF0 ACK sets status bits 0x60, and bit 0x40 of that same
         * byte is exactly what `scif3_driver_pump_tick` reads as "send a
         * 0xF1 keepalive ping" -- with no real serial timing to pace it,
         * ACKing that resulting 0xF1 the same way re-set bit 0x40 right
         * back, producing a genuine, self-sustaining fe/f1/fd ping-pong
         * that never stopped on its own (confirmed: 65k+ frames in the
         * first 12 real seconds alone). On real hardware this ping is
         * presumably paced by something real (an actual timer, or a
         * front panel that doesn't reply instantly) this project hasn't
         * needed to find yet -- simply never ACKing 0xF1 sidesteps the
         * whole question without needing to model that pacing: pump_tick
         * clears bit 0x40 itself before sending, so with nothing setting
         * it again, the ping fires exactly once and the driver settles. */
        if (s->channel == 3) {
            if (byte == 0xFE) {
                s->tx_frame_pos = 0;
            } else if (s->tx_frame_pos == 0) {
                s->tx_frame_type = byte;
                s->tx_frame_pos = 1;
            } else if (s->tx_frame_pos >= 1) {
                if (byte == 0xFD) {
                    if (s->tx_frame_type == 0xF0 || s->tx_frame_type < 0x20) {
                        rza1h_debug("scif",
                            "scif3: front-panel responder: canned "
                            "ACK for outbound type %#x", s->tx_frame_type);
                        rza1h_scif3_frontpanel_ack(s->tx_frame_type);
                        s->frdr = 0xFD; /* the one byte actually delivered
                                          * through the normal RXI path --
                                          * see rza1h_scif3_frontpanel_ack's
                                          * own comment for why the other
                                          * two aren't */
                        s->rx_pending = true;
                        qemu_irq_lower(s->irq_rx);
                        qemu_irq_raise(s->irq_rx);
                    }
                    s->tx_frame_pos = -1;
                }
            }
        }
        break;
    case REG_FSR:
        /* Deliberately NOT wired to lower the TXI line -- found needing
         * this the hard way: body.bin's own per-byte TXI ISR (FUN_20036e34)
         * writes FSR (clearing TDFE/TEND, real-hardware bookkeeping, not a
         * deliberate interrupt acknowledgement) immediately after every
         * FTDR write, in the same host call burst as the REG_FTDR case's
         * own lower+raise above. A version of this case that also called
         * qemu_irq_lower() here immediately undid that raise before the
         * CPU ever got a chance to sample the line -- the first byte of a
         * frame went out, but no second interrupt ever followed, an
         * emulator-only artifact (nothing here happens fast enough on real
         * hardware to lose a genuine edge this way). Nothing in this
         * model needs FSR writes to have any effect at all. */
        break;
    case REG_FCR:
        s->fcr = value;
        break;
    case REG_FDR:
        break; /* read-only on real hardware */
    case REG_SPTR:
        s->sptr = value;
        break;
    case REG_LSR:
        s->lsr = value;
        break;
    case REG_EMR:
        s->emr = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rza1h_scif_ops = {
    .read = rza1h_scif_read,
    .write = rza1h_scif_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int rza1h_scif_can_receive(void *opaque)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    return (!s->rx_pending) && (s->scr & SCR_RE) ? 1 : 0;
}

static void rza1h_scif_receive(void *opaque, const uint8_t *buf, int size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    /* No overrun modeling (LSR.ORER) -- can_receive() already refuses a
     * new byte until the previous one is read, so QEMU's chardev core
     * won't call this while one is still pending. */
    rza1h_debug("scif", "scif%u: RX %02x (real -chardev backend)", s->channel, buf[0]);
    s->frdr = buf[0];
    s->rx_pending = true;
}

static void rza1h_scif_reset(DeviceState *dev)
{
    RZA1HScifState *s = RZA1H_SCIF(dev);

    s->smr = 0;
    s->brr = 0xff; /* real reset value -- slowest possible rate until set */
    s->scr = 0;
    s->fcr = 0;
    s->sptr = 0;
    s->lsr = 0;
    s->emr = 0;
    s->frdr = 0;
    s->rx_pending = false;
    s->tx_frame_pos = -1;
    s->tx_frame_type = 0;
}

static void rza1h_scif_realize(DeviceState *dev, Error **errp)
{
    RZA1HScifState *s = RZA1H_SCIF(dev);

    qemu_chr_fe_set_handlers(&s->chr, rza1h_scif_can_receive,
                             rza1h_scif_receive, NULL, NULL, s, NULL, true);
}

static void rza1h_scif_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HScifState *s = RZA1H_SCIF(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_scif_ops, s,
                          TYPE_RZA1H_SCIF, 0x2C);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_tx);
    sysbus_init_irq(sbd, &s->irq_rx);

    /* Second MMIO region, mapped only for the channel-5 instance (see
     * rz_a1h.c) -- the "channel" property isn't known yet this early
     * (qdev_prop_set_uint32 runs after qdev_new, before realize), so this
     * is created unconditionally for all 8 instances like the primary
     * region above; leaving it unmapped for channels other than 5 is a
     * normal, inert QEMU idiom (see rza1h_scif5_dsp_retry_arm_ops's own
     * comment for what it's for). */
    memory_region_init_io(&s->iomem_dsp_retry, obj,
                          &rza1h_scif5_dsp_retry_arm_ops, s,
                          TYPE_RZA1H_SCIF ".dsp-retry-arm", 4);
    sysbus_init_mmio(sbd, &s->iomem_dsp_retry);
}

static const Property rza1h_scif_properties[] = {
    DEFINE_PROP_CHR("chardev", RZA1HScifState, chr),
    DEFINE_PROP_UINT32("channel", RZA1HScifState, channel, 0),
};

static void rza1h_scif_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rza1h_scif_realize;
    device_class_set_legacy_reset(dc, rza1h_scif_reset);
    device_class_set_props(dc, rza1h_scif_properties);
}

static const TypeInfo rza1h_scif_info = {
    .name          = TYPE_RZA1H_SCIF,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HScifState),
    .instance_init = rza1h_scif_init,
    .class_init    = rza1h_scif_class_init,
};

static void rza1h_scif_register_types(void)
{
    type_register_static(&rza1h_scif_info);
}

type_init(rza1h_scif_register_types)
