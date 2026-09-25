/*
 * Behavioural model of the IC-7300's IF-DSP (TMS320C6745) as seen over the CPU's SCIF5 link.
 *
 * Built from the DSP Program's own code (notes/dsp-protocol.md): the CPU sends 32-bit command
 * words whose top byte is an opcode (the DSP's 256-entry dispatch table), and every link frame
 * carries one DSP word back, chosen from 7 class-tagged TX slots C0..C6 by the same
 * change-stamp priority the DSP's McASP1 ISR (0x11811e78) uses. This model keeps that slot
 * machinery, a per-opcode "latest word" table like the DSP's P[] array, and logs the command
 * traffic so we can see which opcodes the firmware really uses.
 *
 * Pure C, no QEMU device state: scif.c owns the link timing and calls in here.
 */
#ifndef FAKE_DSP_H
#define FAKE_DSP_H

#include "qemu/osdep.h"

#define FAKE_DSP_SLOTS 7
#define FAKE_DSP_QUEUE 64
/* frames between a command's arrival and its effect: >= 2 (the CPU discards the first two
 * reads after a query), <= ~19 (it gives up after 18 more) */
#define FAKE_DSP_CMD_LATENCY_FRAMES 2

typedef struct FakeDsp {
    /* DSP-side state */
    uint32_t p[256];          /* latest command word per opcode (DSP P[] at 0x11817b20) */
    uint32_t c_word[FAKE_DSP_SLOTS], c_stamp[FAKE_DSP_SLOTS];  /* TX slots C0..C6 */
    uint32_t l_word[FAKE_DSP_SLOTS], l_stamp[FAKE_DSP_SLOTS];  /* last sent copies */
    uint32_t queue[FAKE_DSP_QUEUE];       /* commands received since the last frame (see next_word) */
    uint8_t  queue_frames[FAKE_DSP_QUEUE]; /* frames left until each takes effect */
    unsigned queued;
    uint8_t  update_flag;     /* DSP DP+0x1eb: 0xFF normal, 0 inside a firmware write (E2..E3) */
    uint32_t rx_freq_hz;      /* opcode 0x10, incl. the 36 kHz IF offset the CPU adds */

    /* observability */
    uint32_t last_seen[256];  /* for change-only logging (P[] lags by the queue) */
    uint32_t log_freq_hz;     /* 0x10 as seen on arrival, for the log */
    uint32_t count[256];
    uint64_t total;           /* command words */
    uint64_t frames;          /* DSP words sent (CPU one-word reads) */
    uint32_t slot_sent[FAKE_DSP_SLOTS];
    uint64_t next_summary;

    /* experiment: ramp a bitfield of one slot every 16 replies (RZA1H_DSP_SWEEP) */
    int      sweep_slot;      /* -1 = off */
    int      sweep_lo, sweep_width;
    uint32_t sweep_val;
    uint32_t replies;
} FakeDsp;

/* The demodulator mode the fake DSP was last told (opcode 0x22 byte 1: 0 USB, 1 LSB, 2 CW,
 * 4 RTTY, 5 RTTY-R, 0x0a AM, 0x0c FM; 0xff = none yet). A global because the DSP's audio side
 * lives in ssif.c (the RTTY demodulator driving the RTD pin reads it). */
extern uint8_t fake_dsp_mode;

void fake_dsp_reset(FakeDsp *d);
/* A complete command word from the CPU (already un-bit-reversed). */
void fake_dsp_command(FakeDsp *d, uint32_t w);
/* The DSP's next TX word for this frame (consumes the change it reports). */
uint32_t fake_dsp_next_word(FakeDsp *d);

#endif
