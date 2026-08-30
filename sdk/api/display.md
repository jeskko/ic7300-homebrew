# Display: putting pixels on screen

Two layers exist, at very different levels of sophistication — the research question for a first app is
mainly "how much of the heavy one can be skipped." See `app-requirements.md`'s App 2/3 for the per-app
framing.

All addresses below verified against the live Ghidra project (`body.bin`) 2026-08-30.

## ✅ The simple path: icon blitting

**`icon_blit_by_id_v1`** (`0x200ae4d4`, verified) / **`icon_blit_by_id_v2`** (`0x200b0294`, verified, adds
x/y scale params) blit a pre-formatted bitmap to screen. Both bounds-check the icon ID against `708`
before dispatching into an internal 2D command-queue blitter (`notes/bitmaps.md`).

**`g_icon_table`** (`0x20335234`, verified) — 708-entry pointer array, each entry pointing at a 32-byte
header immediately followed by that icon's pixel data:

```
+0x00  ?
+0x04  u32 data_offset   (always 0x20 — header size; data starts right after)
+0x08  ?
+0x0c  u16 width
+0x0e  u16 height
+0x10..0x1f  ?
+data_offset  pixel data, BGRA8888, row stride = ((width+3)>>2)<<4
```

319 unique icons back the 708 table slots; 150 of 319 are individually identified in
`notes/icon_table.csv`. An app doesn't need to use an existing icon — a hand-built bitmap in this exact
format (32-byte header + BGRA8888 pixel data, padded-stride) works too, per `notes/bitmaps.md`'s
consumer analysis. This is the fastest realistic path to pixels on screen — a Tetris-style board can
plausibly be built entirely from small solid-color synthesized icons (`app-requirements.md`'s App 3),
sidestepping the heavier stack below entirely.

## 🔎 Open: which buffer does the blit actually write into?

**Not yet confirmed** — is it the live on-screen EGL window surface (blit shows up immediately), the
960×552 off-screen pixmap (which has *no* confirmed path to the display at all — see the next section),
or some other intermediate buffer? This is flagged in `app-requirements.md` as the single most valuable
next fact for the whole display/graphics SDK area — nothing else here is more load-bearing.

## ✅ The real stack: EGL + OpenVG

**`graphics_stack_startup_egl_openvg`** (`0x20079240`, verified) brings up a genuine EGL+OpenVG context at
boot, called from **`ui_graphics_lifecycle_task`** (`0x2007ef5c`, verified) as that task's first action.
Creates a real 480×272 on-screen window surface (the actual touchscreen resolution) plus a 960×552
off-screen pixmap surface (exactly 2× the window — purpose, supersampled render target vs. tiled canvas,
not confirmed). Confirms pixels-on-screen is achievable via a real, standard graphics API in principle —
but see the two open questions below before planning to use it directly.

## 🔎 Open: is the EGL/OpenVG context safely shareable with a second task?

Whether a new task can call real Khronos-shaped API calls (`eglCreateWindowSurface`, `vgDrawPath`, etc.)
into the already-running context, or whether it's effectively owned/private to `ui_graphics_lifecycle_task`
and not safely shareable, has not been examined. The library's own exported function table/addresses
haven't been catalogued either — this whole path needs real investigation before it's usable by an app,
unlike the icon-blit path above.

## 🔎 Open: the VDC5 display controller itself is unresearched

The RZ/A1H's VDC5 peripheral (the actual hardware framebuffer driver underneath EGL) has zero register
addresses or framebuffer base/format/stride derived independently of the EGL layer. Not necessarily needed
if the icon-blit path pans out for direct pixel access, but would matter for anything wanting to bypass
existing app-level primitives entirely.

## Also confirmed, not yet connected to a display API

`native_show_window_multi_display` genuinely supports attaching a window to multiple simultaneous displays
via a runtime bitmask — real evidence of planned multi-display support (`notes/kernel-rtos.md`), though
whether more than one display is ever actually active needs live hardware to settle, not more static
reading.
