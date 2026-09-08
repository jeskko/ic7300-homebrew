/*
 * RZ/A1H GPIO/port register block -- direct C port of
 * emu/peripherals/gpio.py. See that module's own docstring for the full
 * derivation (register offsets/roles from ~/Downloads/rza1.svd and direct
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
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"

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
        return s->pin_level[port];
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
        set_bits = value & 0xffff;
        clear_bits = (value >> 16) & 0xffff;
        s->p[port] = (s->p[port] | set_bits) & ~clear_bits;
        break;
    case G_PMSR:
        set_bits = value & 0xffff;
        clear_bits = (value >> 16) & 0xffff;
        s->pm[port] = (s->pm[port] | set_bits) & ~clear_bits;
        break;
    case G_PMCSR:
        set_bits = value & 0xffff;
        clear_bits = (value >> 16) & 0xffff;
        s->pmc[port] = (s->pmc[port] | set_bits) & ~clear_bits;
        break;
    default:
        store_le(&s->storage[offset], size, value);
        break;
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
}

static void rza1h_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HGpioState *s = RZA1H_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_gpio_ops, s,
                          TYPE_RZA1H_GPIO, RZA1H_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
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
