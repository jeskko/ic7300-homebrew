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

#endif
