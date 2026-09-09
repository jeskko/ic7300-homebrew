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

## Status, 2026-09-10 — the "DMAC/icount stall" is FIXED for real this time (2026-09-09's
## "RESOLVED, GDB-remote-stub artifact" conclusion turned out to be WRONG — see below). A fresh
## session picked up the prepared `tools/trace_fun200b5f38_wait.py` handoff, ran it to a
## TIMEOUT as predicted, then found the prior session's own diagnosis of that timeout was
## itself mistaken: the CPU was never stuck in `FUN_200b5f38`'s own loop 1 at all — it never
## even reached `FUN_200b5f38`, because the *preceding* call, `FUN_200b5ea4` (the DMAC completion
## wait 2026-09-09 called "resolved"), itself never returns. A **fully GDB-free** cross-check
## (QMP's `human-monitor-command` → `xp`, zero gdbstub involvement at all — new tool,
## `tools/qmp_read_mem.py`) read the actual busy flag firmware polls (`0x203906EE`) and found it
## **permanently stuck at 0x01** across multiple independent natural boots — directly
## contradicting 2026-09-09's "completes under 1ms every trial, 8/8" finding, which turns out to
## have checked only the DMAC model's own internal chain (arm→ptimer→IRQ→ISR entry, genuinely
## fast) without ever independently re-reading the actual RAM flag afterward.
##
## **The real bug, precisely localized**: `FUN_200b5dc0` (the shared low-level "arm" routine,
## 6 real callers) writes `N0TB_0` (the model's own "start the completion ptimer" trigger) and,
## only ONE instruction later, marks its own struct busy — the same flag the ISR clears. Under
## `-icount`, the original 1000ns completion delay was short enough that the ptimer callback and
## its ISR can run to completion *inside that single-instruction gap*, so the firmware's own
## delayed busy=1 write silently overwrites an already-correct completion, permanently. Confirmed
## live (still 100% QMP-only, no breakpoint anywhere near the race): raising
## `DMAC_COMPLETE_DELAY_NS` alone, no other change, took the flag from stuck-at-1 to reliably 0,
## 5/5 trials across two tested values (10ms and 100us) — **fixed by raising it to 100us**
## (`src/dmac.c`). A real, self-inflicted emulation race (real DMA can't outrun the CPU's own
## next instruction the way a sub-microsecond model under `-icount` can), not a GDB artifact —
## very likely a genuine regression introduced by 2026-09-09's own `ptimer` port (which fixed a
## real "never fires at all" bug but, in doing so, likely made completion fire fast enough
## relative to `-icount`'s pacing to newly expose this race).
##
## **With the fix in, boot now progresses well past this point for the first time since the
## `ptimer` port — straight into the already-known `0x200b93fc` job-ring-overflow trap**
## (confirmed a real `b 0x200b93fc` self-branch, not a transient poll; reproduced 3/3 fresh
## trials, ~20-45s in, all with `-icount shift=auto`). This means the ring-overflow fix's own
## prior "confirmed clean, zero overflows across two 130s/160s trials" finding needs a real
## caveat: no prior trial ever ran with a genuinely working DMAC channel 0 *and* `-icount` *and*
## the OSTM/MTU2 real-clock fix all active together until this session's fix went in.
##
## **Re-derived fresh rather than assumed: it's the same `0x20420120` ring, same producer/
## consumer, same burst shape as the prior session's own analysis** (`r0`=`0x2`/`lr`=
## `0x20187c29` at the trap match exactly; `pending` sits at 0-1 for tens of seconds then jumps
## straight to overflow in well under half a second, both re-confirmed live).
##
## **A same-day "SVC dispatch" lead built on `pc=0x200051ec` was WRONG — RETRACTED, checked and
## corrected the same session it was found, not left standing.** After the user applied the
## needed ARM-mode disassembly fix (`scratch/armthumb_fix_requests.txt`), decompiling
## `0x200051c0` for real showed a `GICC_IAR` read (`0xE820200C`) and a `GICC_EOIR` write
## (`0xE8202010`) — the exact addresses `notes/kernel-rtos.md` already documents as the FreeRTOS
## RZ/A1H port's own `INTC_ICCIAR_ADDR`/`INTC_ICCEOIR_ADDR`. This is the **generic hardware-IRQ
## exception vector**, not the SWI/software-interrupt one (that's `swi_handler`, a *separate*
## function at `0x200056dc`, already named by an earlier session). Confirmed live (GDB-free QMP
## read of its RAM-resident dispatch table): table entry 0 is exactly `irq_context_switch_id0`
## (`0x20005960`, the ring's own already-known consumer) and entry 41 is exactly the DMAC ISR
## (`0x200b5b90`) — this is simply the shared entry point for every hardware IRQ the firmware
## handles, unrelated to the message-post/`software_interrupt(0)` path it was wrongly connected
## to. Ghidra renamed to `irq_exception_dispatch` with a corrected comment/bookmark. **The
## `0x20420120` ring's real burst source is open again** — the one solid new fact from this
## thread is a methodology one: narrowing the polling cadence to 0.02s right before the expected
## overflow *suppressed it entirely* (0/1, 50s) — even non-breakpoint periodic
## `interrupt()`+`read_memory()` perturbs this race if frequent enough; the coarse 0.25s cadence
## `trace_job_ring_overflow.py` already used is the proven sweet spot, not just "gentler than a
## breakpoint". See README-history.md's newest two sections for the full derivation, including
## why the breakpoint-based DMAC-race verification attempt tried first this session was also
## discarded as untrustworthy (a real, live example of the project's own documented gdbstub-
## artifact class) before the GDB-free QMP method settled that part.

**Confirmed, solid, foundational (from prior sessions, still true):**
- A custom QEMU machine (`rz-a1h`) builds cleanly against real QEMU v11.1.1 source (pinned,
  vendored checkout under `qemu-src/`, gitignored — `setup.sh` recreates it) and boots real,
  unmodified v1.42 firmware: `base.dat`'s traced sequence runs, the body decompresses, real GIC
  IRQ delivery works (OSTM0 is `body.bin`'s real tick source — GIC ID 134, `CMP`=32000).
- **With `tools/build_riic_eeprom_image.py`'s output supplied as RIIC2's backing image** (see
  "Running it" below), `FUN_2002b29c`'s entire cold-boot-vs-power-state branch decision clears —
  every EEPROM signature check and the real GPIO power-good gate all resolve correctly.
- **SCIF3's TXI (transmit-complete) IRQ is real and verified end-to-end** (found and fixed the
  same day the two bullets below did their work: not wired at all; a level-vs-edge/redundant-
  raise-is-a-no-op bug matching a class `riic.c` had already hit; and an emulator-only lost-edge
  artifact). Full three-bug derivation in README-history.md's "SCIF3 TXI made real" section —
  this Status section only tracks the current, much-further-along state from here on.

**Confirmed, solid, this session — continuing straight on from the SCIF5-responder/MTU2/RSPI2
work above (full narrative in README-history.md's last two sections):**
- **The three "near-identical" wait blocks in the newly-reached `0x2006003c`-area code weren't
  identical** — one has an inverted branch condition, easy to miss skimming but confirmed by
  reading the raw listing closely. It gated on a *fifth* MTU2 compare event: the *same* shared
  status byte (`0xFCFF0305`) the fourth event already covers, a *different* bit (0, not 2),
  paired with a *different* compare-target register (`0xFCFF0308`, not `0x30c`) — confirmed
  live (free-run poll) that this bit genuinely never sets on its own either. This one's real
  requested period *is* known (both call sites use the identical literal `0x7d00`/32000), so
  it's honored exactly rather than approximated — confirming this is a genuinely shared,
  multi-subsystem software-timeout facility, and this particular caller isn't SCIF5-related at
  all (reached via the DMA-descriptor-setup routine, very plausibly graphics/display).
- **Confirmed load-bearing across 2 independent trials**: boot progresses well past this point
  into previously never-reached code, and hits a **real, pre-existing generic
  overflow-protection trap** (already documented by an earlier session as backing a *different*
  ring's overflow protection — a shared, reused mechanism, not ring-specific).
- **Applied this session's own established diagnostic technique before assuming anything**:
  breakpointing the generic ring-push helper found one steady producer (~12 pushes/sec, zero
  overflows across 1092 breakpoint-slowed hits) — but feeding a *different* ring than the one
  that actually overflowed in an un-breakpointed trial. The breakpoint overhead itself likely
  delays reaching whatever triggers the second ring's overflow (this project's own established
  finding: pausing on every hit changes timing enough to mask a real race). A follow-up poll of
  the overflowing ring's own header showed values cycling in a burst-then-drain pattern between
  samples, not a steady climb — a different shape than the earlier-diagnosed rate-mismatch
  overflow, not yet root-caused.

**Confirmed, solid, this session's follow-up — continuing straight on (full narrative in
README-history.md's newest section):**
- The `0x20420120` ring's struct, producer (`FUN_20187bb4`, called only from `FUN_20186c4c`),
  and consumer (`FUN_20187ae4`, drained only inside `irq_context_switch_id0`, GIC ID 0's real
  context-switch handler) are all now fully derived from the actual code, not just polled bytes.
  **Corrects the prior session's own read of its breakpoint trace**: the LR it caught
  (`0x20186c67`) was already the right producer all along — its belief that producer was
  "feeding a different ring" was a misread of an opaque data value as a ring pointer.
- **New tracing technique, built because both breakpoint- and watchpoint-based approaches were
  already shown to mask this exact bug**: `tools/trace_job_ring_overflow.py` free-runs the whole
  boot and only pauses on a fixed wall-clock cadence (not tied to any specific guest code path),
  which stalls producer and consumer proportionally instead of desynchronizing them.
- **Overflow reproduced live, twice, with real numbers**: both trials show the queue sitting
  near-empty for tens of seconds, then going from ~1 pending to fully overflowing (16) within
  well under half a second — a genuine sudden burst, not a steady leak.
- **Tested the leading hypothesis (a DMAC completion cascade from the newly-reached
  "three chained transfers" graphics-DMA-setup code) live — ruled out**: temporary host-side
  `dmac.c` instrumentation showed channel 0 firing only once in a 60-second run (the
  already-known early-boot transfer, not this region). Reading `FUN_2005ff1c` itself directly
  also retracts the prior session's own "DMAC channel kicks" characterization — it only touches
  a software descriptor table, never real DMAC MMIO.

**Follow-up, same session: chased the most concrete lead (`FUN_2005ff1c`'s two callers both
bracket their shared body in a real `cpsid i`/`cpsie i` IRQ-mask pair) live — a real correction
after a second trial, not a clean confirmation.** `FUN_2005ff1c` turns out to be called directly
from `cold_boot_hw_init` itself (retracting the "graphics/display DMA" label — there was never
a live check behind it). A first trial looked like a clean hit (overflow only 0.55s after the
`cpsie`), but a second, independent trial directly contradicted it: the same `cpsid`/`cpsie`
sequence occurred at almost the same boot offset, but the drain trigger (`irq_context_switch_
id0`) then fired **142 times over the next 30 seconds** before an overflow finally happened —
proving the consumer runs reliably (~every 200ms) and ruling out simple starvation-by-masking
as the general cause. A third trial then showed the whole burst (near-empty to overflow)
completing in under ~0.22s of wall-clock time — too fast for reactive polling to ever catch a
producer breakpoint in time, closing off the "catch it in the act" approach as a dead end given
the tools available.

**The actual fix, found by pivoting to a clock-realism angle instead (user-supplied schematic
research was the key unlock): confirmed live across two full trials.** The IC-7300's own
schematic shows crystal `X301` (48.000MHz) on the main CPU's `USB_X1`/`USB_X2` pins — an exact,
unambiguous match for the RZ/A1H manual's clock mode 1, which gives a **fixed, real P0φ =
32.00MHz** (not the 25-33.33MHz range clock mode 0 would have left open). `OSTM_FREQ_HZ` had
always been an explicitly-flagged 500MHz "fast for testing" placeholder — changing it to the
real 32,000,000 *alone* didn't fix the overflow (tested live, a clean negative result, matching
the live-reasoned prediction that QEMU's default wall-clock-paced virtual time means a more
realistic *tick rate* alone doesn't stop unthrottled TCG from bursting through unrealistic
amounts of guest work inside any given real-time gap). Adding **`-icount shift=auto`** (QEMU's
instruction-count-paced virtual time) on top of the same real clock **did**: two independent
trials (130s, 160s) both completed their full duration with zero overflows, vs. every one of
roughly a dozen non-icount trials this session hitting the overflow somewhere in the 35-78s
range. Confirmed genuine forward progress, not a stall, via an added heartbeat print.

**Follow-up, same session: `-icount` wired in as an actual default, and pushed toward the
original goal — found a real, further blocker, not a false alarm.** Built `tools/qemu_launch.py`
(a shared launch helper, `-icount shift=auto` now its own default, replacing every tool
script's own copy-pasted launch line). MTU2's own real clock was also confirmed the same
way OSTM's was: channels 3/4's real `TCR` register value (`0x00`, found via direct
disassembly) decodes to P0φ/1 — the identical real 32,000,000 Hz, not the 25MHz `mtu2.c`
had been using. `MTU2_FREQ_HZ` updated accordingly. Then, running a longer 300s exploration
trial (past what the 130s/160s confirming trials covered) found DMAC's own completion wait
(`FUN_200b5ea4`, the exact busy-wait `dmac.c`'s real channel-0 model was built to unblock)
genuinely stalling — confirmed via `ps` (91% CPU, truly spinning) and Ghidra, not a
misreading. Diagnosis: `ostm.c`/`mtu2.c` both use QEMU's `ptimer` API (icount-aware by
design); `dmac.c` is the only device using a raw `QEMUTimer` directly, a known category of
icount pitfall. **This does not undo the ring-overflow fix above** — both its confirming
trials were real — it's a separate, deeper blocker on the same boot path, only reachable
*because* that fix cleared the way to it.

**Follow-up: ported `dmac.c` to `ptimer` — real improvement, but the underlying problem is
broader than one device.** Confirmed live: the `ptimer` port alone, under the default `-icount
shift=auto`, did *not* fix the stall (identical behavior to before the port). A fixed
`-icount shift=1` *did* clear it — but a full confirming trial then found a completely
different, already-known busy-wait (`scif5_wait_hsk1_ready`, the DSP-link handshake wait) stall
just as hard under that setting, 135 real seconds straight, 91% CPU. This looks like a general
icount-cooperation issue with this machine's own busy-wait-heavy boot style (many sequential
polling loops in `cold_boot_hw_init`), not a single device's bug — changing the icount
configuration relocates which wait stalls rather than eliminating the class of problem. Kept the
`ptimer` port (a real architectural improvement, consistent with `ostm.c`/`mtu2.c`, and it does
help under some configurations) but **the default stays `-icount shift=auto`**, since it's the
one setting empirically confirmed clean across the two original 130s/160s trials — `shift=1`
demonstrably fails faster and harder on a different wait. SCIF/RIIC/RSPI2 were also checked
(background research) and confirmed to have no timing-sensitive constants at all currently —
nothing to correct there regardless of how this resolves.

**Follow-up diagnostic, handoff prep for a new session**: confirmed via temporary debug
instrumentation (reverted after) that DMAC's `ptimer` completion callback fires correctly and
promptly, and confirmed via a live GDB breakpoint that the real ISR (`FUN_200b5b90`) genuinely
gets entered — ruling out "the timer/IRQ mechanism itself never fires" as the explanation.
Yet in a wall-clock-polling trial the *same* single code path (`FUN_200b5ea4` has exactly one
real caller, confirmed via `references_to`) reliably stalls, while under a light-breakpoint
diagnostic it completes in under a second. **Refined, checkable hypothesis**: `-icount
shift=auto`'s own adaptive tuning is likely stateful (adjusts based on observed workload over
time), so different observation methods leave it in a different internal state by the time
this code runs — not yet confirmed against QEMU's own source.

**RESOLVED, 2026-09-09 (new session) — not the way any of the above expected.** Following
exactly the "concrete next steps" plan above: step 1 (read QEMU's own `-icount shift=auto`
source) confirmed the adaptive tuner really is stateful, mechanically supporting the theory —
but step 2 (reproduce via a non-GDB method) is what actually broke the case open, and the answer
it gave was the opposite of expected. Host-side-only instrumentation directly inside `dmac.c`
(independent of GDB entirely) proved the real arm→ptimer→IRQ→guest-ISR chain completes in
**under 1ms of real time**, every trial, 8/8, including trials that reintroduced GDB, breakpoints,
and even a real hit-and-resume cycle one at a time. The *only* configuration that ever failed to
show completion was observing it exactly the way every prior session did: a GDB breakpoint
sitting at the wait's own natural exit — which is the literal fall-through target of the tight
loop's own conditional branch. **Conclusion: this was never a real DMA or `-icount` timing bug —
it's a QEMU GDB-remote-stub reliability artifact** for this address pattern under `-icount
shift=auto` (breakpoints silently not trapping there, and/or a forced `interrupt()` reporting a
stale PC). The root gdbstub-internals cause is still open but no longer blocks this project.
**Durable methodology lesson**: cross-check any future "this busy-wait never clears" finding
against a genuinely GDB-free run (no `-S`/`-gdb` in the launch at all — `qemu_launch.py`'s
`-S -gdb tcp::1234` defaults mean every earlier "hands-off" test on this thread was secretly
still running under gdbstub) before trusting it. Full derivation, including the sequence of
isolating trials that ruled out every other candidate variable (breakpoint presence, client
connection, `-S` itself, QMP-vs-GDB resume) one at a time, in README-history.md's newest section.
`mtu2.c`'s 32MHz re-validation and the original SD-card/VFS testing goal are both open again now
that this false blocker is out of the way.

## Confirmed peripherals

| Device | File | Status |
|---|---|---|
| GIC (Distributor + CPU I/F) | `rz_a1h.c` (QEMU's own `arm_gic`) | Real, working — see the off-by-32 `qdev_get_gpio_in` bug in README-history.md |
| OSTM0 / OSTM1 | `ostm.c` | Real timer + real IRQ. OSTM0 is `body.bin`'s real tick source (ID 134, `CMP`=32000) |
| SPI boot status | `spi_boot.c` | Real — the one register `base.dat`'s SPI-ready poll needs |
| GPIO/port registers | `gpio.c` | Real (masked set/clear, `PNOT` toggle, live `PPR` pin levels) — `P1_6`/`PDV` (power-fail detector) defaults high, see Status above |
| L2C (PL310 cache controller) | `l2c.c` | Real (`CACHE_ID`/`CACHE_TYPE`/`REG7` self-clear semantics) |
| CPG | `rz_a1h.c`'s `add_plain_ram_region()` | Plain storage, no behavior — nothing traced needs more yet |
| MTU2 | `mtu2.c` | Real channel 3's `TGI3A` (GIC 154), channel 4's `TGI4A`/`TGI4C` (GIC 159/161), and two more purely-polled compare-match events sharing one status byte (`0xFCFF0305` bits 2/0, targets `0x30c`/`0x308`, no GIC ID — host-wall-clock deadlines, not a live counter) — **all five confirmed load-bearing** (ch3 unblocks `cold_boot_hw_init`'s task-readiness wait, ch4 unblocks `dsp_boot_handshake`, the fourth unblocks `scif5_cmd_transmit_now`, the fifth unblocks a DMA-descriptor-setup routine, 2026-09-09), see Status above. Every other channel/register/event still plain storage (`regs[]` passthrough). **`MTU2_FREQ_HZ`'s 25MHz was chosen empirically, not derived — not yet re-validated under `-icount` (see Status above), a real candidate for its own re-check** |
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR protocol + virtual EEPROM (only RIIC2 exercised by any traced boot path so far — the diode-matrix EEPROM, `IC351`/`GT24C128B`) |
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI IRQ per channel. Real RXI backing two virtual responders: a front-panel one on channel 3, and a DSP-link one on channel 5 (the latter triggered by a second, tiny MMIO region at `0xFCFE3120` on the channel-5 instance only, not by SCIF registers — see Status above) |
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
- **`tools/trace_job_ring_overflow.py`** (new, 2026-09-09) — targeted, low-perturbation tracer
  for the `0x20420120` job-ring overflow (see Status above): free-runs the boot and samples the
  ring's header on a fixed wall-clock cadence rather than breakpointing/watchpointing the hot
  push/drain path itself (both already shown to mask this specific bug by desynchronizing
  producer and consumer). Supports an optional coarse→fine two-phase polling cadence
  (`[seconds] [poll_interval] [fine_start] [fine_interval]`) to narrow in once a run's
  approximate overflow time is known. **Don't `pkill -f qemu-system-arm` before launching it**
  (or any new script like it) — that pattern matches the invoking shell's own command line and
  self-kills; see README-history.md's newest section. **Same gotcha found again, 2026-09-09,
  this time with `pgrep -f`**: `pgrep -f "build/qemu-system-arm.*rz-a1h"` self-matched too
  (any `-f` full-command-line pattern that appears as literal text in its own invocation is at
  risk, not just `pkill`) — `pgrep -x qemu-system-arm`/`pkill -x qemu-system-arm` (exact
  `comm` name match, not a command-line substring) is the safe alternative when checking for or
  killing a stray instance. **A third, unrelated gdbstub gotcha found the same day**: dropping a
  `gdbrsp.py` connection (letting the Python process exit) while the target is mid-`continue`
  under `-icount` leaves the gdbstub in a state where the *next* client's first packet gets no
  ack (`GdbRspError: no ack for packet 'g', got '$'`) — always let a script run to a clean
  `g.close()`/exit via `wait_stop()` first, or just restart QEMU, rather than reconnecting to a
  live instance a previous script abandoned mid-flight.
- **`tools/trace_irq_mask_window.py`** (new, 2026-09-09) — breakpoints the `cpsid i`/`cpsie i`
  pair bracketing `FUN_2005ff1c`'s shared caller body plus the ring-overflow trap (all rare,
  low-overhead), and can reactively arm a fourth breakpoint on `irq_context_switch_id0`'s own
  entry to count how often the ring's drain trigger actually fires in a given window — the
  technique that found the drain trigger fires reliably (~every 200ms) even right before an
  overflow, ruling out simple starvation. See README-history.md's newest section.
- **`tools/trace_dmac_isr.py`** (2026-09-09) — two low-frequency breakpoints (the real DMAINT0
  ISR entry, `FUN_200b5ea4`'s own wait-loop check) to check whether the DMAC completion ISR
  genuinely gets entered at all. **The "stalls under one observation method but not another"
  puzzle this tool originally chased is now resolved** — see the GDB-remote-stub reliability
  artifact finding in Status above/README-history.md's newest two sections — but the tool itself
  is still a valid, reusable light-breakpoint diagnostic.
- **`tools/measure_dmac_wait_throughput.py`** (new, 2026-09-09) — a *failed* measurement attempt,
  kept as a documented cautionary rather than deleted: tried comparing real elapsed time for N
  consecutive GDB single-steps inside vs. outside the DMAC wait loop, to test whether it has
  disproportionate real per-instruction cost. Result was uninformative, not just wrong — both
  regions measured ~82ms *per single step*, meaning GDB single-step round-trip overhead
  completely dominated the measurement under this `-icount` config, regardless of what
  instruction actually executes. See README-history.md's newest section before retrying this
  technique on a different suspected-slow region.
- **`tools/trace_dmac_wait_completion.py`** / **`_completion2.py`** (new, 2026-09-09) — the
  technique that actually found the two-internal-waits structure of `FUN_200b5ea4` and bracketed
  each one's natural exit with exactly one breakpoint, free-running otherwise untouched — the
  cleanest way to ask "does this specific wait ever complete" without the single-step-timing
  failure mode above. Superseded in spirit by `src/rza1h_debug.h` (below) for *this* specific
  question now that the real answer is known, but the bracketing technique itself is reusable for
  any future suspected busy-wait.
- **`tools/trace_dmac_icount_shift.py`** / **`tools/trace_dmac_hands_off.py`** (new, 2026-09-09)
  — isolating trials built to test (and ultimately rule out) "periodic GDB `interrupt()`/`cont()`
  cycling alone" and "a fully hands-off `ps`-only check" as the DMAC stall's cause. Both came back
  clean/inconclusive on their own; the real resolution needed host-side device-model logging
  instead (see `src/rza1h_debug.h`) — kept for their own reusable isolating-trial patterns.
  See README-history.md's newest sections for the full derivation of why each of these tools
  exists and what each one actually settled.
- **`tools/trace_cold_boot_hw_init_tail.py`** (new, 2026-09-09/10) — waypoint-breakpoint sweep
  across `cold_boot_hw_init`'s own remaining call sequence, the same technique the 2026-09-08
  session used to localize `FUN_2001dd58` inside `riic2_driver_init`. Superseded as the *first*
  step by `trace_fun200b5f38_wait.py` below once smoke tests showed zero hits even at the very
  first waypoint — its own module comment explains why and points to the more targeted tool.
  Still useful if/once `FUN_200b5f38` itself is resolved and the blocker moves further downstream.
- **`tools/trace_fun200b5f38_wait.py`** (2026-09-09/10) — brackets `FUN_200b5f38`'s own
  loop-1-exit (`0x200b5fbc`) and real return (`0x200b5fd8`); ran TIMEOUT as the prior session
  predicted, but the *reason* for that TIMEOUT was corrected 2026-09-10 (the CPU never reaches
  `FUN_200b5f38` at all — see Status above). Kept for the bracketing technique, not the original
  diagnosis, per its own updated module comment.
- **`tools/read_fun200b5f38_flags.py`** (new, 2026-09-10) — the tool that found the real story:
  free-runs past early boot noise, then periodically `interrupt()`s and reads `r15` plus
  `0x203906EE`/`0x203906ED` directly (no breakpoint at all). Found the CPU actually parked inside
  `FUN_200b5ea4`'s *own* loop 2, not `FUN_200b5f38`'s loop 1 — the finding that unraveled
  2026-09-09's "RESOLVED" DMAC conclusion. See Status above.
- **`tools/qmp_read_mem.py`** (new, 2026-09-10) — a general-purpose, fully GDB-free physical-
  memory reader via QMP's `human-monitor-command` → `xp` (needs only `-qmp unix:...`, no
  `-S`/`-gdb` at all). The tool that actually confirmed the DMAC completion race, and settled
  it independently of `gdbrsp.py`'s own remote-serial path — reach for this whenever a finding
  needs a second, structurally-different confirmation before being trusted, the same lesson
  2026-09-09's own "GDB-remote-stub artifact" story should have applied to itself but didn't.
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

**`-icount shift=auto` is now recommended, not optional**, for any boot test that needs to
reach past ~30 seconds of boot time reliably: without it, `body.bin` hits a real job-ring
overflow trap (`0x200b93fc`) somewhere in the 35-78s range with high, near-total reliability
(a genuine emulation-timing-realism gap, not a firmware bug — see Status below and
README-history.md's newest sections for the full derivation). Confirmed clean across two full
130s/160s trials with it enabled, vs. every trial without it hitting the trap.

Omit the `-global rza1h-riic.image=...` line to boot with an empty virtual EEPROM instead — a
real, valid configuration (matches how earlier sessions tested), but boot will stop much
earlier, at the pre-cold-boot-branch watchdog/`wfi` point documented in README-history.md,
rather than reaching the current SCIF3 frontier.

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
5. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — the active thread. `mmc.c`
   built and validated standalone; `riic.c` built and validated end-to-end against a real
   natural boot; `FUN_2002b29c`'s entire cold-boot branch gate now clears; the SCIF3
   front-panel handshake now genuinely completes; boot reaches real `itron_act_tsk` task
   activation; `mtu2.c` (channels 3 and 4) and `dmac.c` together clear `cold_boot_hw_init`'s
   whole task-readiness-wait cluster and `dsp_boot_handshake`'s own wait; a generic RTOS
   job-queue overflow that followed turned out to be a self-inflicted timing artifact, fixed by
   tuning `mtu2.c`'s tick rate; a virtual SCIF5 DSP responder resolved `scif5_send_and_wait_
   reply`'s own reply-ready deadlock; `mtu2.c`'s fourth compare event and `rspi2.c` together
   cleared `scif5_cmd_transmit_now`'s own busy-wait and a real RSPI2 transmit stage; **`mtu2.c`'s
   fifth compare event (2026-09-09, same shared status byte as the fourth, a different bit)
   cleared a DMA-descriptor-setup routine's own busy-wait** — see Status above for the full
   derivation of each. Boot then reached a real, pre-existing generic ring-overflow trap in
   genuinely new territory — **fully resolved, 2026-09-09**: not a firmware bug, but a genuine
   emulation-timing-realism gap (unthrottled TCG bursting through unrealistic amounts of guest
   work between OSTM's own wall-clock-paced real GIC IRQ), fixed by pairing OSTM's real,
   schematic-confirmed clock (32.00MHz) with QEMU's `-icount shift=auto`, confirmed clean across
   two full 130s/160s trials — see Status above and README-history.md's newest sections for the
   full derivation. Always launch with `-icount shift=auto` (see "Running it" above, now this
   machine's own default via `tools/qemu_launch.py`) for any boot test past ~30 seconds.
   **A further, deeper blocker appeared to be found on the very next long trial past this one —
   RETRACTED, 2026-09-09 (new session): not a real blocker at all.** `FUN_200b5ea4`'s completion
   wait looked like it genuinely stalled under `-icount`, diagnosed at the time as a
   `QEMUTimer`-vs-icount interaction issue in `dmac.c` (motivating the `ptimer` port below). A
   fresh session's host-side-only instrumentation (independent of GDB entirely) proved the real
   arm→ptimer→IRQ→guest-ISR chain actually completes in under 1ms of real time every time — what
   was actually unreliable is QEMU's own GDB remote-stub breakpoint/interrupt reporting for this
   address pattern under `-icount shift=auto`, not the device model or the timing. See Status
   above and README-history.md's newest section for the full derivation and the durable
   methodology lesson it leaves behind. The `ptimer` port itself is still a real architectural
   improvement (matches `ostm.c`/`mtu2.c`) and is not being reverted, but the original "found a
   real `-icount` bug" framing should be read with real skepticism now. The original question —
   does the SD-card update flow reach MMCIF against a *properly* kernel-created task, and would
   the whole chain accept and boot custom firmware entirely offline — is open again, no longer
   gated on a `dmac.c` fix. **Passive-observation half checked, same session**: two genuinely
   GDB-free 900s (15-minute) free runs (one with `-d unimp,guest_errors`, one with the new
   `RZA1H_DEBUG=dmac,riic,scif,rspi2`, see `src/rza1h_debug.h` in Directory layout below) found
   boot reaches a real, alive, stable idle state — `mtu2.c`'s scheduler tick fires continuously
   (~964Hz) the whole time, while every other traced peripheral shows zero new activity past the
   first ~8 real seconds. **`force_call_fup.py` retested against this state, same session,
   immediately after — this is where the real, if less exciting, answer actually came from.**
   After fixing a genuine bug in the retest itself (hijacking from cold reset with no stack ever
   set up causes a real Prefetch Abort — free-running naturally first, then hijacking the
   already-running context's real stack, is required, matching the original 2026-09-08
   session's own technique), the retest reproduces that same session's exact behavior:
   legitimate context-switching, never reaching MMCIF, `firmware_update_main` never returning. A
   direct read of `sdcard_file_rpc_dispatch_task`'s own creation struct confirms why, ruling out
   "just needs more time" first (still zero after 120 real seconds of untouched free-running
   boot). **First attributed this to the already-diagnosed 2026-09-08 `FUN_2002b29c` gate —
   corrected same session, not left standing**: `cold_boot_mode_dispatch`'s own decompile shows
   it calls `cold_boot_hw_init()` directly (which demonstrably runs), so that gate is already
   passed; `system_mode_request_dispatch()` is only reached once `cold_boot_hw_init()` itself
   returns, and something inside *that* function's own remaining body is the real blocker.
   **Precisely localized it before handing off (2026-09-09), then actually fixed, 2026-09-10 —
   RETRACTED the "resolved" framing of the DMAC/icount stall a second time, for real this time.**
   The prior session's own localization ("`FUN_200b5f38` stuck in its own loop 1") turned out to
   be one function off: the CPU never reaches `FUN_200b5f38` at all, because `FUN_200b5ea4` (the
   *preceding* call, "resolved" the session before that) itself never returns — its own
   completion flag gets permanently stuck by a real, self-inflicted emulation race (the firmware's
   own "mark busy" write lands one instruction after the arm write, late enough for the model's
   sub-microsecond `ptimer` completion to race ahead of it under `-icount` and get silently
   overwritten). Fixed by raising `dmac.c`'s completion delay past the race window (1000ns → 100us),
   confirmed via a fully GDB-free QMP memory-read cross-check (`tools/qmp_read_mem.py`, new) rather
   than trusting a breakpoint anywhere near the race itself. **With the fix in, boot progresses
   further than ever before — straight into the already-known `0x200b93fc` job-ring-overflow trap,
   reproduced 3/3 fresh trials.** Whether that's the same overflow mechanism re-surfacing under a
   now-different boot timing profile, or something the DMAC fix's own changed scheduling newly
   exposes, is genuinely open — not yet traced. See Status above and README-history.md's newest
   section for the full derivation, including why a breakpoint-based verification attempt was
   itself discarded as untrustworthy (a live instance of this project's own documented gdbstub-
   artifact class) before the GDB-free QMP method settled it. **Concrete next step**:
   `tools/trace_job_ring_overflow.py` against `0x200b93fc`, re-deriving producer/consumer/timing
   fresh rather than assuming the prior `0x20420120`-ring analysis still applies unchanged.
