/*
 * Epson RX-8803LC I2C real-time clock -- the real RTC on the IC-7300's RIIC1
 * bus (0x32, 7-bit address, confirmed live: `main_idle_loop`'s own periodic
 * RTC-read call chain -- FUN_20051820, notes/cold-boot-hw-init-sweep.md and
 * notes/kernel-rtos.md's own confirmation via the RZ/A1H manual + real
 * schematic, `IC351` -- previously always NACKed since nothing responded at
 * this address at all on riic.c's own I2C bus).
 *
 * 2026-09-21 (icom-main-idle-loop-not-reached thread, follow-on): once
 * openvg.c let boot reach `main_idle_loop` for the first time, its own
 * per-iteration RTC read (`riic1_driver_init()` then a real RIIC1
 * transaction) started genuinely failing live -- `RZA1H_DEBUG=riic` showed
 * `i2c_start_transfer(0x32) unexpectedly NACKed`. Root cause: `riic.c`'s own
 * `eeprom_bus` + AT24C EEPROM slave gets attached unconditionally to every
 * RIIC channel (0/1/2), already flagged in this project's own notes as a
 * real, known gap -- so RIIC1 only ever had a fake EEPROM sitting at 0x50,
 * never anything at the RTC's own real address 0x32. Fixed in `riic.c`
 * (gated the EEPROM to channel 2 only, attach this device to channel 1).
 *
 * Register map straight from Epson's own datasheet
 * (`/data/misc/icom/7300/doc/RX-8803LC_en.pdf`, section 8, "Basic time and
 * calendar register" table, page 6) -- only the basic register bank
 * (00h-0Fh) is modeled; the two extension banks (10h-2Fh, 1/100s counter +
 * capture/event registers) are plain storage, matching this project's
 * established permissive-peripheral philosophy (nothing traced needs them:
 * `main_idle_loop`'s own read only ever touches 00h-06h, confirmed by
 * decompiling FUN_20051820's own BCD-range validation, which checks exactly
 * SEC/MIN/HOUR/DAY/MONTH/YEAR and nothing past that).
 *
 * Modeled behavior, deliberately minimal (same spirit as ds1338.c, this
 * project's own closest upstream template): the clock/calendar registers
 * (00h-06h) always reflect the real host wall-clock time via
 * `qemu_get_timedate()`/`to_bcd()` (the same helpers ds1338.c uses), so
 * every read is a genuinely valid, always-in-range BCD date/time -- no
 * counting/ticking logic of our own to get subtly wrong. WEEK (03h) uses
 * the datasheet's own one-hot day-of-week encoding (Sunday=01h, doubling
 * each day up to Saturday=40h), not a linear 0-6 index. VLF (Voltage Low
 * Flag, register 0Eh bit 1) always reads 0 ("data loss not detected") --
 * the real chip only sets it after an actual power interruption, and
 * nothing about a freshly-booted emulated radio should ever look like that.
 * Registers 07h (a real, plain read/write RAM byte on real hardware) and
 * every alarm/timer/extension/control register are plain read/write
 * storage with no behavior attached -- nothing traced sets an alarm or
 * enables the fixed-cycle timer on this boot path.
 *
 * Live-verified: the previously-observed `i2c_start_transfer(0x32)
 * unexpectedly NACKed` is gone, a real transaction now completes, and
 * `main_idle_loop`'s own iteration counter keeps climbing normally
 * afterward (no hang, no regression). Did NOT chase down `FUN_20051820`'s
 * own local buffer (`0x2039031f`, 7 bytes) field-by-field against exactly
 * which byte lands at which index -- a couple of fields read back as
 * exactly what this model produces, others still look default-shaped,
 * consistent with either a partial burst (this debug channel doesn't log
 * every individual byte of a multi-byte transfer, only phase transitions)
 * or the firmware's own BCD-range check triggering its existing corrective
 * re-read path (`FUN_20050d20`/`FUN_20050cd8`/`FUN_20050c1c`, already a
 * straight-line one-shot correction in the decompile, not a retry loop --
 * safe either way, never blocks). Real point is the same one every other
 * device model in this file's own philosophy makes: this unblocks the
 * boot path without pretending to be a byte-for-byte protocol trace.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qemu/bcd.h"
#include "qom/object.h"
#include "system/rtc.h"

#include "rza1h_debug.h"

#define TYPE_RX8803 "rx8803"
OBJECT_DECLARE_SIMPLE_TYPE(RX8803State, RX8803)

#define RX8803_NUM_REGS 0x10

/* Basic register addresses (datasheet section 8.1.2). */
#define RX8803_REG_SEC   0x00
#define RX8803_REG_MIN   0x01
#define RX8803_REG_HOUR  0x02
#define RX8803_REG_WEEK  0x03
#define RX8803_REG_DAY   0x04
#define RX8803_REG_MONTH 0x05
#define RX8803_REG_YEAR  0x06
#define RX8803_REG_FLAG  0x0e

#define RX8803_FLAG_VLF (1 << 1)

/* One-hot day-of-week encoding, straight from the datasheet's own table
 * (page 9): Sunday=01h, Monday=02h, Tuesday=04h, ..., Saturday=40h --
 * i.e. 1 << tm_wday (glibc's tm_wday is already 0=Sunday..6=Saturday). */
static uint8_t rx8803_week_byte(int tm_wday)
{
    return (uint8_t)(1u << (tm_wday % 7));
}

struct RX8803State {
    I2CSlave parent_obj;

    uint8_t regs[RX8803_NUM_REGS];
    uint8_t ptr;
    bool addr_byte;
};

static void rx8803_capture_time(RX8803State *s)
{
    struct tm now;

    qemu_get_timedate(&now, 0);
    s->regs[RX8803_REG_SEC] = to_bcd(now.tm_sec);
    s->regs[RX8803_REG_MIN] = to_bcd(now.tm_min);
    s->regs[RX8803_REG_HOUR] = to_bcd(now.tm_hour);
    s->regs[RX8803_REG_WEEK] = rx8803_week_byte(now.tm_wday);
    s->regs[RX8803_REG_DAY] = to_bcd(now.tm_mday);
    s->regs[RX8803_REG_MONTH] = to_bcd(now.tm_mon + 1);
    s->regs[RX8803_REG_YEAR] = to_bcd(now.tm_year % 100);
    s->regs[RX8803_REG_FLAG] &= ~RX8803_FLAG_VLF; /* see file comment */
}

static int rx8803_event(I2CSlave *i2c, enum i2c_event event)
{
    RX8803State *s = RX8803(i2c);

    switch (event) {
    case I2C_START_RECV:
        /* Same reasoning as ds1338_event: real hardware latches on any
         * START, but the guest can only ever observe it via START_RECV. */
        rx8803_capture_time(s);
        break;
    case I2C_START_SEND:
        s->addr_byte = true;
        break;
    default:
        break;
    }

    return 0;
}

static uint8_t rx8803_recv(I2CSlave *i2c)
{
    RX8803State *s = RX8803(i2c);
    uint8_t res = s->regs[s->ptr];

    rza1h_debug("riic", "rtc: read reg 0x%02x = 0x%02x", s->ptr, res);
    s->ptr = (s->ptr + 1) & (RX8803_NUM_REGS - 1);
    return res;
}

static int rx8803_send(I2CSlave *i2c, uint8_t data)
{
    RX8803State *s = RX8803(i2c);

    if (s->addr_byte) {
        s->ptr = data & (RX8803_NUM_REGS - 1);
        s->addr_byte = false;
        return 0;
    }

    rza1h_debug("riic", "rtc: write reg 0x%02x = 0x%02x", s->ptr, data);
    s->regs[s->ptr] = data;
    s->ptr = (s->ptr + 1) & (RX8803_NUM_REGS - 1);
    return 0;
}

static void rx8803_reset(DeviceState *dev)
{
    RX8803State *s = RX8803(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->addr_byte = false;
    rx8803_capture_time(s);
}

static void rx8803_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(oc);

    isc->event = rx8803_event;
    isc->recv = rx8803_recv;
    isc->send = rx8803_send;
    device_class_set_legacy_reset(dc, rx8803_reset);
}

static const TypeInfo rx8803_info = {
    .name          = TYPE_RX8803,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(RX8803State),
    .class_init    = rx8803_class_init,
};

static void rx8803_register_types(void)
{
    type_register_static(&rx8803_info);
}

type_init(rx8803_register_types)
