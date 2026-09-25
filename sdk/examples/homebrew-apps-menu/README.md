# `homebrew-apps-menu` — a real, visible "Homebrew Apps" menu button

Live-tested 2026-09-25 in `qemu-machine`, driven through the real touchscreen UI (not a
shortcut). This is the other half of the user's original ask that
[`civ-hello-world`](../civ-hello-world/) and [`sd-card-app`](../sd-card-app/) didn't cover: both
of those trigger via a hidden front-panel key combo. This one adds a **real row in the real SD
CARD menu** that does the same SD-card app load `sd-card-app` does — MENU → SET → SD Card →
"Homebrew Apps".

## What it does

![SD Card menu, page 1 of 3](screenshots/sd-card-menu-page1.png)

Tapping **SET → SD Card** now shows **3 pages** instead of the stock 2 — the new row lives alone
on the third:

![SD Card menu, page 3 of 3, showing Homebrew Apps](screenshots/sd-card-menu-page3-homebrew-apps.png)

Tapping it opens `C:\IC-7300\APP.BIN` from the SD card, reads it, and runs it — exactly
`sd-card-app`'s own mechanism, just triggered by a real menu tap instead of a key combo.
`sd-card-app/app_main.s` is reused **completely unmodified** as the loaded app.

## How it's installed — two data patches, no instruction touched

Unlike the other two examples (which retarget one `main_idle_loop` instruction), this one only
ever changes **data**, found by tracing the real menu-list engine (`notes/ui-menu.md`'s "SET-style
settings-list engine" section, traced the same day):

- **`g_settings_category_registry[0x18]`** (`0x20199500`, the SD CARD menu's own entry): item
  count `8 → 9`, list pointer retargeted to a new 9-entry copy (the original 8 items, unchanged,
  plus one new one) appended alongside the hook code.
- **One new 20-byte record** in `g_settings_item_catalog` (`0x2018ed48 + 0x881×20 =
  0x2019975c`), inside a padding gap confirmed unused by anything else: `{action =
  homebrew_menu_action, query = NULL (always selectable), flags = 0x00010700 (copied from the
  real "Format" row), en = jp = "Homebrew Apps"}`.

`homebrew_menu_action` (`menu_hook.s`) is called with **no arguments** — the exact convention
`settings_list_activate_row` uses to tail-call any type-3 row's action — and just does
`sd-card-app`'s open/read/close/call sequence, then returns. Because it never calls
`operating_mode_change_dispatch` the way a screen-switching row (like Format or Firmware Update)
does, tapping it doesn't navigate away — the SD CARD menu just stays where it was, which is
exactly the right behavior for "run this app" as opposed to "go to this settings screen".

## Building and testing

```
python3 sdk/examples/homebrew-apps-menu/build.py
# -> scratch/homebrew_apps_menu_142.dat  (flash this once)
# -> scratch/APP.BIN                     (drop this on the SD card, same as sd-card-app)
```

Then boot and put `APP.BIN` on the card exactly as in `sd-card-app/README.md`. No key combo —
navigate for real: **MENU → SET → (page 2 of 2) SD Card → (page 3 of 3) Homebrew Apps**.

## What was actually observed (2026-09-25 session)

Driven through the real UI via QMP input events and `tools/fp.py`, with `screendump` screenshots
at each step (not just CI-V — the actual rendered screen, confirming nothing else broke):

- Main screen renders normally before touching anything.
- MENU → SET → SD Card: the list genuinely shows **"1/3"** where stock firmware shows "1/2" —
  the item count patch took effect, pagination adjusted itself automatically (this project's own
  code recomputes page count from the registry's count field, not a fixed constant).
- Pages 1–2 show the original 8 items, unchanged, in the original order.
- Page 3 shows exactly one row: **"Homebrew Apps"**, correctly labeled, no rendering glitches.
- Tapping it produces the identical CI-V frame `sd-card-app` produces:
  `fe fe e0 94 51 53 44 41 50 50 fd` (`cmd 0x51 "SDAPP"`) — the SD-card app genuinely ran.
- The menu screen stays on "Homebrew Apps" (selected), no crash, no stray dialog.
- Backing out with EXIT navigates normally all the way to the main screen.
- `civ 03`/`civ 04` (read frequency/mode) both get normal replies throughout and after.
- **Fail-closed, tested with an empty SD card**: boots fine, the menu still shows 3 pages with
  "Homebrew Apps" on page 3, tapping it produces no frame at all, and the radio remains fully
  responsive to CI-V commands afterward.

## Known limitations

- Only one app row, one fixed path, same 32 KB cap as `sd-card-app` — no real app-management UI
  (an app picker, multiple installed apps) yet.
- `-icount` note: not independently re-tested here, but see `civ-hello-world/README.md` — the
  other two examples both hang under `-icount` and work under plain unthrottled execution; this
  one was tested with `--icount off` from the start on the same basis.
- The new catalog record's index (`0x881`) and its home in the post-registry padding gap were
  chosen based on static analysis of the 1.42 image specifically — re-verify both before reusing
  this technique against a different firmware version.
