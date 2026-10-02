/*
 * RZ/A1H 10-bit wired A/D converter (ADC, 0xE8005800) -- minimal fixed-reading stub, same
 * permissive spirit as rspi2.c/riic.c/openvg.c.
 *
 * 2026-09-21, continued (OpenVG-rendering thread). Why this exists:
 *
 *   A full `-d unimp,guest_errors` survey of a 180s PWRK-hold boot (well past main_idle_loop and
 *   the one render-dispatch pass, per README.md's own Status section) found exactly ONE region
 *   still being touched continuously in steady state, out of everything this machine models:
 *   `io-e8000000` at offset 0x5800-0x580e, ~433 hits/sec for the entire run (78090 total reads,
 *   vs. every other region's one-time boot-config few-dozen hits). Every other unimplemented
 *   region this project has ever found this way (VDC50/LVDS, MTU2 TGI4D, the RX-8803LC RTC) turned
 *   out to be a real missing peripheral gating forward progress -- this is the same pattern.
 *
 *   Cross-referenced against the RZ/A1H SVD: 0xE8005800 is the ADC (DRA-DRH data registers at
 *   +0x00-+0x0e, ADCSR at +0x60). Decompiling the real driver confirms it end to end:
 *   `FUN_200b0678` (ADC init) writes ADCSR=0x20bf ONCE (continuous-scan mode, no completion
 *   polling ever follows -- the driver trusts free-running conversion and just rereads whatever's
 *   there), then `FUN_200b5124` (the periodic front-panel/DSP-settings-scan tick, called from
 *   `FUN_200b517c`) unconditionally rereads DRA/DRB/DRC/DRE/DRG/DRH every pass (DRD/DRF are
 *   skipped this boot -- gated on a mode byte that reads clear here, exactly matching the 6-not-8
 *   offsets actually seen in the survey) and right-shifts each raw 16-bit value by 6 before
 *   feeding `dsp_param_table_rebuild_from_settings`. RZ/A1H's ADC left-justifies its 10-bit result
 *   in the top bits of a 16-bit register (bits 15:6), matching that >>6 exactly -- confirms this
 *   is a real 10-bit conversion result, not an arbitrary shift.
 *
 *   Before this device existed, every DRx read fell through to plain unimplemented-device
 *   handling and returned 0 -- permanently, every single pass, forever. Any downstream logic that
 *   only reacts to a control's value *changing* (an S-meter deflection, a knob position, a level
 *   bar) could never see anything happen, since the input never varies from a degenerate all-zero
 *   reading. Not yet confirmed as *the* reason the OpenVG command FIFO stays silent after bring-up
 *   (see README.md's Status section on that), but a real, previously-unmodeled gap in exactly the
 *   subsystem most likely to feed a "does the screen need to redraw" decision, so worth fixing on
 *   its own merits regardless of whether it turns out to be the missing link.
 *
 * What is modeled: DRA-DRH each return a fixed, quiescent 10-bit-in-top-6-bits reading (mid-scale,
 * 0x8000 raw = 0x200 =512/1023) -- plausible neither pegged high nor low, deliberately NOT
 * per-channel-differentiated since no traced code has identified which of the 8 channels maps to
 * which real front-panel control yet. ADCSR/ADCMPER/ADCMPSR/the ADCMPx compare registers are
 * plain read/write storage, matching this project's existing "no behavior modeled yet" convention
 * for registers nothing traced gives meaning to (see cpg's own comment in rz_a1h.c) -- the real
 * driver here never reads any of them back anyway.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "rz_a1h.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HAdcState, RZA1H_ADC)

struct RZA1HAdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t regs[RZA1H_ADC_SIZE];
};

#define ADC_DRA 0x00
#define ADC_DRH 0x0e

/* Mid-scale 10-bit reading (0x200 of 0-0x3ff), left-justified per the real register's own
 * bits-15:6 layout -- see the file comment for why >>6 in the real driver confirms this. */
#define ADC_QUIESCENT_READING 0x8000

static uint64_t rza1h_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HAdcState *s = RZA1H_ADC(opaque);
    uint64_t val = 0;

    if (offset <= ADC_DRH && (offset % 2) == 0) {
        return ADC_QUIESCENT_READING;
    }

    memcpy(&val, &s->regs[offset], size);
    return val;
}

static void rza1h_adc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RZA1HAdcState *s = RZA1H_ADC(opaque);

    memcpy(&s->regs[offset], &value, size);
}

static const MemoryRegionOps rza1h_adc_ops = {
    .read = rza1h_adc_read,
    .write = rza1h_adc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_adc_reset(DeviceState *dev)
{
    RZA1HAdcState *s = RZA1H_ADC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rza1h_adc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HAdcState *s = RZA1H_ADC(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_adc_ops, s,
                          TYPE_RZA1H_ADC, RZA1H_ADC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rza1h_adc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rza1h_adc_reset);
}

static const TypeInfo rza1h_adc_info = {
    .name          = TYPE_RZA1H_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HAdcState),
    .instance_init = rza1h_adc_init,
    .class_init    = rza1h_adc_class_init,
};

static void rza1h_adc_register_types(void)
{
    type_register_static(&rza1h_adc_info);
}

type_init(rza1h_adc_register_types)
