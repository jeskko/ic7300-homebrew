# `qemu-machine/` — a custom QEMU machine for the IC-7300's RZ/A1H

Why this exists: `emu/` (the Unicorn-based emulator) hit a real wall — `body.bin` reaches a
genuine `WFE`-based wait loop that only a correctly-delivered periodic timer interrupt can
end, and driving Unicorn's own (already-correct) IRQ-entry code periodically via repeated
`count`-limited `emu_start` calls triggers a real, reproducible Unicorn engine bug (confirmed
independent of anything IRQ-specific — pure chunking alone corrupts ARM/Thumb state). See
`emu/README.md`'s Status section for the full story and the effort evaluation that led here.
This is not a replacement for `emu/` — that stays the fast, already-fully-working tool for
everything not interrupt-dependent (criteria 1+2 of `emu/mvp.py` need no interrupt delivery at
all) — this is the escalation path for the one thing it can't do.

See [README-history.md](README-history.md) for the full session-by-session narrative and
evidence trail behind everything below — this file carries only the current state and the
active resume point.

## Status, 2026-09-23 — the screen now DRAWS (in the command stream): boot splash + a full
## main-screen frame are rendered end to end. Two real gaps were blocking it, neither was a
## missing front-panel/DSP reply.

User's framing: a real radio shows the splash with no touch/button input, so the emulator must
be missing an automatic trigger — the handoff's "inject a CI-V/front-panel stimulus" plan was the
wrong direction. Chain, each link confirmed live:

1. **The splash was switched OFF by our own EEPROM image.** `opening_screen_build_frame`'s only
   caller, the fade driver `FUN_2002a2a4`, is called from `system_mode_request_dispatch`
   (`0x2002ac5c`) only if `*(0x203de4cc+0x6f)` (= `0x203de53b`) is nonzero (or a reset/special
   mode is active). `tools/probe_splash_gate.py` read every gate input live: all zero. A raw
   search for that pointer finds exactly one factory-reset record, item **`0x70` = "Opening
   Message"** (record `0x20192acc`, options `OFF`/`ON`, type byte 1, default at `+0x08` = **1 =
   ON** — default field confirmed from `reset_apply_item_default`'s own decompile). It sits in
   NVRAM region 2 → EEPROM `0x1a8f` (image byte `0x1a90`, the known +1 dummy-read shift; MY CALL
   is therefore EEPROM `0x1a90`/image `0x1a91`). Our all-zero image = "OFF". Fixed in
   `build_riic_eeprom_image.py` and patched into both committed images (only that byte).
2. **VDC5 had no interrupt model, so the render task hung after its first real frame.** With the
   splash ON, the first ~340 real OpenVG command words ever seen in this project appeared, then
   stopped with `*0x2039064c` stuck at 2. A QMP `pmemsave` of RAM + a scan for return addresses
   into `ui_graphics_lifecycle_task` found the blocked task's saved context: parked in
   `FUN_2007ee08` (`*0x20390634 = 1; wait(sem, forever)`) right after `eglSwapBuffers`. Its only
   signaller `FUN_2007ee20` is called only from `FUN_20073878`, which is **byte-for-byte Renesas'
   own `VDC_Ch0_vline_ISR`** (RZ/A1H software package in `scratch/`, `r_vdc_interrupt.c`):
   `SYSCNT_INT4` enable / `SYSCNT_INT1` status, bit 12 = GR3 VLINE, GIC 78. New **`src/vdc5.c`**:
   same register storage as the old plain-RAM region, plus a 60 Hz `ptimer` latching the output
   vsync/VLINE status bits (write-0-to-clear) and all 23 channel-0 GIC lines (75..97). Result:
   the whole fade-in/hold/fade-out runs, `system_mode_request_dispatch` returns past the splash,
   `main_idle_loop` is reached.
3. **Under `-icount shift=auto` the post-splash frame still stalls — an emulation-speed
   artifact, not a firmware wait.** The render task is found *preempted* (not blocked) mid-way
   through an AVL-tree removal (`FUN_2014e776`), CPU 100% busy, no idle: `main_idle_loop` is paced
   by a tick counter (`*DAT_20053138 >= 2`) and one pass of its ~70 polls takes longer than two
   ticks at the effective `shift=auto` guest speed (virtual time runs ~0.4x wall), so it never
   sleeps and starves the lower-priority renderer. **With fixed `-icount shift=1`** (2 ns/insn ≈
   the real 400 MHz A9) the stream completes cleanly: bring-up + splash frames + one ~5,200-word
   burst at t≈110-120 s (by far the heaviest frame — the main screen), ending on a complete
   `da000000 00000000` frame terminator, then ~90% idle. That is correct behaviour: with no RF/DSP
   input nothing on screen changes, so nothing is redrawn. (`RZA1H_ICOUNT=shift=1
   tools/trace_openvg_command_traffic.py 300` reproduces; ~10,600 words total.)

**Not changed yet (decision for the user):** the machine-wide default is still `shift=auto`
(`qemu_launch.py`, README "Running it"). It was chosen for the job-ring-overflow thread, whose
real cause later turned out to be the `riic.c` TEND bug — so `shift=1` may now be the better
default, but every tool/finding since was taken under `shift=auto`.

**Retracted along the way (recorded, not silently dropped):** a theory that the post-splash
stall was the OpenVG FIFO-space wait (`FUN_2014f73e`, flag bits `0x124`) — tested by also
latching status bit 2 on every command: the stream stopped at the identical word count, so it
was reverted. The latent `0x40`/`0x30` watermark path is still unmodeled but not what blocks.

**Where this leaves the rendering frontier:** there is now a real, complete command stream for
a splash + main screen to decode (the handoff's step 3). The framebuffer is still blank only
because `openvg.c` doesn't rasterize. New tools this session: `probe_splash_gate.py`,
`trace_sticky_sites.py` (sticky breakpoints, every hit logged — `walk_call_sites.py` can't
detect a site that is hit repeatedly and then parked on), `--image` on both walkers,
`RZA1H_DEBUG`/`RZA1H_ICOUNT`/image-arg overrides on `trace_openvg_command_traffic.py`.

## Status, 2026-09-21, continued once more — OpenVG rendering frontier: the render dispatch
## runs exactly ONCE around bring-up completion and produces ZERO GPU commands. There is
## currently no real command-FIFO traffic anywhere in this project to reverse-engineer.

Picked up `icom-openvg-rendering`'s suggested next step (scope real command traffic before
deciding hardware-ISA-replication vs. higher-level software rasterization). Two live,
zero/single-perturbation checks, both GDB-free or single-breakpoint:

1. **`tools/trace_openvg_command_traffic.py`** (new): a 150s PWRK-hold capture with
   `RZA1H_DEBUG=openvg`. Result: 61 total command words, ALL pushed within the first ~10s
   (the already-known `FUN_20150122` bring-up/reset sequence, decompiled this session — its
   header-word encoding is `TAG(0xA)<<28 | (count-1)<<16 | opcode`, found directly from
   `FUN_2014f818`, the real generic "push a command list" function used by ~26 call sites
   spread across `0x2014f900`-`0x20152500`, the actual higher-level OpenVG driver body).
   **Zero additional words appear all the way to t=150s.**
2. **`tools/trace_ui_render_dispatch.py`** (new): single GDB breakpoints on
   `ui_graphics_buffers_init` (`0x2007ef08`) and `ui_graphics_present_frame` (`0x2007ee68`) —
   both fire, once each, at t≈23.2s/23.9s (right when `main_idle_loop` is first reached, per
   the section below). So the render dispatch is NOT stuck/unreached — it runs on schedule,
   and (per check #1) pushes nothing to the GPU while doing so.

**Together these resolve the "why is the VDC50 framebuffer dump blank" question from
`icom-openvg-rendering`'s own confirmed starting point**: it's blank because the one and only
present-frame this boot path ever does is presenting freshly-`memset`-zeroed buffers
(`ui_graphics_buffers_init` zeros them immediately beforehand) — not because rendering is
unreachable, and not because the OpenVG stub silently drops real draw commands. **No code path
this project has ever traced actually calls whatever this driver's `vgDrawPath`/`vgClear`-
equivalent entry points are** (candidates: some subset of the ~26 `FUN_2014f818` call sites in
`0x2014f900`-`0x20152500`, not yet individually decompiled). A real screen update almost
certainly needs a live stimulus this boot never provides — a front-panel touch, a CI-V command,
or a periodic redraw tick — none of which any current tool injects.

**Consequence for the approach decision `icom-openvg-rendering` flagged (replicate the real GPU
command ISA vs. software-rasterize at a higher level)**: moot for now — there is no real command
stream to decode either way, since nothing has ever been observed asking the GPU to draw
anything. **The actual next step is producing ANY real draw traffic first** — most directly by
finding and injecting whatever event the front-panel-touch or menu-redraw path posts to the
render-request ITRON message buffer (created by `FUN_2007ed9c`, descriptor `0x20328e0c`, handle
stored at `*(0x20390634+0x10)` — no sender of a real message into it has been found yet; the one
sender identified so far is the initial bring-up dispatch itself). Once real command words are
observed, decoding a handful of live-captured opcodes is a far more tractable target than
reverse-engineering the whole ISA cold.

### CORRECTION, same day, later — the "render dispatch runs exactly ONCE" claim above is WRONG.
### It's a continuous, healthy render loop that never stops. The real gap is narrower and
### different: nothing ever draws real content into the surfaces it presents, every cycle,
### forever. Found while chasing the user's own follow-up question ("which task shows the boot
### logo/callsign, and what's it waiting on").

The single-hit breakpoint tool above (`trace_ui_render_dispatch.py`) removes each breakpoint the
moment it fires, so it can only ever report "did this happen at least once" — it was never
capable of detecting a repeat, and nobody checked for one. Re-tested properly with breakpoints
that stay armed across many hits: **both `ui_graphics_buffers_init` and `ui_graphics_present_frame`
fire repeatedly, roughly every 40-100ms, for as long as the capture runs** — 15 hits in ~0.6-1.2s
in every re-check, not a single one-shot pair. This is a real, continuously-running redraw loop,
not a one-time bring-up frame.

**What this changes**: the earlier framing ("presents one freshly-zeroed frame, then nothing")
undersold the render pipeline's own health — it's fine, and running exactly as a UI redraw loop
should. **What it doesn't change**: the OpenVG command-FIFO trace is still flat at 61 words
through every re-check this whole session (bring-up only) — so this healthy, continuous loop is
presenting the *same still-blank* pixmap surface every single cycle, because nothing ever draws
into it, not because the loop itself ever stops or skips a cycle.

**Directly checked the real VDC50 framebuffer content again** (`vdc5_framebuffer_peek.py`, fresh
90s capture, well after today's ADC/SCIF5 timing fixes): still effectively blank. Of 4096
dumped rows, exactly 14 (rows 259-272, right at the very bottom edge of the real 272-row screen)
have any nonzero bytes at all, and those bytes decode to widely-scattered, non-repeating RGB565
values with no visible structure (not text/logo-shaped runs) — the same "uninitialized-RAM noise
near the top/bottom" signature `icom-openvg-rendering`'s own original starting point already
described, unchanged by any of today's fixes.

**Tried to identify who's actually supposed to draw the boot logo + configured callsign, and came
up genuinely empty on the main-CPU side**: no symbol, string, or function anywhere in `body.bin`
matches "logo"/"splash"/"callsign"/"boot screen" (checked directly, case-insensitive, name and
listing search). The render dispatch flag itself (`0x2039064c`, `ui_graphics_lifecycle_task`'s own
state-select byte) has no confirmed writer of "1"/"2" found by static address cross-reference
either — attempts to catch the real writer live (a GDB write watchpoint on that exact byte) came
back empty-handed across 250 consecutive `RZA1H_DEBUG`-free samples spanning t=9-40s, despite the
value demonstrably being 1 (confirmed by breakpointing the compare instruction itself and reading
the register directly) — a real, currently unresolved contradiction between the write-watchpoint
and the register-read approaches, not chased to ground this session (worth revisiting: possibly a
hidden-argument kernel primitive writing the value through a path this project's watchpoint
technique doesn't yet know how to catch, in the same family as this codebase's many other
"Ghidra doesn't show this function's real argument" gotchas).

**Retracted**: the "front-panel-MCU draws its own splash" hypothesis above — per the user's own
direct schematic knowledge, the display is wired to the main CPU only; `IC501` (the front-panel MCU)
has no direct access to the display pins at all. This was the wrong direction; the real answer,
found by continuing to dig on the main-CPU side, is below.

### Follow-up, same day, continued — RESOLVED: found the real boot-splash frame builder and the
### exact RAM address it pulls the operator's configured callsign from, via a deep Opus static-
### analysis pass (independently verified — see `notes/ui-menu.md`'s own new section for the full
### derivation, cross-checks, and evidence trail; this is a condensed pointer).

**`opening_screen_build_frame`** (renamed from `FUN_20037c10`) is the real power-on "opening
message" splash-frame builder — called 7 times from the boot fade-in/hold/fade-out driver at
`0x2002a2a4` (brightness ramps 0→0x64 in steps of 0x14). It writes directly into the splash frame
buffer (`0x20403f64`): a brightness pair, a 32-byte glyph field, the effective display language, a
model/variant selector — and, critically, `memmove(dispbuf+8, g_my_call_text, 10)`, copying the
**operator's configured callsign straight into the splash frame**. This is the direct, concrete link
between "SET → DISPLAY → MY CALL" and the boot screen this whole thread has been looking for.

**`g_my_call_text` = `0x203de53c`**, 10 bytes, plain ASCII, space-padded (confirmed three independent
ways — the splash-frame copy above, the text-entry-field descriptor table, and the 326-item
factory-reset defaults table all agree on this exact address; see `notes/ui-menu.md` for the full
chain). **Persisted at EEPROM byte offset `0x1a90`**, loaded at boot by the already-known
`nvram_multirecord_load_and_verify`. Factory default is ten literal space bytes (`0x20`) — meaning
**with the emulator's own synthetic/blank EEPROM image, `g_my_call_text` legitimately reads as all
spaces, and the splash screen correctly shows no callsign text even if rendering worked perfectly**.
This doesn't change anything about the still-open "why does the OpenVG command FIFO stay silent"
question — `opening_screen_build_frame` just publishes a frame descriptor (`*DAT_200377f0 = 2;
FUN_2007edc8();`, the same render-request kick this thread already traced) for the render loop to
pick up; whatever actually turns that descriptor into real GPU commands is still the unresolved
piece. But **`0x20403f64` (the frame buffer `opening_screen_build_frame` writes) is now a concrete,
correct address to watch** for a future session continuing this thread, instead of guessing.

**A genuinely useful gotcha this pass surfaced, worth keeping in mind for future "no writer found
anywhere" dead ends in this codebase**: the earlier "no static write to `DAT_2001a5d4` anywhere"
result (a few sections up) turned out to be a false negative — that literal-pool cell's resolved
target address (`0x2039e4c0`) is independently duplicated across *6 separate literal-pool slots*
compiled into different functions, and Ghidra's `references_to` on a resolved target address only
follows the *specific* literal-pool cell it's tied to, not sibling cells holding the same value. A
raw byte-pattern memory search for the little-endian pointer value itself is what actually finds
every alias — the same class of blind spot `notes/memory-map.md` already documents for SVD-based
cross-referencing, now confirmed to apply to plain literal-pool duplication too.

### Follow-up, same day — per the user's own hypothesis ("some other task is probably waiting on
### an unimplemented peripheral"): a full `-d unimp,guest_errors` steady-state survey found and
### fixed a real one (the ADC), but it turned out NOT to be the render trigger. Log is now
### genuinely flat in steady state — nothing else is left to find this way.

Re-ran this project's own established `-d unimp` survey technique (last used 2026-09-20 for
VDC50/LVDS), but over a full 180s PWRK-hold window instead of just to first-`wfi` — the earlier
survey never ran long enough to distinguish "one-time boot config" from "still happening in
steady state." **Found exactly one region still being touched continuously**: `io-e8000000`
offset `0x5800`-`0x580e`, ~433 hits/sec for the entire 180s (78090 reads total, vs. a few dozen
for every other region combined). Cross-referenced against the RZ/A1H SVD: this is the **ADC**
(10-bit wired A/D converter). Decompiled the real driver end to end: `FUN_200b0678` (init) writes
`ADCSR=0x20bf` once — continuous-scan mode, and critically the driver **never reads ADCSR back**,
so it never checks a completion flag at all, just free-runs and blindly rereads whatever's in the
data registers every pass. `FUN_200b5124` (the periodic front-panel/DSP-settings-scan tick, called
from `FUN_200b517c`) is that reader: 6 of 8 channels (a mode byte gates the other 2, reading clear
in this boot — matches the survey's own 6-not-8 offsets exactly), each right-shifted by 6 before
feeding `dsp_param_table_rebuild_from_settings` — the `>>6` confirms a real 10-bit conversion
result left-justified in the register's top bits, not an arbitrary shift.

**Built `src/adc.c`**: same minimal-stub philosophy as `riic.c`/`openvg.c` — DRA-DRH each return a
fixed mid-scale reading (`0x8000` raw = 0x200 of 0x3ff), everything else plain storage, no IRQ
(the driver never uses one). Confirmed live: the ADC's own `io-e8000000` log lines are gone
entirely post-fix, and — genuinely new information this fix reveals — **the whole `-d unimp` log
now goes completely flat after ~20s** (1931 lines total over 90s, unchanged from t=30s onward).
Before this fix, the ADC's own ~78,000-line/180s noise made it impossible to tell whether anything
*else* was still quietly churning underneath it; now that it's gone, the answer is a clean "no" —
every remaining unimplemented-device hit is one-time boot config, nothing is left continuously
active. **Directly re-checked whether this changed the OpenVG-rendering picture** (re-ran
`trace_openvg_command_traffic.py`): command-word count and distribution are byte-for-byte
identical to before the fix (61 words, all pre-t=10s, zero afterward through t=90s) — **the ADC
was a real, worth-keeping gap, but not the render trigger.** Kept anyway (real hardware fidelity,
zero regression, same reasoning this project has applied to every previous fix that turned out not
to be *the* answer but was still a genuine improvement). **Where this leaves the OpenVG thread**:
the "-d unimp survey for a hidden blocking peripheral" avenue is now exhausted — steady state is
provably quiet on that front. Whatever gates a real screen redraw is either pure software logic
(state/counter/timer-gated) or needs a live external stimulus (front-panel touch, CI-V command)
this emulation has no path to inject yet, not a device this machine still fails to model.

### Follow-up, same day — per the user's own schematic-derived question about the CPU↔FPGA
### differential-I/O pins (`FPDX`/`FPSX`/`FPSR`, `SCPCK`/`SCPSS`/`SCPX`/`SCPR`): found and fixed a
### real, substantial DSP-link (`SCIF5`) protocol bug — a content-blind canned ack was causing an
### 85,000-event/150s retry storm. Cut it by ~86%. Still not the render trigger.

`notes/ic7300-signal-chain.md` already has this fully mapped: `SCPCK`/`SCPSS`/`SCPX`/`SCPR`
(`P8_3/4/6/5`) are RSPI channel 2's alternate function — already modeled (`rspi2.c`), confirmed
real and active by an earlier session. `FPDX`/`FPSX`/`FPSR` (`P8_11/14/15`) are FPGA-only
differential pins with no CPU-side driver ever found directly — except `notes/multi-cpu-images-
history.md` documents `scif5_arm_retry_timer` dynamically rerouting SCIF5's 3rd pin from its
normal `P8_2` onto `P8_11`/`FPDX` when a hidden parameter is nonzero (never observed taken in any
static sample) — i.e. FPDX is SCIF5 traffic, conditionally rerouted.

Captured live with a new tool, `tools/trace_fpga_link_activity.py` (`RZA1H_DEBUG=rspi2,scif` over
a 150s PWRK-hold boot): **RSPI2 fires once** (an 8-byte transaction, `00 03 00 00 00 00 00 00`) —
real, matches the already-confirmed driver. **SCIF5 (the DSP-link responder) fires 85,742 times**
in the same window (400-1500/sec sustained) — all the identical canned "class-0xF ack" this
project added 2026-09-21 earlier the same day to fix the boot-time identity-query stall.

**Root cause, found by decompiling `scif5_classify_reply` (`0x200b0dc4`) directly**: it only
treats reply classes 1/2/8 as "resolved" — class 0xF isn't one of them. For any SCIF5 exchange
that *isn't* the boot-time identity query, the always-0xF ack made `shared_job_ring_dispatch`'s
job sit "still pending" every time, clearing only via its own retry-budget countdown, immediately
followed by the next queued command hitting the identical fate — a genuine retry storm baked in
by the earlier fix, not real ongoing protocol traffic.

**Fixed properly this time, content-aware rather than universal**: `scif.c` now tracks the real
4-byte command word `scif5_bitrev_transmit_word` sends (via a small `REG_FTDR`-write hook,
`scif5_cmd_buf`) and reconstructs it with the same bit-reversal already established for the RX
side. Only the identity-query range (`0xE0000000`-`0xE0000005`) gets the class-0xF ack; everything
else gets class-2 (a trivial ack — `scif5_classify_reply`'s own `class==2` branch resolves
unconditionally on the first reply, no retry). Confirmed live: total SCIF5 events dropped from
85,742 to 12,210 (~86%) over the same 150s window, and the log now shows accurate
`(cmd=xxxxxxxx)` values per ack instead of a blind constant. The remaining traffic is a
repeating `cmd=0x43000000` at a fairly regular ~5-6ms cadence — very likely `dsp_param_sync_tick`
genuinely running (unpaced by any real DSP round-trip time, since our ack is instant), not a bug,
though not confirmed either way.

**Re-checked the OpenVG angle again** (re-ran `trace_openvg_command_traffic.py`): still exactly 61
words, all pre-t=10s, zero after — this fix, like the ADC fix before it, is real and worth keeping
but confirmed NOT the render trigger. The render-request-mailbox-sender question remains the most
concrete open lead for that specific thread.

### Follow-up, same day — per the user's own request ("static analysis on the tasks started — do
### some expect answers from peripherals our emulation can't provide"): swept the full 11-task
### boot-time catalog (`notes/kernel-rtos.md`'s "Living reference"). Found a third real, distinct,
### currently-ACTIVE gap: `spectrum_scope_fft_task` runs every boot, computing FFTs over an
### audio/IQ sample ring that never receives a single real sample.

**Traced the real producer chain by decompile, several calls deep**: `spectrum_scope_fft_task`
(`0x200095d8`) reads one of two double-buffered 512-float sample arrays and runs
`spectrum_scope_fft_and_dbscale` (a genuine radix-2 FFT + dB-scale, already known). The arrays are
filled by `FUN_2000879c` (windows + stores one sample), fed by `FUN_20008868` (converts int16 PCM
to float), fed by `FUN_20067254`, which reads via `FUN_2005fb64` — a classic ring-buffer consumer
at a fixed struct (`DAT_20060700`'s own stored pointer, resolved this session to `0x203fbdc0`):
8 slots of `0x48` bytes, write-index at `+0x240`, read-index at `+0x241`. **If write_idx==read_idx,
it returns an all-zero block instead of real data** — and `notes/ic7300-signal-chain.md` already
ties `DAT_20060700`'s own literal-pool cluster to `SSICR_0`/`SSICR_1` (SSIF0/1, the confirmed real
CPU↔DSP digital-audio link) and DMAC-channel-shaped addresses — i.e. this ring is meant to be
filled by a real DMA-driven audio/IQ stream, and `dmac.c` only models DMAC channel 0.

**Confirmed live, two ways, both decisive**: (1) `tools/trace_spectrum_scope_activation.py`
(single GDB breakpoints, zero-perturbation-if-never-hit) — the task's `itron_act_tsk` call hits at
t≈7.2s and the task's own entry hits at t≈27.1s, i.e. **it is genuinely activated and running on
every current boot**, not a dormant/unreached path. (2) `tools/trace_spectrum_scope_ring.py`
(QMP-only, zero perturbation) — polled `write_idx`/`read_idx` at 1Hz for 90s: `write_idx` stays at
`0` the entire time, never once advancing. **This task is actively computing 512-point FFTs over
pure silence, forever, every single boot** — not a hypothetical, a live, ongoing, currently-real
gap. A quick check of who reads the FFT's own dB-scaled output (`DAT_20008848`) found **zero
consumers anywhere in `body.bin`** outside the FFT function's own body — so even fixing the sample
feed wouldn't yet reach anything that draws it; a second, independent open question (not chased
further this session).

**Not yet fixed** (unlike the ADC/SCIF5 gaps above, this would mean modeling a real DMAC channel +
SSIF0/1 pairing, a bigger undertaking than a permissive stub) — flagged for a decision on whether
it's worth building, given (a) it's real and currently active, unlike the two lower-priority gaps
already fixed today, but (b) its own output currently has no confirmed consumer to unblock anyway.

**Broader task-catalog sweep, same pass** (11 tasks total, see `notes/kernel-rtos.md`'s own
catalog for the full list): most of the rest are either user-action-gated (SD-card menu/file-RPC/
voice-recording/BMP-capture tasks — real `mmc.c` SD-card model already backs these, and they're
not exercised automatically during boot regardless), kernel-internal with no peripheral dependency
at all (`first_task_entry`, `sys_monitor_task_entry`), or already-known, unrelated static-analysis
dead ends (`kernel_start`'s own still-unidentified task; `rtty_decode_log_poll_task`'s decoder,
already documented in `notes/kernel-rtos.md` as depending on DSP-internal demodulation this
project can't reach in `body.bin` at all — same *class* of gap as the FFT task above, already
known, and not boot-critical since it only fires in RTTY decode mode). `ui_graphics_lifecycle_task`
is the OpenVG thread covered at length above.


## Confirmed peripherals

| Device | File | Status |
|---|---|---|
| GIC (Distributor + CPU I/F) | `rz_a1h.c` (QEMU's own `arm_gic`) | Real, working — see the off-by-32 `qdev_get_gpio_in` bug in README-history.md |
| OSTM0 / OSTM1 | `ostm.c` | Real timer + real IRQ. OSTM0 is `body.bin`'s real tick source (ID 134, `CMP`=32000) |
| SPI boot status | `spi_boot.c` | Real — the one register `base.dat`'s SPI-ready poll needs |
| GPIO/port registers | `gpio.c` | Real (masked set/clear, `PNOT` toggle, live `PPR` pin levels) — `P1_6`/`PDV` (power-fail detector) and `P8_9`/`HSK1` (DSP ready/handshake) default high, see Status above |
| L2C (PL310 cache controller) | `l2c.c` | Real (`CACHE_ID`/`CACHE_TYPE`/`REG7` self-clear semantics) |
| CPG | `rz_a1h.c`'s `add_plain_ram_region()` | Plain storage, no behavior — nothing traced needs more yet |
| MTU2 | `mtu2.c` | Real channel 3's `TGI3A` (GIC 154), channel 4's `TGI4A`/`TGI4C` (GIC 159/161), and two more purely-polled compare-match events sharing one status byte (`0xFCFF0305` bits 2/0, targets `0x30c`/`0x308`, no GIC ID — host-wall-clock deadlines, not a live counter) — **all five confirmed load-bearing** (ch3 unblocks `cold_boot_hw_init`'s task-readiness wait — see `mtu2_ch3_periodic_housekeeping_tick` in Status above — ch4 unblocks `dsp_boot_handshake`, the fourth unblocks `scif5_cmd_transmit_now`, the fifth unblocks a DMA-descriptor-setup routine). Every other channel/register/event still plain storage (`regs[]` passthrough). **`MTU2_FREQ_HZ` confirmed real at 32MHz** (P0φ/1, same real clock as OSTM — the original 25MHz was an empirical placeholder, corrected once `-icount` made a real-clock re-check necessary) |
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR/STI/TI/TEI/RI/SPI protocol, real bit-rate-generator-paced timing — **formula corrected 2026-09-10** to implement all 5 real SCLE/NFE/CKS-dependent variants (manual §18.3.12/13), not just the SCLE=0 case, and to reset `FER`/`BRL`/`BRH`/`MR1` to their real hardware power-on defaults; confirmed against the real firmware-programmed register values (`CKS=1`⇒IICφ=16MHz, `FER` left at its real `SCLE=1,NFE=1` reset default) — real rate ≈340kHz, comfortably inside the GT24C128B datasheet's own min/max windows at either supported voltage; START/RESTART/STOP conditions also corrected (`riic_condition_time_ns()`) to their real, much-shorter §18.12 timing instead of a full byte time. **EEPROM data-plane rebuilt 2026-09-10 (same day, later)**: replaced a hand-rolled, read-only byte array (found via this project's own A/B differential test to silently drop every write and wrap at the wrong address-space size) with a real `hw/i2c/core.c` `I2CBus` + `hw/nvram/eeprom_at24c.c` slave (16KB, 2-byte addressing, real 7-bit address 0x50 — confirmed via decompile), plus a genuinely new write-data-loop protocol state machine (decompiled from the real firmware write driver, `FUN_2001dcc4`/`FUN_2001da80`/`FUN_2001db50`) that the old model never had at all — live-validated end to end (write then read-back over a fresh transaction, `tools/test_riic_eeprom_write.py`). See README.md's Status section and README-history.md for the full derivation (including a live regression found and fixed along the way) — only RIIC2 exercised by any traced boot path so far, the real physical EEPROM `IC351`/`GT24C128B` (note: not the same thing as the diode matrix in `notes/diode-matrix.md`, which is a separate, GPIO-scanned resistor array — an earlier session's own label here conflated the two) |
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI IRQ per channel, now real baud-rate-accurate pacing (`FSR.TDFE`/`TEND`, `PCLK`=32MHz, see Status above — 2026-09-10). Real RXI backing two virtual responders: a front-panel one on channel 3, and a DSP-link one on channel 5 (the latter triggered by a second, tiny MMIO region at `0xFCFE3120` on the channel-5 instance only, not by SCIF registers, and now paced by a real (placeholder) delay too — see Status above). **Per-channel bus logger added 2026-09-20**: `RZA1H_DEBUG=scif<N>` (e.g. `scif3` for the front panel) logs just that channel's TX bytes plus one assembled-frame summary line per complete `0xFE...0xFD` frame; `RZA1H_DEBUG=scif` still means every channel. TX-only for now — see `notes/front-panel-protocol-handout.md`'s 2026-09-20 section for why, and for a first real captured-traffic analysis already in progress |
| RX-8803LC RTC | `rx8803.c` | Added 2026-09-21 — real RIIC1 I2C slave (real-time clock). Backs `body.bin`'s live RIIC1 RTC traffic seen at idle steady state (see README.md Status). Note: the firmware's read buffer shows a mix of this model's real output and some still-default-shaped fields — unexplained, not a hang, not chased |
| MMCIF (SD/MMC host) | `mmc.c` | Real command/response/data protocol + virtual SD card, validated standalone — `body.bin`'s own driver not yet reached by any traced boot path |
| DMAC (DMA controller) | `dmac.c` | Real channel 0 only (edge `DMAINT0`/GIC ID 41, real `address_space_read()`/`address_space_write()` transfer, `ptimer`-based one-shot completion) — confirmed load-bearing 2026-09-09, **completion-delay race fixed 2026-09-10** (see Status above — 1000ns raced the firmware's own next instruction under `-icount`, raised to 100us). Every other channel/register still plain storage |
| RSPI2 (Serial Peripheral Interface ch.2) | `rspi2.c` | Minimal — `SPSR2`'s TX-ready bit always set, `SPDR2` writes logged only, no real transaction timing or completion IRQ — **confirmed load-bearing 2026-09-09**, unblocks `rspi2_transmit`'s own busy-wait, see Status above |
| VDC50 (LCD/display controller) + LVDS | `vdc5.c` | Register storage (was a plain-RAM region 2026-09-20..23, so `tools/vdc5_framebuffer_peek.py` reads `GRn_FLM*` back) **plus, since 2026-09-23, a 60 Hz frame-timing interrupt source**: output vsync/VLINE status bits latched in `SYSCNT_INT1-3` (write-0-to-clear), IRQs gated by `SYSCNT_INT4-6`, GIC 75..97. Confirmed load-bearing: `ui_graphics_present_frame` waits on GR3 VLINE (GIC 78) after every swap — see Status above. No compositing/scan-out |
| OpenVG (graphics processor for OpenVG) | `openvg.c` | Added 2026-09-21 — completion-interrupt stub only, every FIFO write completes instantly (GIC 130-133). Confirmed load-bearing (unblocks `slv5_periph_configure`'s own `TMO_FEVR` wait, the one thing keeping `main_idle_loop` from ever being reached). Nothing renders — see Status above for the full render-traffic scoping thread |
| ADC (10-bit wired A/D converter) | `adc.c` | Added 2026-09-21 — found via a full steady-state `-d unimp` survey (the only region still touched continuously deep into boot, ~433 hits/sec). DRA-DRH all return a fixed mid-scale reading; no IRQ (the real driver runs continuous-scan mode and never polls completion status). Confirmed real and load-bearing for hardware fidelity (feeds `dsp_param_table_rebuild_from_settings`), but confirmed live NOT the OpenVG render trigger — see Status above |

## Directory layout

- **`src/`** — our own C sources (tracked in git): `rz_a1h.h`/`rz_a1h.c` (shared addresses,
  the machine itself), plus one file per peripheral in the table above. `ostm.c`, `gpio.c`,
  `l2c.c` are direct C ports of the matching `emu/peripherals/*.py` module; `scif.c`, `mmc.c`,
  `riic.c` are genuinely new work (no Python original).
- **`src/rza1h_debug.h`** (new, 2026-09-09) — shared, permanent debug-logging helper, built
  directly on the lesson that resolved the DMAC/icount stall (see Status above): host-side
  logging inside a device model, independent of GDB entirely, is a reliable way to see what's
  actually happening, unlike (in that one specific case) a GDB breakpoint. `dmac.c`, `mtu2.c`,
  `riic.c`, `scif.c`, `rspi2.c`, `mmc.c` each call `rza1h_debug("<name>", fmt, ...)` at their
  own real "boundary" events (command/register dispatch that changes behavior, IRQ raise,
  phase/mode transitions, completion) — not routine plain-passthrough register reads/writes,
  which would just be noise. Toggle with `RZA1H_DEBUG=<comma-separated device names>` or
  `RZA1H_DEBUG=all` (unset/empty = fully silent). Deliberately kept separate from QEMU's own
  `-d unimp`/`-d guest_errors` (which `mmc.c`, `scif.c`, `rspi2.c`, `riic.c` used to
  (mis)use `LOG_UNIMP` for this same purpose, now migrated) — mixing routine peripheral
  activity into that category would pollute its otherwise-clean "genuinely unimplemented
  access" signal, which this project has directly relied on more than once (e.g. confirming
  no code touches display/VDC5 hardware at all). `gpio.c`/`l2c.c`/`spi_boot.c` don't call it —
  simple plain-storage/self-clearing devices with no real protocol state machine worth tracing.
- **`tools/gdbrsp.py`** — raw GDB-remote-serial-protocol client (registers, memory,
  continue/step/interrupt, real `Z0`/`z0` software breakpoints) — the reliable way to drive
  and inspect this machine dynamically; see its own file comment for why it exists instead of
  `gdb`'s own Python API.
- **`tools/test_irq.py`** / **`tools/trial_irq.py`** — single-shot and repeated-trial
  IRQ-delivery tests (the latter exists because a single boot snapshot isn't a reliable
  regression test once real interrupt-driven scheduling is involved).
- **`tools/test_mmc.py`** — standalone protocol validation for `mmc.c`.
- **`tools/build_sdcard.py`** — builds a real FAT16 SD card image with an update container at
  the documented path (`\IC-7300\<filename>`).
- **`tools/force_call_fup.py`** — forces a direct call into `firmware_update_main` over GDB,
  bypassing the SD-menu/file-browser UI.
- **`tools/test_fup_scheduling.py`** — breakpoint-based baseline-vs-forced-call comparison
  tooling (where `gdbrsp.py`'s breakpoint support was added).
- **`tools/build_riic_eeprom_image.py`** — builds the real, ROM-sourced virtual RIIC2 EEPROM
  image needed to clear `FUN_2002b29c`'s cold-boot branch gate (see Status above) — run this
  before any boot test where reaching real `cold_boot_hw_init`-era code matters.
- **`tools/trace_job_ring_overflow.py`** — the main ring-overflow tracer: free-runs the boot and
  samples the `0x20420120` ring's header on a fixed wall-clock cadence (0.25s default; optional
  coarse→fine two-phase polling) rather than breakpointing/watchpointing the hot push/drain path
  itself. Still the right starting tool for this thread. **Gotcha**: don't `pkill -f`/`pgrep -f`
  a pattern that matches the invoking shell's own command line (self-kills) — use `-x
  qemu-system-arm` (exact `comm` match) instead. **Another**: dropping a `gdbrsp.py` connection
  mid-`continue` under `-icount` leaves the gdbstub unable to ack the next client's first packet
  — let scripts exit cleanly or restart QEMU, don't reconnect to an abandoned instance.
- **`tools/dump_job_ring_at_overflow.py`** (2026-09-10) — reads all 16 queued ring entries plus
  `sdcard_file_rpc_dispatch_task`'s own small result ring at the overflow moment. The tool that
  found the 16 entries are byte-for-byte identical (a generic doorbell, not 16 distinct messages).
- **`tools/trace_20186fb4_callers.py`** (2026-09-10) — breakpoints the one real chokepoint every
  ring-push path must cross; found the real producer (`mtu2_ch3_periodic_housekeeping_tick`, a
  steady ~82ms tick) after two other hypotheses (`trace_file_rpc_burst_source.py`,
  `trace_signal_calls.py`, `trace_task_own_post.py` — kept for their derivation trail, not
  current leads) were tried and ruled out.
- **`tools/trace_id0_drain_gap.py`** / **`tools/trace_sgi0_gic_state.py`** (2026-09-10) — the
  tools that found id0 breakpointing itself suppresses the stall, then (memory-reads only, no
  breakpoint near the GIC) caught the real moment: `GICD_ISPENDR0` shows SGI0 genuinely pending
  while `CPSR.I=1` — see Status above. Keep any further polling here to the bare minimum (1-2
  extra words) — even non-breakpoint extra reads have been shown to suppress the effect.
- **`tools/qmp_read_mem.py`** — general-purpose, fully GDB-free physical-memory reader via QMP's
  `human-monitor-command` → `xp` (only needs `-qmp unix:...`, no `-S`/`-gdb`). Reach for this
  whenever a finding needs a second, structurally-different confirmation before being trusted.
- **`patches/irq-mask-trace.patch`** / **`tools/apply_irq_mask_trace.sh`** / **`tools/
  trace_irq_mask.py`** (2026-09-10) — the new host-side, GDB-free `CPSR.I`-transition trace hook
  and its free-running test harness; see Status above for what it caught (the overflow trap
  reached with `CPSR.I=0`, twice). The patch is deliberately NOT applied by `setup.sh` (unlike
  `hw-arm-build.patch`) — it's a one-investigation diagnostic against core `target/arm/helper.c`,
  not a permanent part of this machine; apply/rebuild manually via the script, drop it
  (`git apply --reverse` in `qemu-src/`) once this resume point is resolved.
- **`patches/rr-loop-trace.patch`** / **`tools/apply_rr_loop_trace.sh`** / **`tools/
  trace_rr_loop_overhead.py`** (2026-09-10) — the round-robin-main-loop diagnostic that localized
  the ring-overflow's own ~4x real-time-inflation gap to QEMU's `cpu_exit()`-per-interrupt +
  round-robin-pass overhead (see Status above — investigation now CLOSED, verdict "inherent, not
  fixable at this project's level"). Same NOT-applied-by-default convention as
  `irq-mask-trace.patch`; reusable for a future re-run via the script, currently reverted
  (`qemu-src/` is clean).
- **`tools/check_overflow_r0.py`** (2026-09-10) — 5-trial, QMP-only (no GDB) check of the
  overflow trap's own `r0` argument; found it's always `2` (producer-side), never `3`
  (consumer-side), ruling out the "consumer gets stuck forwarding a message" hypothesis directly.
- **`tools/eeprom_ab_diff_test.py`** (2026-09-10) — HISTORICAL/regression-reference now, not a
  live comparison: the pure-Python differential test that found `riic.c`'s old hand-rolled EEPROM
  model silently dropped every write and wrapped at the wrong address-space size, motivating the
  real `I2CBus`/`eeprom_at24c.c` rewrite below. Its `OurRiicModel` intentionally models the OLD,
  now-replaced behavior — kept as the record of the bug, not a check against today's `riic.c`.
- **`tools/test_riic_eeprom_write.py`** (2026-09-10) — the live validation for the new write-data-
  loop protocol: drives RIIC2's real registers directly over GDB (bypassing firmware, same spirit
  as `force_call_fup.py`) once the CPU is confirmed stuck at the known ring-overflow trap (real
  time still advancing, firmware provably done touching RIIC2), writes 8 bytes, reads them back
  over a fresh transaction, and checks they match. Confirmed PASS — see Status above.
- **`tools/hotblocks_profile.py`** (2026-09-10) — CPU "heat map" profiler using QEMU's own
  built-in `contrib/plugins/hotblocks.c` TCG plugin (not part of the default build — run `ninja
  -C qemu-src/build contrib/plugins/libhotblocks.so` first). Reports each translation block's
  total retired instructions (`icount * ecount`), sorted, with % share of the run. Requires
  `-d plugin` on the command line or the plugin's own exit report is silently lost (confirmed
  live, 0/3 vs 3/3 trials) — already baked into the tool. Built and verified end-to-end this
  session; the actual analysis (resolving hot addresses to functions, looking for surprises) is
  deliberately deferred to a fresh session — see Status above for the concrete next steps.
- **`tools/trace_post_hsk1_fix.py`** (2026-09-10) — QMP-only PC + ring-header polling, built to
  check the real effect of the `HSK1`/`P8_9` `gpio.c` fix; found the fix works (the ~16-minute
  wait is gone) but the ring still overflows, now much sooner (~4s instead of ~27s).
- **`tools/trace_dsp_param_burst.py`** (2026-09-10) — QMP-only, polls the outer ring header
  *and* the SCIF5 TX ring's own state together; caught the live, direct temporal correlation
  between `dsp_param_sync_tick`'s ~22-word burst and the outer ring's overflow.
- **`tools/trace_sgi0_gic_state_qmp.py`** (2026-09-10) — the tool that closed the whole
  ring-overflow thread: a GDB-free redo of `trace_sgi0_gic_state.py` (that GDB-based version
  suppressed the post-fix, now-faster overflow outright) reading GIC priority/pending state via
  QMP only — caught SGI 0 genuinely latched pending while something else runs at priority `0x10`,
  confirming the real GIC-priority-starvation mechanism (see Status above).
- **`tools/trace_irq_frequency.py`** (2026-09-10) — QMP-only `GICC_HPPIR` + `GICD_ISENABLERn`
  polling; found RIIC2's TEI (ID 205) dominates 93.8% of samples during the backlog window, and
  which IDs get freshly enabled right before it (see Status above).
- **`tools/trace_riic2_burst_source.py`** (2026-09-10) — merges `RZA1H_DEBUG=riic`'s own log with
  outer-ring polling on the same host clock; found the 557-transaction RIIC2 EEPROM scan and its
  exact temporal overlap with the ring backing up.
- **`tools/trace_riic2_scan_caller.py`** (2026-09-10) — one-shot GDB write-watchpoint on RIIC2's
  real `DRT` register (`0xFCFEE83C`), 60 hits then released; found every write's `LR` points back
  into the generic per-GIC-ID dispatcher, confirming the scan runs entirely from interrupt
  context, not a foreground loop.
- **`tools/trace_eeprom_scan_caller.py`** (2026-09-10) — same one-shot-breakpoint technique, one
  level up: breakpoints the generic EEPROM-read chunking wrapper's entry; found every hit in a
  60-hit/~9.7s capture traces back to `cold_boot_hw_init` itself, repeating every ~83ms — see
  Status above for why that's a genuinely open, not yet resolved, surprise.
- **`tools/read_fun200b5f38_flags.py`** / **`tools/trace_fun200b5f38_wait.py`** — the tools that
  found the DMAC completion race (see Status above); kept for their bracketing/direct-read
  techniques, reusable for any future suspected busy-wait.
- Several more 2026-09-09-era DMAC/cold-boot tracers (`trace_dmac_isr.py`,
  `measure_dmac_wait_throughput.py`, `trace_dmac_wait_completion.py`/`_completion2.py`,
  `trace_dmac_icount_shift.py`, `trace_dmac_hands_off.py`, `trace_cold_boot_hw_init_tail.py`,
  `trace_irq_mask_window.py`) are kept for their own reusable isolating-trial/bracketing
  patterns even though the specific stalls they chased are resolved — see README-history.md if
  you need what each one specifically settled.
- **`patches/hw-arm-build.patch`** — the small diff (`hw/arm/Kconfig` + `hw/arm/meson.build`)
  that registers our files in the vendored QEMU checkout.
- **`setup.sh`** — idempotent: clones QEMU `v11.1.1` (shallow) if missing, applies the patch,
  symlinks `src/*.c` in, configures (`arm-softmmu` only), builds.
- **`tools/build_flash.py`** — thin wrapper around `emu/flash_image.py` — produces the flat
  flash image `rz_a1h.c` loads via `-kernel`.
- Gitignored: `qemu-src/`, `flash.bin`, `riic2_eeprom.img` (all regenerated by the tools above).

## Running it

```
qemu-machine/setup.sh                                        # one-time (or after a source edit)
emu/.venv/bin/python3 qemu-machine/tools/build_flash.py       # produces qemu-machine/flash.bin
emu/.venv/bin/python3 qemu-machine/tools/build_riic_eeprom_image.py  # produces riic2_eeprom.img
qemu-machine/qemu-src/build/qemu-system-arm -M rz-a1h -nographic \
    -kernel qemu-machine/flash.bin -serial none -monitor none \
    -global rza1h-riic.image=qemu-machine/riic2_eeprom.img \
    -icount shift=auto \
    -qmp unix:/tmp/qemu.sock,server,nowait   # or -s -S for GDB
```

**`-icount shift=auto` is this machine's own recommended default** (a real OSTM/MTU2 clock-
realism fix, not optional) — see README-history.md. **Boot now reaches `main_idle_loop` and a
stable idle steady state on both power-on branches** (auto-boot and PWRK-hold) — the earlier
`0x200b93fc` job-ring-overflow trap was a real `riic.c` `SR2_TEND` device-model bug, since fixed;
that whole thread is closed (full derivation in README-history.md). The active resume point is now
the OpenVG rendering frontier — see Status above.

Omit the `-global rza1h-riic.image=...` line to boot with an empty virtual EEPROM instead — a
real, valid configuration (matches how earlier sessions tested), but boot will stop much
earlier, at the pre-cold-boot-branch watchdog/`wfi` point documented in README-history.md,
rather than reaching the current ring-overflow frontier.

Inspecting live state: `tools/gdbrsp.py`'s raw GDB remote-serial client (`c`/`s`/`?`/`g`/`G`/
`m`/`M`, plus `set_breakpoint`/`remove_breakpoint`) is the reliable way to both inspect and
drive execution — see `tools/test_irq.py` or `tools/test_fup_scheduling.py` for worked
examples. QMP's `human-monitor-command` → `info registers` also still works for a read-only
spot-check.

## Extension roadmap

1. ~~Root-cause the GDB scripting reliability gap~~ — done, `tools/gdbrsp.py`.
2. ~~Find what really arms `body.bin`'s tick source~~ — done, OSTM0/ID 134/`CMP`=32000, now
   annotated in Ghidra (`ostm0_tick_arm_and_get_irq_id` at `0x200b93b0`).
3. ~~Port the remaining Unicorn-side peripherals to real C devices~~ — done: `gpio.c`/`l2c.c`
   (real behavior), `riic.c` (real), `mtu2.c` channel 3 + channel 4 (both `TGI4A`/`TGI4C`) plus
   a fourth purely-polled rate-limiter event, `dmac.c` channel 0, and `rspi2.c` (all real,
   2026-09-09 — all confirmed load-bearing, see Status above); `add_plain_ram_region()` still
   covers CPG (plain storage, nothing traced needs more) and everything in MTU2/DMAC outside
   their modeled channels/events.
4. ~~SCIF UART output~~ — done: TX plus real per-channel TXI, real RXI + two virtual
   responders (a front-panel one on channel 3, a DSP-link one on channel 5, 2026-09-09). The
   SCIF3 front-panel handshake fully resolves, boot reaches real ITRON task activation, and the
   SCIF5 `scif5_send_and_wait_reply` reply-ready deadlock is resolved too.
5. ~~Reach a stable idle steady state~~ — done (2026-09-21): the `0x200b93fc` job-ring-overflow
   trap (a real `riic.c` `SR2_TEND` bug) and the last boot blocker (a missing OpenVG GPU
   completion interrupt) are both fixed; boot now reaches `main_idle_loop` and a self-sustaining
   idle loop on both power-on branches. `mmc.c`/`riic.c`, `FUN_2002b29c`'s cold-boot branch gate,
   the SCIF3 front-panel handshake, real ITRON task activation, `cold_boot_hw_init`'s
   task-readiness-wait cluster (MTU2/DMAC/RSPI2/SCIF5), the OSTM/MTU2 real-clock + `-icount` fix,
   the DMAC completion race, and the RIIC1 RTC are all real, confirmed, and hold.
6. **OpenVG rendering (current frontier)** — the render dispatch runs as a healthy continuous
   loop but presents a still-blank surface: nothing has ever been observed posting a real draw
   request to the render-request mailbox. See README.md's Status section for the full scoping
   thread and the concrete next step.
7. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — `body.bin`'s own MMCIF driver
   has still never been reached by any traced boot path; reaching it (e.g. via the SD-update
   flow) remains the actual Phase-0 payoff. Full derivation of every step in README-history.md.
