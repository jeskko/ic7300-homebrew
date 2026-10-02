# `sdk/` — writing homebrew apps for the IC-7300

An app is a plain C program. You build it into a `NAME.BIN`, put it in `\homebrew\` on the SD
card, and start it from **MENU > SET > SD Card > Homebrew Apps**. Behind that menu row is a
one-time **loader firmware** (`loader/`): stock v1.42 with a small loader appended, installed
once like any firmware update. After that, installing an app is just copying a file.

> **Emulator only so far.** None of this has run on a real radio. Read the warning in the
> top-level [README](../README.md#scope) before flashing anything: you need a verified way to
> re-flash a radio that no longer boots.

## Quick start (in the emulator)

After the one-time emulator setup in the top-level README (`qemu-machine/setup.sh`, your own
`7300_142.dat` in `firmware/`), from the repo root:

```
D=scratch/homebrew; mkdir -p $D
python3 sdk/loader/build.py                                   # -> scratch/hb_loader_142.dat
python3 qemu-machine/tools/build_flash.py scratch/hb_loader_142.dat $D/flash.bin
python3 sdk/tools/build_app.py -o $D/HELLO.BIN sdk/examples/hello-gui/main.c
python3 qemu-machine/tools/build_sdcard.py -o $D/sdcard.img --size-mb 128
mmd   -i $D/sdcard.img@@1M ::homebrew
mcopy -i $D/sdcard.img@@1M $D/HELLO.BIN ::homebrew/
python3 qemu-machine/tools/run_gui.py --no-pwrk --icount off --flash $D/flash.bin --sd $D/sdcard.img
```

In the window: MENU > SET > SD Card, page to **Homebrew Apps**, tap **HELLO**. Use
`--icount off` for anything touching the SD card (heavy mounts stall under `-icount`).

A minimal app:

```c
#include "hb/app.h"

int main(void)
{
    ui_message_box("Hello, world!");    /* the radio's own dialog; returns after OK */
    return 0;
}
```

## How an app runs

- `main()` runs on the radio's UI thread, but on its own 16 KB stack, as a coroutine. A blocking
  call (`ui_message_box()`, `hb_wait_until()`, `hb_yield()`) switches back to the firmware, which
  carries on normally, and resumes your code on a later UI loop pass. The radio keeps working while
  your app is up. Don't busy-wait: a loop that never yields freezes the UI.
- The image (code, data, bss, stack) may be up to 1 MB, plus a 448 KB heap. It lives in RAM the
  firmware never uses.
- When `main()` returns, the runtime puts everything back: the screen, input, the audio tap.
- The loader and an app must agree on the ABI version (`hb/abi.h`, currently v4). Rebuild apps
  when the loader changes.

## API

| Header | What it gives you |
|---|---|
| `hb/app.h` | `main()` conventions, `hb_yield()`, `hb_wait_until()`, `ui_message_box()` (firmware popup, up to 6 lines + OK) |
| `hb/gfx.h` | A full-screen 480×272 RGB565 double-buffered canvas over the radio's UI: `hb_gfx_open/present/close`, `hb_clear`, `hb_fill_rect`, `hb_line`, `hb_fill_triangle`, `hb_text` (built-in 5×7 font, any integer scale) |
| `hb/input.h` | Touch in screen pixels (`hb_touch_read`), front-panel keys (`hb_key_down`), a ms clock |
| `hb/heap.h` | `hb_malloc` / `hb_calloc` / `hb_realloc` / `hb_free` |
| `hb/audio.h` | Receive audio, 12 kHz mono int16, gap-filled (`hb_audio_open/read/close`) |
| `hb/math.h` | `hb_sinf`, `hb_cosf` (there is no libm) |
| `hb/abi.h`, `hb/firmware.h` | The loader ABI and the stock-firmware addresses the runtime uses (1.42 only) |

No libc beyond what `runtime/libc.c` provides. Build with `sdk/tools/build_app.py`, which needs
`arm-none-eabi-gcc` on `PATH`.

## Examples

| Example | Shows |
|---|---|
| [`hello-gui`](examples/hello-gui/) | The smallest app: one firmware dialog |
| [`about-box`](examples/about-box/) | Two blocking dialogs in a row |
| [`cube`](examples/cube/) | The graphics canvas and touch: a spinning, shaded 3D cube |
| [`minesweeper`](examples/minesweeper/) | A full touch game drawn as raw raster, with text and keys |
| [`sstv-rx`](examples/sstv-rx/) | The audio tap: a Scottie/Martin SSTV receiver |

`civ-hello-world`, `sd-card-app` and `homebrew-apps-menu` are the earlier proofs of concept the
loader replaced. They're kept for the record; don't start new work from them.

## Limits (today)

- Firmware **v1.42 only**: every firmware address the loader and runtime use is specific to it.
- At most 14 apps in the picker; names are 8.3 (`SnakeGame.BIN` shows as `SNAKEG~1`).
- The dials aren't grabbed: while an app is up, turning them still tunes the radio.
- Real-hardware unknowns: cache maintenance before jumping into a freshly loaded app, frame rate,
  resistive-touch slop, audio block loss. See [`loader/README.md`](loader/README.md), "Open items".

## Docs

- [`loader/README.md`](loader/README.md) — the loader's patches, the app picker, the ABI, the
  coroutine runtime, graphics and input internals, the end-to-end emulator test, open items.
- [`api/`](api/) — per-subsystem reference (task model, CI-V, display, input, filesystem, audio,
  settings), each backed by the RE notes in `../notes/`.
- [`app-requirements.md`](app-requirements.md), [`roadmap.md`](roadmap.md),
  [`app-loader-design.md`](app-loader-design.md), [`sstv-app-design.md`](sstv-app-design.md) —
  design and planning.
- [`README-history.md`](README-history.md) — how the SDK came together.

`sdk/` is the forward-looking half of the project: design decisions and plans live here, while
confirmed facts about the stock firmware belong in `../notes/` (and get cited from here). When RE
work in `notes/` answers one of the open questions in `app-requirements.md` or `roadmap.md`,
update the `sdk/` doc at the same time.
