/*
 * IC-7300 / Renesas RZ/A1H research machine -- shared addresses.
 *
 * Every value here is already-confirmed ground truth from this project's own
 * static-analysis notes and the Unicorn-based emulator (emu/board.py and
 * the modules under emu/peripherals/) -- not new derivation, just carried
 * over into the QEMU port. See qemu-machine/README.md for the migration
 * this is part of.
 */
#ifndef HW_ARM_RZ_A1H_H
#define HW_ARM_RZ_A1H_H

/* notes/base-loader.md's 2026-09-08 correction: true flash address of the
 * ARM exception vector table / CPU reset entry. Matches emu/flash_image.py's
 * FLASH_BASE and emu/board.py's RESET_VECTOR. */
#define RZA1H_FLASH_BASE   0x18000000
#define RZA1H_FLASH_SIZE   0x04000000  /* 64 MB, per notes/memory-map.md */

/* notes/memory-map.md's "on-chip RAM page", matches emu/board.py's
 * RAM_BASE/RAM_SIZE. */
#define RZA1H_RAM_BASE     0x20000000
#define RZA1H_RAM_SIZE     0x00A00000  /* 10 MB */

/* Already-confirmed ground truth from notes/memory-map.md and
 * notes/kernel-rtos.md (matches emu/peripherals/gic.py exactly). */
#define RZA1H_GIC_DIST_BASE 0xE8201000
#define RZA1H_GIC_CPU_BASE  0xE8202000

/* GIC interrupt ID count -- must be a multiple of 32 and cover the highest
 * real interrupt ID this machine wires up (134 for OSTM0, see ostm.c). */
#define RZA1H_GIC_NUM_IRQ  192

/* Number of GIC-internal interrupt IDs (SGIs 0-15 + PPIs 16-31) that
 * precede the first SPI -- arm_gic's own gic_set_irq() offsets every
 * external qdev_get_gpio_in() index by this much to get the absolute
 * interrupt ID (confirmed against hw/intc/arm_gic.c and cross-checked
 * against hw/arm/fsl-imx6.c's own IRQ #defines, which are pre-subtracted
 * the same way). Not IC-7300-specific -- this is generic to any board
 * wiring a device straight to arm_gic by absolute interrupt ID. */
#define RZA1H_GIC_NUM_INTERNAL 32

/* OSTM0's real base address (emu/peripherals/ostm.py's OSTM_BASE for
 * instance 0). See ostm.c for the real interrupt ID (134) this device's
 * IRQ output gets wired to. */
#define RZA1H_OSTM0_BASE   0xFCFEC000
#define RZA1H_OSTM1_BASE   0xFCFEC400

#define TYPE_RZA1H_OSTM "rza1h-ostm"

/* Confirmed via direct disassembly of the real base.dat, see
 * notes/base-loader.md's boot-sequence step 2 and emu/peripherals/spi_boot.py. */
#define RZA1H_SPI_BOOT_STATUS_BASE 0x3FEFA048
#define TYPE_RZA1H_SPI_BOOT_STATUS "rza1h-spi-boot-status"

/* All addresses below are already-confirmed ground truth carried over
 * unchanged from the matching emu/peripherals/ Python module (see each C
 * file's own comment for the full derivation) -- 2026-09-08's port of the
 * remaining Unicorn-side peripherals to real QEMU devices. */

/* gpio.c -- matches emu/peripherals/gpio.py's PORT_BASE, one MMIO claim
 * covering both the PORT_BASE and IBC_BASE clusters (IBC_BASE is
 * PORT_BASE + 0x4000, see gpio.py's own IBC_BASE constant) since
 * board.py registers them as a single region too. */
#define RZA1H_GPIO_BASE            0xFCFE3000
#define RZA1H_GPIO_SIZE            0x00005000
#define RZA1H_GPIO_IBC_REL_OFFSET  0x4000
#define TYPE_RZA1H_GPIO "rza1h-gpio"

/* l2c.c -- matches emu/peripherals/l2c.py's BASE/SIZE (real PL310 base). */
#define RZA1H_L2C_BASE 0x3FFFF000
#define RZA1H_L2C_SIZE 0x00001000
#define TYPE_RZA1H_L2C "rza1h-l2c"

/* cpg.c is not a dedicated device -- see rz_a1h.c: matches
 * emu/peripherals/cpg.py's two plain-storage clusters exactly, wired as
 * two bare RAM regions (no side effects modeled, same as the Python
 * version). */
#define RZA1H_CPG_MAIN_BASE         0xFCFE0000
#define RZA1H_CPG_MAIN_SIZE         0x00000500
#define RZA1H_CPG_DEEP_STANDBY_BASE 0xFCFF1800
#define RZA1H_CPG_DEEP_STANDBY_SIZE 0x00000020

/* Matches emu/peripherals/mtu2.py's BASE/SIZE -- also a bare RAM region,
 * see rz_a1h.c. */
#define RZA1H_MTU2_BASE 0xFCFF0000
#define RZA1H_MTU2_SIZE 0x00000400

/* Matches emu/peripherals/riic.py's per-instance base addresses (one
 * RIIC0-2 instance each, all bare RAM regions -- see rz_a1h.c). RIIC2 is
 * the diode-matrix EEPROM's controller, per notes/ic7300-hardware.md. */
#define RZA1H_RIIC0_BASE 0xFCFEE000
#define RZA1H_RIIC1_BASE 0xFCFEE400
#define RZA1H_RIIC2_BASE 0xFCFEE800
#define RZA1H_RIIC_SIZE  0x00000044

/* scif.c -- matches ~/Downloads/rza1.svd's SCIF0-7 base addresses, +0x800
 * apart each. Real confirmed roles (notes/ic7300-signal-chain.md): SCIF0
 * is the CI-V UART, SCIF1 the service/calibration link, SCIF3 the
 * front-panel link, SCIF5 the DSP link -- all eight wired identically in
 * rz_a1h.c regardless (nothing in scif.c depends on channel role). */
#define RZA1H_SCIF0_BASE 0xE8007000
#define RZA1H_SCIF_STRIDE 0x800
#define RZA1H_SCIF_COUNT 8
#define TYPE_RZA1H_SCIF "rza1h-scif"

#endif
