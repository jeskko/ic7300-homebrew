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

## Status, 2026-09-10, new session — read QEMU's own gdbstub/icount internals per the prior
## session's explicit handoff order; the working "GDB pause distorts icount" theory is half
## confirmed, half refuted, and a well-motivated follow-on hypothesis is refuted too

**Step 1 of the prior handoff, done.** Read `gdbstub/gdbstub.c`, `system/cpu-timers.c`,
`accel/tcg/icount-common.c`, `accel/tcg/tcg-accel-ops-rr.c` directly. Result, precisely:
**confirmed** — any GDB request forces a full `vm_stop()` while running (`gdbstub.c`'s
`gdb_read_byte()`), and `QEMU_CLOCK_VIRTUAL` genuinely freezes with exactly zero drift for the
whole pause (`icount_start_warp_timer()`/`icount_account_warp_timer()` both explicitly no-op
while `!runstate_is_running()`, and `cpu_thread_is_idle()` treats any non-running runstate as
idle regardless of real CPU halted state, parking the TCG main loop entirely). **Refuted** — the
"pending deadlines queue up and release in a burst at resume" half: with zero drift during the
freeze, resuming is indistinguishable from an uninterrupted run for every `QEMU_CLOCK_VIRTUAL`-
backed `ptimer` (i.e. every peripheral this project models) — no burst mechanism exists.
**Also refuted**, a natural next hypothesis (shift=auto's adaptive retuning skewing on a
real-duration pause): its own wall-clock reference (`cpu_get_clock()`) is frozen by the *same*
`cpu_disable_ticks()` call `vm_stop()` already makes — a deliberate QEMU safeguard, not a gap.
This project's own device models were also checked and ruled out (grep confirmed every real-
timing value in `src/*.c` goes through `QEMU_CLOCK_VIRTUAL`/`ptimer`, no stray real-clock reads).

**Net**: three specific, plausible icount-level mechanisms all predict a read-only GDB stop/
inspect/resume cycle should be perfectly transparent to guest timing — yet this project has
repeatedly, empirically observed real GDB-based suppression (id0 breakpoint preventing the
original overflow outright; `trace_sgi0_gic_state.py`'s GDB polling suppressing the faster
post-HSK1-fix overflow while QMP-only tracing reproduced it every time). **Directly confirmed**
why QMP-based techniques never show this: `hmp_physical_memory_dump`/`hmp_info_registers`
(`monitor/hmp-cmds.c`) both read live state with zero `vm_stop()` calls in either path — the vCPU
never stops for a QMP read. **What remains genuinely open**: why the GDB side distorts anything
at all, given the freeze/no-burst/no-shift-skew findings above all say it shouldn't. Sharper than
before (three specific mechanisms eliminated with citations), not closed. Full derivation,
including the exact functions/lines checked, in README-history.md's newest section.

**Step 2 of the prior handoff, done — both open items resolved, plus a real documentation bug
found and fixed.**

**The "genuine re-entry vs. GDB artifact" question is CLOSED: it was the artifact.** Built
`tools/trace_eeprom_addr_gdbfree.py`: adds one new host-side debug line to `riic.c`
("EEPROM addr=... resolved", fired once both address bytes of any real I2C transaction are known
— GDB-free, no perturbation, same convention as every other `rza1h_debug()` call site) and
correlates it against wall-clock time with zero GDB involvement at all. Ran 3 independent trials
(20s/25s/60s) — **byte-for-byte reproducible each time** (279 total EEPROM-address events, same
histogram): EEPROM offset `0x3df0` is targeted **exactly twice, ~44-47ms apart, then never
again** — nothing like the GDB-based capture's "60 hits over ~9.7s, every ~83ms". A live-probing
artifact after all, cleanly confirmed by a structurally different, fully independent technique —
consistent with (though not itself proof of a specific cause for) the still-open GDB-distortion
tension flagged just above.

**Also resolved, and it changes the whole picture: this was never a read.** Direct decompilation
(Ghidra) traced the exact call chain: `cold_boot_hw_init` → `FUN_20029198` (a 3-instruction
wrapper, sole caller confirmed via `references_to`) → `FUN_2001e484(0x3df0, DAT_2002a090, 0x10)`.
Per the read/write correction above, `FUN_2001e484` is the **write** path — this call **writes**
the fixed 16-byte ROM literal at `DAT_2002a090` (`"SX3765 V0.9H-000"`, a fourth, previously-
undocumented format-version-signature string in the same family as `0x3e80`/`0x3fc0`'s already-
catalogued `"SX3765 Vx.xx-000"` strings — see `notes/eeprom-catalogue.md`, updated) to EEPROM
offset `0x3df0`. A single, bounded, sensible "stamp a legacy-compatibility signature slot" write,
called once per boot from one static call site — not a mysterious repeating read with no loop
around it. The whole "does `cold_boot_hw_init` really get re-entered" puzzle from update #11
dissolves: it doesn't.

**The denser, ~557-transaction burst's own caller, also found**: the same GDB-free tool's wider
(60s) run caught the real thing directly — a tight, ~0.5s (t≈4.18-4.70s), fully sequential,
32-byte-chunked scan striding from EEPROM offset `~0x0420` through `~0x1fe0` (then `0x3ac0`
through `0x3e80`/`0x3e44`) — **~230 chunk-reads, each a real address-resolution event**, landing
exactly on the address ranges `notes/eeprom-catalogue.md` had *already*, independently (pure
static analysis, no live trace) catalogued as `FUN_2006cb84`'s own "combined settings struct
(~0x1a80 bytes)" load (its documented sub-blocks — `0x40`/`0x12e0`/`0x1620`, plus `0x3e44`'s
already-confirmed diode-matrix read — are exactly contiguous with what this capture shows).
Confirmed directly via decompile: `FUN_2006cb84` calls `FUN_2001e510` (the real getter, per the
correction above) for each of its ~7 known parameter IDs. Two fully independent methods — an
older static catalogue entry and this session's brand-new live capture — landing on the same
answer is real, convergent confirmation: **`FUN_2006cb84` is the denser burst's caller**, not one
of the other ~23 uninspected sites.

**One real documentation bug found and fixed, worth flagging on its own**: `README.md`'s own
"Ghidra fix applied" paragraph (2026-09-10, prior session) had `FUN_2001e484`/`FUN_2001e510`'s
read/write roles backwards, silently contradicting `notes/eeprom-catalogue.md`'s own original,
correct labeling. Corrected in place with a visible annotation (not silently) — see that
paragraph below.

**NEXT SESSION**: both items the prior handoff named are now closed (see above) — the old
"NEXT SESSION, IN THIS ORDER" block a little further below is stale, superseded by this section,
kept only for its own historical trail. No fix was attempted this session for the underlying
ring-overflow trap itself (`0x200b93fc`, GIC-priority-starvation, mechanistically closed since
the prior session — see "CLOSED, same day" below) — boot still stops there, same as before this
session, since this session was pure investigation/tooling (one inert debug-log line in
`riic.c`), not a mitigation attempt. **The concrete next step**: decide on and build an actual
fix for that overflow so boot can progress toward the real Phase-0 payoff (the SD-card/VFS/MMCIF
driver path, see "Extension roadmap" below) — candidates not yet weighed against each other:
raise SGI 0's own GIC priority above `0x10` (risks masking something that currently relies on
being preemptible), grow the 16-slot ring's capacity (a firmware-side struct-layout change, more
invasive), or find a way to reduce/pace the RIIC2 scan's own IRQ density further (already tried
once via real bus-speed pacing — confirmed insufficient alone, see "Added real I2C bus-speed
pacing" below). Worth a real discussion of trade-offs with the user before picking one, not a
unilateral pick.

## Status, 2026-09-10 (updated same day) — DMAC completion race fixed; a real virtual DSP-ready
## signal built and confirmed (`scif5_wait_hsk1_ready`'s ~16-minute software timeout is gone, boot
## now visibly exercises much more of `cold_boot_hw_init`); **the job-ring-overflow trap's GIC
## mechanism is closed** (SGI 0 sits at the lowest priority in the system, `0xFE`, every real
## peripheral IRQ at `0x10`); real baud-rate/bus-speed pacing added to both `scif.c` and `riic.c`,
## confirmed genuinely working (a 557-transaction RIIC2 scan now spans ~4.8 real seconds instead
## of milliseconds); **the actual dominant contributor during the overflow turned out to be a
## massive, IRQ-chained RIIC2 (I2C/EEPROM) scan, not the SCIF5/DSP burst** — still overflows even
## with real pacing. The Ghidra disassembly gap is resolved (user ran the queued fix), and tracing
## through it raised a genuinely surprising, unresolved question (see "Ghidra fix applied" below)
## that a live GDB probe can't be trusted to answer on its own until a second, adjacent open
## question is settled first (see "New open question" below). This section is deliberately a
## tight current-state summary, not the narrative — for the full derivation, README-history.md's
## final six 2026-09-10 sections cover the ring-overflow
##
## **STALE — both steps below are done, see the new 2026-09-10 "new session" Status section at
## the top of this file for the resolution and the actual current next step.**
## ~~NEXT SESSION, IN THIS ORDER (explicit user instruction, 2026-09-10)~~:
## 1. **First**, read QEMU's own internals to confirm or correct the GDB/icount-perturbation
##    theory in "New open question" below — `qemu-src/gdbstub/gdbstub.c` (does servicing a
##    register/memory request genuinely require stopping the vCPU in this build, and does that
##    freeze `-icount`'s virtual clock?) and `qemu-src/system/cpu-timers.c` /
##    `qemu-src/accel/tcg/cpu-exec.c`'s icount deadline handling (do pending `ptimer` deadlines
##    queue up and release in a burst at resume?) — the same two files a much earlier, 2026-09-09
##    session already flagged for a related (and never fully settled) icount question, see
##    README-history.md's "2026-09-09 update #10" section for that prior context.
## 2. **Then**, armed with a real answer to whether/how live GDB probing distorts this system,
##    resume tracing the EEPROM-scan caller: `cold_boot_hw_init`'s own repeated ~83ms/16-byte read
##    (see "Ghidra fix applied" below) needs re-confirming (is it real re-entry, or a GDB-
##    breakpoint artifact?), and the original dense 557-transaction burst's own caller is still
##    unfound among ~23 other read-side call sites of `FUN_2001e484` (see README.md's Directory
##    layout section — `tools/trace_eeprom_scan_caller.py` is the tool, already built).
## mechanism specifically; everything from "2026-09-09: SCIF3 TXI made real" onward covers the
## whole cold-boot chain that got boot this far in the first place.
##
## **DMAC completion race (fixed)**: `dmac.c`'s completion delay was short enough under `-icount`
## that the model's own ptimer callback could race the firmware's own next instruction and get
## silently overwritten — permanently stuck the busy-wait right after `cold_boot_hw_init`'s DMAC
## calls, on every natural boot. Fixed by raising `DMAC_COMPLETE_DELAY_NS` to 100us (`src/dmac.c`,
## confirmed 5/5 trials, GDB-free). This was a genuine regression from 2026-09-09's own `ptimer`
## port, not a repeat of that session's earlier (wrong) "GDB-remote-stub artifact" conclusion —
## see README-history.md's "DMAC/icount stall really was a real bug after all" section.
##
## **With that fix in, boot now reliably reaches the `0x200b93fc` job-ring-overflow trap** (a
## real `b .` self-branch, not a transient poll) — the same `0x20420120` ring a 2026-09-09
## session first found, now finally reachable end-to-end for the first time with a genuinely
## working DMAC channel 0 in the mix. **Full mechanism now traced, precisely, end to end**:
## - **Producer**: `mtu2_ch3_periodic_housekeeping_tick` (MTU2 ch3 TGI3A, GIC ID 154 — this
##   project's oldest, most-confirmed real timer) posts a generic "results ready" doorbell to a
##   fixed job object every ~82ms, like clockwork, for the entire boot. Also conditionally pumps
##   SSIF0/SSIF1 audio hardware — a fresh, unchased lead for the long-standing RTTY/SSTV
##   audio-source thread (see `icom-custom-code-goal` memory).
## - **Coalescing**: each push fires GIC SGI 0 directly unless one's already pending, in which
##   case it just flags "one more requested" (`sgi0_request_coalesced`); the consumer's own exit
##   path (`irq_nesting_exit_and_refire`) clears that flag and re-fires once more if needed. This
##   scheme looks architecturally sound on its own.
## - **Consumer**: `irq_context_switch_id0` drains the ring unconditionally as its first action
##   whenever it runs.
## - **The real cause of the overflow**: the ring stays healthy for ~27-45s of real boot time,
##   then the consumer goes from a rock-steady ~12Hz to almost nothing for a genuine ~10 real
##   seconds. Confirmed directly (GDB-free memory polling, no breakpoint near the GIC or `id0` —
##   a direct breakpoint on `id0`'s own entry was shown to *prevent* the stall outright): right as
##   the backlog forms, `GICD_ISPENDR0` shows SGI 0 genuinely latched pending at the GIC — correct
##   hardware behavior — while `CPSR.I` (the CPU's own IRQ mask) reads 1 at the same instant. The
##   GIC isn't the problem; **the CPU has interrupts globally masked for an unusually long stretch
##   at this exact point in this boot profile**, and everything downstream is just waiting on it.
##
## **New, same-day: a permanent (opt-in) IRQ-mask trace hook, and what it actually caught.**
## Built `patches/irq-mask-trace.patch` (applied via `tools/apply_irq_mask_trace.sh`, NOT part of
## `setup.sh`'s automatic patching — see that script's own comment for why this one stays manual)
## — a ~40-line addition to `qemu-src/target/arm/helper.c` that logs every real 0→1/1→0
## transition of `CPSR.I`, host-side only (`fprintf` to stderr, gated by `RZA1H_IRQ_TRACE=1`), at
## the *only two* places in all of `target/arm` that ever change it: `cpsr_write()` (covers
## `MSR CPSR`, `CPSID`/`CPSIE`, exception return, GDB-stub writes) and
## `take_aarch32_exception()` (automatic masking on exception entry) — confirmed by grepping
## every `env->daif` writer in the tree, so no per-instruction filtering is needed. This is
## categorically different from every prior tracing approach here: no GDB, no breakpoints, no
## polling at all — just a passive log inside an already-executing C helper — so it should not
## suffer the suppression effect that broke every GDB-based technique tried before it.
## `tools/trace_irq_mask.py` free-runs a boot with it enabled and reports the widest gaps.
##
## **First real run (70s, GDB-free) directly caught the overflow**: 370,067 real transitions
## logged in the first ~27s (matching the previously-established 27-45s overflow window), each
## one cross-checked against Ghidra and found to be exactly the expected mechanism — ordinary,
## healthy nested-IRQ dispatch (the generic per-GIC-ID dispatcher at `0x2000523c`-`0x20005248`,
## literally `cpsie i` / `blx r2` / `cpsid i` around each handler call — this is the same address
## range an earlier session already correctly re-identified as "the generic IRQ vector", see the
## retracted SVC-dispatch lead below). Then: **total silence for the remaining ~43s** — no further
## transition ever logged, with the very last one being a `CLR` (I→0, unmasked) right before a
## `blx r2` handler call. A second, independent, structurally different check (plain QMP
## `human-monitor-command` → `info registers`, the project's own established read-only spot-check
## — zero GDB, zero breakpoints) on a fresh 40s run landed the CPU squarely at the known overflow
## trap, **`PC=0x200b93fc`, `CPSR.I=0`** — genuinely unmasked — while `ps` showed the process still
## burning ~135-140% CPU (actively spinning the trap's own `b .`, not idle/WFI).
##
## **What this means, precisely**: the trap itself is hit with interrupts *unmasked*, not masked
## — the natural read is that the earlier-observed `CPSR.I=1`/`GICD_ISPENDR0`-pending correlation
## (README-history.md's "Caught the real moment directly") was real but caught an *earlier* phase
## (the backlog *building up* toward 16), and by the time the 16th push actually lands and trips
## the trap, whatever masked window caused the buildup has already lifted — control was already
## back inside an ordinary, unmasked, nested-IRQ-enabled dispatch call when the overflow itself
## triggered. So the "held masked" finding isn't retracted, but it no longer explains the trap
## moment itself — the question shifts from "why is I still masked at the overflow" (unsupported
## by this new, direct evidence) to **why the consumer (`irq_context_switch_id0`) doesn't drain
## the backlog fast enough to avoid hitting capacity before whatever masked window's damage is
## undone** — back to a throughput/scheduling-gap question at the consumer, not a CPU-mask one.
##
## **Open resume point (revised)**: (1) rerun `tools/trace_sgi0_gic_state.py`'s already-built
## GIC-state instrumentation (GICD_ISPENDR0/ISACTIVER0, GICC_PMR/RPR) far enough to catch a real
## hit now that the DMAC fix makes the trap reliably reachable — it was built before that fix and
## never got a confirmed catch; (2) a fresh, so-far-unconfirmed possibility surfaced by this same
## run worth ruling out on its own: total silence for 43 straight real seconds while `CPSR.I=0`
## and the CPU is actively executing is also consistent with a genuine QEMU/TCG+`-icount`
## artifact around a tight two-instruction self-branch loop (`b .`) never yielding back to the
## main loop's pending-timer/IRQ check, independent of anything firmware-side — worth a targeted
## QEMU-internals check (does a synthetic tight `b .` loop under `-icount shift=auto` ever take a
## real pending timer IRQ at all?) before assuming this is purely a firmware scheduling bug.
## Budget any further live confirmation carefully per the usual caution here — this thread has
## repeatedly shown live tracing can suppress the very effect being chased — but note this
## specific new tool is host-side-only (no GDB, no polling) and so far has reproduced cleanly
## twice in a row.
##
## **Follow-up, same day: the "lazy consumer" hypothesis tested directly, plus a real bonus find.**
## `FUN_20187ae4` (the outer ring's actual drain body, called from `irq_context_switch_id0`) is
## not fire-and-forget — per entry it re-reads the job object's own live status byte and
## dispatches synchronously to one of three handler functions, and one of those
## (`FUN_201877e4`'s type==1 branch) can itself call the *same* trap function with a *different*
## error code (`FUN_200b93fc(3)`) if a downstream per-job-object secondary buffer is full — a
## second, genuinely distinct way to reach the trap, previously noticed then dismissed by an
## earlier session as "not otherwise relevant". This directly tests whether the consumer gets
## *stuck forwarding a message* (the user's hypothesis) rather than the ring simply filling
## because the producer outpaces it. **Checked live, 5/5 independent trials, fully GDB-free**
## (`tools/check_overflow_r0.py`, coarse ~3s QMP `info registers` polling only): every single hit
## landed within `t=24-27s` (much tighter than the earlier 27-45s estimate) with **`r0=2`, never
## `3`** — confirmed (`FUN_20187bb4`, the producer) that `r0=2` specifically means the *producer's*
## own atomic capacity check found the ring already full on its next periodic push. The three
## dispatch handlers' own helper calls (`FUN_20187c8c`/`FUN_20187664`, a priority-ordered
## linked-list insert/pop pair) are also fast, bounded, non-blocking — no busy-waits, no hardware
## polling. **So the direct "consumer gets stuck processing message N" hypothesis is not what's
## happening here** (5/5, not a single trial) — it's genuinely a throughput/rate problem upstream
## of the drain, not a downstream blockage inside it.
##
## **Real bonus find from the same 5 trials**: at nearly every ~3s poll before the trap, in every
## single trial, the CPU was caught sitting at `0x200b48f4` — inside **`scif5_wait_hsk1_ready`**
## (the DSP-link handshake busy-wait, already named and flagged in an earlier, 2026-09-09 session
## as a wait capable of stalling hard — "135 real seconds straight, 91% CPU" — but only under a
## non-default `-icount shift=1`, believed not to recur under this machine's actual default,
## `shift=auto`). Seeing it dominate CPU time this heavily right up to the overflow, *under the
## actual default setting*, on every single trial, is a new data point worth its own follow-up:
## either this wait's own completion is what's gating overall forward progress here (separate
## from the ring question), or it's a major, previously-underestimated CPU-time competitor with
## whatever's supposed to keep the ring drained — not yet distinguished which.
##
## **`scif5_wait_hsk1_ready` traced fully, same day — the bonus find was the real story all along.**
## Decompiled it: it polls **`PPR8` bit 9 — the `HSK1` signal, physical pin `P8_9`** — with a
## bounded software fallback (a counter incremented once every 200 calls of a tick-cascade
## function, `FUN_200b7910`, itself called only on every *other* `mtu2_ch3_periodic_housekeeping_
## tick` — the exact same ~82ms MTU2 IRQ that drives the job-ring producer). **`HSK1`/`P8_9` is
## not a new pin — it's already independently confirmed in `notes/ic7300-signal-chain.md` (27th
## session) as a real DSP hardware ready/handshake line**, found there via a completely different
## code path (`firmware-update.md`'s "3 extra chunks" DSP/Front-CPU update mechanism polling the
## same `PPR8` bit 9) — two unrelated call sites agreeing this bit is a genuine handshake signal
## is strong, convergent confirmation, not a new guess. **Our `gpio.c` model has no virtual DSP
## responder — nothing ever drives `P8_9` high**, so this bit reads 0 forever in emulation, and
## `scif5_wait_hsk1_ready` always falls through to its software timeout: 30 counter increments ×
## (200 sub-ticks × 2 MTU2 ticks × ~82ms) ≈ **~16.4 minutes of continuous real-time busy-waiting**
## before `cold_boot_hw_init` can even reach `dsp_boot_handshake()`. Every trial run so far (all
## ≤90s) was still deep inside this one wait the entire time — the ~24-27s "overflow window" is
## just an early slice of it, not something waiting on this loop's own outcome: the ring overflow
## is driven independently, in parallel, by the same 82ms MTU2 IRQ regardless of what the
## foreground `cold_boot_hw_init` code is doing. **This reframes the whole thread**: the boot was
## never actually going to get past this DSP handshake at all within any trial run to date; the
## ring-overflow trap is a real, correctly-diagnosed side effect, but not "the" blocker on its own.
## **Built and confirmed, same day**: `gpio.c`'s reset now sets `pin_level[8] |= 0x200`
## unconditionally (the same justified-exception pattern as `P1_6`/`PDV`). Confirmed working as
## intended — `scif5_wait_hsk1_ready` no longer eats its ~16-minute timeout, and boot visibly
## progresses much further into `cold_boot_hw_init`: the first ~3.5s now shows healthy,
## repeated, fully-drained round-trips through the same generic job-ring mechanism (write/read
## staying in lockstep, `pending` back to 0 every sample — very likely `dsp_boot_handshake`'s own
## command/reply cycles finally running for the first time this project). **But the ring-overflow
## trap still fires** — now at **`t≈3.5-4s`** instead of `~24-27s`, same `r0=2`, same address. So
## this fix is real progress (confirmed: it does what it was built to do, and boot now exercises
## meaningfully more code than any previous trial) but does not by itself resolve the
## ring-overflow resume point — if anything it sharpens it, since the *same* underlying
## producer-outpaces-consumer bug now reproduces in ~4s instead of ~27s, a ~7x faster iteration
## loop for whoever chases it next. `tools/trace_post_hsk1_fix.py` is the tool that found this
## (QMP-only, PC + ring-header polling, no GDB).
##
## **Follow-up, same day: static-traced a concrete, well-motivated mechanism for the new ~4s
## overflow** (user's framing: the code wants to chat with a peripheral and the overflow happens
## around that — right, though the actual mechanism is subtler than "no reply"). Right after
## `dsp_boot_handshake()`, `cold_boot_hw_init` calls **`dsp_cmd_table_init()`** — builds a fresh
## 24-entry DSP parameter table, then calls `dsp_param_sync_tick()` once. That function diffs
## ~22 "current" values against shadow copies and calls `scif5_ring_push_word()` once per
## mismatch — since the table was *just* freshly initialized, essentially all ~20 of them
## mismatch on this first call, queuing ~20 words into a dedicated 87-slot SCIF5 TX ring (a
## *different* ring from the outer 16-slot job-ring). `dsp_cmd_table_init` then busy-waits —
## **genuinely unbounded, no timeout at all** (unlike `scif5_wait_hsk1_ready`'s bounded fallback)
## — for that whole SCIF5 ring to drain. Draining happens **one word per call** of
## `scif5_ring_pop_and_send`, already confirmed (an earlier, 2026-08-29 session's own code
## comment) to be **paced by the same ~82ms MTU2 ch3 tick that drives the outer ring's own
## doorbell producer** — so this initial burst alone takes **≈1.6+ seconds** to drain, during
## which that one MTU2 tick is doing double duty (draining/transmitting a real DSP-parameter
## word *and* posting its usual outer-ring doorbell) every time it fires. This lines up with the
## observed trace almost exactly: the ~3s of healthy activity is `dsp_boot_handshake` plus the
## start of this drain; the sudden overflow right after is consistent with the MTU2 tick doing
## measurably more work per firing during this exact window.
##
## **Live-confirmed, same day, with one correction to the pacing guess above.** Built
## `tools/trace_dsp_param_burst.py`: polls, every 0.15s, fully GDB-free (QMP `xp` only), both the
## outer ring header *and* the SCIF5 TX ring's own active flag + write/read indices, together.
## **Real, direct temporal correlation caught**: for the first ~2.6s, the outer ring climbs
## steadily with `write==read` every sample (healthy, always-drained) and the SCIF5 ring sits
## fully idle (`active=0`). At `t=2.74s`, the SCIF5 ring shows `active=1` with `write=read=23` —
## **the entire ~23-word burst was written *and* fully drained within a single 0.15s poll gap**,
## much faster than the ~1.6s the "MTU2-paced drain" reading above predicted — so that specific
## pacing guess was wrong, corrected here rather than left standing: `dsp_param_sync_tick`'s ~22
## pushes are NOT individually MTU2-tick-paced on the write side; they happen back-to-back,
## synchronously, in one call, and evidently drain very fast too. **Right in the same ~0.6s
## window** (`t=2.74s` to `t=3.35s`), the outer ring goes from its previously rock-steady
## always-drained pattern to a real backlog (`write=4,read=2` at `t=2.89s`) and then straight to
## **`pending=16`, full overflow, by `t=3.35s`** — a direct, live, temporal correlation between
## the SCIF5 burst and the outer ring's destabilization, not just a static-analysis inference.
##
## **CLOSED, same day: the causal mechanism, live-confirmed with real GIC priority data.** The
## GDB-based `trace_sgi0_gic_state.py` was tried first and, this time, suppressed the (now much
## faster) overflow outright — 15s of GDB `interrupt()`-polling, zero overflows, vs. every
## GDB-free QMP trial overflowing by `t=3-4s`. Rebuilt it GDB-free instead
## (`tools/trace_sgi0_gic_state_qmp.py`, QMP `xp`/`info registers` only — same technique as
## `trace_dsp_param_burst.py`) and it reproduced immediately, catching the whole thing directly:
## at `t=3.005s`, `GICC_RPR=0x10` (something actively running at GIC priority `0x10`) right as a
## backlog starts (`pending=1`); at `t=3.109s`, **`GICD_ISPENDR0` shows SGI 0 genuinely latched
## pending** while `RPR` drops back to idle (`0xff`); by `t=3.316s` — 207ms later — the ring has
## fully overflowed (`pending=16`) with **SGI 0 still sitting unserviced** (`SGI0_pend=1`,
## `SGI0_active=0`) while `RPR=0x10` again (something else at that same priority running instead).
##
## **Read the actual GIC priority configuration directly (one-shot QMP, no live trace needed) —
## this is the real answer**: `GICD_IPRIORITYR` for **SGI 0 (ID 0) is `0xFE`** — the *lowest*
## priority in the entire system (ARM GIC: lower number = higher priority) — while **every real
## hardware peripheral IRQ that matters here sits at `0x10`**: MTU2 ch3/ch4 (IDs 154/159/161),
## DMAC0 (41), and SCIF5's RXI/BRI/ERI (243/241/242) all `0x10`; SCIF5-TXI (244) `0x7f`. This is
## real, by-design firmware configuration, not an emulation artifact — SGI 0 (the ring-drain
## signal, `irq_context_switch_id0`) is deliberately the lowest-priority interrupt in the whole
## system, meant to run only when nothing else needs the CPU. Under `dsp_param_sync_tick`'s ~22
## rapid SCIF5 transmits, a dense burst of genuine priority-`0x10` hardware interrupt activity
## keeps preempting/deferring SGI 0 (per standard GIC semantics: a running interrupt is never
## preempted by one of equal-or-lower priority) long enough for the ~82ms MTU2 doorbell producer
## to overflow the fixed 16-slot queue before SGI 0 ever gets a turn. **Mechanism fully closed —
## no further mystery here.**
##
## **One more layer, though, worth flagging before calling this "just how the real hardware
## behaves": `scif.c`'s own module comment already documents, as a deliberate original-scope
## decision, "no baud-rate-accurate transmit pacing — bytes go out immediately"** (unlike real
## SCIF hardware, where each byte takes real, baud-rate-limited time to physically transmit). That
## means this emulation compresses `dsp_param_sync_tick`'s ~22-transmit burst into a
## near-instantaneous flurry of priority-`0x10` IRQs, whereas real hardware would spread the same
## burst out over real transmission time — very plausibly giving SGI 0 real gaps to sneak into
## that this emulation's current SCIF5 model doesn't offer. So the priority-starvation mechanism
## itself is real and correctly diagnosed, but **whether it would actually overflow a real,
## physical IC-7300 is still an open question** — this may be substantially (or entirely) a
## timing-realism gap in `scif.c`, the same class of issue the OSTM/MTU2 clock-realism and DMAC
## completion-delay fixes both turned out to be.
##
## The tight-`b .`-loop/icount question raised earlier is now moot — the real mechanism is this
## GIC priority-starvation effect, not a QEMU/icount artifact.
##
## **Both timing fixes built and confirmed same day — real, worthwhile hardening, but (as
## predicted before writing either) neither changes this specific overflow's own timing.**
## Traced one level deeper before implementing anything: `dsp_param_sync_tick`'s own burst
## doesn't go through classic FTDR/FSR at all, nor through the ACK-worthy DSP-link sentinels —
## every entry it pushes has its type byte hardcoded to 0 in `shared_job_ring_dispatch` (a
## SCIF5-ring-internal dispatcher, not a bridge to the outer ring as an earlier comment implied),
## which only sets a generic RTOS event flag; the actual transmit
## (`scif5_ring_pop_and_send`→`scif5_bitrev_transmit_word`) writes the DSP-link's own
## `0xFCFE3120` arm register with a *third*, deliberately-unacked sentinel (`0xa0000000`) and
## never polls anything afterward — so nothing in this path is gated by any register this
## device model controls; a peripheral-side delay alone can't throttle it.
## - **`scif.c`**: real baud-rate-accurate TX pacing added — `FSR.TDFE`/`TEND` now genuinely go
##   low for a computed byte time (standard Renesas SCI/SCIF BRG formula, `PCLK`=P0φ=32MHz, the
##   same clock already established for OSTM/MTU2) after every `FTDR` write, instead of always
##   reading back ready. `ptimer`-based (`scif_byte_time_ns()`), matching this project's own
##   established DMAC/OSTM/MTU2 idiom. Confirmed live: boot still completes the SCIF3
##   front-panel and SCIF5 DSP-link handshakes correctly, no regression.
## - **`scif.c`**: the virtual DSP-link ack (`rza1h_scif5_dsp_ack`, fired from the `0x10000000`/
##   `0x40000000` arm-sequence sentinels `dsp_boot_handshake` itself busy-waits on) now fires
##   after a real, `ptimer`-paced delay (`DSP_ACK_DELAY_NS`, 50µs — an honestly-labeled
##   placeholder, no real DSP datasheet exists to derive an exact figure from, same spirit as
##   `dmac.c`'s own `DMAC_COMPLETE_DELAY_NS`) instead of instantly.
##
## **Confirmed via `tools/trace_dsp_param_burst.py`**: boot behavior is otherwise unchanged, and
## the overflow still happens, now at `t≈4.6-5.7s` (slightly later — the added real delays above
## push a few things back a little, exactly as expected) with the same `write=read=23`-in-one-
## poll-gap burst signature as before. Both fixes are kept as permanent, genuine improvements to
## this machine's timing fidelity — they were never expected to change this specific overflow,
## and didn't. **Actually pacing the `dsp_param_sync_tick`/`0xa0000000` burst itself would need
## either a firmware-side polled hook this project hasn't found (if one exists), or a genuine
## CPU bus-stall on the arm-register write — a materially riskier class of QEMU-internals change
## this project has deliberately avoided elsewhere (see the retracted icount/tight-loop lead
## above) — not attempted.**
##
## **User's next question: have we looked at the burst's own *source* — is the sender expecting
## an answer, and spamming when it doesn't get one? Real answer, found by tracing
## `dsp_param_sync_tick`'s *other* caller** (it has exactly two — `dsp_cmd_table_init`, the
## cold-boot one this whole thread has been chasing, and one more, not previously looked at).
## The second is `dsp_param_table_rebuild_from_settings` (already named, 2026-09-07 session) —
## gated on `*DAT_200b2b18`, a "settings changed" dirty flag with **no static writer found**
## (same conclusion that 2026-09-07 session reached, re-confirmed, not contradicted, by a fresh
## literal-address search this session). When set, it rebuilds the *entire* 24-entry DSP
## parameter table from live RTTY/mode settings (Mark Frequency, Twin Peak Filter, USOS, etc.)
## and calls `dsp_param_sync_tick()` at the end — **the identical mechanism** that overflows the
## ring at cold boot: a freshly-rebuilt table diffs against stale shadows, most slots mismatch,
## and the same ~20+-word burst results. This function itself is called from a single, large
## "refresh all live status" aggregator (`FUN_200b5124`), which in turn has **7 separate call
## sites** scattered across the firmware — consistent with a routine, general status-refresh
## hook (very plausibly the main idle/UI-update loop), not anything failure- or retry-driven.
##
## **So: not quite "no answer → spam"** — no retry-on-no-reply mechanism drives this specific
## burst (that mechanism *does* exist elsewhere in this same DSP-comms code —
## `shared_job_ring_dispatch`'s case 1 calls `scif5_arm_retry_timer(0)` when a real DSP command
## reply doesn't resolve — but every entry `dsp_param_sync_tick` itself pushes is hardcoded to
## the *other* type, bypassing that path entirely, as already traced above). The real shape is
## "rebuild-everything-and-resync", triggered either by cold-boot table init (confirmed,
## overflows) or by any live settings change setting this dirty flag (not yet confirmed to
## overflow, but structurally identical and worth checking). **New, real-hardware-relevant
## implication**: if this second path fires with the ring anywhere close to already busy, it
## could in principle reproduce the same overflow *during normal runtime operation*, not just at
## cold boot — a genuinely open question this session didn't have time to chase further (would
## need finding what actually sets `DAT_200b2b18`, the same open item 2026-09-07 already flagged).
##
## **User's next question: have we looked at IRQ frequency / which IRQ dominates during the
## overflow, or whether some IRQ gets newly enabled right before it?** Built
## `tools/trace_irq_frequency.py` (QMP-only, `GICC_HPPIR` + `GICD_ISENABLERn` polling, no GDB).
## **Answer, directly measured, and it wasn't SCIF5/DSP at all**: during the backlog/overflow
## window, **`GICC_HPPIR` shows ID 205 — RIIC2's TEI (Transmit-End) — in 93.8% of samples**. Its
## `IPRIORITYR` is `0x10`, the same high-priority tier as everything else that can starve `SGI0`
## (`0xFE`). Separately, SCIF5-BRI/ERI/RXI (241/242/243) and DMAC0 (41) *do* get newly enabled
## right around `t≈2.8s` (matching `scif5_dsp_link_driver_init`'s own bring-up) — but RIIC2's own
## IRQs were already enabled earlier in boot; it's a rate spike there, not a fresh enable. Both of
## the user's hypotheses were right, just for different IDs.
##
## **Traced the source, same day**: `RZA1H_DEBUG=riic` (this project's own established host-side
## logging) correlated against the ring header on the same host clock shows **557 real I2C
## transactions in a single ~6s run** — a classic random-address EEPROM read repeated
## byte-at-a-time (`START` → write `0xA0` → 2 address bytes → `RESTART` → write `0xA1` → `STOP`),
## scanning real, sequential regions of the physical EEPROM (`IC351`/`GT24C128B`) including
## addresses right next to the `0x3e00`/`0x3e80` fields `notes/eeprom-catalogue.md` already
## documents. **A one-shot GDB watchpoint on RIIC2's real `DRT` register (`0xFCFEE83C`, 60 hits,
## then released) found every single write happens with `LR` pointing back into the generic
## per-GIC-ID dispatcher** (`0x20005248`, the same `cpsie i`/`blx r2`/`cpsid i` mechanism this
## whole thread found long ago) — meaning **this entire scan runs from inside an interrupt
## handler, not a foreground polling loop**: each transaction's own completion IRQ directly
## triggers the next, a fully IRQ-chained state machine that never yields back to normal
## scheduling between bytes. The actual driver code (`0x2001d9bc` onward, `riic2_driver_init`'s
## registered ISRs) has never been disassembled by Ghidra (raw bytes only, a known class of gap
## this project has hit before) — a fix request is queued in `scratch/armthumb_fix_requests.txt`
## (`ICOM.ARM-Thumb` → `FixArmThumbMode.java` in Ghidra) for whoever can run it in the GUI; until
## then, *why* the driver scans this many bytes one at a time (real EEPROM content size? a bug?
## the real cold-boot signature/branch-decision read, just far bigger than previously catalogued?)
## stays open.
##
## **Added real I2C bus-speed pacing to `riic.c` anyway — confirmed working, but not sufficient
## on its own.** Every `qemu_irq_raise()` that represents a new real STI/TI/TEI/RI/SPI event now
## goes through a `ptimer`-backed `riic_schedule_irq()` instead of firing synchronously — real
## RIIC bit-rate-generator formula (`BRL`/`BRH`, `CKS` prescaler not independently confirmed and
## treated as `/1`, same honestly-flagged simplification class as `scif.c`'s own `PCLK` caveat),
## `PCLK`=32MHz. **Confirmed live**: the same 557-transaction scan that used to complete in a
## couple of milliseconds now spans **~4.8 real seconds** — the fix is genuinely working, a huge,
## verified change in realism. **But the ring still overflows** — this specific scan is simply
## large enough (557 individual byte-reads, each a full transaction since this driver doesn't use
## I2C's own sequential-burst-read capability) that even at real bus speed, sustained
## priority-`0x10` traffic over several real seconds is still enough to intermittently starve
## `SGI0` long enough to overflow a queue gated only by a periodic ~82ms doorbell. This reframes
## the question once more: it's no longer really about I2C timing realism (that's now genuinely
## modeled) — it's about *why* the firmware performs a 557-entry, one-byte-at-a-time scan of this
## region at all, which needs the disassembly fix above to actually read.
##
## **Ghidra fix applied, same day — the real driver code read for the first time, and it raises a
## sharper question than "why 557 reads".** The 5 low-level RIIC2 protocol handlers (STI/TI/TEI/
## RI/SPI, matching `riic.c`'s own model exactly) decompile cleanly now. Traced the real
## call graph: both a generic EEPROM chunking wrapper (`FUN_2001e484`, 24 callers project-wide)
## and its sibling (`FUN_2001e510`, 49+ callers) exist, each chunking into ≤32-byte pieces
## (respecting a real page-boundary check) and calling one of two low-level entry points
## (`FUN_2001dcc4`/`FUN_2001dd58`) that actually drive the 5-handler state machine — confirming
## this whole area is one shared, generic EEPROM-access API, not something built for one specific
## caller. [**CORRECTED, 2026-09-10, new session**: this paragraph originally called
## `FUN_2001e484` "a generic 'read N EEPROM bytes' chunking wrapper" and `FUN_2001e510` its
## "write-side sibling" — backwards. Direct decompilation confirms `FUN_2001e484`→`FUN_2001dcc4`
## is the WRITE path (embeds source data into the outgoing request via `FUN_2017c710`) and
## `FUN_2001e510`→`FUN_2001dd58` is the READ path (stores a destination pointer for the ISR to
## fill in) — matching `notes/eeprom-catalogue.md`'s own original, correct labeling
## ("`FUN_2001e510`=get, `FUN_2001e484`=set"), which this paragraph had silently contradicted.
## See below for why this matters far beyond a naming nit.] **A bounded, one-shot GDB breakpoint
## on `FUN_2001e484`'s entry** (60 hits, then released — same technique as the earlier DRT
## watchpoint) caught something unexpected: **every single hit, across the whole ~9.7s capture,
## comes from the identical call site** — `LR=0x2002b17c`, which resolves to `cold_boot_hw_init`
## itself (the `bl` at `0x2002b178`, immediately before `tuner_jack_signal_precheck()` in its own
## tail), targeting a fixed 16 bytes at EEPROM offset `0x3df0`, repeating roughly every **~83ms —
## the same period as the already-established MTU2 tick**. [**RESOLVED, 2026-09-10, new session
## — this was wrong on two independent counts, see the new Status section above**: (1) given the
## corrected labeling just above, this call is a WRITE, not a read; (2) it does not repeat every
## ~83ms at all — a GDB-free re-confirmation found it fires exactly twice, ~44ms apart, then
## never again, matching its single static call site exactly. The "~83ms, 60 hits" picture was a
## live-GDB-probing artifact.]
##
## **That's the surprising part**: `cold_boot_hw_init`'s own decompiled body (read many sessions
## ago, re-checked again here) is a single, linear, no-loop sequence — this exact call site has
## no loop around it in the code as written. For it to fire dozens of times at a steady ~83ms
## cadence, `cold_boot_hw_init` itself would need to be genuinely re-entered repeatedly, not run
## once at cold boot as its own name implies. **Not yet confirmed which explanation is right** —
## a real re-entry (this code reused as more than a one-time boot step, maybe a periodic
## re-init/health-check path under a name that no longer fully describes it), or an artifact of
## this specific live-probing technique itself (see the new question below about how well this
## project actually understands *why* GDB/memory inspection changes this system's behavior) —
## and it still doesn't yet explain the original 557-transaction *dense burst* signature
## specifically (a steady 83ms/1-transaction cadence alone works out to a few dozen transactions
## over several seconds, not 557) — so a second, denser caller likely still exists among the
## other ~23 read-side call sites, not yet found. Open resume point, not closed this session.
##
## **New open question, prompted directly by the user: how well is the GDB/QMP-perturbation
## effect itself actually understood, mechanistically?** Honest answer: well-supported
## empirically, not yet confirmed against QEMU's own source. The consistent pattern across this
## entire project (a GDB breakpoint at `id0`'s entry preventing the original stall outright; extra
## GDB memory reads beyond 1-2 words suppressing it; `trace_sgi0_gic_state.py`'s GDB polling
## suppressing the post-fix, faster overflow entirely this session while every QMP-only trace
## reproduced it reliably) is consistent with one specific, plausible mechanism: GDB's remote-
## serial protocol needs the vCPU stopped (or at least synchronized) to service register/memory
## requests reliably in this single-threaded TCG build, and under `-icount` (virtual time tied
## directly to instructions actually retired, not wall-clock), stopping the vCPU freezes virtual
## time — any `ptimer`-driven deadline (MTU2/DMAC/RIIC) that would have fired during that freeze
## instead queues up and releases in a burst, or in altered relative order, right at resume. QMP's
## `human-monitor-command`/`xp` reads, by contrast, don't require stopping the vCPU the same way
## (monitor-thread-side state access, decoupled from the TCG/icount loop) — matching why every
## QMP-only trace this session reproduced cleanly. **This is a working, well-corroborated theory,
## not something confirmed by reading QEMU's own `gdbstub.c`/`icount.c` source this session** —
## genuinely open if a fully source-level-confirmed answer is wanted.
##
## Two wrong leads were chased and retracted the same session they came up, not left standing —
## an SVC-dispatch misattribution (the address was actually the generic IRQ vector — now directly
## confirmed again above) and a `sdcard_file_rpc_dispatch_task` misattribution (alive and doing
## real work, genuinely, but not the source of this specific burst). See README-history.md if the
## exact reasoning ever matters.

**Confirmed, solid, foundational (still true):**
- A custom QEMU machine (`rz-a1h`) builds cleanly against real QEMU v11.1.1 source (pinned,
  vendored checkout under `qemu-src/`, gitignored — `setup.sh` recreates it) and boots real,
  unmodified v1.42 firmware: `base.dat`'s traced sequence runs, the body decompresses, real GIC
  IRQ delivery works (OSTM0 is `body.bin`'s real tick source — GIC ID 134, `CMP`=32000).
- **With `tools/build_riic_eeprom_image.py`'s output supplied as RIIC2's backing image** (see
  "Running it" below), `FUN_2002b29c`'s entire cold-boot-vs-power-state branch decision clears —
  every EEPROM signature check and the real GPIO power-good gate all resolve correctly.
- **SCIF3's TXI (transmit-complete) IRQ is real and verified end-to-end**, and the whole
  cold-boot chain through MTU2/DMAC/RSPI2/SCIF5 and the OSTM/MTU2 real-clock + `-icount` fix are
  all real, confirmed, and hold up under the now-working DMAC channel 0 too. Full derivation
  (spanning several sessions) in README-history.md.

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
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR protocol + virtual EEPROM, now with real bit-rate-generator-paced timing (2026-09-10, see Status above) — only RIIC2 exercised by any traced boot path so far, the real physical EEPROM `IC351`/`GT24C128B` (note: not the same thing as the diode matrix in `notes/diode-matrix.md`, which is a separate, GPIO-scanned resistor array — an earlier session's own label here conflated the two) |
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI IRQ per channel, now real baud-rate-accurate pacing (`FSR.TDFE`/`TEND`, `PCLK`=32MHz, see Status above — 2026-09-10). Real RXI backing two virtual responders: a front-panel one on channel 3, and a DSP-link one on channel 5 (the latter triggered by a second, tiny MMIO region at `0xFCFE3120` on the channel-5 instance only, not by SCIF registers, and now paced by a real (placeholder) delay too — see Status above) |
| MMCIF (SD/MMC host) | `mmc.c` | Real command/response/data protocol + virtual SD card, validated standalone — `body.bin`'s own driver not yet reached by any traced boot path |
| DMAC (DMA controller) | `dmac.c` | Real channel 0 only (edge `DMAINT0`/GIC ID 41, real `address_space_read()`/`address_space_write()` transfer, `ptimer`-based one-shot completion) — confirmed load-bearing 2026-09-09, **completion-delay race fixed 2026-09-10** (see Status above — 1000ns raced the firmware's own next instruction under `-icount`, raised to 100us). Every other channel/register still plain storage |
| RSPI2 (Serial Peripheral Interface ch.2) | `rspi2.c` | Minimal — `SPSR2`'s TX-ready bit always set, `SPDR2` writes logged only, no real transaction timing or completion IRQ — **confirmed load-bearing 2026-09-09**, unblocks `rspi2_transmit`'s own busy-wait, see Status above |

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
- **`tools/check_overflow_r0.py`** (2026-09-10) — 5-trial, QMP-only (no GDB) check of the
  overflow trap's own `r0` argument; found it's always `2` (producer-side), never `3`
  (consumer-side), ruling out the "consumer gets stuck forwarding a message" hypothesis directly.
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
realism fix, not optional) — see Status above. **Even with it on, boot currently still hits the
`0x200b93fc` job-ring-overflow trap** reliably, ~20-45s in, now that the DMAC completion race
(also fixed, see Status above) lets boot reach far enough to exercise it — the full mechanism
behind that trap is traced in Status above and README-history.md; it's the active resume point,
not a config problem to work around by tuning `-icount` further.

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
5. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — the active thread, and the
   reason this whole `qemu-machine/` effort exists. `mmc.c`/`riic.c` built and validated;
   `FUN_2002b29c`'s cold-boot branch gate, the SCIF3 front-panel handshake, real ITRON task
   activation, `cold_boot_hw_init`'s whole task-readiness-wait cluster (MTU2/DMAC/RSPI2/SCIF5),
   the OSTM/MTU2 real-clock + `-icount` fix, and (2026-09-10) the DMAC completion race are all
   real, confirmed, and hold. **Current frontier**: the `0x200b93fc` job-ring-overflow trap,
   fully mechanistically understood as of 2026-09-10 (steady MTU2-driven producer, sound SGI
   coalescing, correctly-latched GIC, CPU IRQ mask held too long) but not yet root-caused at the
   "what holds the mask" level — see Status above for the current state and concrete next step.
   `body.bin`'s own MMCIF driver has still never been reached by any traced boot path; that
   remains the actual Phase-0 payoff once this trap is past. Full derivation of every step in
   README-history.md.
