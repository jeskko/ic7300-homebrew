/*
 * Behavioural model of the IC-7300's FPGA (IC1351) band-scope side, as seen over the CPU's
 * RSPI2 link. Spec: notes/fpga-link.md (static analysis of body.bin v1.42).
 *
 * The link, from the FPGA's point of view (one MOSI byte in, one MISO byte out per clock):
 *   - Register frame: [start index 0..6][data...] into a 7-byte register file.
 *     reg0 flags, reg1-3 signed 24-bit offset from the VFO (Hz), reg4-5 half-span / 50 Hz,
 *     reg6 = seq << 4.
 *   - Sweep read: MOSI 90 00 [00 x475]; MISO xx H [s0..s474], with
 *     H = (reg6 & 0xf0) | sweep counter. The CPU drops a sweep unless H's high nibble matches
 *     the seq it last sent and the low nibble changed since its last accepted sweep.
 *     Samples: display = clamp(raw + 2*ref_dB - 40, 0, 160), so at ref 0 dB raw 40..200
 *     fills the scope.
 *
 * Pure C, no QEMU device state: rspi2.c owns the SPI timing and calls in here. The sweep
 * counter comes from the caller's clock (one sweep per `sweep_ns`), so no timer is needed.
 */
#ifndef FAKE_FPGA_H
#define FAKE_FPGA_H

#include "qemu/osdep.h"

#define FAKE_FPGA_REGS 7
#define FAKE_FPGA_SAMPLES 475
#define FAKE_FPGA_READ_CMD 0x90
#define FAKE_FPGA_MAX_SIGNALS 8

typedef struct FakeFpgaSignal {
    int32_t offset_hz;       /* from the VFO */
    uint8_t raw;             /* peak raw level */
} FakeFpgaSignal;

typedef enum {
    FAKE_FPGA_IDLE,          /* waiting for a transfer's first byte */
    FAKE_FPGA_REG_WRITE,     /* register frame, next byte goes to reg[idx] */
    FAKE_FPGA_READ_HDR,      /* 0x90 received, next clock returns the header */
    FAKE_FPGA_READ_DATA,     /* returning samples */
} FakeFpgaState;

typedef struct FakeFpga {
    uint8_t reg[FAKE_FPGA_REGS];
    FakeFpgaState state;
    unsigned idx;            /* register index or sample index */
    uint8_t sweep[FAKE_FPGA_SAMPLES];

    /* configuration (env knobs, see fake_fpga.c) */
    uint64_t sweep_ns;
    uint8_t floor_raw;
    FakeFpgaSignal sig[FAKE_FPGA_MAX_SIGNALS];
    int nsig;
    uint32_t rng;

    /* observability */
    uint64_t last_sweep_no;
    uint64_t reads, served, frames;
} FakeFpga;

void fake_fpga_reset(FakeFpga *f);
/* A new SPI transfer starts (chip-select edge). */
void fake_fpga_begin(FakeFpga *f);
/* One byte clocked: `mosi` from the CPU, returns MISO. `now_ns` = emulated time. */
uint8_t fake_fpga_xfer(FakeFpga *f, uint8_t mosi, uint64_t now_ns);

#endif
