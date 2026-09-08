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
 * real interrupt ID this machine wires up. Was 192 (enough for OSTM0's 134)
 * until riic.c wired RIIC2's real interrupts, the highest of which
 * (INTIICNAKI2, ID 210, see riic.c) needs at least 211 -- rounded up to
 * the next multiple of 32; raised again to 256 (2026-09-08 third pass)
 * once scif.c wired real TXI interrupts, the highest of which (TXI7, ID
 * 252, see below) needs at least 253. */
#define RZA1H_GIC_NUM_IRQ  256

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

/* Matches emu/peripherals/mtu2.py's BASE/SIZE. Upgraded from a bare RAM
 * region (2026-09-09) to a real device for channel 3 specifically, once
 * body.bin's own cold-boot task-readiness busy-wait was found depending on
 * a real periodic interrupt only MTU2 channel 3 can produce -- see mtu2.c's
 * own comment and qemu-machine/README.md's Status section. GIC ID 154
 * (TGI3A, channel 3's compare-match-A interrupt) confirmed via
 * ~/Downloads/rza1.svd's ICDISR4 register (index*32+bit formula, the same
 * one that already gave OSTM0 its ID 134). */
#define RZA1H_MTU2_BASE 0xFCFF0000
#define RZA1H_MTU2_SIZE 0x00000400
#define RZA1H_MTU2_TGI3A_IRQ 154
#define TYPE_RZA1H_MTU2 "rza1h-mtu2"

/* riic.c -- matches emu/peripherals/riic.py's per-instance base addresses
 * (RIIC0-2, stride 0x400). RIIC2 is the diode-matrix EEPROM's controller,
 * per notes/ic7300-hardware.md. Upgraded from a bare RAM region (2026-09-08
 * second pass) to a real device once body.bin's own cold-boot I2C2 read was
 * found busy-waiting forever on this channel's completion interrupt -- see
 * riic.c's own comment and qemu-machine/README.md. INTIICTEI<n> (the first
 * of each channel's 6 wired real interrupt IDs -- TEI/RI/TI/SPI/STI/NAKI,
 * consecutive, per scratch/r01an5093ej0170-rza1-swpkg's r_intc.h) is
 * 189 + 8*channel. */
#define RZA1H_RIIC0_BASE 0xFCFEE000
#define RZA1H_RIIC1_BASE 0xFCFEE400
#define RZA1H_RIIC2_BASE 0xFCFEE800
#define RZA1H_RIIC_STRIDE 0x00000400
#define RZA1H_RIIC_SIZE  0x00000044
#define RZA1H_RIIC_IRQ_BASE0 189 /* INTIICTEI0 */
#define RZA1H_RIIC_IRQ_STRIDE 8  /* per channel, to INTIICTEI(n+1) */
#define TYPE_RZA1H_RIIC "rza1h-riic"

/* scif.c -- matches ~/Downloads/rza1.svd's SCIF0-7 base addresses, +0x800
 * apart each. Real confirmed roles (notes/ic7300-signal-chain.md): SCIF0
 * is the CI-V UART, SCIF1 the service/calibration link, SCIF3 the
 * front-panel link, SCIF5 the DSP link -- all eight wired identically in
 * rz_a1h.c regardless (nothing in scif.c depends on channel role). */
#define RZA1H_SCIF0_BASE 0xE8007000
#define RZA1H_SCIF_STRIDE 0x800
#define RZA1H_SCIF_COUNT 8
#define TYPE_RZA1H_SCIF "rza1h-scif"

/* scif.c's real TXI (transmit-complete) interrupt IDs -- found needing this
 * 2026-09-08 (third pass) once live testing showed scif3_driver_pump_tick
 * (body.bin) becomes a permanent no-op after its very first send: it sets a
 * software "TX in flight" flag and the only code that ever clears it is
 * whatever ISR services the interrupt scif3_send_frame explicitly arms
 * before returning (calls a confirmed generic `gic_enable_irq(id)` helper,
 * FUN_200b8308, with id=0xec=236) -- with scif.c's TX side having "no IRQ
 * line wired to the GIC yet" (this file's own prior comment), that ISR can
 * never run, and *nothing* past the first frame ever proceeds again. IDs
 * derived from ~/Downloads/rza1.svd's ICDISR6/ICDISR7 fields (register
 * index * 32 + bit, the same formula ostm.c's ID 134 already established)
 * -- SCIF-n's group is BRIn=221+4n, ERIn=222+4n, RXIn=223+4n, TXIn=224+4n;
 * TXI3 lands on 236, exactly matching the firmware's own 0xec literal
 * above -- a genuine independent cross-check, not just SVD-derived.
 * 2026-09-09: RXI wired too, needed for the virtual front-panel responder
 * (see scif.c's own comment) -- RXI3 (235) already confirmed enabled in
 * ISENABLER7 (`0x1e00` = bits 9-12 = BRI3/ERI3/RXI3/TXI3 all four, read
 * live the same session TXI3 was confirmed) alongside TXI3, so no
 * additional firmware-side arming was needed to use it. BRI/ERI still not
 * wired -- nothing traced needs them. */
#define RZA1H_SCIF_TXI_BASE0 224
#define RZA1H_SCIF_TXI_STRIDE 4
#define RZA1H_SCIF_RXI_BASE0 223
#define RZA1H_SCIF_RXI_STRIDE 4

/* mmc.c -- matches the real reference struct layout (mmc_iodefine.h, see
 * mmc.c's own comment) -- 0x80 bytes covers offset 0x00 (CE_CMD_SETH)
 * through 0x7c (CE_VERSION) inclusive. First-pass plain storage only,
 * real bit-level command/response semantics not yet confirmed. */
#define RZA1H_MMC_BASE 0xE804C800
#define RZA1H_MMC_SIZE 0x00000080
#define TYPE_RZA1H_MMC "rza1h-mmc"

#endif
