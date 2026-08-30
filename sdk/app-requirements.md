# Custom apps: what else needs researching, per example app

Started 2026-08-30, following `sdk/roadmap.md`'s Phase 2/3 (app design/build). Scope of
this file: for four representative example apps, what RTOS/hardware-interface/graphics-library knowledge
this project already has vs. what still needs research — **deliberately excluding** the app-launching
mechanism and memory placement (Phase 2's own open questions), since that's expected to get much easier
once a full live-device memory dump is available via JTAG. Ordered roughly by dependency (each app builds
on the previous one's requirements).

**See also `sdk/api/`** (added 2026-08-30, a synthesis sweep of `notes/*.md` into per-subsystem reference
docs) — `task-model.md`, `serial-civ.md`, `display.md`, `input.md`, `filesystem.md`, `audio.md`,
`settings.md`. Each open question below has a matching, more detailed writeup there with verified function
addresses; this file stays the per-app view, `api/` is the per-subsystem view of the same underlying facts.

## Shared infrastructure every app needs

- ✅ **Task creation**: fully understood. `itron_act_tsk` trampolines activate a task from a
  `{entry_point, priority, flags, stack_size}` 16-byte descriptor (`notes/kernel-rtos.md`'s task catalog) —
  a custom app's own task can very plausibly be spun up the same way once a descriptor can be placed
  somewhere the scheduler reads it (the out-of-scope "launching" question).
- 🔎 **Privilege/ASID model — needs research**: this port has real Icom customizations beyond stock
  FreeRTOS, specifically **unprivileged user-mode tasks with per-task ASID/MMU isolation**
  (`notes/kernel-rtos.md`'s RTOS-identity section). Every example app below eventually calls into shared
  subsystems (SCIF drivers, the graphics stack, the SD filesystem) that existing tasks reach via
  established call paths — not yet confirmed whether a *new* task needs anything special (a syscall-style
  trampoline, a specific ASID assignment, a privilege elevation) to call the same functions, or whether
  ordinary direct calls work as long as the task is created through the normal mechanism. Real open
  question, worth settling early since it affects every app equally.
- ✅ **Task-local timing**: FreeRTOS tick/delay primitives are available (matched directly against the
  real R01AN5093 FreeRTOS source, `notes/kernel-rtos.md`) — no additional research needed for basic game
  loop / periodic-task timing.

## App 1 — Hello world to a serial port

**Closest to already-buildable of the four.** Two ways to get text out:

- ✅ **Reuse the CI-V transport as-is.** The full RX *and* TX path is understood:
  `civ_frame_rx_statemachine` for RX framing, and a real TX buffer/preamble-building mechanism already
  traced on the CI-V dispatcher work (`notes/kernel-rtos.md`'s CI-V section — the same `FE FE <dst> <src>`
  framing, sent out through the driver on genuine command replies). A "hello world" that just emits text
  as a CI-V-framed message over the existing REMOTE/USB link needs **no new hardware research at all** —
  every piece (SCIF0 driver init, TX framing, the physical REMOTE-jack/USB path) is already confirmed.
  This is the standout candidate for a genuine first working "app," even before the launching-mechanism
  question is settled, since it could plausibly be tested as a modification to an *existing* handler
  first.
- 🔎 **A truly independent UART "hello world"** (not piggybacking on CI-V's own framing/semantics) needs
  one thing confirmed: the exact low-level SCFTDR/SCFSR polling or the shared TX-queue mechanism
  (`FUN_20011598`, seen called from multiple SCIF0-adjacent sites — plausibly a generic "queue this byte
  for TX on whichever channel" helper, exact register-level behavior not individually re-derived) needs a
  clean, standalone trace so a new task could call it directly without going through the CI-V frame-state
  machine. Low effort — this function is already located, just not documented as its own primitive yet.
- 🔎 **Coexistence**: SCIF0 is *actively used* for real CI-V traffic. A custom app wanting to share it
  needs to either queue politely alongside existing traffic (using the shared TX primitive above, which
  should already serialize correctly) or accept that a naive "hello world" might garble a legitimate CI-V
  session in progress — worth deciding by which failure mode is acceptable for a first test.

## App 2 — Hello world on the display

Builds on genuine confirmed infrastructure, but at a level of sophistication ("real EGL/OpenVG") that's
more than a minimal hello-world needs — the research question is really "how much of that can we skip."

- ✅ **The graphics stack is real and already running**: `graphics_stack_startup_egl_openvg` brings up a
  genuine EGL+OpenVG context at boot (`ui_graphics_lifecycle_task`, `notes/kernel-rtos.md`), creating a
  real 480×272 on-screen window surface (the actual touchscreen resolution) plus a 960×552 off-screen
  pixmap. This proves pixels-on-screen is achievable in principle.
- ✅ **A simpler, already-understood primitive exists**: `icon_blit_by_id_v1`/`_v2`
  (`notes/bitmaps.md`) blit a 32-byte-header-plus-BGRA8888-pixel-data icon (708 of them catalogued and
  individually identified) to screen, with a known, simple pixel format (tightly packed, row stride
  padded to 4 pixels). **This is very plausibly the fastest realistic path to "hello world on the
  display"** — if an app can call this existing function with a target coordinate and one of the 708
  known icons (or a hand-built same-format bitmap), it sidesteps needing to touch EGL/OpenVG at all for a
  first test.
- 🔎 **Not yet confirmed**: what buffer `icon_blit_by_id_v1`/`_v2` actually writes into — is it the live
  on-screen EGL window surface directly (meaning a blit shows up immediately), the 960×552 pixmap (which
  per the graphics-lifecycle finding has *no* confirmed path to the display at all — see
  `notes/kernel-rtos.md`'s multi-display investigation), or some other intermediate buffer? Settling this
  is the single most valuable next step for this app specifically.
- 🔎 **The display controller itself (RZ/A1H's VDC5 peripheral) has not been researched at all** in this
  project — no register addresses, no framebuffer base/format/stride derived independently of the EGL
  layer. Not necessarily needed if the icon-blit path pans out, but would matter for anything wanting
  direct pixel access without going through existing app-level primitives (relevant for Tetris below,
  which needs more than pre-baked icons).
- 🔎 **Whether a new task can call into the already-running EGL/OpenVG context** (real Khronos-shaped API
  calls like `eglCreateWindowSurface`/`vgDrawPath`) or whether that context is effectively owned/private to
  `ui_graphics_lifecycle_task` and not safely shareable from a second task — not examined. The EGL/OpenVG
  library's own exported function table/addresses haven't been catalogued either.

## App 3 — A simple game (e.g. Tetris)

Builds on App 2's display access plus real-time input, which is the substantial new research area here.

- 🔎 **Front-panel input is only partially decoded.** `scif3_frame_dispatch_by_type`
  (`notes/front-panel-firmware.md`) parses front-panel packets into a shared status buffer, but **only 2
  of up to 32 message types have any field meaning attached** (the MENU+FUNCTION combo, from the
  service-mode investigation). A game needs at minimum: main-dial rotation (direction + step count),
  and some kind of confirm/rotate/drop action (a soft key, physical button, or touchscreen tap) — none of
  these specific message types are decoded yet. This is the largest concrete research gap for this app.
- 🔎 **The touchscreen controller is unidentified.** `IC152` (`UC6528XBNQ4GRC`) is flagged in the hardware
  BOM as "likely display/touch controller — function not confirmed" (`notes/ic7300-hardware.md`) — its
  actual protocol (I2C? SPI? which main-CPU pins?) hasn't been traced at all. Not required if the game
  uses only the physical main dial + a couple of buttons instead of touch, which is a reasonable scope
  reduction for a first version.
- ✅ **Simple shape drawing**: if the icon-blit path from App 2 pans out, a Tetris board can likely be
  built entirely from small solid-color square icons (easy to synthesize in the confirmed BGRA8888
  format) without needing real vector drawing — avoids depending on the less-understood OpenVG path
  entirely for a first version.
- ✅ **Game loop timing**: covered by the shared FreeRTOS primitives above — no new research needed.
- ⏭️ **Sound effects**: deliberately worth deferring — would need understanding the DSP-side audio
  playback pipeline (SSIF0/1, `notes/ic7300-signal-chain.md`), which is real infrastructure but adds
  scope without being essential to a first playable version.

## App 4 — An SSTV receiving app

Builds on App 2's display access; the substantial new area is getting at live receive-audio samples.

- 🔎 **The single biggest open question: where do live, receive-path demodulated audio samples actually
  live, and how would a new task read them?** The best existing lead is `voice_recording_file_task`
  (`notes/kernel-rtos.md`) — it already streams *some* audio to the SD card via a 4-slot ring buffer, but
  its own audio **source** (which SSIF channel, what sample rate/bit depth, RX-demodulated audio vs. the
  mic/TX path) was never traced back to its producer — the task's own file-I/O side is fully understood.
  the manual's `1A 05 01 82`-family "QSO recorder"/audio-source-select commands (`notes/kernel-rtos-history.md`'s
  CI-V table capture) confirm Icom has a real, configurable "which audio to record" feature at the
  protocol level, meaning a single shared audio-sample subsystem plausibly exists and is a good next
  place to look for a tappable RX-audio buffer. This is genuinely the load-bearing question for this app
  — everything else follows once real audio samples are reachable.
- ✅ **The SSTV decode algorithm itself needs no Icom-specific research** — detecting the ~1200 Hz sync
  pulse, mapping the 1500–2300 Hz tone range to per-pixel luminance, and handling the timing for common
  modes (Robot36, Martin, Scottie) is standard, publicly documented ham-radio DSP technique, independent
  of anything reverse-engineered here.
- ✅ **Displaying the decoded image**: same requirements as App 2, already covered above.
- 🔎 **Real-time budget**: not yet assessed whether a task on the *main* CPU (as opposed to the DSP,
  which is already busy doing the actual demodulation) has enough spare cycles to run SSTV decode
  continuously without falling behind — plausible given the RZ/A1H's clock speed and this being a fairly
  light DSP task by modern standards, but worth a real check once the audio-tap question is settled and
  a task can be tested.

## Summary table

| App | Display needed | Input needed | New HW/RTOS research required |
|---|---|---|---|
| 1. Serial hello world | no | no | Low — CI-V path is fully ready; standalone-UART variant needs one primitive documented |
| 2. Display hello world | yes (minimal) | no | Medium — confirm icon-blit's target buffer; VDC5 itself not required if that pans out |
| 3. Tetris | yes | yes (real-time) | High — front-panel packet decoding for game controls is the main gap; touch is optional/deferrable |
| 4. SSTV receiver | yes (reuse #2) | no | High, but concentrated — one big question (tap the RX-audio buffer); the decode algorithm itself is free |
