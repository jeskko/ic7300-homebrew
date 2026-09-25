# Input: front-panel controls and touch

The largest concrete research gap of any SDK area — needed for App 3 (a game) in `app-requirements.md`,
not needed by Apps 1/2/4.

All addresses below verified against the live Ghidra project (`body.bin`) 2026-08-30.

## ✅ Available in the SDK now: touch, with an input grab (2026-09-25)

`hb_touch_read()` (`sdk/include/hb/input.h`) returns touch down/X/Y in screen pixels from the
front-panel register file. While an app holds the screen (`hb_gfx_open()`), the loader skips the
firmware's own touch and key handling so the UI underneath doesn't react. Live-tested with
`sdk/examples/cube/`. Key and dial reads for apps aren't wrapped yet;
`notes/front-panel-report.md` has the full field and key tables. (The sections below predate
that file and are superseded by it.)

## 🔎 Front-panel packet protocol: framing understood, most message types not decoded

Physical link: **SCIF3**, base `0xE8008800`, pins `P6_0`/`P6_1` — a 33-byte packet-framed UART, same
`0xFE`/`0xFD` byte-framing template as CI-V/SCIF0. **`scif3_frame_dispatch_by_type`** (`0x20036bb8`,
verified) parses front-panel packets into a shared status buffer.

**Only 2 of up to 32 message types have decoded field meanings** (the MENU+FUNCTION service-mode combo,
from the service-mode entry investigation — `notes/front-panel-firmware.md`). A game needs at minimum:
main-dial rotation (direction + step count) and some confirm/rotate/drop action (soft key, physical
button, or touchscreen tap) — none of these specific message types are decoded yet. This is the largest
concrete gap in the whole SDK survey.

## 🔎 Touchscreen: controller identity known, protocol completely unresearched

`IC152` (`UC6528XBNQ4GRC`) is flagged in the hardware BOM as "likely display/touch controller — function
not confirmed" (`notes/ic7300-hardware.md`). Its actual protocol (I2C? SPI? which main-CPU pins?) hasn't
been traced at all. **Not required** if a first app uses only the physical main dial + a couple of buttons
instead of touch — a reasonable scope reduction, per `app-requirements.md`'s App 3.

## What would settle this fastest

Per `notes/front-panel-firmware.md`'s own open questions: decoding more of the 32 SCIF3 message types by
correlating live front-panel activity (dial turns, specific button presses) against captured packet bytes
— this is a live-testing (JTAG or SCIF3 sniff) task more than a static-analysis one, similar to several
other open items across this project.
