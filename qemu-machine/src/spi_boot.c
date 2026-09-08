/*
 * RZ/A1H SPI multi-I/O boot status register -- always "ready".
 *
 * The one real peripheral base.dat's boot code polls before reading flash
 * (notes/base-loader.md's boot-sequence step 2; address/bit confirmed by
 * direct disassembly of the real base.dat). Direct C equivalent of
 * emu/peripherals/spi_boot.py -- always reports bit 0 set, no real SPI
 * transaction timing modeled, matching that module's own reasoning: this is
 * sufficient to let the boot loop proceed, real hardware behavior isn't
 * needed to reach body.bin's entry point.
 *
 * Mapped at a small, high-priority region directly over one 4-byte address
 * within the broader "spi-status-and-neighbors" unimplemented-device range
 * in rz_a1h.c -- that catch-all would otherwise always read 0 here, which
 * would spin the boot loop's poll forever (the same failure shape as
 * l2c.py's REG7_INV_WAY hang in the Unicorn version, just here it would be
 * this project's very first QEMU boot attempt hitting it immediately).
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HSpiBootStatusState, RZA1H_SPI_BOOT_STATUS)

struct RZA1HSpiBootStatusState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
};

static uint64_t rza1h_spi_boot_status_read(void *opaque, hwaddr offset,
                                           unsigned size)
{
    return 1; /* bit 0 = ready */
}

static void rza1h_spi_boot_status_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    /* base.dat's boot-time poll loop never writes here; ignore if
     * something does. */
}

static const MemoryRegionOps rza1h_spi_boot_status_ops = {
    .read = rza1h_spi_boot_status_read,
    .write = rza1h_spi_boot_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_spi_boot_status_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HSpiBootStatusState *s = RZA1H_SPI_BOOT_STATUS(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_spi_boot_status_ops, s,
                          TYPE_RZA1H_SPI_BOOT_STATUS, 4);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const TypeInfo rza1h_spi_boot_status_info = {
    .name          = TYPE_RZA1H_SPI_BOOT_STATUS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HSpiBootStatusState),
    .instance_init = rza1h_spi_boot_status_init,
};

static void rza1h_spi_boot_status_register_types(void)
{
    type_register_static(&rza1h_spi_boot_status_info);
}

type_init(rza1h_spi_boot_status_register_types)
