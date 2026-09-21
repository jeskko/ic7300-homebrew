/*
 * RZ/A1H "Renesas Graphics Processor for OpenVG(TM)" (0xE8100000, bus-matrix
 * slave SLV5) -- minimal completion-interrupt stub, in the same permissive
 * spirit as rspi2.c / scif.c / riic.c.
 *
 * 2026-09-21 (icom-main-idle-loop-not-reached thread). Why this exists, and
 * how the need was established, end to end:
 *
 *   cold_boot_mode_dispatch (body.bin 0x2002b1c8) runs cold_boot_hw_init once
 *   -- which a GDB breakpoint walk (tools/trace_cold_boot_hw_init_walk.py)
 *   confirmed now completes and RETURNS -- then loops
 *     while (system_mode_request_dispatch(), !shutdown)
 *         ... main_idle_loop() ...
 *   A second walk (tools/walk_call_sites.py) showed system_mode_request_dispatch
 *   (0x2002a6b8) itself never returns: it stalls in FUN_200375e4 (0x200375e4),
 *     while (*0x2039064c != 0) rtos_wait_obj1_forever();
 *   the consumer half of a handshake its immediate predecessor call,
 *   FUN_200378cc(0) (0x200378cc), just posted:
 *     *0x2039064c = 1; FUN_2007edc8();   // wake ui_graphics_lifecycle_task
 *   Only ui_graphics_lifecycle_task's own main loop (0x2007f03c / 0x2007f058)
 *   ever writes 0x2039064c back to 0 -- and that task never reaches its main
 *   loop: it is itself stuck, four levels down, in
 *     ui_graphics_lifecycle_task (0x2007ef5c)
 *       -> graphics_stack_startup_egl_openvg (0x20079240), the "vgStartUp" step
 *         -> slv5_periph_connect_disconnect_handler (0x201009d2)
 *           -> slv5_periph_configure (0x2014ee46)
 *             -> FUN_20153ec2(ctx+0x10, 1, 0)  ==  wait-event-flag, pattern 1,
 *                                                  timeout TMO_FEVR
 *   (each link confirmed live by its own breakpoint walk, not inferred).
 *
 *   That event flag is set from exactly one place: FUN_2014e9f8 (0x2014e9f9,
 *   Thumb), the graphics processor's interrupt handler. slv5_periph_configure
 *   registers it via FUN_2007ec74 on GIC interrupt IDs 0x82-0x85 with priority
 *   0x30 and unmasks all four. The RZ/A1H hardware manual's Table 7.3 (List of
 *   Interrupt IDs) confirms IDs 130-133 are exactly "OpenVG(TM) graphics
 *   processor" INT0/INT1/INT2/INT3, level-triggered -- an exact match, read
 *   from the manual, not derived.
 *
 *   The ISR reads the interrupt-status register at base+0x4018, masks it with
 *   the software-held enable mask (ctx+0x24, set to 0x74f right before the
 *   wait and also written to base+0x4014), acknowledges by writing the status
 *   back to base+0x401c, and then routes bits to three event flags. Status
 *   bit 0 is the one that reaches the flag the stalled wait is parked on:
 *     uVar8 = (status & 1) | current_flag;
 *     if (uVar8 != current_flag) (*(ctx+0xc4))(uVar8);   // set_flg
 *   so a single base+0x4018 bit-0 assertion, delivered on GIC 130, is all the
 *   hardware fidelity this boot path actually needs.
 *
 *   Before this device existed, 0xE8100000-0xE813FFFF was covered only by
 *   rz_a1h.c's plain-RAM "io-e8100000" catch-all: base+0x4018 always read 0,
 *   no interrupt source existed at all, so the wait could never be satisfied
 *   and the ENTIRE boot deadlocked one call short of main_idle_loop.
 *
 * Register map (offsets from 0xE8104000; derived from the driver's own use,
 * NOT from the manual -- Renesas' section 44 is a one-page feature summary
 * with no register descriptions at all, the IP being third-party):
 *   +0x00  command FIFO write port (FUN_20150122 pushes a command list here)
 *   +0x04  written 0 during bring-up (reset/control)
 *   +0x10  read once into the driver context, never validated (ID/version)
 *   +0x14  interrupt enable        (written 0x74f)
 *   +0x18  interrupt status        (read by the ISR)
 *   +0x1c  interrupt clear         (ISR writes the status back to ack)
 *   +0x24  read-modify-write, bit 31 set during command submission
 * Two further registers at base+0x20064 / base+0x20068 (FIFO watermark
 * thresholds, written 0x1ff / 0xfb) are left to the plain-RAM catch-all --
 * the driver never reads them back.
 *
 * What is modeled: nothing renders. A write to the command FIFO latches the
 * "operation complete" status bit and, if enabled, raises INT0. The driver's
 * own acknowledge write clears it. That is deliberately the entire contract:
 * this unblocks the bring-up handshake without pretending to be a graphics
 * accelerator. Anything downstream that needs real rendering results (actual
 * EGL surface content, the 480x272 window / 960x552 pixmap blits) will still
 * behave as if the GPU produced nothing -- a known, accepted limit, same
 * philosophy as scif.c's virtual DSP responder.
 *
 * Also deliberately NOT modeled: the FIFO-full / FIFO-below-watermark
 * hysteresis (status bits 0x40 and 0x30). The driver only consults those once
 * its own software free-slot counter (0x20390acc, initialized to 0x1ff) runs
 * below 7, which the short bring-up command lists never approach. Asserting
 * 0x40 would actively mislead the driver into believing the FIFO is full, so
 * this model never sets it.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HOpenVGState, RZA1H_OPENVG)

struct RZA1HOpenVGState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq int0;
    uint32_t intsts;
    uint32_t inten;
    uint64_t commands;
    uint8_t regs[RZA1H_OPENVG_SIZE];
};

#define OPENVG_CMD     0x00
#define OPENVG_CTRL    0x04
#define OPENVG_ID      0x10
#define OPENVG_INTEN   0x14
#define OPENVG_INTSTS  0x18
#define OPENVG_INTCLR  0x1c

/* The two "operation complete" status sources this bring-up path actually
 * waits on, both read straight off the ISR's own bit routing (FUN_2014e9f8):
 *
 *   bit 0  -> (status & 1)     -> event flag ctx+0x10, via the callback at
 *                                 ctx+0xc4. slv5_periph_configure's first
 *                                 wait, FUN_20153ec2(ctx+0x10, 1, 0), is
 *                                 parked on exactly this bit.
 *   bit 1  -> (status & 0x60a) -> event flag ctx+0xc. Its second wait,
 *                                 FUN_20153ec2(ctx+0xc, 2, 0), needs bit 1
 *                                 of that flag, i.e. status bit 1.
 *
 * Both were established the same way: arm the wait's own call site with a
 * breakpoint, confirm the walk stops there, add the bit, confirm the walk
 * moves past it (tools/walk_call_sites.py).
 *
 * Bits 3/9/10 also route to ctx+0xc and bits 2/8 to ctx+0x14, but nothing
 * traced waits on them, and they may well be error indications -- so this
 * model leaves them alone rather than asserting every enabled source. Bit 6
 * (FIFO above high-watermark) is deliberately never asserted; see the file
 * comment. */
#define OPENVG_INT_DONE ((1u << 0) | (1u << 1))

static void rza1h_openvg_update_irq(RZA1HOpenVGState *s)
{
    qemu_set_irq(s->int0, (s->intsts & s->inten) != 0);
}

static uint64_t rza1h_openvg_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(opaque);
    uint64_t val = 0;

    switch (offset) {
    case OPENVG_INTSTS:
        return s->intsts;
    case OPENVG_INTEN:
        return s->inten;
    default:
        break;
    }

    memcpy(&val, &s->regs[offset], size);
    return val;
}

static void rza1h_openvg_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(opaque);

    switch (offset) {
    case OPENVG_CMD:
        /* Command words are consumed instantly and discarded; completion is
         * reported immediately. See the file comment for why "instantly" is
         * the right fidelity here. */
        s->commands++;
        s->intsts |= OPENVG_INT_DONE;
        rza1h_debug("openvg", "cmd %08x (#%" PRIu64 "), intsts=%08x inten=%08x",
                    (uint32_t)value, s->commands, s->intsts, s->inten);
        rza1h_openvg_update_irq(s);
        return;
    case OPENVG_INTEN:
        s->inten = (uint32_t)value;
        rza1h_debug("openvg", "inten <- %08x", s->inten);
        rza1h_openvg_update_irq(s);
        return;
    case OPENVG_INTCLR:
        s->intsts &= ~(uint32_t)value;
        rza1h_debug("openvg", "ack %08x, intsts=%08x", (uint32_t)value,
                    s->intsts);
        rza1h_openvg_update_irq(s);
        return;
    case OPENVG_INTSTS:
        /* Read-only status; the driver acknowledges through INTCLR. */
        return;
    default:
        break;
    }

    memcpy(&s->regs[offset], &value, size);
}

static const MemoryRegionOps rza1h_openvg_ops = {
    .read = rza1h_openvg_read,
    .write = rza1h_openvg_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_openvg_reset(DeviceState *dev)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->intsts = 0;
    s->inten = 0;
    s->commands = 0;
    rza1h_openvg_update_irq(s);
}

static void rza1h_openvg_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HOpenVGState *s = RZA1H_OPENVG(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_openvg_ops, s,
                          TYPE_RZA1H_OPENVG, RZA1H_OPENVG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->int0);
}

static void rza1h_openvg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rza1h_openvg_reset);
}

static const TypeInfo rza1h_openvg_info = {
    .name          = TYPE_RZA1H_OPENVG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HOpenVGState),
    .instance_init = rza1h_openvg_init,
    .class_init    = rza1h_openvg_class_init,
};

static void rza1h_openvg_register_types(void)
{
    type_register_static(&rza1h_openvg_info);
}

type_init(rza1h_openvg_register_types)
