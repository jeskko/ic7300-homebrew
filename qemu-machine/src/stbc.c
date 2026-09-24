/*
 * RZ/A1H standby request/acknowledge registers (power-down modes, manual chapter 55):
 * STBREQ1 0xFCFE0030, STBREQ2 0xFCFE0034, STBACK1 0xFCFE0040, STBACK2 0xFCFE0044.
 *
 * Before stopping a bus-master module, software sets its STBREQ bit and waits for the module's
 * STBACK bit ("ready for standby"). body.bin does this when it shuts the graphics stack down
 * on power-off (FUN_2007ebd0 via eglTerminate's teardown: STBREQ2 bit 0, then polls STBACK2
 * bit 0) -- the step the post-firmware-update restart waits behind (2026-09-25). Every modelled
 * module is always idle, so each STBACK bit simply follows its STBREQ bit. The rest of the
 * power-down block (STBCRn etc.) stays in rz_a1h.c's unimplemented catch-all.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HStbcState, RZA1H_STBC)

#define STBC_SIZE 0x18      /* 0x30..0x47, based at STBREQ1 */
#define STBREQ1   0x00
#define STBREQ2   0x04
#define STBACK1   0x10
#define STBACK2   0x14

struct RZA1HStbcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t req[2];
};

static uint64_t stbc_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HStbcState *s = RZA1H_STBC(opaque);

    switch (offset) {
    case STBREQ1: return s->req[0];
    case STBREQ2: return s->req[1];
    case STBACK1: return s->req[0];     /* every module acknowledges at once */
    case STBACK2: return s->req[1];
    default:      return 0;
    }
}

static void stbc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    RZA1HStbcState *s = RZA1H_STBC(opaque);

    if (offset == STBREQ1 || offset == STBREQ2) {
        s->req[offset / 4] = value;
        rza1h_debug("stbc", "STBREQ%d = %02x", (int)(offset / 4) + 1, (uint8_t)value);
    }
}

static const MemoryRegionOps stbc_ops = {
    .read = stbc_read,
    .write = stbc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void stbc_reset(DeviceState *dev)
{
    RZA1HStbcState *s = RZA1H_STBC(dev);

    s->req[0] = s->req[1] = 0;
}

static void stbc_init(Object *obj)
{
    RZA1HStbcState *s = RZA1H_STBC(obj);

    memory_region_init_io(&s->iomem, obj, &stbc_ops, s, TYPE_RZA1H_STBC, STBC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void stbc_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), stbc_reset);
}

static const TypeInfo stbc_info = {
    .name          = TYPE_RZA1H_STBC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HStbcState),
    .instance_init = stbc_init,
    .class_init    = stbc_class_init,
};

static void stbc_register_types(void)
{
    type_register_static(&stbc_info);
}

type_init(stbc_register_types)
