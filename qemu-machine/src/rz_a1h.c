/*
 * IC-7300 / Renesas RZ/A1H research machine.
 *
 * A from-scratch QEMU machine for a SoC QEMU has no board support for --
 * see qemu-machine/README.md for why this exists (the Unicorn-based
 * emulator in emu/ hit a real engine limitation: no correctly-delivered
 * asynchronous interrupt injection). The first slice modeled only the GIC
 * and two OSTM timers; 2026-09-08's second pass (after confirming real IRQ
 * delivery and finding body.bin's real tick source) added the rest of
 * emu/peripherals/'s modules -- GIC/OSTM0/OSTM1/GPIO/L2C/CPG/MTU2/RIIC0-2
 * are now real. Everything else still falls through to QEMU's own generic
 * "unimplemented-device", the same role emu/peripherals/stub.py plays in
 * the Unicorn version.
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
#include "system/system.h"
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

/* CPG (still, and RIIC before it got upgraded too -- see rz_a1h.h's own
 * comments) is, like its emu/peripherals/ Python original, plain read/write
 * storage with zero side effects modeled -- a bare RAM region is the
 * simplest and most direct C equivalent of "arbitrary read/write, no
 * behavior", not a simplification of anything that Python module actually
 * did. MTU2 was the same until 2026-09-09 (see mtu2.c). Needs the
 * `_overlap` variant since each of these ranges sits inside the broader
 * "io-fcfe0000" unimplemented-device catch-all mapped later in
 * rza1h_init() -- see spi_boot_status's own comment for why plain
 * memory_region_add_subregion() can't be used for a sub-range of an
 * already-mapped sibling region. */
static void add_plain_ram_region(MemoryRegion *sysmem, const char *name,
                                 hwaddr base, hwaddr size)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);

    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, base, mr, 0);
}

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
    DeviceState *gpio;
    DeviceState *l2c;
    DeviceState *mmc;
    DeviceState *mtu2;
    DeviceState *dmac;
    DeviceState *rspi2;
    size_t i;
    int ch;

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
    /* Real RZ/A1H silicon implements only 5 priority bits (ICDIPRn[7:3]), not QEMU arm_gic's
     * own default of 8 -- confirmed directly against Renesas' own sample driver
     * (scratch/r01an5093ej0170-rza1-swpkg/.../r_intc_configure.c, R_INTC_SetPriority()): its
     * own `priority` argument is documented range 0-31 and gets shifted left by 3 before the
     * real ICDIPRn write, with the driver's own comment stating outright "Priority[7:3] of
     * ICDIPRn is valid bit". Without this, arm_gic's own gic_fullprio_mask() (hw/intc/
     * arm_gic.c, read directly) leaves all 8 bits significant, so a firmware priority write
     * with nonzero low 3 bits (this project's own live capture found GICD_IPRIORITYR values
     * like SCIF5-TXI's 0x7f, whose low 3 bits are 0b111) is stored and compared verbatim here,
     * where real hardware would mask those bits to write-ignored on arrival (storing/reading
     * back 0x78, not 0x7f) -- confirmed correct behavior is already implemented in
     * gic_fullprio_mask()/gic_dist_set_priority(), just never enabled by this machine before
     * now. 2026-09-10, prompted by the user's own real-world fact (never observed the ring
     * overflow on real hardware) -- see README.md's Status section for the full derivation and
     * whether this changes the observed overflow (checked, not assumed). */
    qdev_prop_set_uint32(gic, "num-priority-bits", 5);
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

    /* Extension-roadmap item 3, 2026-09-08: port the remaining Unicorn-side
     * peripherals (emu/peripherals/{gpio,cpg,l2c,mtu2,riic}.py) to real
     * devices here, now that (1) real IRQ delivery and (2) body.bin's real
     * tick source are both confirmed working -- see README.md's Status
     * section. gpio.c/l2c.c carry real behavior (masked set/clear
     * registers, PL310 cache-ID/REG7 semantics) so they're dedicated
     * devices; cpg/riic were plain storage, covered by
     * add_plain_ram_region() (see its own comment) -- riic.c was later
     * upgraded to a real device the same session, and mtu2.c on
     * 2026-09-09 (channel 3 only, see its own comment). */
    gpio = qdev_new(TYPE_RZA1H_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(gpio), 0, RZA1H_GPIO_BASE, 0);

    /* INTC external-IRQ front-end (ICR1/IRQRR), 2026-09-20 -- a second, separate MMIO
     * region + IRQ output on the same gpio.c device (its own real hardware domain, the
     * P1_7/PWRK pin), added once live PWRK-press testing needed IRQ7 (GIC ID 39) to
     * actually reach the CPU instead of only changing a passively-read register -- see
     * gpio.c's own plate comment for the real, confirmed base-address derivation. */
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(gpio), 1, RZA1H_INTC_EXT_BASE, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                       qdev_get_gpio_in(gic,
                           RZA1H_EXT_IRQ7_IRQ - RZA1H_GIC_NUM_INTERNAL));
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 1,
                       qdev_get_gpio_in(gic,
                           RZA1H_EXT_IRQ3_IRQ - RZA1H_GIC_NUM_INTERNAL));

    /* l2c.c's own address (0x3ffff000) isn't inside any unimplemented-device
     * range above -- it was simply unmapped before this, so a plain (non-
     * overlap) mapping is correct and sufficient here. */
    l2c = qdev_new(TYPE_RZA1H_L2C);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(l2c), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(l2c), 0, RZA1H_L2C_BASE);

    add_plain_ram_region(sysmem, "rza1h.cpg-main",
                         RZA1H_CPG_MAIN_BASE, RZA1H_CPG_MAIN_SIZE);
    add_plain_ram_region(sysmem, "rza1h.cpg-deep-standby",
                         RZA1H_CPG_DEEP_STANDBY_BASE, RZA1H_CPG_DEEP_STANDBY_SIZE);

    /* mtu2.c -- upgraded from a bare RAM region to a real device, 2026-09-09,
     * once body.bin's own cold-boot task-readiness busy-wait was found
     * depending on a real periodic interrupt (GIC ID 154, TGI3A) that only
     * a real MTU2 channel 3 can produce -- see mtu2.c's own comment.
     * Channel 4's TGI4A (GIC ID 159) added the same session, once the
     * DSP-link handshake right after was found needing it too. TGI4B (GIC
     * ID 160) added 2026-09-20, once the RIIC2 ring-overflow fix (riic.c)
     * let boot reach the internal tuner relay-network's own cold-boot
     * init, which needs it -- see mtu2.c's own comment. */
    mtu2 = qdev_new(TYPE_RZA1H_MTU2);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mtu2), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(mtu2), 0, RZA1H_MTU2_BASE, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(mtu2), 0,
                       qdev_get_gpio_in(gic,
                           RZA1H_MTU2_TGI3A_IRQ - RZA1H_GIC_NUM_INTERNAL));
    sysbus_connect_irq(SYS_BUS_DEVICE(mtu2), 1,
                       qdev_get_gpio_in(gic,
                           RZA1H_MTU2_TGI4A_IRQ - RZA1H_GIC_NUM_INTERNAL));
    sysbus_connect_irq(SYS_BUS_DEVICE(mtu2), 2,
                       qdev_get_gpio_in(gic,
                           RZA1H_MTU2_TGI4B_IRQ - RZA1H_GIC_NUM_INTERNAL));
    sysbus_connect_irq(SYS_BUS_DEVICE(mtu2), 3,
                       qdev_get_gpio_in(gic,
                           RZA1H_MTU2_TGI4C_IRQ - RZA1H_GIC_NUM_INTERNAL));

    /* riic.c -- upgraded from a bare RAM region to a real device,
     * 2026-09-08 second pass, once body.bin's own cold-boot RIIC2 read
     * was found busy-waiting forever on this channel's completion
     * interrupt with no way for it to ever fire (see riic.c's own
     * comment and README.md). All 3 channels wired identically (only
     * RIIC2's read is confirmed exercised by any currently-traced boot
     * path, but the other two cost nothing extra to wire correctly too). */
    for (ch = 0; ch < 3; ch++) {
        DeviceState *riic = qdev_new(TYPE_RZA1H_RIIC);
        hwaddr base = RZA1H_RIIC0_BASE + ch * RZA1H_RIIC_STRIDE;
        int irq_base = RZA1H_RIIC_IRQ_BASE0 + ch * RZA1H_RIIC_IRQ_STRIDE;
        int j;

        qdev_prop_set_uint32(riic, "channel", ch);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(riic), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(riic), 0, base);
        for (j = 0; j < 6; j++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(riic), j,
                               qdev_get_gpio_in(gic,
                                   irq_base + j - RZA1H_GIC_NUM_INTERNAL));
        }
    }

    /* Extension-roadmap item 4: eight real SCIF UARTs, matching real
     * hardware addresses/count (see rz_a1h.h). serial_hd(i) hands channel
     * i whichever `-serial`/`-chardev` backend the command line gave it,
     * or NULL (scif.c's own CharBackend handling degrades gracefully --
     * TX is always logged regardless, see its own comment). */
    for (i = 0; i < RZA1H_SCIF_COUNT; i++) {
        DeviceState *scif = qdev_new(TYPE_RZA1H_SCIF);

        qdev_prop_set_uint32(scif, "channel", i);
        qdev_prop_set_chr(scif, "chardev", serial_hd(i));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(scif), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(scif), 0,
                                RZA1H_SCIF0_BASE + i * RZA1H_SCIF_STRIDE, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(scif), 0,
                           qdev_get_gpio_in(gic,
                               RZA1H_SCIF_TXI_BASE0 + i * RZA1H_SCIF_TXI_STRIDE
                                   - RZA1H_GIC_NUM_INTERNAL));
        sysbus_connect_irq(SYS_BUS_DEVICE(scif), 1,
                           qdev_get_gpio_in(gic,
                               RZA1H_SCIF_RXI_BASE0 + i * RZA1H_SCIF_RXI_STRIDE
                                   - RZA1H_GIC_NUM_INTERNAL));
        /* Channel 5's second MMIO region (see scif.c's own comment and
         * rz_a1h.h's RZA1H_SCIF5_DSP_RETRY_ARM_BASE) -- every other
         * channel leaves it created but unmapped, a normal, inert QEMU
         * idiom (rza1h_scif_init's own comment explains why it can't be
         * skipped at creation time instead). */
        if (i == 5) {
            sysbus_mmio_map_overlap(SYS_BUS_DEVICE(scif), 1,
                                    RZA1H_SCIF5_DSP_RETRY_ARM_BASE, 0);
        }
    }

    /* Extension-roadmap item 5: real MMCIF command/response/data protocol,
     * see mmc.c's own comment. */
    mmc = qdev_new(TYPE_RZA1H_MMC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mmc), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(mmc), 0, RZA1H_MMC_BASE, 0);

    /* dmac.c -- channel 0 only, added 2026-09-09 once cold_boot_hw_init's
     * init chain was found depending on a real DMA-completion interrupt
     * right after MTU2 channel 3 (see mtu2.c and dmac.c's own comments).
     * Overlap-mapped: sits inside the broader "io-e8200000"
     * unimplemented-device catch-all above. */
    dmac = qdev_new(TYPE_RZA1H_DMAC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dmac), 0, RZA1H_DMAC_BASE, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(dmac), 0,
                       qdev_get_gpio_in(gic,
                           RZA1H_DMAC_CH0_IRQ - RZA1H_GIC_NUM_INTERNAL));

    /* rspi2.c -- added 2026-09-09 once mtu2.c's own scif5_cmd_transmit_now
     * rate-limiter fix unblocked the shared DSP-comms ring far enough to
     * reach a real job-type-3 (RSPI2 transmit) entry (see rspi2.c's own
     * comment). Overlap-mapped: sits inside the broader "io-e8000000"
     * unimplemented-device catch-all above. No IRQ wired -- purely
     * polled, see rspi2.c's own comment. */
    rspi2 = qdev_new(TYPE_RZA1H_RSPI2);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(rspi2), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(rspi2), 0, RZA1H_RSPI2_BASE, 0);

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
