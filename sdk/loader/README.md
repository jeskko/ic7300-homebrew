# `sdk/loader/` — the homebrew loader firmware (flash once)

`build.py` takes a stock **1.42** container and produces `scratch/hb_loader_142.dat`: the
stock firmware plus `loader.c`, appended at `0x20600000` and wired in with two patches. Install
it once with SET > SD Card > Firmware Update. From then on, an app is just an `APP.BIN` on the
SD card, built from C with `sdk/tools/build_app.py` and started from
MENU > SET > SD Card > **Homebrew Apps**.

This replaces the proof-of-concept chain in `sdk/examples/{civ-hello-world,sd-card-app,
homebrew-apps-menu}`. The menu row, file I/O and fail-closed read checks are the same, and it
adds what a real app needs:

| | proof-of-concept examples | this loader (ABI v1) |
|---|---|---|
| App language | hand-written ARM assembly | C, with `sdk/include/hb/` + `sdk/runtime/` |
| APP.BIN check | none, any bytes were executed | 16-byte header: magic `HB01`, ABI version, entry and RAM end, all bounds-checked |
| App lifetime | one call, must return immediately | may run as long as it likes, via the idle tick |
| Blocking calls | impossible (would freeze the UI) | `ui_message_box()`, `hb_wait_until()`, `hb_yield()` |
| Relaunch while running | n/a | refused (no reload over resident code) |

## Patches (`build.py`, each checked against the stock bytes first)

1. **Idle tick**: `main_idle_loop`'s `bl civ_tx_pump` (`0x20052f64`) becomes `bl hb_idle_hook`.
   The hook runs `civ_tx_pump` exactly as before, then the resident app's `idle_hook`, if one is
   set.
2. **Menu row**: the same two data patches as `homebrew-apps-menu`. The SD CARD registry goes
   8 → 9 items, and a new catalog record's action is `hb_menu_action`. The list, label and
   record live in the image's confirmed-unused padding gap.

## ABI v1 (`sdk/include/hb/abi.h`)

- `APP.BIN` is loaded at `0x20610000`. Code, data, bss and stack must fit in 128 KB
  (`HB_APP_REGION`).
- It starts with `struct hb_app_header {magic, abi_version, entry, image_end}`. The loader
  refuses the file unless the magic and version match, `entry` is word-aligned inside the bytes
  actually read, and `image_end` lies within the region.
- After cache maintenance, the loader calls `entry(&api)` from the menu tap, on the UI thread.
  `api` is `{abi_version, fw_build = 0x0142, idle_hook}`. While the app leaves `idle_hook` set,
  the loader calls it once per `main_idle_loop` pass and won't load another APP.BIN.

## The app runtime (`sdk/runtime/`)

The runtime lets apps make blocking calls without freezing the UI. Everything the firmware does
runs on one UI thread, `main_idle_loop`, including touch handling, dialog drawing and the menu
action that starts the app. A `while (!ok) {}` inside the app would therefore stop the very loop
that could set `ok`.

So `crt0.S` / `runtime.c` run `main()` as a **coroutine** on its own 16 KB stack, still on the
UI thread:

1. `_hb_start` zeroes bss, builds the coroutine's first frame and switches into `main()`.
2. A blocking call (`hb_wait_until(done, arg)`) records its condition and switches back to the
   host stack. The runtime then sets `api->idle_hook = hb_idle` and returns from the menu
   action, so the firmware carries on normally.
3. Each loop pass, `hb_idle()` checks the condition and, once it holds, switches back into the
   app, which continues right after its blocking call.
4. When `main()` returns, the runtime clears `idle_hook`; the app is gone.

Every firmware call an app makes therefore happens on the UI thread, as with the stock code.
There are no cross-task races, no new RTOS task, and no open ASID question
(`sdk/api/task-model.md`).

`ui_message_box(text)` (`ui_dialog.c`) uses the firmware's own popup machinery
(`notes/ui-menu.md`, "Popup message dialogs"):

- The message table has no free record past its last entry; it runs straight into string data.
  So the call borrows the stock one-button dialog, item `0x66` ("The USB SEND/Keying settings
  were corrected." [OK]).
- It first waits for any firmware dialog to go away. Then it saves record `0x53`, swaps in our
  up-to-6 text lines plus `OK`, and calls `ui_show_message_dialog(0x66, on_ok, 0, 0)`.
- It waits until item `0x66` is no longer the active dialog, then restores the stock text.
- It returns whether OK was tapped. The OK callback just notes the tap and returns 2, as the
  stock no-callback path does.

## Open items

- **Real hardware**: nothing here has run on the radio yet. The cache maintenance before
  jumping into APP.BIN in particular is unverified (QEMU models no cache incoherency).
- **Coroutine stack vs. the RTOS**: while the app runs, the UI task's `sp` points into
  `0x2061xxxx`, outside the stack FreeRTOS allocated for it. Preemption and IRQs during
  `main()` worked in every emulator run. Stock FreeRTOS's method-1 overflow check only fires
  for `sp` *below* the task's stack base, which this is not. Whether this build enables any
  check hasn't been looked at. Worth re-checking if the UI ever dies on hardware while an app
  is up.
- **Borrowed dialog**: if the firmware itself wants dialog `0x66` while ours is up (it shows it
  after a settings load that corrected USB SEND/Keying), it would briefly get our text. Nothing
  else ever sees the swap. A dedicated record would need the message table relocated.
- Inherited from the proof-of-concept loader and still open: `fw_rpc_wait` is unbounded if the
  SD RPC ring is full; there's no SD-ready / recorder-busy gate before loading; and only one app
  (`APP.BIN`) is supported, with no picker yet.
- `--icount`: tested only with `--icount off`, like the earlier examples (open item in
  `civ-hello-world/README.md`).
