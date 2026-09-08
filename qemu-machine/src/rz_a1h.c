/*
 * IC-7300 / Renesas RZ/A1H research machine.
 *
 * A from-scratch QEMU machine for a SoC QEMU has no board support for --
 * see qemu-machine/README.md for why this exists (the Unicorn-based
 * emulator in emu/ hit a real engine limitation: no correctly-delivered
 * asynchronous interrupt injection) and what's deliberately deferred out of
 * this first slice (every peripheral except the GIC and one real timer
 * falls through to QEMU's own generic "unimplemented-device", exactly
 * mirroring emu/peripherals/stub.py's role in the Unicorn version).
 *
 * Deliberately does *not* use hw/arm/boot.c's arm_load_kernel() -- that
 * machinery is Linux-boot-oriented (kernel image format detection, device
 * tree, ATAGs), none of which applies here. This SoC's real firmware is a
 * bare flat binary executed directly from flash at a fixed physical
 * address (notes/base-loader.md's fully-traced boot-mode-3 XIP
 * convention), so this machine loads that image as ROM and manually
 * overrides the CPU's reset PC -- the same pattern hw/arm/boot.c's own
 * do_cpu_reset() uses internally for its own non-Linux ("raw") boot case,
 * just without needing any of its surrounding kernel-image machinery.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gic.h"
#include "hw/misc/unimp.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/reset.h"
#include "target/arm/cpu-qom.h"

#include "rz_a1h.h"

/* Every I/O region notes/memory-map.md documents, mirroring
 * emu/board.py's IO_REGIONS list exactly (same names, same comments on
 * where each one came from) -- covered generically by QEMU's own
 * unimplemented-device until ported to a real device, same role
 * emu/peripherals/stub.py's StubPeripheral plays in the Unicorn version. */
struct RzA1hIoRegion {
    const char *name;
    hwaddr base;
    hwaddr size;
};

static const struct RzA1hIoRegion rza1h_io_regions[] = {
    { "spi-status-and-neighbors", 0x3FEFA000, 0x00002000 },
    { "boot-gpio-pokes",          0x3FFFC000, 0x00004000 },
    { "io-e8000000",              0xE8000000, 0x00020000 },
    { "io-e8030000",              0xE8030000, 0x00020000 },
    { "io-e8100000",              0xE8100000, 0x00040000 },
    { "io-e8200000",              0xE8200000, 0x00030000 },
    { "io-fc000000",              0xFC000000, 0x00080000 },
    { "io-fcfe0000",              0xFCFE0000, 0x00020000 },
    { "io-ffff0000",              0xFFFF0000, 0x00010000 },
};

static void rza1h_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    /* Real hardware's boot-mode-3 SPI-flash XIP entry point -- confirmed
     * ground truth, see notes/base-loader.md and RZA1H_FLASH_BASE's own
     * comment in rz_a1h.h. */
    cpu_set_pc(cs, RZA1H_FLASH_BASE);
}

static void rza1h_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    ARMCPU *cpu;
    DeviceState *gic;
    DeviceState *ostm0;
    DeviceState *ostm1;
    DeviceState *spi_boot_status;
    size_t i;

    cpu = ARM_CPU(cpu_create(machine->cpu_type));

    memory_region_add_subregion(sysmem, RZA1H_RAM_BASE, machine->ram);

    /* rom_add_file_fixed() below only *registers* the blob to be copied in
     * at reset time -- it does not create the backing memory region, so a
     * real region has to exist here first or that copy has nowhere to land
     * (confirmed the hard way: without this, the CPU's very first
     * instruction fetch at RZA1H_FLASH_BASE faults immediately, since
     * nothing is mapped there at all). */
    {
        MemoryRegion *flash = g_new(MemoryRegion, 1);
        memory_region_init_rom(flash, NULL, "rza1h.flash", RZA1H_FLASH_SIZE,
                               &error_fatal);
        memory_region_add_subregion(sysmem, RZA1H_FLASH_BASE, flash);
    }

    if (!machine->kernel_filename) {
        error_report("use -kernel to point at a flat flash image built by "
                     "qemu-machine/tools/build_flash.py");
        exit(1);
    }
    if (rom_add_file_fixed(machine->kernel_filename, RZA1H_FLASH_BASE, -1) < 0) {
        error_report("could not load flash image '%s'", machine->kernel_filename);
        exit(1);
    }

    /* Real ARM GIC (PL390-family), placed at the real hardware addresses --
     * see rz_a1h.h. Not the MPCore wrapper (this SoC is single-core, and
     * the real hardware exposes the distributor/CPU-interface directly at
     * these addresses, not behind an intermediate container). */
    gic = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(gic, "num-cpu", 1);
    qdev_prop_set_uint32(gic, "num-irq", RZA1H_GIC_NUM_IRQ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, RZA1H_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 1, RZA1H_GIC_CPU_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gic), 0,
                       qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));

    /* OSTM0 -- the one real timer this slice exists to validate. Interrupt
     * ID 134 is Renesas's own reference HAL's INTC_ID_OSTM0TINT, confirmed
     * to be an absolute GIC interrupt ID -- see ostm.c's own file comment
     * for the full derivation. `qdev_get_gpio_in(gic, N)`'s N is *not*
     * that absolute ID, though -- confirmed 2026-09-08 by cross-checking
     * hw/arm/fsl-imx6.c's own IRQ #defines (e.g. FSL_IMX6_UART1_IRQ used
     * directly as a gpio-in index) against arm_gic's own gic_set_irq():
     * external gpio-in index 0 maps to absolute interrupt ID GIC_INTERNAL
     * (32, the first SPI) -- i.e. gpio-in index = absolute ID - 32. Wiring
     * `qdev_get_gpio_in(gic, 134)` directly (this code's first version)
     * silently wired OSTM0's line to absolute ID 166, not 134 -- confirmed
     * the hard way: GICD_ISPENDR4 (covering IDs 128-159) never showed the
     * bit set no matter how the distributor/CPU-interface/ICFGR were
     * configured via a manual GDB-driven test (qemu-machine/tools/
     * test_irq.py), because the line was never reaching an interrupt
     * inside that window at all. */
    ostm0 = qdev_new(TYPE_RZA1H_OSTM);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ostm0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ostm0), 0, RZA1H_OSTM0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ostm0), 0,
                       qdev_get_gpio_in(gic, 134 - RZA1H_GIC_NUM_INTERNAL));

    /* OSTM1 -- found needing this same session: base.dat's GPIO-pin-settle
     * routine (FUN_2002b878, see emu/peripherals/gpio.py's docstring for
     * the Unicorn-side trace of this exact function) busy-waits on OSTM1's
     * CNT register as a timeout guard. Interrupt ID 135 is Renesas's
     * INTC_ID_OSTM1TINT, wired for symmetry with OSTM0 though this
     * particular caller only polls CNT and never takes the interrupt. */
    ostm1 = qdev_new(TYPE_RZA1H_OSTM);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ostm1), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ostm1), 0, RZA1H_OSTM1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ostm1), 0,
                       qdev_get_gpio_in(gic, 135 - RZA1H_GIC_NUM_INTERNAL));

    for (i = 0; i < ARRAY_SIZE(rza1h_io_regions); i++) {
        const struct RzA1hIoRegion *r = &rza1h_io_regions[i];
        create_unimplemented_device(r->name, r->base, r->size);
    }

    /* Mapped on top of the "spi-status-and-neighbors" unimplemented-device
     * region above (unimplemented-device maps at priority -1000, so a
     * normal-priority mapping here wins) -- see spi_boot.c's own comment
     * for why this can't just fall through to the generic stub. */
    spi_boot_status = qdev_new(TYPE_RZA1H_SPI_BOOT_STATUS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi_boot_status), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(spi_boot_status), 0,
                            RZA1H_SPI_BOOT_STATUS_BASE, 0);

    qemu_register_reset(rza1h_cpu_reset, cpu);
}

static void rza1h_machine_init(MachineClass *mc)
{
    mc->desc = "IC-7300 / Renesas RZ/A1H (research, incomplete)";
    mc->init = rza1h_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->default_ram_size = RZA1H_RAM_SIZE;
    mc->default_ram_id = "rza1h.ram";
}

/* DEFINE_MACHINE_ARM (not the plain DEFINE_MACHINE) -- confirmed necessary
 * by directly tracing why a plain DEFINE_MACHINE-registered "rz-a1h-machine"
 * QOM type was real (present in `qom-list-types implements=machine`) but
 * invisible to `-M help`/`-M rz-a1h`: qemu-system-arm's machine lookup
 * (system/vl.c's find_machine(), via target_machine_typename()) walks
 * TYPE_TARGET_ARM_MACHINE specifically, not plain TYPE_MACHINE -- every
 * real ARM board in this QEMU tree uses this same macro (or the older
 * manual TypeInfo + arm_machine_interfaces[] equivalent), never bare
 * DEFINE_MACHINE. See include/hw/arm/machines-qom.h. */
DEFINE_MACHINE_ARM("rz-a1h", rza1h_machine_init)
