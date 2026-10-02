/*
 * RZ/A1H SCIF (Serial Communication Interface with FIFO) -- Extension-
 * roadmap item 4. No emu/peripherals/ Python original exists for this one
 * (genuinely new work, not a port) -- register offsets/widths/bit layout
 * confirmed via rza1.svd (the same SVD every other peripheral
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
#include "hw/core/ptimer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"
#include "fake_dsp.h"

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

    /* Real baud-rate-accurate TX pacing (2026-09-10) -- see this file's own
     * module comment ("no baud-rate-accurate transmit pacing") and
     * scif_byte_time_ns()'s own comment for the formula/assumptions.
     * tx_busy mirrors real hardware's TDFE/TEND=0 window; tx_timer fires
     * once the real byte time elapses and puts it back to the ready state,
     * re-raising TXI if TIE is still enabled (level-triggered, matching
     * this file's own already-established TXI semantics). */
    ptimer_state *tx_timer;
    bool     tx_busy;

    /* Real pacing for the virtual DSP-link responder's own ack (channel 5
     * only) -- see rza1h_scif5_dsp_retry_arm_write's own comment. */
    ptimer_state *dsp_ack_timer;

    /* Virtual front-panel responder state (channel 3 only) -- see this
     * file's own comment for the full derivation. Tracks the outbound
     * frame currently being assembled on the TX side just enough to
     * recognize a complete 0xFE/<type>/.../0xFD frame and capture its
     * type byte; frame *contents* beyond the type byte are not parsed
     * (not needed for the one canned reply this model sends). */
    int      tx_frame_pos;   /* -1 = idle (waiting for 0xFE); 0 = next byte
                               * is the type byte; >0 = mid-frame */
    uint8_t  tx_frame_type;

    /* Real pacing for the virtual front-panel responder's own ack (channel
     * 3 only) -- 2026-09-10, for-fun/entertainment experiment per the
     * user's own request, same idiom as dsp_ack_timer above. See
     * FRONTPANEL_ACK_DELAY_NS's own comment. */
    ptimer_state *frontpanel_ack_timer;
    uint8_t  frontpanel_pending_ack_type; /* stashed for the timer callback,
                                            * since ptimer callbacks only
                                            * get `opaque`, not an argument */

    /* Virtual front panel (channel 3, 2026-09-24): the 32-byte report the RL78 front CPU
     * presents (g_scif3_rx_status_buffer layout, see rza1h_scif3_fp_reset), a queue of frames
     * still to deliver (snapshots, [offset][data...]), the fpctl control chardev, and the
     * mouse-as-touchscreen input handler. frontpanel_ack_timer paces the queue. */
    uint8_t  fp[32];
    bool     fp_booted;      /* the full report has been sent */
    uint8_t  fpq[16][34];
    uint8_t  fpq_len[16];
    int      fpq_head, fpq_count;
    bool     fpq_timer_armed;
    CharFrontend fpctl;
    char     fpctl_line[160];
    int      fpctl_pos;
    QemuInputHandlerState *fp_input;
    int      fp_mouse_x, fp_mouse_y;
    bool     fp_mouse_down, fp_mouse_dirty;
    bool     fp_shift, fp_ctrl;   /* modifier state, for the wheel */

    /* Bus logger (2026-09-20) -- generic per-channel TX frame assembly for
     * observability, deliberately independent of the channel-3/5 virtual-
     * responder state above (which drives real reply behavior and is
     * already fragile enough per this file's own comments -- this is
     * logging-only, touches nothing behavioral). Every channel here uses
     * the same 0xFE/.../0xFD framing convention (CI-V/SCIF0, service-mode/
     * SCIF1, front-panel/SCIF3, DSP-link/SCIF5 all confirmed sharing one
     * driver template -- notes/ic7300-signal-chain.md), so this applies
     * uniformly rather than being gated to specific channels. See
     * scif_bus_log_tx()'s own comment for how to enable it. */
    uint8_t  bus_tx_buf[40];
    int      bus_tx_len; /* -1 = idle (waiting for 0xFE) */

    /* DSP-link command-word tracking (channel 5 only) -- 2026-09-21, see
     * rza1h_scif5_dsp_ack's own updated comment for why this exists (the
     * canned reply needs to know which command it's answering, not just
     * fire the same class every time). */
    uint8_t  scif5_cmd_buf[4];
    int      scif5_cmd_pos;

    /* Behavioural IF-DSP model behind the link (channel 5 only, 2026-09-24): fed every
     * complete command word, asked for the DSP's word at each reply -- see fake_dsp.h. */
    FakeDsp  dsp;

    /* CI-V bus echo (channel 0 only, 2026-09-24): the last transmitted byte, fed back into RX
     * when its byte time ends -- see rza1h_scif_tx_complete. */
    int      echo_byte;   /* -1 = none */
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
#define SCR_RIE (1 << 6)
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

/* Bus logger (2026-09-20), per the user's own request for a per-bus-selectable serial
 * traffic log, starting with the front-panel bus (channel 3). Layered on top of
 * rza1h_debug.h's existing name-matching (see that header's own comment): `RZA1H_DEBUG=scif`
 * still means "every SCIF channel" as before, and `RZA1H_DEBUG=scif<N>` (e.g. `scif3` for the
 * front panel, `scif0` for CI-V) now additionally selects one channel on its own -- pass
 * either or both (comma-separated) to rza1h_debug.h's own existing syntax. Deliberately TX-
 * side only for now: the RX side's virtual responders (channel 3/5, above) precompute their
 * replies directly into guest RAM rather than feeding them through FRDR byte-by-byte (see
 * their own comments for why -- real, hard-won timing this logger has no business touching),
 * so a generic byte-level RX accumulator can't see full reply frames the same way; TX is the
 * genuinely uncomplicated direction and is what "what gets sent to the front panel" asks for
 * first anyway. */
static void scif_bus_log_tx(RZA1HScifState *s, uint8_t byte)
{
    char tag[8];

    snprintf(tag, sizeof(tag), "scif%u", s->channel);
    if (!rza1h_debug_enabled("scif") && !rza1h_debug_enabled(tag)) {
        return;
    }

    if (byte == 0xFE) {
        s->bus_tx_len = 0;
    } else if (s->bus_tx_len >= 0 && s->bus_tx_len < (int)sizeof(s->bus_tx_buf)) {
        s->bus_tx_buf[s->bus_tx_len++] = byte;
    }

    if (byte == 0xFD && s->bus_tx_len > 0) {
        /* bus_tx_buf already holds [type, payload..., 0xFD] -- the leading 0xFE isn't
         * stored (it only resets the accumulator above), so "fe " is prepended here but
         * the trailing 0xFD is already the buffer's own last byte, not appended again. */
        GString *hex = g_string_new("fe ");
        int i;

        for (i = 0; i < s->bus_tx_len; i++) {
            g_string_append_printf(hex, "%02x ", s->bus_tx_buf[i]);
        }
        rza1h_debug(tag, "scif%u: TX frame type=0x%02x [%s] (%d bytes)",
                   s->channel, s->bus_tx_buf[0], g_strchomp(hex->str), s->bus_tx_len + 1);
        g_string_free(hex, TRUE);
        s->bus_tx_len = -1;
    }
}

/* Real RZ/A1H SCIF asynchronous-mode bit-rate formula -- the standard Renesas SCI/SCIF BRG
 * design shared across this whole chip family (H8/SH/RX/RZ all use the identical formula):
 *   B = PCLK / (64 * 2^(2n-1) * (N+1))
 * where n = SMR.CKS[1:0] (0-3, selecting PCLK/PCLK4/PCLK16/PCLK64) and N = BRR (0-255). PCLK is
 * taken as P0φ = 32MHz here -- the same real, schematic-confirmed clock already established for
 * OSTM/MTU2 (see ostm.c/mtu2.c's own comments); SCIF shares the same P0φ domain per the RZ/A1H
 * clock tree, not independently re-derived against a second schematic reference. 10 bits/byte
 * (start + 8 data + stop, no parity) -- every SCIF driver traced in this project uses 8N1. */
#define SCIF_PCLK_HZ 32000000

static uint64_t scif_byte_time_ns(RZA1HScifState *s)
{
    int n = s->smr & 0x3;
    uint64_t divisor = 32ULL << (2 * n); /* 64 * 2^(2n-1), integer-safe form */
    uint64_t bit_ns = divisor * (s->brr + 1) * 1000000000ULL / SCIF_PCLK_HZ;

    return bit_ns * 10;
}

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
        /* TDFE/TEND now real (2026-09-10) -- 0 while `tx_timer` is counting
         * down a genuine, baud-rate-derived byte time (see
         * scif_byte_time_ns()), same as real hardware between writing
         * FTDR and the byte actually finishing transmission. Any driver
         * that correctly polls TDFE/TEND before its next FTDR write (every
         * traced SCIF driver in this project does) is now genuinely paced;
         * DR/RDF reflect whether a real received byte (from the chardev
         * backend, or the front-panel responder below) is waiting in
         * frdr. */
        return (s->tx_busy ? 0 : (FSR_TDFE | FSR_TEND)) |
               (s->rx_pending ? (FSR_DR | FSR_RDF) : 0);
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
        /* let a chardev backend deliver its next byte (no-op without one) */
        qemu_chr_fe_accept_input(&s->chr);
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
/* Real pacing for the virtual front-panel ack (2026-09-10) -- added for fun/entertainment, per
 * the user's own request, to see what effect it has on the earlier hotblocks heat-map's SCIF3
 * cluster. Exactly the same idiom and the same honest caveat as DSP_ACK_DELAY_NS just below in
 * this file: no real IC501 datasheet round-trip-time exists to derive this from (this is a
 * virtual responder, not a modeled physical RL78), so this is a deliberately arbitrary
 * placeholder, not a datasheet-derived value. Deliberately a full order of magnitude above
 * DSP_ACK_DELAY_NS (50us) -- 1ms is a round, easily-reasoned stand-in for "an embedded 8/16-bit
 * MCU's own interrupt-latency-plus-one-scheduler-tick" turnaround, not a tuned/measured value.
 * First tried at 200us (2026-09-10): live-verified harmless to real boot behavior (identical
 * trap/r0/timing, 3/3 unplugged trials) but the effect on a *profiled* capture was itself
 * confounded by the profiling plugin's own overhead (see README-history.md's newest section) --
 * raised here per the user's own follow-up request, to separate this value more clearly from
 * DSP_ACK_DELAY_NS. Even if this exceeds the identify handshake's own loop-2 timeout margin (12
 * ticks -- see scif3_frontpanel_identify_handshake's own decompile in README-history.md), that's
 * confirmed harmless: loop 2's own timeout fallback just returns without further action, and
 * scif3_frontpanel_init_and_latch_version's version-latch copy is unconditional either way (see
 * this file's own front-panel-responder comment and README.md's heat-map Status section). */
#define FRONTPANEL_ACK_DELAY_NS 1000000

/* (rza1h_scif3_frontpanel_ack, the original one-frame precompute, was folded into
 * rza1h_scif3_deliver_frame below on 2026-09-24.) */

/* Deliver one complete inbound frame, [type/offset][data...], to the firmware: precompute what
 * scif3_frame_rx_statemachine would hold after 0xFE and those bytes (frame buffer, byte count =
 * 1 + len, "escape pending" bit clear) and deliver only the 0xFD terminator through the real
 * FRDR/RXI path -- the technique rza1h_scif3_frontpanel_ack pioneered (see its comment). The
 * state machine accepts up to 33 bytes after 0xFE, i.e. offset + 32 data bytes. */
static void rza1h_scif3_deliver_frame(RZA1HScifState *s, const uint8_t *f, int len)
{
    AddressSpace *as = &address_space_memory;
    uint32_t frame_buf, desc, status_addr;
    uint8_t byte, status;

    address_space_read(as, SCIF3_FRAME_BUF_PTR_ADDR, MEMTXATTRS_UNSPECIFIED, &frame_buf, 4);
    address_space_read(as, SCIF3_DESC_PTR_ADDR, MEMTXATTRS_UNSPECIFIED, &desc, 4);
    address_space_read(as, SCIF3_STATUS_PTR_ADDR, MEMTXATTRS_UNSPECIFIED, &status_addr, 4);
    address_space_write(as, frame_buf, MEMTXATTRS_UNSPECIFIED, f, len);
    byte = len + 1;
    address_space_write(as, desc, MEMTXATTRS_UNSPECIFIED, &byte, 1);
    address_space_read(as, status_addr, MEMTXATTRS_UNSPECIFIED, &status, 1);
    status &= 0xFE;
    address_space_write(as, status_addr, MEMTXATTRS_UNSPECIFIED, &status, 1);

    s->frdr = 0xFD;
    s->rx_pending = true;
    qemu_irq_lower(s->irq_rx);
    qemu_irq_raise(s->irq_rx);
}

/* in_cb: called from the timer's own callback, which ptimer already runs inside a transaction
 * (a nested ptimer_transaction_begin asserts -- the crash a fast mouse-wheel burst hit). */
static void rza1h_scif3_fp_arm_cb(RZA1HScifState *s, bool in_cb)
{
    if (s->fpq_timer_armed) {
        return;
    }
    s->fpq_timer_armed = true;
    if (!in_cb) {
        ptimer_transaction_begin(s->frontpanel_ack_timer);
    }
    ptimer_set_count(s->frontpanel_ack_timer, FRONTPANEL_ACK_DELAY_NS);
    ptimer_run(s->frontpanel_ack_timer, 1);
    if (!in_cb) {
        ptimer_transaction_commit(s->frontpanel_ack_timer);
    }
}

static void rza1h_scif3_fp_arm(RZA1HScifState *s)
{
    rza1h_scif3_fp_arm_cb(s, false);
}

/* Queue a frame: 0xF0 (identify reply) when off == 0xF0, else [off][fp[off..off+n-1]] as they
 * are now. Touch, dial, knob and pot frames (off >= 0x13) carry absolute values, so one still
 * waiting for the same offset is replaced rather than queued again -- drags and fast wheel
 * spins coalesce instead of overflowing the queue. Key frames (0x0d-0x11) always queue, so a
 * press and its release both reach the firmware. */
static void rza1h_scif3_fp_queue(RZA1HScifState *s, int off, int n)
{
    uint8_t f[34];
    int len = 1;

    f[0] = off;
    if (off != 0xF0) {
        n = MIN(n, 32 - off);
        memcpy(f + 1, s->fp + off, n);
        len += n;
    }
    if (off >= 0x13 && off < 0x20) {
        for (int i = 0; i < s->fpq_count; i++) {
            int k = (s->fpq_head + i) % 16;
            if (s->fpq[k][0] == off && s->fpq_len[k] == len) {
                memcpy(s->fpq[k], f, len);
                s->fpq_len[k] = len;
                return;
            }
        }
    }
    if (s->fpq_count == 16) {
        rza1h_debug("scif", "scif3: front-panel queue full, dropping frame at %#x", off);
        return;
    }
    memcpy(s->fpq[(s->fpq_head + s->fpq_count) % 16], f, len);
    s->fpq_len[(s->fpq_head + s->fpq_count) % 16] = len;
    s->fpq_count++;
    rza1h_scif3_fp_arm(s);
}

/* The queue's pacing timer (FRONTPANEL_ACK_DELAY_NS between frames): deliver the next frame
 * once the firmware has taken the previous terminator byte. */
static void rza1h_scif3_frontpanel_ack_timer_fire(void *opaque)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);
    int k = s->fpq_head;

    s->fpq_timer_armed = false;
    if (!s->fpq_count) {
        return;
    }
    if (s->rx_pending) {
        rza1h_scif3_fp_arm_cb(s, true);
        return;
    }
    rza1h_debug("scif3fp", "deliver frame at %#x, %d data bytes", s->fpq[k][0],
                s->fpq_len[k] - 1);
    rza1h_scif3_deliver_frame(s, s->fpq[k], s->fpq_len[k]);
    s->fpq_head = (s->fpq_head + 1) % 16;
    s->fpq_count--;
    if (s->fpq_count) {
        rza1h_scif3_fp_arm_cb(s, true);
    }
}

/* Power-on report: version text, no touch, knobs at rest, pots from env. Layout (field map
 * from body.bin, notes in qemu-machine/README.md "front panel"): 0x01-0x0c version ASCII (1-3
 * shown as d.dd), 0x0d-0x11 key bits, 0x13 touch tag (0 = touching), 0x14/0x16 touch X/Y BE16
 * pixels, 0x18 main dial BE16 counter, 0x1b/0x1c TWIN PBT counters, 0x1d MULTI counter,
 * 0x1e AF pot, 0x1f RF/SQL pot. */
static void rza1h_scif3_fp_reset(RZA1HScifState *s)
{
    const char *ver = getenv("RZA1H_FP_VERSION");
    const char *e;

    memset(s->fp, 0, sizeof(s->fp));
    memset(s->fp + 1, ' ', 12);
    ver = ver ? ver : "100";
    memcpy(s->fp + 1, ver, MIN(strlen(ver), 12));
    s->fp[0x13] = 0xFF;
    s->fp[0x1e] = (e = getenv("RZA1H_FP_AF")) ? strtol(e, NULL, 0) : 0x80;
    s->fp[0x1f] = (e = getenv("RZA1H_FP_RFSQL")) ? strtol(e, NULL, 0) : 0x60;
    s->fp_booted = false;
    s->fpq_head = s->fpq_count = 0;
    s->fpq_timer_armed = false;
}

/* fpctl: one ASCII command per line, one reply line ("ok ..." / "err ..."). The command set
 * is what tools/fp.py speaks: get | w OFF HEX | bit OFF BIT 0/1 | add8 OFF N | add16 OFF N |
 * touch X Y | release. OFF is hex, N and X/Y decimal. */
static void rza1h_scif3_fpctl_exec(RZA1HScifState *s, char *line)
{
    char reply[80] = "ok";
    char *argv[40];
    int argc = 0;
    char *save, *t;

    for (t = strtok_r(line, " \t\r", &save); t && argc < 40; t = strtok_r(NULL, " \t\r", &save)) {
        argv[argc++] = t;
    }
    if (!argc) {
        return;
    }
    if (!strcmp(argv[0], "get")) {
        int n = snprintf(reply, sizeof(reply), "ok ");
        for (int i = 0; i < 32; i++) {
            n += snprintf(reply + n, sizeof(reply) - n, "%02x", s->fp[i]);
        }
    } else if (!strcmp(argv[0], "w") && argc == 3) {
        int off = strtol(argv[1], NULL, 16), n = strlen(argv[2]) / 2;
        if (off < 0 || off + n > 32 || !n) {
            snprintf(reply, sizeof(reply), "err range");
        } else {
            for (int i = 0; i < n; i++) {
                char h[3] = { argv[2][2 * i], argv[2][2 * i + 1], 0 };
                s->fp[off + i] = strtol(h, NULL, 16);
            }
            rza1h_scif3_fp_queue(s, off, n);
        }
    } else if (!strcmp(argv[0], "bit") && argc == 4) {
        int off = strtol(argv[1], NULL, 16), bit = atoi(argv[2]);
        if (off < 0 || off >= 32 || bit < 0 || bit > 7) {
            snprintf(reply, sizeof(reply), "err range");
        } else {
            s->fp[off] = atoi(argv[3]) ? s->fp[off] | (1 << bit) : s->fp[off] & ~(1 << bit);
            rza1h_scif3_fp_queue(s, off, 1);
        }
    } else if ((!strcmp(argv[0], "add8") || !strcmp(argv[0], "add16")) && argc == 3) {
        int off = strtol(argv[1], NULL, 16), n = atoi(argv[2]);
        bool w16 = argv[0][3] == '1';
        if (off < 0 || off + (w16 ? 2 : 1) > 32) {
            snprintf(reply, sizeof(reply), "err range");
        } else if (w16) {
            uint16_t v = (s->fp[off] << 8 | s->fp[off + 1]) + n;
            s->fp[off] = v >> 8;
            s->fp[off + 1] = v;
            rza1h_scif3_fp_queue(s, off, 2);
        } else {
            s->fp[off] += n;
            rza1h_scif3_fp_queue(s, off, 1);
        }
    } else if (!strcmp(argv[0], "touch") && argc == 3) {
        int x = MIN(MAX(atoi(argv[1]), 0), 479), y = MIN(MAX(atoi(argv[2]), 0), 271);
        s->fp[0x13] = 0;
        s->fp[0x14] = x >> 8; s->fp[0x15] = x;
        s->fp[0x16] = y >> 8; s->fp[0x17] = y;
        rza1h_scif3_fp_queue(s, 0x13, 5);
    } else if (!strcmp(argv[0], "release")) {
        s->fp[0x13] = 0xFF;
        rza1h_scif3_fp_queue(s, 0x13, 1);
    } else {
        snprintf(reply, sizeof(reply), "err unknown command");
    }
    rza1h_debug("scif3fp", "fpctl: %s -> %s", argv[0], reply);
    strcat(reply, "\n");
    qemu_chr_fe_write_all(&s->fpctl, (const uint8_t *)reply, strlen(reply));
}

static int rza1h_scif3_fpctl_can_receive(void *opaque)
{
    return 64;
}

static void rza1h_scif3_fpctl_receive(void *opaque, const uint8_t *buf, int size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    for (int i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            s->fpctl_line[s->fpctl_pos] = 0;
            rza1h_scif3_fpctl_exec(s, s->fpctl_line);
            s->fpctl_pos = 0;
        } else if (s->fpctl_pos < (int)sizeof(s->fpctl_line) - 1) {
            s->fpctl_line[s->fpctl_pos++] = buf[i];
        }
    }
}

/* Window input -> front panel (2026-09-24). Mouse: left button down/drag/up = touch/move/
 * release in LCD pixels (console absolute 0..0x7fff scaled to 480x272). Wheel: MAIN DIAL
 * (FP_WHEEL_DIAL counts per notch); with shift MULTI, with ctrl TWIN PBT inner, with
 * ctrl+shift PBT outer. Keyboard: the table below; keys are held while held (so holding =
 * long press), knob keys step once per press. tools/run_gui.py prints the same map -- keep
 * them in sync. RZA1H_FP_NO_MOUSE / RZA1H_FP_NO_KEYS drop the mouse or keyboard part. */
#define FP_WHEEL_DIAL 5

typedef struct FpKey {
    unsigned code;       /* Linux KEY_* */
    int8_t   off, bit;   /* report byte/bit for a push key; bit < 0 = knob step below */
    int16_t  step;       /* knob: signed step (add16 for off 0x18, add8/absolute otherwise) */
} FpKey;

static const FpKey fp_keymap[] = {
    { KEY_T, 0x0d, 0 }, { KEY_U, 0x0d, 1 }, { KEY_V, 0x0d, 2 }, { KEY_M, 0x0d, 3 },
    { KEY_F, 0x0d, 4 }, { KEY_S, 0x0d, 5 }, { KEY_Q, 0x0d, 6 }, { KEY_X, 0x0d, 7 },
    { KEY_P, 0x0e, 0 }, { KEY_N, 0x0e, 1 }, { KEY_B, 0x0e, 2 }, { KEY_R, 0x0e, 3 },
    { KEY_ESC, 0x0e, 4 }, { KEY_O, 0x0e, 5 }, { KEY_K, 0x0e, 6 }, { KEY_D, 0x0e, 7 },
    { KEY_W, 0x0f, 0 }, { KEY_E, 0x0f, 1 }, { KEY_PAGEUP, 0x0f, 2 }, { KEY_PAGEDOWN, 0x0f, 3 },
    { KEY_I, 0x0f, 4 }, { KEY_J, 0x0f, 5 }, { KEY_C, 0x0f, 6 }, { KEY_L, 0x0f, 7 },
    { KEY_ENTER, 0x10, 2 }, { KEY_Z, 0x10, 3 },
    { KEY_F1, 0x11, 5 }, { KEY_F2, 0x11, 4 }, { KEY_F3, 0x11, 3 }, { KEY_F4, 0x11, 2 },
    { KEY_UP, 0x11, 7 }, { KEY_DOWN, 0x11, 6 },
    /* knobs: left/right = main dial -1/+1, +/- = AF, ]/[ = RF/SQL */
    { KEY_LEFT, 0x18, -1, -1 }, { KEY_RIGHT, 0x18, -1, 1 },
    { KEY_EQUAL, 0x1e, -1, 8 }, { KEY_KPPLUS, 0x1e, -1, 8 },
    { KEY_MINUS, 0x1e, -1, -8 }, { KEY_KPMINUS, 0x1e, -1, -8 },
    { KEY_RIGHTBRACE, 0x1f, -1, 8 }, { KEY_LEFTBRACE, 0x1f, -1, -8 },
};

/* Step a knob: 0x18 is the BE16 main-dial counter; 0x1b-0x1d wrapping u8 counters; 0x1e/0x1f
 * absolute pots (clamped 0..255). */
static void rza1h_scif3_fp_knob(RZA1HScifState *s, int off, int n)
{
    if (off == 0x18) {
        uint16_t v = (s->fp[0x18] << 8 | s->fp[0x19]) + n;
        s->fp[0x18] = v >> 8;
        s->fp[0x19] = v;
        rza1h_scif3_fp_queue(s, 0x18, 2);
    } else if (off >= 0x1e) {
        s->fp[off] = MIN(MAX(s->fp[off] + n, 0), 255);
        rza1h_scif3_fp_queue(s, off, 1);
    } else {
        s->fp[off] += n;
        rza1h_scif3_fp_queue(s, off, 1);
    }
}

static void rza1h_scif3_fp_input_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    RZA1HScifState *s = RZA1H_SCIF(dev);

    if (evt->type == INPUT_EVENT_KIND_ABS) {
        int *v = evt->abs.axis == INPUT_AXIS_X ? &s->fp_mouse_x : &s->fp_mouse_y;
        *v = qemu_input_scale_axis(evt->abs.value, INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX,
                                   0, evt->abs.axis == INPUT_AXIS_X ? 479 : 271);
        s->fp_mouse_dirty |= s->fp_mouse_down;
    } else if (evt->type == INPUT_EVENT_KIND_BTN) {
        InputButton b = evt->btn.button;

        if (b == INPUT_BUTTON_LEFT && !getenv("RZA1H_FP_NO_MOUSE")) {
            s->fp_mouse_down = evt->btn.down;
            s->fp_mouse_dirty = true;
        } else if ((b == INPUT_BUTTON_WHEEL_UP || b == INPUT_BUTTON_WHEEL_DOWN) &&
                   evt->btn.down) {
            int dir = b == INPUT_BUTTON_WHEEL_UP ? 1 : -1;
            if (s->fp_ctrl) {
                rza1h_scif3_fp_knob(s, s->fp_shift ? 0x1c : 0x1b, dir);
            } else if (s->fp_shift) {
                rza1h_scif3_fp_knob(s, 0x1d, dir);
            } else {
                rza1h_scif3_fp_knob(s, 0x18, dir * FP_WHEEL_DIAL);
            }
        }
    } else if (evt->type == INPUT_EVENT_KIND_KEY && !getenv("RZA1H_FP_NO_KEYS")) {
        unsigned code = evt->key.key;
        bool down = evt->key.down;

        if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
            s->fp_shift = down;
            return;
        }
        if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
            s->fp_ctrl = down;
            return;
        }
        for (size_t i = 0; i < ARRAY_SIZE(fp_keymap); i++) {
            const FpKey *k = &fp_keymap[i];
            if (k->code != code) {
                continue;
            }
            if (k->bit >= 0) {
                bool on = s->fp[k->off] & (1 << k->bit);
                if (on != down) {   /* ignore autorepeat */
                    s->fp[k->off] ^= 1 << k->bit;
                    rza1h_scif3_fp_queue(s, k->off, 1);
                    rza1h_debug("scif3fp", "key %u %s -> %#x.%d", code, down ? "down" : "up",
                                k->off, k->bit);
                }
            } else if (down) {
                rza1h_scif3_fp_knob(s, k->off, k->step);
            }
            return;
        }
    }
}

static void rza1h_scif3_fp_input_sync(DeviceState *dev)
{
    RZA1HScifState *s = RZA1H_SCIF(dev);

    if (!s->fp_mouse_dirty) {
        return;
    }
    s->fp_mouse_dirty = false;
    if (s->fp_mouse_down) {
        s->fp[0x13] = 0;
        s->fp[0x14] = s->fp_mouse_x >> 8; s->fp[0x15] = s->fp_mouse_x;
        s->fp[0x16] = s->fp_mouse_y >> 8; s->fp[0x17] = s->fp_mouse_y;
        rza1h_scif3_fp_queue(s, 0x13, 5);
    } else {
        s->fp[0x13] = 0xFF;
        rza1h_scif3_fp_queue(s, 0x13, 1);
    }
}

static const QemuInputHandler rza1h_scif3_fp_input_handler = {
    .name  = "IC-7300 front panel",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_KEY,
    .event = rza1h_scif3_fp_input_event,
    .sync  = rza1h_scif3_fp_input_sync,
};

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
 * 2026-09-21 CORRECTION -- the class-2 ack above was NOT harmless: live GDB
 * tracing (icom-main-idle-loop-not-reached thread) confirmed it makes
 * `main_idle_loop` unreachable for a genuinely long, but bounded, time.
 * `dsp_identity_query_cmd0`-`cmd5` (the `0xE0000000`-`0xE0000005` identity/
 * version-query protocol, all 6 confirmed structurally identical) don't
 * accept a class-2 reply as anything but "not done yet" -- ONLY top nibble
 * `0xF` is accepted (9 is aliased to 1 for storage, never treated as
 * success; that supersedes this comment's own older "9 or 0xE" claim,
 * which was based on a different, unrelated pair of callers --
 * `dsp_page_transfer_verify`'s firmware-update chunk-verify protocol,
 * expects `0xE` and isn't on the boot path). Each of the 6 commands retries
 * up to 18 times (~30s) before giving up and moving to the next one --
 * with every single reply always class-2, ALL 6 always burn their full
 * budget, adding up to ~3 minutes of real boot time before
 * `cold_boot_hw_init` (which calls all 3 query-record orchestrators
 * unconditionally) can finish -- not a deadlock, but far longer than any
 * capture this project had ever run (60-90s) before this was found, which
 * is why it looked unreachable. Sending a class-`0xF` reply instead (see
 * `raw012[0]` below) satisfies all 6 commands' identical accept check on
 * their very first real reply and removes this stall entirely, at zero
 * cost to the "permissive canned ACK, not real DSP protocol fidelity"
 * philosophy this responder already follows elsewhere (mmc.c's virtual SD
 * card, riic.c's virtual EEPROM, rza1h_scif3_frontpanel_ack above) -- the
 * payload bytes' actual real-DSP *meaning* (version numbers? a part ID?)
 * is not recoverable from `body.bin` alone (its only consumer,
 * `factory_file_load`, is an on-demand SD-card menu action, not boot code)
 * and is left as an open question for whoever wants real protocol fidelity
 * here rather than just an unblocking ack.
 *
 * The 3 precomputed bytes (0x0f, 0x00, 0x00) plus the 4th delivered byte
 * (0x00) assemble to raw_word_LE = 0x0000000f; scif5_rx_isr's own `rbit`
 * turns that into 0xf0000000 -- top byte 0xf0, high nibble 0xf. Worked out
 * by hand (rbit is self-inverse: rbit32(0xf0000000) = 0x0000000f) rather
 * than guessed -- same technique the original 0x04/0x20000000 pairing used.
 *
 * 2026-09-21 CORRECTION, same day, later -- the class-0xF reply above fixed
 * `dsp_identity_query_cmd0`-`cmd5`, but turned out to be actively harmful to
 * every OTHER SCIF5 exchange: `scif5_classify_reply` (`0x200b0dc4`) only
 * treats classes 1/2/8 as "resolved" -- class 0xF isn't one of them, so for
 * any command that isn't the boot-time identity query, this ack made
 * `shared_job_ring_dispatch`'s case-1 job sit "still pending" every single
 * time, only ever clearing via its own retry-budget countdown, immediately
 * followed by the *next* queued command hitting the exact same fate. Live-
 * confirmed via a 150s `RZA1H_DEBUG=rspi2,scif` capture: 85,742 canned-ack
 * events in that window (400-1500/sec sustained) -- a genuine retry storm,
 * not real ongoing protocol traffic. Fix: track the real 4-byte command word
 * `scif5_bitrev_transmit_word` sends (via `scif5_cmd_buf`, filled by the
 * `REG_FTDR` write case above) and only answer class-0xF for the identity-
 * query range (`0xE0000000`-`0xE0000005`); everything else gets class-2 (a
 * trivial ack -- `scif5_classify_reply`'s own `uVar6 == 2` branch sets its
 * "resolved" flag unconditionally, so the very first reply now resolves the
 * job instead of retrying). `raw_word_LE = 0x00000004` for class-2, by the
 * same hand derivation: `rbit32(0x00000004) = 0x20000000`, top byte 0x20,
 * high nibble 2. */
static uint32_t rza1h_rbit32(uint32_t v)
{
    v = (v >> 16) | (v << 16);
    v = ((v & 0xff00ff00) >> 8) | ((v & 0x00ff00ff) << 8);
    v = ((v & 0xf0f0f0f0) >> 4) | ((v & 0x0f0f0f0f) << 4);
    v = ((v & 0xcccccccc) >> 2) | ((v & 0x33333333) << 2);
    v = ((v & 0xaaaaaaaa) >> 1) | ((v & 0x55555555) << 1);
    return v;
}

/* Reply word (as the firmware sees it, i.e. after scif5_rx_isr's rbit) for
 * each identity/version query dsp_identity_query_cmd0..cmd5 sends
 * (0xE0000000 + n). Top nibble must be 0xF ("done"); dsp_identity_format_reply
 * (0x200b23e0) turns W into the 6-byte record [W>>16 & 0xff] '.' [W>>8 & 0xff]
 * [W & 0xff] + two decimal digits of (W>>24 & 0xf), in the version records at
 * 0x203def00 (cmd0/1 -> +0x00/+0x06, cmd4/5 -> +0x0d/+0x13, cmd2/3 -> +0x1a/+0x20).
 *
 * 2026-09-23: dsp_fpga_identity_version_check (0x2002a3c0, called from
 * system_mode_request_dispatch at every boot) memcmp()s the first 4 bytes of
 * the even records against ROM strings "3.11" (+0x00), "2.00" (+0x0d) and
 * "3.16" (+0x1a); anything else raises the "DSP/FPGA firmware is wrong
 * version. Retry updating the firmware." dialog (message 0x26), and a record
 * left at its "    " prefill (+0x00, i.e. cmd0 unanswered) raises "DSP is not
 * working correctly." (0x27) instead. The old all-zero reply formatted to
 * "\0.\0\0" and failed the check. The even replies below are therefore
 * required values; the odd ones only feed the SET > Version screen (DSP
 * Program / DSP Data / FPGA fields, via ui_version_screen_populate_fields)
 * and were first guessed by mirroring the even value.
 *
 * 2026-09-24: now read from the DSP side (notes/dsp-protocol.md, "Identity"):
 * the DSP's 0xE0 handler (0x11804728 in the DSP Program) takes bit 0 = half,
 * bits 2..1 = record, and replies with 3 ASCII chars in bytes 2..0 plus the hex
 * value of a 4th char in the low nibble of byte 3. Record 0 (cmd0/1) is the
 * compiled-in "3110"/"1070" (the image's trailing tag "31101070"), record 2
 * (cmd4/5) is the DSP Data image's trailing tag "20001000", read from the DSP's
 * flash at runtime. Record 1 (cmd2/3) is what the FPGA reports at runtime, so
 * its odd half is still a guess. */
/* (The identity replies now come from fake_dsp.c's model of the DSP's 0xE0 handler, together
 * with every other reply -- see rza1h_scif5_dsp_ack.) */

static void rza1h_scif5_dsp_ack(RZA1HScifState *s)
{
    AddressSpace *as = &address_space_memory;
    uint32_t struct_base;
    uint32_t cmd = 0, reply;
    uint32_t raw;
    uint8_t raw012[3];
    uint8_t count = 3;

    if (s->scif5_cmd_pos == 4) {
        uint32_t raw_le = s->scif5_cmd_buf[0] | (s->scif5_cmd_buf[1] << 8) |
                          (s->scif5_cmd_buf[2] << 16) | (s->scif5_cmd_buf[3] << 24);
        cmd = rza1h_rbit32(raw_le);
    }
    /* 2026-09-24: the reply is whatever the DSP model's TX-slot selection picks -- the same
     * change-stamp priority the real DSP's ISR uses. The command itself was already applied
     * (fake_dsp_command from the REG_FTDR case), so an identity query's C6 change wins here,
     * and ordinary commands get the idle C1/C2 word (class 1/2, both "resolved" for
     * scif5_classify_reply, like the old fixed class-2 ack). */
    reply = fake_dsp_next_word(&s->dsp);
    /* The firmware bit-reverses the assembled little-endian 4-byte word, so
     * send rbit32(reply): bytes 0-2 precomputed into the job struct, byte 3
     * through the normal FRDR/RXI path (see this function's comment above;
     * the old fixed 0x0f,0,0 + 0x00 is exactly rbit32(0xF0000000)). */
    raw = rza1h_rbit32(reply);
    raw012[0] = raw;
    raw012[1] = raw >> 8;
    raw012[2] = raw >> 16;

    address_space_read(as, SCIF5_STRUCT_PTR_ADDR, MEMTXATTRS_UNSPECIFIED,
                       &struct_base, 4);
    if (struct_base == 0) {
        return; /* not yet initialized this boot */
    }

    address_space_write(as, struct_base + 0x13, MEMTXATTRS_UNSPECIFIED,
                        raw012, 3);
    address_space_write(as, struct_base + 0xc, MEMTXATTRS_UNSPECIFIED,
                        &count, 1);

    rza1h_debug("scif", "scif5: DSP-link responder: reply %08x (cmd=%08x)",
                reply, cmd);
    s->frdr = raw >> 24;
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

/* Real pacing for the virtual DSP-link ack (2026-09-10) -- `dsp_boot_handshake`'s own two
 * busy-waits (`*DAT_200b6378`, see this file's own module comment) block on real forward
 * progress here, so an instant ack was itself a piece of the same "no timing realism" gap
 * flagged for classic SCIF TX above, just on this DSP-link-specific path instead. No real DSP
 * datasheet exists to derive an exact round-trip time from (this is a virtual responder, not a
 * modeled physical chip) -- `DSP_ACK_DELAY_NS` is a deliberately modest, honestly-labeled
 * placeholder, the same order of magnitude as `dmac.c`'s own `DMAC_COMPLETE_DELAY_NS`, not a
 * datasheet-derived value. This does NOT pace `scif5_ring_pop_and_send`'s own burst path (the
 * `0xa0000000` sentinel, deliberately still ignored below) -- that path's own re-invocation rate
 * isn't gated by anything this device model's read/write handlers can see (see README.md's
 * Status section for why a peripheral-side delay alone can't throttle it). */
#define DSP_ACK_DELAY_NS 50000

static void rza1h_scif5_dsp_ack_timer_fire(void *opaque)
{
    rza1h_scif5_dsp_ack(RZA1H_SCIF(opaque));
}

static void rza1h_scif5_dsp_retry_arm_write(void *opaque, hwaddr offset,
                                            uint64_t value, unsigned size)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    if (value == 0x10000000 || value == 0x40000000) {
        ptimer_transaction_begin(s->dsp_ack_timer);
        ptimer_set_count(s->dsp_ack_timer, DSP_ACK_DELAY_NS);
        ptimer_run(s->dsp_ack_timer, 1); /* oneshot */
        ptimer_transaction_commit(s->dsp_ack_timer);
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
        /* DSP-link command-word tracking (channel 5 only) -- assembles the
         * real 4-byte command scif5_bitrev_transmit_word sends, in transmit
         * order, so rza1h_scif5_dsp_ack can answer based on what was
         * actually asked instead of firing one fixed canned class always.
         * See that function's own comment for the byte-order derivation. */
        if (s->channel == 5) {
            if (s->scif5_cmd_pos >= 0 && s->scif5_cmd_pos < 4) {
                s->scif5_cmd_buf[s->scif5_cmd_pos++] = byte;
            } else {
                s->scif5_cmd_pos = 0;
                s->scif5_cmd_buf[s->scif5_cmd_pos++] = byte;
            }
            /* every complete word goes to the DSP model -- including the fire-and-forget ring
             * bursts (param sync, frequency), which never get a reply here */
            if (s->scif5_cmd_pos == 4) {
                uint32_t raw_le = s->scif5_cmd_buf[0] | (s->scif5_cmd_buf[1] << 8) |
                                  (s->scif5_cmd_buf[2] << 16) |
                                  ((uint32_t)s->scif5_cmd_buf[3] << 24);
                fake_dsp_command(&s->dsp, rza1h_rbit32(raw_le));
            }
        }
        /* Always surfaced, chardev or not -- the whole point of this
         * roadmap item is *observability*, and most testing won't bother
         * wiring a real -chardev per channel. Per-channel-selectable (see
         * scif_bus_log_tx's own comment) -- RZA1H_DEBUG=scif still means
         * every channel, RZA1H_DEBUG=scif<N> selects just one. */
        {
            char _tag[8];
            snprintf(_tag, sizeof(_tag), "scif%u", s->channel);
            if (rza1h_debug_enabled("scif") || rza1h_debug_enabled(_tag)) {
                rza1h_debug(_tag, "scif%u: TX %02x ('%c')",
                           s->channel, byte, (byte >= 0x20 && byte < 0x7f) ? byte : '.');
            }
        }
        scif_bus_log_tx(s, byte);
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, &byte, 1);
        }
        /* Real baud-rate pacing (2026-09-10): TDFE/TEND genuinely go low
         * now and only come back (re-raising TXI, if TIE is enabled, from
         * rza1h_scif_tx_complete()) once a real byte time has elapsed --
         * see scif_byte_time_ns(). This is now a genuine transition, not
         * the old "already true, force an edge" workaround the SCR case
         * above still needs (TDFE really was already 1 there if nothing's
         * in flight). A write that lands while a previous byte is still
         * "in flight" (a driver that doesn't poll TDFE first, e.g. the
         * DSP-link's own arm-sequence path -- see this file's own comment
         * on scope) just restarts the timer for a fresh byte time, the
         * simplest reasonable behavior given this model's no-FIFO-depth
         * convention (matching FDR always reporting empty). */
        qemu_irq_lower(s->irq_tx);
        s->tx_busy = true;
        if (s->channel == 0) {
            s->echo_byte = byte;
        }
        ptimer_transaction_begin(s->tx_timer);
        ptimer_set_count(s->tx_timer, scif_byte_time_ns(s));
        ptimer_run(s->tx_timer, 1); /* oneshot */
        ptimer_transaction_commit(s->tx_timer);
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
                            "ACK for outbound type %#x (delayed %uns, "
                            "for-fun experiment -- see FRONTPANEL_ACK_"
                            "DELAY_NS's own comment)",
                            s->tx_frame_type,
                            (unsigned)FRONTPANEL_ACK_DELAY_NS);
                        /* Deferred (2026-09-10, for-fun/entertainment
                         * experiment) -- was a synchronous call to
                         * rza1h_scif3_frontpanel_ack() right here; see
                         * rza1h_scif3_frontpanel_ack_timer_fire's own
                         * comment for why this is now a real, if
                         * arbitrary, delayed reply instead. */
                        /* 2026-09-24: through the front-panel queue. The first data
                         * frame gets the full power-on report (that's what latches the
                         * version and ends the no-touch/zero state); later ones get a
                         * 1-byte echo of the model's own byte, not a 0 (a 0 at 0x13
                         * reads as a touch, at 0x1e as AF = 0). */
                        if (s->tx_frame_type == 0xF0) {
                            rza1h_scif3_fp_queue(s, 0xF0, 0);
                        } else if (!s->fp_booted) {
                            s->fp_booted = true;
                            rza1h_scif3_fp_queue(s, 0, 32);
                        } else {
                            rza1h_scif3_fp_queue(s, s->tx_frame_type, 1);
                        }
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
    {
        char _tag[8];
        snprintf(_tag, sizeof(_tag), "scif%u", s->channel);
        if (rza1h_debug_enabled("scif") || rza1h_debug_enabled(_tag)) {
            rza1h_debug(_tag, "scif%u: RX %02x (real -chardev backend)", s->channel, buf[0]);
        }
    }
    s->frdr = buf[0];
    s->rx_pending = true;
    /* 2026-09-24: raise RXI for chardev bytes too (level: RIE & RDF), so a host-side
     * CI-V client on SCIF0 reaches the firmware's real RX ISR. Before this only the
     * built-in channel-3/5 responders ever raised irq_rx. */
    if (s->scr & SCR_RIE) {
        qemu_irq_lower(s->irq_rx);
        qemu_irq_raise(s->irq_rx);
    }
}

/* ptimer callback for the real baud-rate pacing above -- fires once a real byte time has
 * elapsed since the triggering REG_FTDR write, putting TDFE/TEND back to the ready state and
 * re-raising TXI (level-triggered, same as every other TXI transition in this file) if TIE is
 * still enabled. */
static void rza1h_scif_tx_complete(void *opaque)
{
    RZA1HScifState *s = RZA1H_SCIF(opaque);

    s->tx_busy = false;
    /* CI-V is a single-wire bus: on the real [REMOTE] jack every byte the radio sends comes
     * straight back into its own receiver, and the firmware's CI-V transmitter waits for
     * that echo (collision detection, cf. the 0xFC jam handling in
     * civ_frame_rx_statemachine) before sending the next byte. Without it a reply stalls
     * after its first 0xFE. A byte the host is sending at the same moment wins; the echo
     * is then lost, as in a real collision. */
    if (s->channel == 0 && s->echo_byte >= 0) {
        uint8_t b = s->echo_byte;

        s->echo_byte = -1;
        if (!s->rx_pending && (s->scr & SCR_RE)) {
            rza1h_scif_receive(s, &b, 1);
        }
    }
    if (s->scr & SCR_TIE) {
        qemu_irq_raise(s->irq_tx);
    }
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
    s->bus_tx_len = -1;
    s->tx_busy = false;
    s->echo_byte = -1;
    s->scif5_cmd_pos = 0;
    fake_dsp_reset(&s->dsp);
    ptimer_transaction_begin(s->tx_timer);
    ptimer_stop(s->tx_timer);
    ptimer_transaction_commit(s->tx_timer);
    ptimer_transaction_begin(s->dsp_ack_timer);
    ptimer_stop(s->dsp_ack_timer);
    ptimer_transaction_commit(s->dsp_ack_timer);
    ptimer_transaction_begin(s->frontpanel_ack_timer);
    ptimer_stop(s->frontpanel_ack_timer);
    ptimer_transaction_commit(s->frontpanel_ack_timer);
    rza1h_scif3_fp_reset(s);
}

static void rza1h_scif_realize(DeviceState *dev, Error **errp)
{
    RZA1HScifState *s = RZA1H_SCIF(dev);

    qemu_chr_fe_set_handlers(&s->chr, rza1h_scif_can_receive,
                             rza1h_scif_receive, NULL, NULL, s, NULL, true);

    s->tx_timer = ptimer_init(rza1h_scif_tx_complete, s,
                              PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                              PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->tx_timer);
    ptimer_set_freq(s->tx_timer, 1000000000); /* 1 tick = 1ns, matching dmac.c's own idiom */
    ptimer_transaction_commit(s->tx_timer);

    /* Harmless to create on every instance (like iomem_dsp_retry above) -- only ever armed via
     * the channel-5-only MMIO region's write handler. */
    s->dsp_ack_timer = ptimer_init(rza1h_scif5_dsp_ack_timer_fire, s,
                                   PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                   PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->dsp_ack_timer);
    ptimer_set_freq(s->dsp_ack_timer, 1000000000);
    ptimer_transaction_commit(s->dsp_ack_timer);

    /* Harmless to create on every instance, same rationale as dsp_ack_timer above -- only ever
     * armed on the channel-3 instance's own REG_FTDR case. */
    s->frontpanel_ack_timer = ptimer_init(rza1h_scif3_frontpanel_ack_timer_fire, s,
                                          PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                          PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->frontpanel_ack_timer);
    ptimer_set_freq(s->frontpanel_ack_timer, 1000000000);
    ptimer_transaction_commit(s->frontpanel_ack_timer);

    if (s->channel == 3) {
        qemu_chr_fe_set_handlers(&s->fpctl, rza1h_scif3_fpctl_can_receive,
                                 rza1h_scif3_fpctl_receive, NULL, NULL, s, NULL, true);
        s->fp_input = qemu_input_handler_register(dev, &rza1h_scif3_fp_input_handler);
        qemu_input_handler_activate(s->fp_input);
    }
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
    DEFINE_PROP_CHR("fpctl", RZA1HScifState, fpctl),   /* channel 3: front-panel control */
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
