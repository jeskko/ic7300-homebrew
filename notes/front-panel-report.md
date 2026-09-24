# Front panel → main CPU: the SCIF3 report (keys, dials, pots, touch)

Static analysis of `body.bin` v1.42 (Ghidra, base 0x20005000), 2026-09-24, spot-checked live in
the emulator. Confidence: ✅ confirmed (decompile/disassembly and, where noted, a live check
agree), 🟢 strong inference, 🟡 guess.

**Summary.** The RL78 front-panel MCU (`IC501`) reports keys, dials, pots and touch to the main
CPU as a single 32-byte register file carried over `SCIF3`, sent only on change (there's no
polling and no keep-alive beyond the boot handshake). The main CPU never writes back except
during the boot handshake and touch calibration. Key names, long-press handling, the RF/SQL →
squelch chain and the emulator's fake front panel are all covered below.

## 1. Transport: frames and the register file ✅

Frames are `FE <offset> <data...> FD`, produced by the RL78 and parsed by
`scif3_frame_rx_statemachine` (0x20036c68): up to 33 bytes accepted after `FE`; `0xFF` is an
escape byte (next byte − 0x10). `scif3_frame_dispatch_by_type` (0x20036bb8) copies the payload
into the 32-byte register file `g_scif3_rx_status_buffer` at 0x203dcab6, at the given offset
(offsets ≥ 0x20 are rejected), and sets status bit 0x04. Types `0xF0`/`0xF1` are the boot
handshake (§6), not data.

The main CPU **never polls this buffer** — the front CPU must send a frame whenever something
changes, and there's no timeout/keep-alive check beyond the boot handshake's 75-tick wait.
`scif3_rx_buffer_reset_defaults` (0x2002aeec) sets the power-on defaults: `[0]=0`,
`[1..0x0c]=0x20`, `[0x0d..0x1d]=0`, `[0x1e]=[0x1f]=1`. A shadow copy,
`g_frontpanel_latched_status` (0x203dca96), mirrors the same offsets and is what the key/dial
scanners diff against to find what changed.

## 2. Field table ✅ unless marked

| Off | Field | Encoding | Consumer | Conf |
|---|---|---|---|---|
| 0x00 | unknown | — | no reader found | ✅ (negative result) |
| 0x01–0x0c | front-CPU version | ASCII, bytes 1–3 shown as `d.dd` | latched at 0x2002af94 | ✅ |
| 0x0d–0x11 | key bitfield, 40 bits | active-high; bit *n* = byte `0x0d+n/8` bit `n%8` | `scif3_key_bitfield_scan_and_resolve` (0x2002fbc8), one byte/tick, rotating, lowest newly-set bit per byte only; table 0x2018d980 | ✅ |
| 0x10 b0/b1 | two status flags | single bits | b0 read at 0x2006562c; b1 at 0x200209f0 → 0x203def00+0x2af and 0x200b5ec4 | 🟡 meaning unknown |
| 0x12 | unknown | — | no reader found | ✅ (negative result) |
| 0x13 | touch tag | 0 = calibrated pixel coords; 1 = raw coords (calibration only); other = no touch | `FUN_200640c8` (returns 9999/9999 on tag mismatch) | ✅ |
| 0x14–0x15 | touch X | BE16, pixels 0..479 (tag 0) or raw 12-bit (tag 1) | | ✅ |
| 0x16–0x17 | touch Y | BE16, pixels 0..271 (tag 0) or raw (tag 1) | | ✅ |
| 0x18–0x19 | MAIN DIAL | BE16 wrapping up-counter, CW = up | consumer 0x200176fc, `delta = int16(new − old)`, clamped ±255/read; 1 count = 1 tuning step (10 Hz default) | ✅ live-verified |
| 0x1a | dial-fast flag | u8 (auto tuning step) | | 🟢 |
| 0x1b | TWIN PBT inner | u8 counter | consumer 0x200194a0, clamped ±15/read | ✅ |
| 0x1c | TWIN PBT outer | u8 counter | same consumer | ✅ |
| 0x1d | MULTI | u8 counter (also RIT/ΔTX — there's no separate RIT knob) | consumer 0x20018ed0 | ✅ |
| 0x1e | AF pot | u8, 0..255 absolute | `FUN_2006ca3c` → `FUN_2006c8b0` | ✅ |
| 0x1f | RF/SQL pot | u8, 0..255 absolute | `FUN_2006ca3c` → `FUN_2006c7e8`; pickup mode `FUN_2006c950` needs ≥10 counts movement when flags 0x203902d4/…fe/…d5 are set | ✅ |

## 3. RF/SQL and squelch (live-verified) ✅

`FUN_200443cc` rescales the raw 0x1f byte: `raw < 0x66` → squelch level `0x203902e3 = 0`; else
`(raw − 0x66) * 255 / 0x98`. `FUN_2004424c` sets "squelch open" (`0x203902d9`): forced open when
`0x203902e3 < 0x2a`, otherwise by comparing the S-meter (`0x2039023a`) against a threshold
(`0x2039023c`, +364 hysteresis). When `0x203902d9 == 0`: `FUN_2002024c` sets the "muted" flag
(`0x203def00+0x2db`), `0x200b3308` clears the "audio on" flag (`+0x2dc`), and `FUN_20009024`
switches the audio scope to a ROM zero table (mode byte `0x20390017 = 2`). So an RF/SQL value
≥ 0x66 with no signal present shows as a blank audio scope. The emulator's default, 0x60, keeps
squelch open (and keeps `bench_boot` pixel-exact).

## 4. Key-code table 🟢 unless marked

Names come from the factory FRONT CHECK sequencer (misnamed `scif1_svc_command_dispatch`,
0x20012f5c; state byte 0x20390050; strings at 0x20197164+N*0x20; key table at 0x2018b516, byte =
low nibble, bit = high nibble), cross-checked against three power-on key combos:
`0x2002aeb4` tests offset 0x0f bit 6 + bit 1 (CLEAR+V/M all-reset); `0x2002adf0` tests offset
0x0d bit 3 + offset 0x0e bit 4 (MENU+EXIT touch calibration); `0x2002ae38` tests offset 0x0d
bits 3+4 (MENU+FUNCTION service mode). All three combos check out against the table below, so
the table itself is ✅ even though most individual byte.bit → key-name pairings are 🟢.

| Code | Key | byte.bit | Command ID |
|---|---|---|---|
| 0x01 | MIC UP | 0x11.7 | 3 |
| 0x02 | MIC DN | 0x11.6 | 4 |
| 0x03 | TRANSMIT | 0x0d.0 | 5 |
| 0x04 | TUNER | 0x0d.1 | 6 |
| 0x05 | RIT | 0x0f.4 | 0xc |
| 0x06 | ΔTX | 0x0f.5 | 0xd |
| 0x07 | CLEAR | 0x0f.6 | 0xe |
| 0x08 | XFC | 0x0d.7 | special (0x2d, 0x2002f0c8; also read as a held level) |
| 0x09 | A/B | 0x0f.0 | 0x11 |
| 0x0a | P.AMP/ATT | 0x0e.0 | 0x4c |
| 0x0b | V/M | 0x0f.1 | 0x12 |
| 0x0c | SPLIT | 0x0f.7 | 0x13 |
| 0x0d | M-CH UP | 0x0f.2 | 0x14 |
| 0x0e | M-CH DN | 0x0f.3 | 0x15 |
| 0x0f | MENU | 0x0d.3 | 0x1c |
| 0x10 | NOTCH | 0x0e.1 | 0x33 |
| 0x11 | QUICK | 0x0d.6 | 0x39 |
| 0x12 | VOX/BK-IN | 0x0d.2 | 7 |
| 0x13 | NB | 0x0e.2 | 0x23 |
| 0x14 | NR | 0x0e.3 | 0x25 |
| 0x15 | EXIT | 0x0e.4 | 0x1e |
| 0x16 | SPEECH/LOCK | 0x0e.6 | 0x2c |
| 0x17 | MPAD | 0x0e.7 | 0x1a |
| 0x18 | AUTO TUNE | 0x0e.5 | 0x2f |
| 0x19 | PBT-CLR | 0x10.3 | 0x32 |
| 0x1a | MULTI push | 0x10.2 | 0x40 |
| 0x1b | M.SCOPE | 0x0d.5 | 0x21 |
| 0x1c | FUNCTION | 0x0d.4 | 0x1d |
| 0x1d/0x1e | synthetic touch-press / touch-hold | — | from `FUN_20064944` |
| 0x1f–0x22 | external keypad 1–4 | 0x11.5 / 0x11.4 / 0x11.3 / 0x11.2 | gated by settings 0x203de4cc+0x55/56/57 |
| 0x23/0x24 | auto-repeat M-CH UP/DN | — | `FUN_2002fa9c` |
| 0x25/0x26 | auto-repeat mic UP/DN | — | `FUN_2002fa9c` |

POWER is not in this report 🟢. Live-verified: MENU opens the menu, EXIT closes it ✅.

**Correction of an earlier read:** `notes/ui-menu.md` and `notes/front-panel-firmware.md`
previously stated MENU = key code 9 and QUICK = key code 12. Those were FRONT CHECK MODE's
*list-position numbers* (a display sequence), not the key codes above. See those files' own
2026-09-24 correction notes.

## 5. Long press ✅

`FUN_2002fd44` (misnamed `ui_queue_screen_open_request`) arms a hold: it's given a key bit
index, a threshold of 0x54 ticks, and release/hold callbacks. `FUN_2002eec8` re-tests the live
bit every tick to see if it's still held.

## 6. Touch ✅

Chain: buffer offsets 0x13–0x17 → `FUN_200640c8` → `FUN_20064944` (state 0x203fc708) → hit-test
`FUN_20064884`/`FUN_20064120` against rects `{u16 id, x0, x1, y0, y1}` in screen pixels → key
codes 0x1d/0x1e. Pressed is a level, sampled every tick, not an edge.

Calibration is done by the RL78, not the main CPU 🟢: the calibration screen (0x20041e18) reads
raw coordinates (tag 1), accepts 4 points inside windows at 0x20199ae8 (pt0 X 3449–4095
Y 3053–4095; pt1 X 0–697 Y 3053–4095; pt2 X 3449–4095 Y 0–917; pt3 X 0–697 Y 0–917), writes them
BE16 `(x,y)×4` into the outbound TX file at 0x203dca54+0x10..0x1f, and sends it. `TX[0]=1` while
calibrating (set/cleared at 0x20041d8c/0x20041dd8, misnamed
`scif3_status_svcmode5_flag_set`/`_clear`).

Live-verified: touching (146, 90) on the MENU screen opens the audio scope ✅.

Hardware note (service manual block diagram): IC152, a UC6528, is the resistive touch
controller (XR/YD/XL/YU analog in; a `TW*` serial link to IC501). The same diagram gives the
main dial as quadrature `MAINDAK`/`MAINDBK` and MULTI as quadrature `MFDAK`/`MFDBK` plus a push
contact `MFK`. Its push-switch list is not reliable — it omits VOX/BK-IN, PBT-CLR, FUNCTION and
EXIT, and lists SET, M-CH UP/DN, TS, and CLEAR twice.

## 7. Boot handshake ✅

Status byte `0x203902d3`: bit 0x80 = identify pending (send `F0`); bit 0x40 = send `F1` after
an `F0` reply; bit 0x20 = send the full 33-byte TX file; bit 0x10 = send a changed range only;
bit 0x08 = send a 0x13-byte frame at offset 0x21; bit 0x02 = TX busy. The main CPU sends `F0`,
expects `F0` back; then sends `F1` plus the full TX frame; a second wait loop allows up to 12
ticks for any data frame back, which should carry the front-CPU version bytes.

## 8. Emulator model ✅

`qemu-machine/src/scif.c` (commit 9a1fe0c) models a fake front panel: a 32-byte mirror of the
register file, with frames delivered through a paced queue (1 ms apart) by precomputing the RX
state machine's end state and delivering only the trailing `0xFD` byte — the same technique an
older comment in the file explains for a different link. The first outbound data frame carries
the full power-on report (version `"100"`, touch tag 0xFF/no-touch, AF 0x80, RF/SQL 0x60,
overridable with `RZA1H_FP_VERSION` / `RZA1H_FP_AF` / `RZA1H_FP_RFSQL`); later frames are a
1-byte echo of whatever changed in the mirror.

A control chardev, `-chardev socket,id=fpctl,path=P,server=on,wait=off`, is wired into `SCIF3`
in `rz_a1h.c`. Line protocol: `get` | `w OFF HEX` | `bit OFF BIT 0/1` | `add8 OFF N` |
`add16 OFF N` | `touch X Y` | `release`. `qemu-machine/tools/fp.py` wraps it:
`fp.py press MENU`, `fp.py dial +20`, `fp.py touch 240 136`, `fp.py state`,
`fp.py seq "..."`; `fp.py keys` lists all known key names. In the GTK window, the mouse's left
button doubles as a touch press (`RZA1H_FP_NO_MOUSE=1` disables that). `tools/run_gui.py` opens
the control socket at `/tmp/qemu_run_gui_fp.sock`.

## Open questions

- Buffer bytes 0x00 and 0x12, and offset 0x10 bits 0/1: no consumer found yet.
- The real "no touch" tag value (0xFF is the emulator's choice, not confirmed against hardware).
- Which of MIC UP/DN is actually 0x01 vs 0x02 — the byte.bit assignment is confirmed, the
  physical up/down direction is not.
- The tick period of the key scan (ticks/second, not yet measured against a clock).
- Full TX (outbound) file layout: `TX[1]` looks like LED bits (POWER/TX/RX LEDs at 1/4/2 per
  FRONT CHECK's own conventions); `TX[3]` looks backlight-like. Neither is confirmed.
- Why the real radio's default RF/SQL setting doesn't show an "RFG" indicator on screen at the
  emulator's chosen default (0x60) — may just be a UI-threshold difference, not investigated.
