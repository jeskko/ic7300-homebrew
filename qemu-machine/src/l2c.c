/*
 * ARM PL310-style L2 cache controller (RZ/A1H's `L2C`) -- direct C port of
 * emu/peripherals/l2c.py. See that module's own docstring for the full
 * derivation (register offsets from ~/Downloads/rza1.svd, the real
 * REG7_INV_WAY self-clearing bug found and fixed there first).
 *
 * Not a faithful cache model, same as the Python version: no actual
 * caching behavior, REG1_CONTROL's enable bit isn't backed by anything.
 * REG0_CACHE_ID/REG0_CACHE_TYPE return real architected PL310 constants
 * (a driver might plausibly branch on an identification read); every
 * REG7_* cache-maintenance-operation register reads back 0 unconditionally
 * (write triggers an operation that completes instantly, real hardware
 * just isn't instant) -- the fix for the emu/ session's real hang, ported
 * here so this device doesn't reproduce it. Everything else is plain
 * read/write storage.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HL2cState, RZA1H_L2C)

struct RZA1HL2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t storage[RZA1H_L2C_SIZE];
};

#define CACHE_ID_OFFSET   0x000
#define CACHE_TYPE_OFFSET 0x004
#define REG7_LO           0x700
#define REG7_HI           0x7ff

/* Real architected PL310 values (implementer ARM=0x41, PL310 part number),
 * matching l2c.py's own CACHE_ID/CACHE_TYPE constants exactly -- not
 * IC-7300-specific. */
#define CACHE_ID   0x410000C8
#define CACHE_TYPE 0x1C100100

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

static uint64_t rza1h_l2c_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HL2cState *s = RZA1H_L2C(opaque);

    if (offset == CACHE_ID_OFFSET) {
        return CACHE_ID;
    }
    if (offset == CACHE_TYPE_OFFSET) {
        return CACHE_TYPE;
    }
    if (offset >= REG7_LO && offset <= REG7_HI) {
        return 0; /* operation always "already complete" */
    }
    return load_le(&s->storage[offset], size);
}

static void rza1h_l2c_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RZA1HL2cState *s = RZA1H_L2C(opaque);

    if (offset == CACHE_ID_OFFSET || offset == CACHE_TYPE_OFFSET) {
        return; /* read-only */
    }
    if (offset >= REG7_LO && offset <= REG7_HI) {
        return; /* triggers an operation that completes instantly */
    }
    store_le(&s->storage[offset], size, value);
}

static const MemoryRegionOps rza1h_l2c_ops = {
    .read = rza1h_l2c_read,
    .write = rza1h_l2c_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_l2c_reset(DeviceState *dev)
{
    RZA1HL2cState *s = RZA1H_L2C(dev);

    memset(s->storage, 0, sizeof(s->storage));
}

static void rza1h_l2c_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HL2cState *s = RZA1H_L2C(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_l2c_ops, s,
                          TYPE_RZA1H_L2C, RZA1H_L2C_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rza1h_l2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rza1h_l2c_reset);
}

static const TypeInfo rza1h_l2c_info = {
    .name          = TYPE_RZA1H_L2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HL2cState),
    .instance_init = rza1h_l2c_init,
    .class_init    = rza1h_l2c_class_init,
};

static void rza1h_l2c_register_types(void)
{
    type_register_static(&rza1h_l2c_info);
}

type_init(rza1h_l2c_register_types)
