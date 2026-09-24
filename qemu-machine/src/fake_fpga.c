/*
 * FPGA band-scope behavioural model -- see fake_fpga.h and notes/fpga-link.md.
 *
 * What is modelled: the register file, the 0x90 sweep read with its header handshake,
 * FPGA-side sweep pacing (the counter nibble advances once per sweep_ns of emulated time,
 * which is the only thing that sets the scope's update rate: the CPU polls every main-loop
 * pass), and a synthetic spectrum (noise floor plus a few carriers).
 * What is NOT: real RF. Carrier positions are relative to the scope centre (the displayed
 * frequency in centre mode). reg1-3, the offset the FPGA would tune by, is logged but not
 * applied. That's fine for centre mode; fixed mode would need the VFO, which the FPGA gets
 * from elsewhere.
 *
 * Env knobs:
 *   RZA1H_FPGA_SWEEP_HZ  sweeps per second (default 30)
 *   RZA1H_FPGA_FLOOR     noise-floor raw level (default 60 = 10 dB above the scope bottom
 *                        at ref 0 dB)
 *   RZA1H_FPGA_SIGNALS   "offset_hz:raw,..." carriers, offset from the centre
 *                        (default "5000:170,-12000:130,1500:110"; "none" = floor only)
 * Logging (RZA1H_DEBUG=fpga): register frames (decoded) and a line per 64 served sweeps.
 */
#include "fake_fpga.h"
#include <math.h>
#include "rza1h_debug.h"

static void fake_fpga_parse_env(FakeFpga *f)
{
    const char *e;
    unsigned hz = 30;

    if ((e = getenv("RZA1H_FPGA_SWEEP_HZ")) && atoi(e) > 0) {
        hz = atoi(e);
    }
    f->sweep_ns = 1000000000ull / hz;

    f->floor_raw = 60;
    if ((e = getenv("RZA1H_FPGA_FLOOR"))) {
        f->floor_raw = MIN(atoi(e), 255);
    }

    e = getenv("RZA1H_FPGA_SIGNALS");
    if (!e) {
        e = "5000:170,-12000:130,1500:110";
    }
    f->nsig = 0;
    while (*e && strcmp(e, "none") && f->nsig < FAKE_FPGA_MAX_SIGNALS) {
        char *end;
        long off = strtol(e, &end, 0);
        if (*end != ':') {
            break;
        }
        long raw = strtol(end + 1, &end, 0);
        f->sig[f->nsig].offset_hz = off;
        f->sig[f->nsig].raw = MIN(MAX(raw, 0), 255);
        f->nsig++;
        if (*end != ',') {
            break;
        }
        e = end + 1;
    }
}

void fake_fpga_reset(FakeFpga *f)
{
    memset(f, 0, sizeof(*f));
    f->rng = 0x7300;
    f->last_sweep_no = UINT64_MAX;
    fake_fpga_parse_env(f);
    rza1h_debug("fpga", "reset: %u sweeps/s, floor %u, %d signals",
                (unsigned)(1000000000ull / f->sweep_ns), f->floor_raw, f->nsig);
}

static uint32_t fake_fpga_rand(FakeFpga *f)
{
    f->rng = f->rng * 1103515245u + 12345u;
    return f->rng >> 16;
}

static int32_t reg_offset_hz(const FakeFpga *f)
{
    int32_t v = (f->reg[1] << 16) | (f->reg[2] << 8) | f->reg[3];
    return (v << 8) >> 8;  /* sign-extend 24 bits */
}

static uint32_t reg_half_span_hz(const FakeFpga *f)
{
    return ((f->reg[4] << 8) | f->reg[5]) * 50u;
}

/* One synthetic sweep across +-half-span. A carrier is a peak falling 12 raw counts
 * (6 dB) per bin or per 100 Hz, whichever is wider, so it stays visible at wide spans. */
static void fake_fpga_make_sweep(FakeFpga *f)
{
    uint32_t half = reg_half_span_hz(f);
    double bin_hz = half ? 2.0 * half / (FAKE_FPGA_SAMPLES - 1) : 1.0;
    double step_hz = MAX(bin_hz, 100.0);

    for (int i = 0; i < FAKE_FPGA_SAMPLES; i++) {
        double fhz = (i - (FAKE_FPGA_SAMPLES - 1) / 2) * bin_hz;
        int v = f->floor_raw + (int)(fake_fpga_rand(f) % 11) - 5;

        for (int k = 0; k < f->nsig; k++) {
            double d = fabs(fhz - f->sig[k].offset_hz);
            int s = f->sig[k].raw - (int)(12.0 * d / step_hz);
            v = MAX(v, s);
        }
        f->sweep[i] = MIN(MAX(v, 0), 255);
    }
}

static void fake_fpga_log_regs(FakeFpga *f)
{
    f->frames++;
    rza1h_debug("fpga", "regs #%" PRIu64 ": r0=%02x offset=%+d Hz half-span=%u Hz seq=%u",
                f->frames, f->reg[0], reg_offset_hz(f), reg_half_span_hz(f), f->reg[6] >> 4);
}

void fake_fpga_begin(FakeFpga *f)
{
    if (f->state == FAKE_FPGA_REG_WRITE) {
        fake_fpga_log_regs(f);
    }
    f->state = FAKE_FPGA_IDLE;
}

uint8_t fake_fpga_xfer(FakeFpga *f, uint8_t mosi, uint64_t now_ns)
{
    switch (f->state) {
    case FAKE_FPGA_IDLE:
        if (mosi == FAKE_FPGA_READ_CMD) {
            f->state = FAKE_FPGA_READ_HDR;
            f->reads++;
        } else if (mosi < FAKE_FPGA_REGS) {
            f->state = FAKE_FPGA_REG_WRITE;
            f->idx = mosi;
        } else {
            rza1h_debug("fpga", "unknown command byte %02x", mosi);
        }
        return 0;

    case FAKE_FPGA_REG_WRITE:
        if (f->idx < FAKE_FPGA_REGS) {
            f->reg[f->idx++] = mosi;
        }
        return 0;

    case FAKE_FPGA_READ_HDR: {
        uint64_t no = now_ns / f->sweep_ns;

        if (no != f->last_sweep_no) {
            f->last_sweep_no = no;
            fake_fpga_make_sweep(f);
            if (++f->served % 64 == 1) {
                rza1h_debug("fpga", "sweep %" PRIu64 " (reads %" PRIu64 "), seq %u, "
                            "centre sample %u", f->served, f->reads, f->reg[6] >> 4,
                            f->sweep[FAKE_FPGA_SAMPLES / 2]);
            }
        }
        f->state = FAKE_FPGA_READ_DATA;
        f->idx = 0;
        return (f->reg[6] & 0xf0) | (no & 0x0f);
    }

    case FAKE_FPGA_READ_DATA:
        return f->idx < FAKE_FPGA_SAMPLES ? f->sweep[f->idx++] : 0;
    }
    return 0;
}
