# `hello-gui` — the first homebrew GUI app

```c
#include "hb/app.h"

int main(void)
{
    ui_message_box("Hello, world!");
    return 0;
}
```

Picking **HELLO** from **MENU → SET → SD Card → Homebrew Apps** opens the radio's own popup dialog with our
text and an OK button. `ui_message_box()` blocks until OK is tapped, then `main()` returns and
the app exits. The rest of the radio keeps running the whole time.

![Hello, world! dialog over the SD CARD menu](screenshots/hello-dialog.png)

After OK the dialog closes and the SD CARD menu is back, exactly as before the tap:

![SD CARD menu after OK](screenshots/after-ok.png)

It's plain C built against the SDK (`sdk/include/hb/`, `sdk/runtime/`). It runs on top of the
one-time loader firmware in [`sdk/loader/`](../../loader/). How it works is described in
[`sdk/loader/README.md`](../../loader/README.md): coroutine runtime, APP.BIN header, idle tick,
borrowed dialog.

## Build and run in the emulator

```
python3 sdk/loader/build.py                        # -> scratch/hb_loader_142.dat (flash once)
python3 sdk/tools/build_app.py --keep sdk/examples/hello-gui/build \
    -o scratch/homebrew/HELLO.BIN sdk/examples/hello-gui/main.c
```

Copy `HELLO.BIN` into `\homebrew\` on the card, then pick **HELLO** from SD Card > Homebrew Apps.
`sdk/loader/README.md` has the full emulator recipe and the end-to-end test
(`sdk/loader/test_emu.py`), which launches this app alongside `about-box`.

On real hardware: install `hb_loader_142.dat` once with SET > SD Card > Firmware Update. **Read the warning in the top-level [README](../../../README.md#scope) first** — nothing here has run on a real radio, and you need a verified way to re-flash a radio that no longer boots before trying.
**Not yet tried on real hardware** (see the loader README's open items).

## What was verified (2026-09-25, `qemu-machine`, `--icount off`)

This was verified against loader v1, which loaded a fixed `C:\IC-7300\APP.BIN`. The same checks
now run through the app picker in `sdk/loader/test_emu.py`. The test then lived here as
`test_emu.py`; it booted the loader firmware, tapped through the real menus like a user would,
and read guest memory over QMP with a screenshot at each step. All checks passed:

- The main screen comes up normally with the loader's `main_idle_loop` hook in place.
- Before the tap, no app is resident (loader `idle_hook == NULL`).
- On the tap, dialog item `0x66` is up with the app's `on_ok` as its OK callback. The
  borrowed record's line 0 reads `Hello, world!` and its button slot reads `OK`.
- While the dialog is up, the app is suspended inside `hb_wait_until` (`idle_hook == hb_idle`)
  and `main()` hasn't returned.
- On OK, the dialog closes (`active_item == 0`) and `on_ok` has fired. `main()` returns,
  `idle_hook` is cleared, and the stock dialog's text pointers are restored.
- **A second launch in the same session behaves identically.** Every check above passes again,
  so the runtime re-zeroes its state and rebuilds the coroutine each time.
- EXIT back to the main screen works; the radio stays responsive.
- **Fail-closed**: a card with no `APP.BIN`, and a card holding the old headerless
  `sd-card-app` binary, both make the tap a no-op. There's no dialog, nothing stays resident,
  and the UI stays responsive. The old binary is refused by the header check; this test
  shows only that nothing observable happened.

## One mistake caught along the way

The first build rendered `OK` as a second text line and left the button blank. My first dump
of the message-record table had misread its layout; the corrected layout is in
`notes/ui-menu.md` and `sdk/include/hb/firmware.h`. Button labels live in fixed slots 6/7, not
"the lines after the text". Even that broken build proved the rest of the path: the blank
button still dismissed the dialog, the app resumed, and the stock text was restored.
