# Settings and misc peripherals

Not tied to any of the four example apps specifically, but real, confirmed primitives an app could
plausibly want — grouped here rather than left scattered across `notes/`.

All addresses below verified against the live Ghidra project (`body.bin`) 2026-08-30.

## ✅ EEPROM: generic parameter get/set API

**`FUN_2001e510`** (`0x2001e510`, verified) = get, **`FUN_2001e484`** (`0x2001e484`, verified) = set, both
`(param_id, dest, length)`. The getter alone has **73 call sites**; `notes/eeprom-catalogue.md` is a living
document tracking identified parameter IDs from sampling ~15-20 of them so far — includes the diode-matrix
region-scan result, several large unidentified settings blocks, and a discovered versioned-settings-format
dispatch mechanism (multiple stored format signatures, each with its own parser). Real "read/write radio
settings" surface if an app ever wants persistent state or radio configuration, though most individual
parameter IDs beyond the diode-matrix one remain unidentified — check `notes/eeprom-catalogue.md` before
assuming a specific ID's meaning.

Chip: `IC351` (`GT24C128B`), I2C via `ECK`/`EDT` → `P1_4`/`P1_5` (RIIC2).

## ✅ RTC

`IC381` (`RX-8803LC`), dedicated CPU pins `RTC_IRQ`/`RTC_SCL`/`RTC_SDA` → `P1_1`-`P1_3` (RIIC1). Not yet
connected to a specific driver function in this survey — `notes/ic7300-hardware.md` has the pin mapping;
`voice_recording_file_task`'s RTC-date-stamped filename generation (`notes/kernel-rtos.md`) is existing
proof the RTC is readable from application-level code, but the specific read function wasn't traced in this
pass.

## 🔎 Frequency/mode read helpers

**`FUN_200623bc`** (`0x200623bc`, verified to exist, not yet renamed) is used by the undocumented CI-V
`0x2A` handler to read the current operating frequency for a ceiling check (confirmed as a genuine
"read current VFO/operating frequency" helper in `notes/kernel-rtos.md`'s CI-V section, cross-checked
against the documented `1C 01` tuner command's own precheck path). Worth pulling together as its own small
API area — several apps would plausibly want "what frequency/mode is the radio on" without going through
full CI-V request/response framing — but only this one helper is confirmed so far; a mode-read equivalent
hasn't been specifically located.

## Not yet connected to any SDK use, listed for completeness

- **Tuner engage primitive**: `tuner_engage_gpio_toggle` (`0x2001e720`, verified) — the shared low-level
  trigger reached by three independent paths (documented CI-V `1C 01`, undocumented CI-V `0x2A`, and the
  external tuner-jack `EKEY` signal). Not an obvious app-facing primitive (no clear use case for an app
  triggering the antenna tuner), noted here only because it's one of the most thoroughly-traced pieces of
  non-core functionality in the whole project.
- **SD-card menu**: `sd_menu_dispatch_task` (`0x20027528`, verified) — a 42-case dispatcher for the SD-card
  operations menu. Flagged in `roadmap.md`'s Phase 2 as a candidate injection point for a "run app" menu
  entry — out of scope for this file, listed for cross-reference only.
