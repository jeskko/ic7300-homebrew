/*
 * RZ/A1H GPIO/port register block -- direct C port of
 * emu/peripherals/gpio.py. See that module's own docstring for the full
 * derivation (register offsets/roles from rza1.svd and direct
 * decompilation of real body.bin code; PSR/PMSR/PMCSR/PNOT semantics are a
 * structurally well-motivated inference, not independently confirmed --
 * same caveat carried over here unchanged).
 *
 * One wide MMIO claim covering the whole PORT_BASE/IBC_BASE cluster (well
 * over a hundred registers) with an internal address decode, exactly
 * mirroring the Python module's own `_by_addr` table -- see
 * `gpio_group_table[]` below for the same (prefix, offset, valid-port-
 * range) rows as gpio.py's `_PLAIN_16`/`_SET_CLEAR_32`/`_IBC_PLAIN_16`.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HGpioState, RZA1H_GPIO)

#define NUM_PORTS 12 /* ports 0..11 */

typedef enum {
    G_NONE,
    G_P, G_PSR, G_PPR, G_PM, G_PMC, G_PFC, G_PFCE, G_PNOT, G_PMSR, G_PMCSR,
    G_PFCAE, G_SNCR, G_PIBC, G_PBDC, G_PIPC,
} RegGroup;

struct GpioGroupRow {
    RegGroup group;
    hwaddr base_offset;   /* relative to this device's own MMIO base (PORT_BASE) */
    int port_lo, port_hi; /* inclusive; SNCR uses lo=hi=-1 (no port index) */
};

/* Same rows as gpio.py's _PLAIN_16/_SET_CLEAR_32 (PORT_BASE-relative) and
 * _IBC_PLAIN_16 (IBC_BASE-relative, offset by RZA1H_GPIO_IBC_REL_OFFSET
 * here since this device covers both clusters in one MemoryRegion). */
static const struct GpioGroupRow gpio_group_table[] = {
    { G_P,     0x000, 1, 11 },
    { G_PSR,   0x100, 1, 11 },
    { G_PPR,   0x200, 0, 11 },
    { G_PM,    0x300, 1, 11 },
    { G_PMC,   0x400, 0, 11 },
    { G_PFC,   0x500, 1, 11 },
    { G_PFCE,  0x600, 1, 11 },
    { G_PNOT,  0x700, 1, 11 },
    { G_PMSR,  0x800, 1, 11 },
    { G_PMCSR, 0x900, 0, 11 },
    { G_PFCAE, 0xA00, 1, 11 },
    { G_SNCR,  0xC00, -1, -1 },
    { G_PIBC,  RZA1H_GPIO_IBC_REL_OFFSET + 0x000, 0, 11 },
    { G_PBDC,  RZA1H_GPIO_IBC_REL_OFFSET + 0x100, 1, 11 },
    { G_PIPC,  RZA1H_GPIO_IBC_REL_OFFSET + 0x200, 1, 11 },
};

struct RZA1HGpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint16_t p[NUM_PORTS];         /* P: output data latch */
    uint16_t pm[NUM_PORTS];        /* PM: direction, reset all-input (0xffff) */
    uint16_t pmc[NUM_PORTS];       /* PMC: GPIO vs. alternate function */
    uint16_t pin_level[NUM_PORTS]; /* PPR: simulated real pin level, read-only */
    /* Fallback plain storage for every other register (PFC/PFCE/PFCAE/
     * PBDC/PIPC/SNCR) -- keyed by raw offset within this device, like
     * gpio.py's self._storage dict keyed by absolute address. */
    uint8_t storage[RZA1H_GPIO_SIZE];

    uint32_t sr_shift;             /* P2 74AHC595 serial stream, see gpio_p2_update */
    uint16_t p2_prev;

    /* INTC external-IRQ front-end (2026-09-20) -- a genuinely separate small register
     * block/MMIO region from the port registers above, see its own ops' plate comment. */
    MemoryRegion iomem_extirq;
    qemu_irq irq7;
    qemu_irq irq3;
    uint16_t icr0, icr1;
    uint16_t irqrr;
};

/* --- INTC external-IRQ front-end (2026-09-20) ---
 * A small, separate register block for this real IC-7300 firmware's own external-IRQ
 * sense/pending logic (ICR1 sense-select, IRQRR pending/ack) -- confirmed via direct
 * decompile (pwrk_irq7_config_init/pwrk_irq7_isr, notes/ic7300-signal-chain.md's "PWRK
 * handler located" section) to sit at a DIFFERENT base (0xFCFEF800, derived directly
 * from the real pointer values those functions use) than the generic Renesas reference
 * package's own documented `INTC` struct base (0xE8201000 -- this project's already-
 * modeled GIC distributor's own base). A real, confirmed divergence from the generic
 * sample header, not a mistake -- this real firmware's own ICR1/IRQRR pointers land 2
 * bytes apart (0xFCFEF802/0xFCFEF804) exactly matching the reference struct's own
 * *field* layout, just at a different base address.
 *
 * Two lines modeled: IRQ7 (PWRK, GIC ID 39) and, added 2026-09-20, IRQ3 (GIC ID 35).
 * Live-checked `GICD_ICFGR2`: both IDs are left at the `arm_gic` default (level-triggered,
 * `0b00`) -- real firmware never touches them -- so this front-end must itself hold the
 * GIC-facing line asserted (a real, software-clearable latch, exactly like `riic.c`'s
 * `SR2`/`mtu2.c`'s `TSR`) rather than pulse it, matching the standard Renesas architecture
 * where the chip-specific IRQ front-end does its own edge detection and presents a plain
 * level to the downstream ARM-architected distributor, cleared by writing 0 to the
 * front-end's own pending-flag bit.
 *
 * **CORRECTED, same day, after a fresh-eyes review caught a real bit-arithmetic error**: the
 * `ICR1` value both `pwrk_irq7_config_init` and IRQ3's own config function (`FUN_20010ea8`)
 * program is bit-`N`0S=1/bit-`N`1S=0 (bit14=1/bit15=0 for IRQ7, bit6=1/bit7=0 for IRQ3) --
 * with `IRQnS0` as the LSB of the 2-bit sense field, that value is binary `01`, not `10` as
 * first (wrongly) read here. Cross-checked directly against the vendored Renesas reference
 * driver already in this repo (`scratch/r01an5093ej0170-rza1-swpkg/.../r_switch_driver.c`),
 * which sets the *identical* bit for its own `IRQ3`/`IRQ5` with an explicit comment: `// IRQ3
 * config = '01' = Falling Edge`. **Both lines are falling-edge, not rising-edge** -- since
 * PWRK is active-low, `IRQ7` genuinely fires on the *press* (high-to-low), not the release.
 * This also means `pwrk_irq7_isr` is not purely a "release/re-press detector" as first
 * described -- it is the *only* place `press_active` (the flag whose value at
 * `power_state_pwrk_wait_and_bringup`'s own finalize check decides whether bring-up
 * completes) is set based on hold duration: if `P1_7` is still low for the ISR's own full
 * ~90ms debounce window (it enters on the press, so this is testing a held press, not a
 * bounce), `press_active` is cleared to 0 -- an entirely ordinary "hold the power key long
 * enough" completion path, independent of `civ_state`/`IRQ3` entirely. See
 * qemu-machine/README.md's PWRK Status sections for the full correction and re-test.
 *
 * IRQ3's own real handler (`FUN_20186a58`, confirmed via its real `register_event_handler`-
 * equivalent call site) is the gate `power_state_pwrk_wait_and_bringup`'s own CI-V/SCIF1
 * servicing sub-loop needs (`civ_state` from `2` to `3`) -- a real, separate CI-V privilege-
 * tier mechanism, not part of the ordinary power-on completion path above. The real pin is
 * now CONFIRMED, not just the most likely of several candidates: `FUN_20010ea8` (IRQ3's own
 * config function, structurally identical to `pwrk_irq7_config_init`) does the same 6-
 * register port-mux septet write, at the identical `0xFCFE7100` (PBDC) base, at port 7's own
 * offset (`+0x1c`), bit `0x800` (bit 11) -- **`P7_11`**, matching `notes/
 * ic7300-signal-chain.md`'s `CRXD`/`CBSY` label exactly, and called from
 * `power_state_dispatch` (`0x2002b29c`) itself, before either boot branch splits off. */
#define INTC_EXT_ICR0  0x0
#define INTC_EXT_ICR1  0x2
#define INTC_EXT_IRQRR 0x4
#define INTC_EXT_IRQ3F 0x08
#define INTC_EXT_IRQ7F 0x80

static uint64_t rza1h_gpio_extirq_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HGpioState *s = RZA1H_GPIO(opaque);

    switch (offset) {
    case INTC_EXT_ICR0: return s->icr0;
    case INTC_EXT_ICR1: return s->icr1;
    case INTC_EXT_IRQRR: return s->irqrr;
    default: return 0;
    }
}

static void rza1h_gpio_extirq_write(void *opaque, hwaddr offset, uint64_t value,
                                    unsigned size)
{
    RZA1HGpioState *s = RZA1H_GPIO(opaque);

    switch (offset) {
    case INTC_EXT_ICR0: s->icr0 = value; break;
    case INTC_EXT_ICR1: s->icr1 = value; break;
    case INTC_EXT_IRQRR: {
        /* Real hardware: write 0 to a bit clears that flag, write 1 is a no-op -- same
         * AND-style convention already established for riic.c's SR2/mtu2.c's TSR. */
        uint16_t cleared = s->irqrr & ~(uint16_t)value;

        s->irqrr &= value;
        if (cleared & INTC_EXT_IRQ7F) {
            qemu_irq_lower(s->irq7);
        }
        if (cleared & INTC_EXT_IRQ3F) {
            qemu_irq_lower(s->irq3);
        }
        break;
    }
    default: break;
    }
}

static const MemoryRegionOps rza1h_gpio_extirq_ops = {
    .read = rza1h_gpio_extirq_read,
    .write = rza1h_gpio_extirq_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t load_le(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static void store_le(uint8_t *p, unsigned size, uint64_t value)
{
    for (unsigned i = 0; i < size; i++) {
        p[i] = (value >> (8 * i)) & 0xff;
    }
}

static bool gpio_decode(hwaddr offset, RegGroup *group, int *port)
{
    for (size_t i = 0; i < ARRAY_SIZE(gpio_group_table); i++) {
        const struct GpioGroupRow *row = &gpio_group_table[i];

        if (row->port_lo < 0) { /* SNCR: single register, no port index */
            if (offset == row->base_offset) {
                *group = row->group;
                *port = -1;
                return true;
            }
            continue;
        }
        if (offset < row->base_offset) {
            continue;
        }
        hwaddr rel = offset - row->base_offset;
        if (rel % 4 != 0) {
            continue;
        }
        int n = rel / 4;
        if (n >= row->port_lo && n <= row->port_hi) {
            *group = row->group;
            *port = n;
            return true;
        }
    }
    *group = G_NONE;
    return false;
}

/* SSIF word-select inputs (2026-09-24, audio link, notes/dsp-protocol.md): P2_9 = SSIWS0 and
 * P3_5 = SSIF1's WS, driven by the (FPGA's) 96 kHz I2S frame clock. SSIF bring-up
 * (FUN_200605fc) counts 10 edges on each under a 1 ms timeout and only enables its audio pumps
 * if they arrive. A square wave on the virtual clock, no timer. Nothing else reads these pins. */
#define SSIF_FRAME_HZ 96000
static uint16_t ssif_ws_pins(int port)
{
    bool level;

    static int off = -1;

    if (off < 0) {
        const char *e = getenv("RZA1H_SSIF");
        off = e && !strcmp(e, "off");   /* keep the pins still: audio never starts */
    }
    if (off || (port != 2 && port != 3)) {
        return 0;
    }
    level = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 2 * SSIF_FRAME_HZ /
             NANOSECONDS_PER_SECOND) & 1;
    return level ? (port == 2 ? 0x200 : 0x20) : 0;
}

static uint64_t rza1h_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HGpioState *s = RZA1H_GPIO(opaque);
    RegGroup group;
    int port;

    if (!gpio_decode(offset, &group, &port)) {
        return 0; /* unassigned register within this block's claimed range */
    }

    switch (group) {
    case G_P:
        return s->p[port];
    case G_PPR:
        return s->pin_level[port] | ssif_ws_pins(port);
    case G_PM:
        return s->pm[port];
    case G_PMC:
        return s->pmc[port];
    /* Set/clear registers: reading back isn't independently confirmed to
     * mean anything in particular, so mirror the underlying target
     * register, same as gpio.py's _read_named(). */
    case G_PSR:
        return s->p[port];
    case G_PMSR:
        return s->pm[port];
    case G_PMCSR:
        return s->pmc[port];
    default:
        return load_le(&s->storage[offset], size);
    }
}


/*
 * Port-2 serial shift-register bus (2026-09-23, from the user's schematic
 * reading): MDAT (P2_0) = SI, MCK (P2_1) = SCK, DRESH (P2_7) = /G, latch
 * strobes MSTB1 (P2_2) -> IC1301-IC1303 (3x SN74AHC595 in series, RF-unit
 * band-pass filters etc.), PSTB (P2_5) -> IC751 (PA-unit LPF relays), plus
 * MSTB2 (P2_3) / DSTB (P2_4) to further, not yet identified latches. The
 * firmware streams it by DMA (FUN_200b5dc0 -> FUN_200b5c60 writes one PSR2
 * word per half clock, MSB first). Nothing downstream is modeled; each latch
 * is decoded into the schematic signal names and logged (RZA1H_DEBUG=sr595).
 */
static const char *const sr_mstb1_names[24] = {
    "B0S", "B1S", "B2S", "B3S", "B4S", "B5S", "B6S", "B7S",
    "B8S", "B9S", "B10S", "B11S", "B11TXS", "B12S", "B12TXS", NULL,
    "NSBS", "AMS", "HPR2S", "HPR1S", "HPOFS", "HATTS", "HATOFS", NULL,
};
static const char *const sr_pstb_names[8] = {
    "LPF0.03-2", "LPF2-4", "LPF4-7.3", "LPF7.3-14.35", "LPF14.35-21.45",
    "LPF21.45-33", "LPF33-76", NULL,
};

static void gpio_sr_log(const char *strobe, uint32_t v, int nbits,
                        const char *const *names)
{
    char line[256] = "";

    for (int i = 0; names && i < nbits; i++) {
        if ((v >> i & 1) && names[i]) {
            g_strlcat(line, " ", sizeof(line));
            g_strlcat(line, names[i], sizeof(line));
        }
    }
    rza1h_debug("sr595", "%s latch %0*x:%s", strobe, (nbits + 3) / 4,
                v & (uint32_t)((1ull << nbits) - 1), line);
}

static void gpio_p2_update(RZA1HGpioState *s)
{
    uint16_t now = s->p[2], rise = now & ~s->p2_prev;

    if (rise & (1 << 1)) {                  /* MCK: shift in MDAT */
        s->sr_shift = s->sr_shift << 1 | (now & 1);
    }
    if (rise & (1 << 2)) {
        gpio_sr_log("MSTB1(IC1301-1303)", s->sr_shift, 24, sr_mstb1_names);
    }
    if (rise & (1 << 3)) {
        gpio_sr_log("MSTB2", s->sr_shift, 32, NULL);
    }
    if (rise & (1 << 4)) {
        gpio_sr_log("DSTB", s->sr_shift, 32, NULL);
    }
    if (rise & (1 << 5)) {
        gpio_sr_log("PSTB(IC751)", s->sr_shift, 8, sr_pstb_names);
    }
    s->p2_prev = now;
}

static void rza1h_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RZA1HGpioState *s = RZA1H_GPIO(opaque);
    RegGroup group;
    int port;
    uint16_t set_bits, clear_bits;

    if (!gpio_decode(offset, &group, &port)) {
        return;
    }
    if (port == 2 && (group == G_P || group == G_PSR || group == G_PNOT)) {
        rza1h_debug("gpio2", "P2 %s <- %08x (P2 was %04x)",
                    group == G_P ? "P" : group == G_PSR ? "PSR" : "PNOT",
                    (uint32_t)value, s->p[2]);
    }

    switch (group) {
    case G_P:
        s->p[port] = value & 0xffff;
        break;
    case G_PPR:
        break; /* read-only real pin level; ignore writes */
    case G_PM:
        s->pm[port] = value & 0xffff;
        break;
    case G_PMC:
        s->pmc[port] = value & 0xffff;
        break;
    case G_PNOT:
        s->p[port] ^= value & 0xffff;
        break;
    /* Masked set/clear: lower 16 bits = bits to set, upper 16 = bits to
     * clear -- see this file's own module docstring / gpio.py's for the
     * "not independently confirmed" caveat on this inference. */
    case G_PSR:
        /* High half = per-bit write enable, low half = data (2026-09-23
         * correction: the old "low = set, high = clear" reading turned
         * the firmware's "drive bit n high" (E=D=1<<n) into a no-op). */
        clear_bits = (value >> 16) & 0xffff;
        set_bits = value & clear_bits;
        s->p[port] = (s->p[port] & ~clear_bits) | set_bits;
        break;
    case G_PMSR:
        /* High half = per-bit write enable, low half = data (2026-09-23
         * correction: the old "low = set, high = clear" reading turned
         * the firmware's "drive bit n high" (E=D=1<<n) into a no-op). */
        clear_bits = (value >> 16) & 0xffff;
        set_bits = value & clear_bits;
        s->pm[port] = (s->pm[port] & ~clear_bits) | set_bits;
        break;
    case G_PMCSR:
        /* High half = per-bit write enable, low half = data (2026-09-23
         * correction: the old "low = set, high = clear" reading turned
         * the firmware's "drive bit n high" (E=D=1<<n) into a no-op). */
        clear_bits = (value >> 16) & 0xffff;
        set_bits = value & clear_bits;
        s->pmc[port] = (s->pmc[port] & ~clear_bits) | set_bits;
        break;
    default:
        store_le(&s->storage[offset], size, value);
        break;
    }
    if (port == 2) {
        gpio_p2_update(s);
    }
}

static const MemoryRegionOps rza1h_gpio_ops = {
    .read = rza1h_gpio_read,
    .write = rza1h_gpio_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_gpio_reset(DeviceState *dev)
{
    RZA1HGpioState *s = RZA1H_GPIO(dev);

    memset(s->p, 0, sizeof(s->p));
    for (int i = 0; i < NUM_PORTS; i++) {
        s->pm[i] = 0xffff; /* reset: all input */
    }
    memset(s->pmc, 0, sizeof(s->pmc));
    memset(s->pin_level, 0, sizeof(s->pin_level));
    memset(s->storage, 0, sizeof(s->storage));

    /* P1_6 ("PDV") is IC361's (NJU7704F3) real power-fail/brownout
     * detector output wired directly into the CPU -- confirmed low ==
     * brownout, high == supply healthy (notes/firmware-update.md's
     * main_idle_loop watchdog-reset trace, and independently
     * cross-confirmed 2026-09-08 tracing FUN_2002b29c's own cold-boot
     * branch decision -- both read this exact bit and both need it high
     * for the normal/no-fault path). A real board's supply is healthy by
     * the time firmware runs at all; defaulting every unmodeled GPIO
     * input to 0 (this device's general, otherwise-correct convention)
     * would read as a permanent brownout condition here, which no real
     * boot ever observes -- so this one pin gets a real, justified
     * exception rather than staying at the generic default. */
    s->pin_level[1] |= 0x40;

    /* P8_9 ("HSK1") is the DSP's real hardware ready/handshake line, polled by
     * `scif5_wait_hsk1_ready` right after `scif5_dsp_link_driver_init` during cold boot, and
     * separately by the firmware-update DSP/Front-CPU chunk-transfer path
     * (`notes/ic7300-signal-chain.md`'s 27th session traced both) -- two independent call sites
     * agreeing this is a real ready signal, not a guess. A real DSP is presumably already
     * powered and asserting this line well before the main CPU's boot reaches either check, so
     * (like `P1_6` above) this pin gets a justified exception instead of the generic all-zero
     * default -- without it, `scif5_wait_hsk1_ready` never sees it ready and busy-waits its full
     * ~16-minute software timeout instead (see qemu-machine/README.md's Status section). */
    s->pin_level[8] |= 0x200;
    s->pin_level[8] |= 0x80;   /* P8_7 RTD idles at mark (see rza1h_gpio_set_rtd) */

    /* P1_7 ("PWRK") is the front-panel power key -- a plain pull-up + switch-to-ground button,
     * confirmed active-low (2026-09-20, see notes/ic7300-signal-chain.md's "PWRK handler
     * located" section and power_state_pwrk_wait_and_bringup's own busy-wait, 0x20029918:
     * `while ((PPR1 & 0x80) != 0) ...`). Idle/unpressed reads high on real hardware; defaulting
     * to the generic all-zero convention here would read as "already pressed" from the very
     * first check, letting that wait loop exit trivially at t=0 instead of genuinely waiting --
     * same justified-exception reasoning as P1_6/P8_9 above. See rza1h_gpio_set_pwrk_pressed()
     * for how to actually simulate a live press for testing. */
    s->pin_level[1] |= 0x80;

    /* P7_11 ("CRXD"/"CBSY", the leading IRQ3 candidate -- see the extirq front-end's own
     * plate comment) idles high/not-busy by default, same "unmodeled real input defaults
     * to its real idle level, not the generic all-zero convention" reasoning as P1_6/
     * P8_9/P1_7 above. See rza1h_gpio_set_civ_bus_busy() for how to simulate it going busy. */
    s->pin_level[7] |= 0x800;

    s->icr0 = s->icr1 = 0;
    s->irqrr = 0;
    qemu_irq_lower(s->irq7);
    qemu_irq_lower(s->irq3);
}

/* Live PWRK press/release for testing (2026-09-20) -- not a real hardware input this project
 * has any other way to drive, and this project's own PPR write handler correctly ignores guest
 * writes (PPR is real-hardware read-only), so a host-side QOM property is the only way to
 * simulate a user physically pressing the button while a boot is running. Toggle via QMP:
 * `qom-set /machine/unattached/device[N] pwrk-pressed true` (find N via `qom-list`/
 * `info qom-tree`), or `-global rza1h-gpio.pwrk-pressed=on` to start already pressed. */
static bool rza1h_gpio_get_pwrk_pressed(Object *obj, Error **errp)
{
    RZA1HGpioState *s = RZA1H_GPIO(obj);

    return (s->pin_level[1] & 0x80) == 0;
}

static void rza1h_gpio_set_pwrk_pressed(Object *obj, bool pressed, Error **errp)
{
    RZA1HGpioState *s = RZA1H_GPIO(obj);
    bool was_pressed = (s->pin_level[1] & 0x80) == 0;

    if (pressed) {
        s->pin_level[1] &= ~0x80; /* pulled low -- pressed */
        if (!was_pressed) {
            /* Falling edge (high -> low) -- see the INTC external-IRQ front-end's own
             * plate comment for the corrected ICR1 polarity: real firmware's ICR1
             * configuration fires IRQ7 on the PRESS, not the release. */
            s->irqrr |= INTC_EXT_IRQ7F;
            qemu_irq_raise(s->irq7);
        }
    } else {
        s->pin_level[1] |= 0x80; /* released -- idle high */
    }
}

/* Live CI-V-bus-busy assertion for testing (2026-09-20) -- the IRQ3 counterpart to
 * rza1h_gpio_set_pwrk_pressed() above, same rationale (no other host-side way to simulate
 * this line changing while a boot is running). `true` pulls P7_11 low (bus asserted busy),
 * raising IRQ3 on that falling edge -- see the extirq front-end's own plate comment for the
 * corrected ICR1 polarity (both IRQ7/IRQ3 are falling-edge, not rising). `false` releases it
 * back to idle-high with no interrupt. Toggle via QMP `qom-set ... civ-bus-busy true` (then
 * `false`), or `-global rza1h-gpio.civ-bus-busy=on` to start already busy. */
static bool rza1h_gpio_get_civ_bus_busy(Object *obj, Error **errp)
{
    RZA1HGpioState *s = RZA1H_GPIO(obj);

    return (s->pin_level[7] & 0x800) == 0;
}

static void rza1h_gpio_set_civ_bus_busy(Object *obj, bool busy, Error **errp)
{
    RZA1HGpioState *s = RZA1H_GPIO(obj);
    bool was_busy = (s->pin_level[7] & 0x800) == 0;

    if (busy) {
        s->pin_level[7] &= ~0x800;
        if (!was_busy) {
            s->irqrr |= INTC_EXT_IRQ3F;
            qemu_irq_raise(s->irq3);
        }
    } else {
        s->pin_level[7] |= 0x800;
    }
}

/* P8_7 = RTD, the DSP's RTTY receive data (demodulated FSK mark/space, 1 = mark), driven by
 * ssif.c's fake-DSP demodulator through the "rtd" GPIO line; sampled by the firmware at 1 kHz
 * from MTU2 TGI1A (rtty_rx_tgi1a_sample_rtd, 0x200b0ad8). See notes/dsp-protocol.md. */
static void rza1h_gpio_set_rtd(void *opaque, int n, int level)
{
    RZA1HGpioState *s = RZA1H_GPIO(opaque);

    if (level) {
        s->pin_level[8] |= 0x80;
    } else {
        s->pin_level[8] &= ~0x80;
    }
}

static void rza1h_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HGpioState *s = RZA1H_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_gpio_ops, s,
                          TYPE_RZA1H_GPIO, RZA1H_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    memory_region_init_io(&s->iomem_extirq, obj, &rza1h_gpio_extirq_ops, s,
                          TYPE_RZA1H_GPIO ".extirq", RZA1H_INTC_EXT_SIZE);
    sysbus_init_mmio(sbd, &s->iomem_extirq);
    sysbus_init_irq(sbd, &s->irq7);
    sysbus_init_irq(sbd, &s->irq3);
    qdev_init_gpio_in_named(DEVICE(obj), rza1h_gpio_set_rtd, "rtd", 1);

    object_property_add_bool(obj, "pwrk-pressed", rza1h_gpio_get_pwrk_pressed,
                             rza1h_gpio_set_pwrk_pressed);
    object_property_add_bool(obj, "civ-bus-busy", rza1h_gpio_get_civ_bus_busy,
                             rza1h_gpio_set_civ_bus_busy);
}

static void rza1h_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rza1h_gpio_reset);
}

static const TypeInfo rza1h_gpio_info = {
    .name          = TYPE_RZA1H_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HGpioState),
    .instance_init = rza1h_gpio_init,
    .class_init    = rza1h_gpio_class_init,
};

static void rza1h_gpio_register_types(void)
{
    type_register_static(&rza1h_gpio_info);
}

type_init(rza1h_gpio_register_types)
