/*
 * IF-DSP behavioural model -- see fake_dsp.h and notes/dsp-protocol.md.
 *
 * What is modelled (from the DSP Program v3.11 code):
 *   - TX slot selection, exactly as the McASP1 ISR does it:
 *       C0 if its word changed; else C3 if its stamp changed; else C6 if its stamp changed;
 *       else C5 while inside a firmware write (update_flag == 0); else C4 while the L[18]
 *       burst counter runs (never, here: its setter isn't known); else C1 or C2 by
 *       bit 23 of P[0x00].
 *   - Slot classes are fixed per slot (C0=0 C1=1 C2=2 C3=7 C4=8 C5=E C6=F).
 *   - 0xE0 identity replies from the three version tags (DSP Program compiled in, FPGA and
 *     DSP Data read from the DSP's flash on the real board).
 *   - 0xE2/0xE3 bracket a firmware write (update_flag), 0x10 RX frequency, P[] per opcode.
 * What is NOT: audio, meters (the C0..C4 contents stay at their init values unless
 * RZA1H_DSP_SLOT / RZA1H_DSP_SWEEP set them), firmware-page checksums (C5 is never updated,
 * so a firmware update through the emulator would fail verification).
 *
 * Logging (RZA1H_DEBUG=dsp): every command whose word differs from that opcode's previous
 * one (the real handlers mostly act only on change), first sight of each opcode, opcodes the
 * real DSP ignores, and a per-opcode count summary every FAKE_DSP_SUMMARY_EVERY words.
 */
#include "fake_dsp.h"
#include "rza1h_debug.h"

/* summary every N link frames: commands are rare (a whole boot sends ~50 words, only on
 * change), frames are ~90/s of emulated steady state */
#define FAKE_DSP_SUMMARY_EVERY 2000

/* One line per opcode the DSP Program's dispatch table (RAM 0x11818100) implements. NULL =
 * default handler (a bare return: the real DSP ignores the word). Meanings are the notes'
 * best reading; "?" = mechanics known, purpose not. */
static const char *const dsp_op_name[256] = {
    [0x00] = "ctrl (bit23 picks C1/C2 fallback; bit9/5/13/12 mode+reset)",
    [0x01] = "slew enable/dir? (bit2)",
    [0x10] = "RX frequency (+36 kHz IF), 2 words by bit23",
    [0x20] = "filter coeffs A (PBT/notch-like, two 9-bit fields)",
    [0x21] = "filter shape preset (6-bit idx + 12-bit signed)",
    [0x22] = "operating mode / demod select (byte1)",
    [0x23] = "gain/reference context (byte1, nibble)",
    [0x24] = "3..7 (kHz?) param + tables",
    [0x25] = "IF filter width/shape (10-bit mag, sign, 2-bit type)",
    [0x27] = "? (store only)",
    [0x40] = "? (store only)",
    [0x41] = "decay/smoothing rate (16 steps)",
    [0x42] = "two levels (byte1, byte2)",
    [0x43] = "tone generator (byte1 on, byte2 freq)",
    [0x44] = "commit of 0x42",
    [0x48] = "effect block reset/level",
    [0x49] = "two log-taper levels",
    [0x4a] = "two log-taper levels (alt)",
    [0x4b] = "? (store only)",
    [0x4c] = "time-constant preset (nibble)",
    [0x4d] = "? (store only)",
    [0x4e] = "? (store only)",
    [0x4f] = "? (store only)",
    [0x61] = "rate (52 steps) + signed fine offset",
    [0x62] = "shape selector (nibble)",
    [0x6b] = "? (bit1: math path / merge into P[0])",
    [0x80] = "? (store only)",
    [0x81] = "? (store only)",
    [0xa0] = "DSP-flash store page byte (0x28000 area)",
    [0xb0] = "firmware page data (3 bytes/word)",
    [0xe0] = "identity query",
    [0xe1] = "erase DSP-flash store 0x28000-0x4ffff",
    [0xe2] = "begin firmware write (page address)",
    [0xe3] = "end firmware write",
};

static const char *op_name(uint8_t op)
{
    if (op >= 0xa0 && op <= 0xaf) {
        return dsp_op_name[0xa0];
    }
    if (op >= 0xb0 && op <= 0xbf) {
        return dsp_op_name[0xb0];
    }
    return dsp_op_name[op];
}

/* Version tags, 4 chars per half: DSP Program (compiled into the DSP image, "31101070"),
 * FPGA (first 8 bytes of the FPGA image, "31601130"; on the real board the reply comes from
 * digits the DSP reads back, assumed equal), DSP Data (its trailing tag "20001000"). */
static const char dsp_version_tag[3][9] = { "31101070", "31601130", "20001000" };

static uint32_t hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
        return (c - 7) & 0xf;
    }
    return 0;
}

/* DSP 0xE0 handler (0x11804728): bit0 = half, bits 2..1 = record (2 and 3 both = DSP Data).
 * Bytes 2..0 = 3 ASCII chars, low nibble of byte 3 = hex value of the 4th, class stays F. */
static void identity_reply(FakeDsp *d, uint32_t w)
{
    unsigned rec = (w >> 1) & 3;
    const char *t = dsp_version_tag[rec > 2 ? 2 : rec] + ((w & 1) ? 4 : 0);

    d->c_word[6] = 0xF0000000 | (hexval(t[3]) << 24) |
                   ((uint32_t)(uint8_t)t[0] << 16) | ((uint32_t)(uint8_t)t[1] << 8) |
                   (uint8_t)t[2];
    d->c_stamp[6]++;
}

static void log_summary(FakeDsp *d)
{
    char buf[1024];
    int n = 0;

    for (int op = 0; op < 256 && n < (int)sizeof(buf) - 16; op++) {
        if (d->count[op]) {
            n += snprintf(buf + n, sizeof(buf) - n, " %02x:%u", op, d->count[op]);
        }
    }
    rza1h_debug("dsp", "summary: %" PRIu64 " frames, slots sent C0..C6 = %u %u %u %u %u %u %u; "
                "%" PRIu64 " command words, per opcode:%s", d->frames,
                d->slot_sent[0], d->slot_sent[1], d->slot_sent[2], d->slot_sent[3],
                d->slot_sent[4], d->slot_sent[5], d->slot_sent[6], d->total, buf);
}

/* 0x10 handler (0x11809fb8): bit23=0 -> bits 10..0 are freq bits 26..16; bit23=1 -> low 16 */
static uint32_t freq_merge(uint32_t f, uint32_t w)
{
    if (w & 0x00800000) {
        return (f & 0xffff0000) | (w & 0xffff);
    }
    return (f & ~0x07ff0000u) | ((w & 0x7ff) << 16);
}

/* A command's effect on the DSP state (the handler). */
uint8_t fake_dsp_mode = 0xff;

static void apply(FakeDsp *d, uint32_t w)
{
    uint8_t op = w >> 24;

    d->p[op] = w;
    switch (op) {
    case 0x10:
        d->rx_freq_hz = freq_merge(d->rx_freq_hz, w);
        break;
    case 0x22:
        fake_dsp_mode = (w >> 16) & 0xff;
        break;
    case 0xe0:
        identity_reply(d, w);
        break;
    case 0xe2:
        d->update_flag = 0;
        break;
    case 0xe3:
        d->update_flag = 0xff;
        break;
    default:
        break;
    }
}

/* Called once per frame, after the frame's word was picked: commands whose latency has run
 * out take effect (FIFO, all with the same latency, so always a prefix of the queue). */
static void apply_queued(FakeDsp *d)
{
    unsigned n = 0;

    for (unsigned i = 0; i < d->queued; i++) {
        if (--d->queue_frames[i] == 0) {
            apply(d, d->queue[i]);
            n = i + 1;
        }
    }
    memmove(d->queue, d->queue + n, sizeof(d->queue[0]) * (d->queued - n));
    memmove(d->queue_frames, d->queue_frames + n, sizeof(d->queue_frames[0]) * (d->queued - n));
    d->queued -= n;
}

void fake_dsp_command(FakeDsp *d, uint32_t w)
{
    uint8_t op = w >> 24;
    uint32_t prev = d->last_seen[op];
    bool first = d->count[op] == 0;
    const char *name = op_name(op);

    d->count[op]++;
    d->total++;
    d->last_seen[op] = w;
    /* fire-and-forget bursts can bring many words between frames; apply the oldest early
     * rather than drop anything (state order is preserved either way) */
    if (d->queued == FAKE_DSP_QUEUE) {
        apply(d, d->queue[0]);
        memmove(d->queue, d->queue + 1, sizeof(d->queue[0]) * (FAKE_DSP_QUEUE - 1));
        memmove(d->queue_frames, d->queue_frames + 1,
                sizeof(d->queue_frames[0]) * (FAKE_DSP_QUEUE - 1));
        d->queued--;
    }
    d->queue[d->queued] = w;
    d->queue_frames[d->queued++] = FAKE_DSP_CMD_LATENCY_FRAMES;

    if (!name) {
        rza1h_debug("dsp", "cmd %08x: opcode %02x ignored by the real DSP (default handler)",
                    w, op);
    } else if (first || w != prev) {
        if (op == 0x10) {
            d->log_freq_hz = freq_merge(d->log_freq_hz, w);
            rza1h_debug("dsp", "cmd %08x [%02x %s]%s f=%u Hz (dial %u Hz)", w, op, name,
                        first ? " FIRST" : "", d->log_freq_hz, d->log_freq_hz - 36000);
        } else {
            rza1h_debug("dsp", "cmd %08x [%02x %s]%s", w, op, name, first ? " FIRST" : "");
        }
    }
}

static void sweep_step(FakeDsp *d)
{
    uint32_t mask;

    /* one step per 16 replies: a slot that changes every frame (C0 especially) would
     * otherwise win every frame, and class 0 never resolves a pending CPU command */
    if (d->sweep_slot < 0 || (d->replies++ & 15) != 0) {
        return;
    }
    mask = (d->sweep_width >= 32 ? 0xffffffffu : ((1u << d->sweep_width) - 1)) << d->sweep_lo;
    d->sweep_val++;
    d->c_word[d->sweep_slot] = (d->c_word[d->sweep_slot] & ~mask) |
                               ((d->sweep_val << d->sweep_lo) & mask);
    d->c_stamp[d->sweep_slot]++;
}

static int pick(FakeDsp *d)
{
    if (d->c_word[0] != d->l_word[0] || d->c_stamp[0] != d->l_stamp[0]) {
        return 0;
    }
    if (d->c_stamp[3] != d->l_stamp[3]) {
        return 3;
    }
    if (d->c_stamp[6] != d->l_stamp[6]) {
        return 6;
    }
    if (d->update_flag == 0) {
        return 5;
    }
    /* C4 (class 8) would go here while the DSP's L[18] burst counter runs; its setter is not
     * known yet (notes/dsp-protocol.md), so it's never sent. */
    return (d->p[0x00] & 0x00800000) ? 2 : 1;
}

/* One link frame: the DSP's ISR picks the word from the slots as they are *now* and marks it
 * sent. A command takes effect only FAKE_DSP_CMD_LATENCY_FRAMES frames after it arrived,
 * because on the real DSP it goes through the RX ring to the main loop, which drains it
 * between audio blocks. The CPU side is built for that: scif5_arm_retry_timer is a "receive
 * one word" (line turnaround + one-word read), and e.g. dsp_identity_query_cmd0 transmits the
 * query, throws away 2 reads, then waits up to 18 more for the class-F answer. An answer in
 * either of the first two frames is lost (both eager and 1-frame-lag versions of this model
 * failed exactly that way: "DSP is not working correctly"). */
uint32_t fake_dsp_next_word(FakeDsp *d)
{
    int k;
    uint32_t w;

    sweep_step(d);
    k = pick(d);
    d->l_word[k] = d->c_word[k];
    d->l_stamp[k] = d->c_stamp[k];
    w = d->c_word[k];
    d->slot_sent[k]++;
    apply_queued(d);
    if (++d->frames >= d->next_summary) {
        d->next_summary = d->frames + FAKE_DSP_SUMMARY_EVERY;
        log_summary(d);
    }
    return w;
}

/* RZA1H_DSP_SLOT="k=0xWORD[,k=0xWORD...]" sets slot k's initial word (class nibble forced to
 * the slot's own). RZA1H_DSP_SWEEP="k:lo:width" ramps bits lo..lo+width-1 of slot k by one every
 * 16 replies -- for finding which slot/field drives which on-screen meter. */
static const uint32_t slot_class[FAKE_DSP_SLOTS] = {
    0x00000000, 0x10000000, 0x20000000, 0x70000000, 0x80000000, 0xE0000000, 0xF0000000,
};

static void parse_env(FakeDsp *d)
{
    const char *e = getenv("RZA1H_DSP_SLOT");
    int k, lo, width;

    while (e && *e) {
        unsigned long v;
        char *end;

        k = strtol(e, &end, 0);
        if (*end != '=' || k < 0 || k >= FAKE_DSP_SLOTS) {
            break;
        }
        v = strtoul(end + 1, &end, 0);
        d->c_word[k] = slot_class[k] | (v & 0x0fffffff);
        d->c_stamp[k]++;
        e = (*end == ',') ? end + 1 : end;
    }

    e = getenv("RZA1H_DSP_SWEEP");
    if (e && sscanf(e, "%d:%d:%d", &k, &lo, &width) == 3 &&
        k >= 0 && k < FAKE_DSP_SLOTS && lo >= 0 && width > 0 && lo + width <= 28) {
        d->sweep_slot = k;
        d->sweep_lo = lo;
        d->sweep_width = width;
    }
}

void fake_dsp_reset(FakeDsp *d)
{
    memset(d, 0, sizeof(*d));
    /* initial P[] values from the DSP's init (0x118131f8): each slot holds its own opcode */
    for (int op = 0; op < 256; op++) {
        d->p[op] = d->last_seen[op] = (uint32_t)op << 24;
    }
    d->p[0x20] = d->p[0x21] = 0xffffffff;
    d->p[0x22] = 0x22ffffff;
    fake_dsp_mode = 0xff;
    d->p[0x40] = 0x40005555;
    memcpy(d->c_word, slot_class, sizeof(d->c_word));
    memcpy(d->l_word, slot_class, sizeof(d->l_word));
    d->update_flag = 0xff;  /* .cinit value */
    d->sweep_slot = -1;
    d->next_summary = FAKE_DSP_SUMMARY_EVERY;
    parse_env(d);
    /* the env overrides count as changes only for C0/C3/C6 (the stamp-checked slots);
     * C1/C2 are the idle rotation and go out regardless */
    for (int k = 1; k < FAKE_DSP_SLOTS; k++) {
        if (k != 3 && k != 6) {
            d->l_stamp[k] = d->c_stamp[k];
        }
    }
}
