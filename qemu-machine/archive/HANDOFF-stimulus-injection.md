# Handoff: stimulus injection into `qemu-machine` (to unblock the OpenVG render frontier)

> **SUPERSEDED 2026-09-23 — kept for the trail.** No external stimulus was needed: the goal of
> this handoff (real draw traffic) was reached by fixing two automatic-path gaps instead — the
> synthetic EEPROM had "Opening Message" OFF, and VDC5 had no VLINE interrupt, so the render task
> hung after its first frame. See README.md's 2026-09-23 Status section. The CI-V / front-panel
> injection vectors below are still valid ideas for *later* (driving UI changes), just not the
> blocker they were assumed to be.

**Written for a fresh session picking up the OpenVG rendering thread.** Read
[README.md](../README.md)'s Status section and the `icom-openvg-rendering` project memory first for
the full derivation; this file is the concrete plan for the *next* move, not a re-derivation.
Created 2026-09-23.

## The one-line goal

Get **any real draw traffic** out of the firmware by delivering an external stimulus the emulator
currently has no way to inject. Right now boot reaches a healthy, continuous UI redraw loop that
presents a **blank** surface forever, because nothing ever asks the GPU to draw real content. The
emulator sits at idle with no front-panel touches, no CI-V commands, no external input of any
kind — so the "please redraw with this content" path is never exercised. Inject one, capture the
resulting OpenVG command words, and the whole "decode the GPU command stream" problem becomes
tractable from concrete examples instead of cold-reversing an undocumented ISA.

## What is already confirmed (do NOT re-derive)

- The render pipeline is **healthy and running**. `ui_graphics_buffers_init` and
  `ui_graphics_present_frame` fire every 40–100 ms; the VDC50 `GR2` plane gets a real 480×272
  RGB565 framebuffer pointer. The frame is blank by construction, not stalled.
- The OpenVG command FIFO is modeled as a completion stub in `src/openvg.c` (base
  `RZA1H_OPENVG_BASE` = `0xE8104000`, i.e. `0xE8100000 + 0x4000`; completion IRQs GIC 130–133).
  Every FIFO write completes instantly and raises the completion interrupt. **Nothing renders.**
- A 150 s capture (`tools/trace_openvg_command_traffic.py`) shows exactly **61 command words,
  all before t≈10 s** — the `FUN_20150122` bring-up/reset sequence — and **zero after**. The
  generic "push a command list" function is `FUN_2014f818` (~26 call sites, `0x2014f900`–
  `0x20152500`); none has ever been observed firing outside bring-up.
- The **render-request mailbox** an external stimulus needs to feed: an ITRON message buffer
  created by `FUN_2007ed9c`, descriptor `0x20328e0c`, runtime handle at `*(0x20390634+0x10)`.
  The redraw kick pattern (seen from `opening_screen_build_frame`) is `*DAT_200377f0 = 2;
  FUN_2007edc8();`. The render-dispatch flag itself is `0x2039064c`. The one sender found so far
  is just the initial bring-up dispatch — it draws nothing.
- The splash builder `opening_screen_build_frame` (`0x20037c10`) writes into frame buffer
  `0x20403f64` and copies the callsign from `g_my_call_text` (`0x203de53c`) — a concrete,
  correct address to watch once real content flows.

## The plan (in order)

1. **Pick an injection vector** (two good candidates below) and wire it into the machine.
2. **Fire a stimulus** that should cause a redraw with real content, and re-run
   `tools/trace_openvg_command_traffic.py`. Success = command words appearing **after** t≈10 s
   (i.e. beyond the 61 bring-up words), and/or a real message landing in the mailbox at
   `0x20328e0c`.
3. **Only then** decide the rendering approach (replicate the GPU command ISA vs. software-
   rasterize at a higher level). That decision is moot until there's a real command stream in
   hand — don't spend effort on it before step 2 produces one.

## Injection vector A — CI-V command over SCIF0 (recommended first)

**Why first:** the CI-V dispatcher is the best-understood input path in the whole project, so a
CI-V command's effect is predictable, and SCIF0 is a real UART that QEMU can drive through a
normal chardev — the least-hacky injection available.

- CI-V is **SCIF0**, `RZA1H_SCIF0_BASE` = `0xE8007000`. Its RXI GIC ID is `RZA1H_SCIF_RXI_BASE0`
  = **223** (stride 4; SCIF3's 235 = 223 + 3×4).
- Dispatcher chain (all in `notes/kernel-rtos.md`): `civ_rx_frame_stage_and_dispatch`
  (`0x2000b258`) → `civ_dispatch_lookup_validate` (`0x2000b03c`, indexes `g_civ_cmd_table` at
  `0x2018aa2c`) → `civ_dispatch_invoke_handler` (`0x2000acd8`, `g_civ_handler_table` at
  `0x2018ab84`).
- **Choose a command that changes visible state** so a redraw is actually warranted — e.g. CI-V
  "set operating frequency" (`0x05`) or "set mode" (`0x06`). A frequency change should update the
  main readout and, plausibly, kick a render request.

**Two ways to wire it:**

- *(cleanest)* Expose SCIF0 as a QEMU chardev. The machine currently runs `-serial none`; give
  SCIF0 a `chardev` (socket or pty) in `rz_a1h.c` so host-sent bytes arrive through the firmware's
  own real RX path, then send a CI-V frame from a small host script. This reuses QEMU's chardev
  plumbing and needs no per-byte hackery.
- *(reuses existing pattern)* Extend `src/scif.c`'s RX injection. It already feeds bytes back
  through the `frdr`/`rx_pending`/RXI path for the SCIF3 front-panel responder — but `irq_rx` is
  currently wired for **channel 3 only** (see the `irq_rx` field comment). Wire SCIF0's RXI (223)
  the same way and push the CI-V frame bytes one at a time, exactly like the SCIF3 canned-ack does.

## Injection vector B — front-panel event over SCIF3

**Why second:** a physical button/dial/touch event is the *most direct* cause of a screen change,
and the emulator **already has the SCIF3 RX injection path built** (the canned identify-ack). The
catch: only ~2–3 of the ~32 SCIF3 message types are decoded (`notes/front-panel-protocol-handout.md`),
so crafting a valid "button N pressed" frame is uncertain and may need trial-and-error against the
receiver `scif3_frame_rx_statemachine` (`0x20036c68`) / `scif3_frame_dispatch_by_type`.

- Reuse the exact mechanism in `src/scif.c` (feed a `0xFE…0xFD` frame byte-by-byte through the
  frdr/rx_pending/RXI path; RXI GIC ID 235; see that file's own long comment for the
  precompute-end-state trick that avoids the synchronous-RXI timing trap).
- Highest-value bonus: whatever frame format you find here directly advances SDK input decoding
  and the still-open "which function switches the visible screen" question in `notes/ui-menu.md`.

## How to measure (existing tools — don't rebuild)

- `tools/trace_openvg_command_traffic.py` — the primary success check (command-FIFO word count).
- `tools/vdc5_framebuffer_peek.py` — dump the real framebuffer to PNG; re-check for real content.
- `tools/trace_render_state_flag_writes.py`, `tools/trace_render_dispatch_loop_values.py`,
  `tools/trace_ui_render_dispatch.py` — render-loop/flag observers.
- `tools/walk_call_sites.py` — "which of these call sites was last reached"; point it at the ~26
  `FUN_2014f818` sites once a stimulus is firing, to catch which draw calls light up.
- `tools/qmp_read_mem.py` — GDB-free ground-truth memory reads (watch the mailbox at `0x20328e0c`
  / handle at `*(0x20390634+0x10)`). `gdbrsp.py` / `qemu_launch.py` are the shared helpers.

## Gotchas to respect

- **GDB perturbation is real and documented** (see the `icom-gdb-perturbation-resolved` memory):
  keep extra memory reads to a minimum, prefer QMP-only pollers for "is X happening" checks, and
  watch the watchpoint-hang gotcha (remove/step/re-arm/continue).
- **Latent OpenVG stub hang:** `openvg.c`'s `FUN_2014f73e` FIFO-hysteresis path (status bits
  `0x40`/`0x30`) has never been exercised — a fuller command queue from real draw traffic might
  finally hit it. If boot starts hanging in the GPU stub once traffic flows, that's the first
  suspect; extend the stub to model those status bits.
- **The "hidden-argument / aliased-literal" blind spot:** a write-watchpoint on the render-state
  flag `0x2039064c` previously came back empty despite the value provably being 1. If a "no writer
  found" result looks suspicious, do a raw byte-pattern search for the pointer value (it's
  duplicated across literal-pool cells; Ghidra `references_to` only follows one). Same class of
  gotcha appears repeatedly in this codebase.

## Key addresses (quick reference)

| Thing | Value |
|---|---|
| Render-request mailbox descriptor / handle | `0x20328e0c` / `*(0x20390634+0x10)` (creator `FUN_2007ed9c`) |
| Redraw kick | `*DAT_200377f0 = 2; FUN_2007edc8()` |
| Render-dispatch flag | `0x2039064c` |
| Generic "push command list" (draw) | `FUN_2014f818` (~26 sites, `0x2014f900`–`0x20152500`) |
| OpenVG FIFO base / IRQs | `RZA1H_OPENVG_BASE` `0xE8104000` / GIC 130–133 |
| Splash builder / frame buffer / callsign | `opening_screen_build_frame` `0x20037c10` / `0x20403f64` / `g_my_call_text` `0x203de53c` |
| CI-V RX dispatch / SCIF0 base / RXI | `civ_rx_frame_stage_and_dispatch` `0x2000b258` / `0xE8007000` / GIC 223 |
| Front-panel RX statemachine / SCIF3 RXI | `scif3_frame_rx_statemachine` `0x20036c68` / GIC 235 |
