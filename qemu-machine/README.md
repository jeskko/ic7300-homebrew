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

## Status, 2026-09-20 — the ring-overflow thread's core mechanism is FOUND, via two rounds of
## fresh-eyes Opus review plus live QEMU `gic_*` tracepoint instrumentation (a new, zero-
## perturbation capability). **It's a real device-model bug in `riic.c`, not a GIC issue at
## all.** A same-day intermediate theory (GIC priority-mask boundary collision) was proposed,
## looked promising, but was directly disproven by live data and is recorded below only so the
## retraction is on record — skip to "The actual mechanism" if short on time.

### Retracted: the GIC priority-mask "boundary collision" theory

First pass proposed that the project's own earlier (correct) 5-bit GIC-priority-width fix
truncates both per-IRQ priority and `GICC_PMR` to the same `0xF8`-aligned grid, and that SGI0's
priority `0xFE` truncating to `0xF8` would collide with `GICC_PMR` also settling at `0xF8`,
making `best_prio < priority_mask` (`arm_gic.c:200`) permanently false. A live QMP snapshot
(`PMR=0xf8` on every sample) seemed to confirm it, and it was committed to this file as "very
likely resolved."

**Directly disproven by checking the actual live numbers, not just the mask arithmetic**: SGI0's
real configured priority reads `0xF0` (240), not `0xF8`. `240 < 248` is **true** — the delivery
gate is satisfied, not blocked. A live `gic_*` tracepoint capture (see below) confirms SGI0 gets
acknowledged 1100+ times over a few seconds of boot — nowhere near "structurally undeliverable."
A second Opus pass, given the corrected numbers and this project's own `arm_gic.c`, confirmed the
retraction cleanly (checked `gic_get_group_priority`/`BPR`/security-extension aliasing — none of
them create an asymmetry here; `gic_fullprio_mask()` applies identically to both sides of the
comparison). One real nuance survives from the original theory, kept because it's true and
useful context: firmware's `irq_nesting_enter` idiom (`PMR=0xff; PMR=readback-1;`, a byte-for-
byte match to Renesas' own CMSIS macro in `scratch/r01an5093ej0170-rza1-swpkg/.../rt_HAL_CA.h`)
does transiently leave `PMR` at `0xF0` for microseconds during nested-IRQ entry, which *would*
briefly block anything else also at `0xF0` — but this is restored within the same handler, not a
sustained multi-millisecond effect, and not what causes the overflow.

### The actual mechanism, found and independently source-verified

**New capability used**: QEMU's own `gic_*` event tracepoints
(`--trace 'gic_update_bestirq' --trace 'gic_acknowledge_irq' --trace 'gic_set_irq' --trace
'gic_cpu_write' --trace 'gic_dist_write' -D <logfile>`, optionally `-msg timestamp=on` for
wall-clock-stamped lines) give a full, real, in-order log of every GIC arbitration decision —
zero perturbation, no GDB, no QMP polling — something this thread never had before. Live capture
of a full boot-to-overflow run found:

- Producer = IRQ 154 (MTU2 TGI3A, the periodic ~82ms ring doorbell) — 1471 acknowledgements
  against 1470 `GICD_SGIR` (software-generated-interrupt) writes, a clean 1:1 doorbell-per-tick.
- At one specific moment, **IRQ 205 (TEI, RIIC2's transmit-end interrupt) asserts level-1 and is
  never lowered again** — a real, level-triggered line stuck permanently high. Being
  level-triggered, the GIC correctly re-pends it after every EOI: 568 further acknowledge/EOI/
  re-pend cycles of IRQ 205 follow, with no further `gic_set_irq` needed to keep it pending.
- From that instant, **SGI0 is never selected as `best_irq` again** — zero
  `gic_update_bestirq ... irq 0` events for the rest of the run, out of ~456K GIC events total.
  This is genuine, continuous priority starvation (IRQ 205 at priority `0x10` permanently
  outranks SGI0's `0xF0`), not a mask block and not a producer-side latch bug.
- Exactly **16 SGIR doorbells land undrained across 29.2ms** before the 17th push overflows the
  ring and hits the trap — matching this thread's own already-established "16-slot ring, ~30ms
  margin" numbers exactly, now with a named, mechanistic cause instead of an unexplained gap.
- The final acknowledged IRQ before the trap is 154 (the producer's own MTU2 tick) — it never
  gets its matching EOI, since the ISR's own tail is what calls into the ring's overflow-check
  function that hits the trap (`b .`, never returns) — this is *why* `running_priority` is stuck
  at `0x10` forever after the crash, confirming that part of the post-crash trace as a real but
  downstream symptom, not a separate cause.

**Root cause, independently confirmed by me at the source level before trusting it** (this
project's own established practice — don't take a subagent's claim on faith): `riic.c`'s own
comment on `SR2_START`/`SR2_STOP`/`SR2_NACK` (around line 144) asserts "TI/TEI/RI never touch
SR2 at all in their own handlers." **This is factually wrong.** Decompiled the real TEI ISR
(`FUN_2001db50`, registered for GIC ID 205 in `riic2_driver_init`) directly: its first action is
`*(byte*)(RIIC2_base+0x24) &= 0xBF` — `RIIC2_base+0x24` is exactly `RIIC_REG_SR2`, and `& 0xBF`
clears bit `0x40` = **TEND**, a real SR2 bit real firmware's TEI handler genuinely clears. The
device model doesn't track `TEND` (or `TDRE`/`RDRF`) as an `SR2` bit at all — only
`START`/`STOP`/`NACK` are modeled, and `IRQ_TEI`/`IRQ_TI` are only ever lowered from specific
`DRT`/`CR2` write paths (see the `CR2=SP` handling in `riic.c`'s `RIIC_REG_DRT` case). In the
specific write-loop scenario the trace caught, firmware's real TEI ISR (`FUN_2001db50`) does
**not** reach any `DRT`/`CR2` write in either of its own two branches — it only touches local
state and clears the (unmodeled) `TEND` bit, apparently deferring the actual next bus action to
a separate, later call. Against the current, incomplete device model, nothing in that specific
path ever calls `qemu_irq_lower(IRQ_TEI)`, so the line latches high — matching the trace's
"asserted level-1, never lowered" finding exactly.

**Concrete next steps, not yet done**: (1) add `SR2_TEND` (`0x40`) — and probably `TDRE`
(`0x80`)/`RDRF`(`0x20`) — to the modeled `SR2` bits, set on raising `TI`/`TEI`/`RI`, cleared (and
`qemu_irq_lower()`'d) on the matching `SR2`-write-clears-it path, mirroring the existing
`START`/`STOP`/`NACK` handling; (2) before coding, use `RZA1H_DEBUG=riic` over the same boot
window to see exactly which register access real firmware's TEI-handler *tail* (the code that
runs after `FUN_2001db50` posts whatever event/flag it posts) actually performs, so the fix
lowers the line at the real, correct access rather than a guessed one; (3) consider a
belt-and-braces invariant/log if any RIIC IRQ line stays asserted across more than N
acknowledgements, to catch this whole class of bug faster in the future. Given this pins the
overflow on a genuine, fixable device-model gap (not real GIC hardware behavior), it also
reopens — favorably — the "would real hardware also overflow" question: real RZ/A1H silicon's
own TEND/TDRE/RDRF handling is presumably complete and correct, so this specific failure mode is
very likely **QEMU-model-specific after all**, consistent with the user's own real hardware
never having shown this symptom.

### FIXED, same day — implemented, built, and confirmed. The ring-overflow trap no longer fires.

Added `SR2_TEND` (`0x40`) to `riic.c`'s modeled `SR2` bits, set alongside every `IRQ_TEI` raise
(both call sites — the post-address-bytes `RIIC_WAIT_MEM_LO` case and the write-data-loop's own
`RIIC_WAIT_RESTART` re-arm), and added the matching `qemu_irq_lower(s->irq[IRQ_TEI])` to the
`SR2` write handler alongside the existing `START`/`STOP`/`NACK` cases — mirroring their exact
pattern. The existing explicit `IRQ_TEI` lowers at the `CR2=RS`/`CR2=SP`/next-`DRT`-write sites
are kept as-is (harmless, idempotent belt-and-braces for scenarios that don't go through this
exact SR2-clear path). `TI`/`RI` untouched — their own existing lower sites are the driver's own
synchronous continuation within the same access, not proven to share this gap.

Reconfigured and rebuilt `qemu-src/build` (an unrelated system `libibverbs` version bump had
gone stale in the existing `build.ninja`; `../configure --target-list=arm-softmmu` from the
build dir fixed it, no source-level issue). `riic.c` compiled clean.

**Confirmed working, two independent trials, GDB-free**: `tools/trace_sgi0_gic_state_qmp.py`
run for 25s and again for 60s — **zero overflows in either**, vs. every prior trial this whole
thread has ever run overflowing by `t≈4s`. Independently sampled `PC` every 2s over a fresh 30s
run to rule out a silent stall masquerading as "no crash": genuine forward progress through
early boot (`0x20005dbc` → `0x20006324`), then settling into a **new, different, small bounded
busy-wait** (`FUN_200b3c5c`, `0x200b3cb8`: `ldrb r0,[r6]; cmp r0,#0; bne` — waiting on some
not-yet-identified flag byte to clear) starting around `t≈6s` and holding through `t=30s`, `ps`
confirming genuine CPU activity (~178%, actively spinning this small loop, not idle/WFI) rather
than the trap's frozen single-PC `b .` signature. This is real, substantial further boot
progress — not just "the crash stopped happening" — matching this whole project's established
pattern of each fixed blocker revealing the next one. `FUN_200b3c5c` is a fresh, unidentified
frontier, not chased further this session.

### FIXED, same day, continued — the next blocker (`FUN_200b3c5c`) traced and fixed too. Boot now
### reaches a genuine, stable, self-sustaining idle steady-state — the furthest this whole
### `qemu-machine/` project has ever gotten, by a wide margin.

`FUN_200b3c5c` (the internal antenna-tuner relay network's own cold-boot init, calling
`tuner_relay_serial_bus_init` then setting all 4 relay-command "dirty" bits and waiting for the
byte to clear back to 0) traced to `tuner_relay_serial_bus_init`'s own
`register_event_handler(0xa0, tuner_relay_tstb_strobe_dispatch)` call. `0xa0` = **160 decimal =
TGI4B**, confirmed against `scratch/r01an5093ej0170-rza1-swpkg`'s `INTC_ID_TGI4B` — MTU2 channel
4's compare-match-B interrupt, never modeled in `mtu2.c` (only `TGI4A`/`TGI4C` existed). Traced
the real arming call (`FUN_200b3844`, the relay-shift-out helper) writing
`TGRB_4 = TCNT_4 + 0x480` — the identical "explicit register write while channel already
running" arming style as the already-modeled `TGI4A`, just its own compare register/status bit.
Derived `TGRB_4`'s real offset (`0x21e`, undocumented until now) directly from the decompiled
pointer arithmetic — lands exactly 2 bytes past the already-confirmed `TGRA_4` (`0x21c`), the
expected real MTU2 layout.

**Fixed**: added a fourth `RZA1HMtu2Event` (`ch4b`) to `mtu2.c`, modeled identically to `ch4a`
(same `TCR_4`/`TIER_4`/`TSR_4` sharing, own `TGRB_4`/bit-1 pair, `arms_on_tstr=false`), wired to
GIC ID 160 in `rz_a1h.c`/`rz_a1h.h` alongside the existing `TGI4A`/`TGI4C` connections.

**Confirmed working**: PC sampling over 40s showed the `FUN_200b3c5c` busy-wait gone, replaced
by real varying execution (the ring producer, `irq_exception_dispatch`, other cluster code)
interspersed with `0x200b93ac` — **not** a new trap (that's `0x200b93fc`, 80 bytes later and a
bare `b .`) but a completely standard ARM `dsb; sev; wfe; wfe; b` idle-loop idiom, i.e. the
RTOS's own idle task legitimately waiting for the next event. Reran the ring/GIC tracer for a
full 60s: **zero overflows**, ring pressure rising to `pending=15` several times and always
fully draining back to `write==read` shortly after — genuine, healthy, self-sustaining periodic
operation, not a fluke. This is the furthest any session of this whole `qemu-machine/` project
has ever reached — a real, stable steady-state, not just "further before the next wall."
Whatever comes next (front-panel/display bring-up, CI-V, etc.) is unexplored territory for a
future session.

### Side exploration, same day — forced the OTHER power-state branch (PWRK-wait, never before
### exercised) for general interest: confirmed reachable, added a live "press the button" test
### capability, and found a genuinely new, further frontier gap on that branch.

Per the user's own curiosity (do we know if we're at the PWRK-wait point yet, and could we force
it and try triggering the key ourselves) — see the answer given earlier the same day in this
README's history for the full "why not" (the auto-power-on-vs-wait-for-PWRK branch is decided by
one EEPROM-persisted bit, and our virtual EEPROM happens to say "was on"). Tried actually forcing
the other branch:

- **Flipped the EEPROM bit.** `pwrk_power_state_read_from_eeprom` reads offset `0x3e00`, but
  `riic.c`'s own long-established "dummy read after switching to receive mode" driver quirk
  shifts the effective delivered byte to `mem_addr+1` — so the real byte that matters is at file
  offset `0x3e01` (`0xff` in the working `riic2_eeprom.img`). Built a **separate** test image
  (`riic2_eeprom_pwrk_test.img`, not committed, not gitignore-tracked, trivially regenerable —
  see the one-liner below) with that byte's bit 7 cleared. Confirmed live: boot now parks at
  `0x20029b18`, inside `power_state_pwrk_wait_and_bringup` (`0x20029914-0x20029de7`) — the
  branch never once exercised by any prior session of this whole project.

- **Found and fixed a real, adjacent gap before the experiment could even be meaningful**:
  `gpio.c`'s `pin_level[1]` (Port 1) never had bit `0x80` (`P1_7`/`PWRK`) set at reset at all —
  defaulting to 0 (the driver's active-low convention means this reads as "already pressed" from
  the very first check), which would have made the wait loop exit trivially at `t=0` rather than
  actually waiting. Added the same "justified exception" reset default already used for
  `P1_6`/`PDV` and `P8_9`/`HSK1` — `pin_level[1] |= 0x80` (idle-high, unpressed) — a real
  correctness fix in its own right, not just an experiment enabler.

- **Added a genuine, reusable live-press capability**: a `pwrk-pressed` QOM boolean property on
  `gpio.c` (`rza1h_gpio_get/set_pwrk_pressed()`), the only way to simulate this button being
  physically pressed since `riic.c`'s own `PPR` write handler correctly ignores guest writes
  (real hardware read-only) and there was no other host-side hook. Settable at launch
  (`-global rza1h-gpio.pwrk-pressed=on`) or live via QMP `qom-set`.

- **Result, genuinely interesting**: read the raw disassembly of the wait loop carefully (not
  just decompiled C, which had obscured the real structure) — `power_state_pwrk_wait_and_bringup`
  has a **built-in timeout that proceeds into bring-up regardless of whether a press was ever
  seen**: a real press (`PPR1` bit `0x80` reading low) jumps into the same debounce-timeout check
  a plain timeout-without-a-press eventually falls through to anyway. Tested both directly, fine-
  grained (0.2s polling): booting with `-global rza1h-gpio.pwrk-pressed=on` from `t=0` and
  booting with the pin correctly left unpressed the whole time produced **identical PC traces and
  identical timing** (~1.4s under `-icount shift=auto` to reach the same landing point,
  `0x20029b18`) — the observable behavior in this emulation is currently dominated entirely by
  the debounce/timeout logic, not by the raw press signal, so this specific live-press capability
  doesn't (yet) produce a visibly different trace on this exact path. Kept anyway — a real,
  reusable capability for whenever a future session finds a scenario where it does matter (e.g.
  the release-detect `pwrk_irq7_isr` path, or a scenario with a real, unbounded wait).

- **New frontier found, not chased further**: once past that landing point (`0x20029b18`, inside
  a `WaitForInterrupt()`/`wfi`), the CPU **never wakes again** — confirmed via a full 60s trial,
  PC frozen at the identical address for the entire run, `ps` showing only ~4% CPU (a genuine
  halted `wfi`, not a hot spin) — unlike the auto-power-on path's own idle state, which cycles
  normally on MTU2's periodic ticks. This branch has never been exercised by any prior session,
  so an unmodeled dependency here (something this specific code path needs to actually receive an
  interrupt and continue) is expected, matching this whole project's established pattern — a
  fresh, real, further frontier for whenever this branch is picked up again, not the main
  auto-power-on path this session's other fixes were about.

One-liner to regenerate the test image if needed: flip bit 7 of byte `0x3e01` in a copy of
`riic2_eeprom.img` (see the Python snippet in this session's own transcript, or just re-derive:
`data[0x3e01] &= 0x7f`).

### Follow-up, same day — actually wired `IRQ7` for real and confirmed live: a genuine PWRK
### press/release *does* wake the CPU out of that `wfi`, and real firmware's own ISR runs.

The "new frontier" above turned out to be an incomplete emulation gap, not a dead end — the
model only had a *readable* `PPR1` bit for `PWRK`, with no path from a pin transition to an
actual GIC interrupt. Real `P1_7` feeds two genuinely separate things on real hardware: the
plain GPIO port's own read-back register, and a dedicated RZ/A1H-specific external-IRQ
front-end (`ICR1` sense-select, `IRQRR` pending/ack) that turns a qualifying edge into `IRQ7`
(GIC ID 39). Only the first ever existed in this emulation.

**Built the missing piece.** Traced `ICR1`/`IRQRR`'s real, live pointer values
(`pwrk_irq7_config_init`/`pwrk_irq7_isr`) to `0xFCFEF802`/`0xFCFEF804` — a genuine, confirmed
divergence from the generic Renesas reference package's own documented `INTC` struct base
(`0xE8201000`, which is this project's already-modeled GIC distributor's own base — real
firmware's pointers land at a different base entirely, though the same relative field layout).
Live-checked `GICD_ICFGR2`: ID 39 is left at the `arm_gic` default (level-triggered) — real
firmware never touches it — so the front-end has to hold the line asserted and only release it
when firmware clears `IRQRR`'s own flag bit, exactly like `riic.c`'s `SR2`/`mtu2.c`'s `TSR`. Also
confirmed live that this firmware's own `ICR1` value for `IRQ7` selects rising-edge sensing —
fires on release, not the initial press, matching `pwrk_irq7_isr`'s already-understood role.
Added a second `MemoryRegion` + a real `qemu_irq` output to `gpio.c` (the natural home — same
physical pin), wired to GIC ID 39 in `rz_a1h.c` alongside the existing connections; the
`pwrk-pressed` property now raises `IRQ7` on the release (rising) edge.

**Confirmed live, rigorously — not just "the wiring compiles"**: a naive first test (press,
short delay, release, then poll `PC`/`IRQRR` at ~0.3-0.5s granularity) looked like nothing
happened, PC still parked at the same address. Didn't trust that — added temporary `gic_set_irq`
instrumentation (a debug build only, reverted after) and caught the real sequence: our raise
*does* reach the GIC (confirmed, real level change registered), and is followed, essentially
immediately, by a **second `gic_set_irq` call lowering the same line — originating from a genuine
guest-side MMIO write** (confirmed via `addr2line` against the actual call stack:
`memory_region_dispatch_write` → `subpage_write`, the real dispatch path for a CPU store
instruction, not a host-side function call). That's real firmware's own `pwrk_irq7_isr` clearing
`IRQRR` as its documented first action — i.e. the "nothing happened" read was wrong: the CPU
*did* wake, ran the real ISR, and returned to idle fast enough that the earlier coarse polling
never caught it in the act. Re-ran with fast (20ms) sampling right after a press/release and
directly caught `PC=0x200297d8` — squarely inside `pwrk_irq7_isr`'s own body (`0x20029774`-
`0x200297ff`) — with `IRQRR` reading back cleared afterward and the CPU returning to the exact
same `wfi` landing point. **A real press-then-release genuinely wakes the CPU, runs real
firmware, and returns to idle cleanly.**

This also sharpens (rather than closes) the still-real "never wakes again" observation: real
firmware's own `pwrk_irq7_isr` is only ever *armed* by a genuine PWRK event — nothing else on
this specific boot branch currently drives it (no periodic MTU2/other tick is active here,
unlike the auto-power-on path's own idle state), so *without* a live press this WFI genuinely
has nothing to wake it, which is now a correctly-modeled real property of this branch rather
than a modeling gap.

### Follow-up, same day — traced why a live press/release doesn't advance this branch any
### further: it loops back to wait for another press, gated on a second, still-unmodeled
### external IRQ. A real, concrete next blocker, structurally identical to the PWRK/IRQ7 case.

Built `tools/trace_pwrk_wait_advance.py` (QMP-only: drives `pwrk-pressed` true→false live, then
polls PC *and* three resolved live flag bytes at fine granularity) to answer the open question
directly rather than guess from static analysis alone. Real user-manual text supplied mid-session
confirmed the scope: turning the radio ON is a plain, short push (not a hold) — matching this
branch (`power_state_pwrk_wait_and_bringup`) exactly, and cross-checked against the ISR's own
debounce constant (`DAT_2002a0d8` = 90000, converted via the already-established 32MHz `P0φ`
tick formula to **90ms** — a plain switch-bounce filter, not anything hold-duration-related). The
2-second-hold-to-power-OFF and CLEAR+V/M-held-during-POWER "All Reset" combo the manual also
describes are both real but out of scope here — different code paths, not chased this session.

**First result, and a real self-correction**: a live press/release genuinely re-runs
`pwrk_irq7_isr` (already known), but the debounce math was mis-derived by hand initially — a
clean, bounce-free release actually leaves `DAT_2002a0dc+6` (the "press still active" byte, live
address `0x203901ef`, resolved by reading the actual pointer each `DAT_2002axxx` symbol holds)
set to **1**, not cleared to 0 as first assumed. Caught directly via live flag-byte polling
(added to the tool) rather than trusted from the hand derivation — confirmed empirically across
a full press/release cycle, `press_active` never left 1.

**Real mechanism, fully traced**: after the ISR returns, `power_state_pwrk_wait_and_bringup`
checks a second flag, `*DAT_2002a0e8` (live address `0x2039030f`, called `civ_state` in the
tool) for the value `3` before it will run its CI-V/SCIF1 servicing sub-loop at all — the *only*
place `press_active` can ever be cleared to 0, which is the *only* way the function reaches its
finalize path (`*DAT_2002a104 = 1`, live address `0x20390311`). `civ_state` is set to `2` right
before the wait loop and **never becomes 3** in any live trial — so every cycle falls through to
`FUN_20005d2c()` and loops back to the top, re-entering the wait-for-a-new-press loop (confirmed
live: PC visits `0x2002993c`/`0x2002994c`/`0x20029950`, the initial press-detect loop, right
after the ISR returns) rather than progressing. This, not "stuck in the same wfi", is the real
reason repeated presses don't help on their own.

**Root cause of *why* `civ_state` never reaches 3, found by hex-searching for every literal-pool
copy of its target address (`0x2039030f`) and decompiling each referencing function**: two
near-identical handlers, `FUN_20186a58` and `FUN_20188d30`, each set `civ_state = 3` when
`state==2` and a live port-read bit (`0xFCFE3200+4`, bit `0x40`) reads high. **Confirmed, via the
real registration call site** (`0x20010fc0`: `mov r4,#0x23; ldr r1,=FUN_20186a58; bl
0x200b9490` — the same generic ID-based handler-registration idiom as `pwrk_irq7_isr`'s own
`register_event_handler`), that `FUN_20186a58` is genuinely registered as **GIC ID 35 (external
`IRQ3`)**'s real interrupt handler (`FUN_20188d30` registers identically for GIC ID 36, `IRQ4`,
at `0x2001200c`) — not dead code. **Neither `IRQ3` nor `IRQ4` exists anywhere in `gpio.c`'s
external-IRQ front-end** (confirmed in `rz_a1h.c`: the front-end added this same day for `IRQ7`
is the *only* `sysbus_connect_irq` off `gpio.c`) — this is a second, structurally identical gap
to the one just fixed for PWRK, on a different pin.

**A live check also ruled out the one alternate, software-only path**: `FUN_20029510` (called
from inside the initial press-bringup sequence) posts the matching RTOS event flag in software
(`FUN_200b8308(0x24)`) without needing real hardware `IRQ4` at all — but only when a small
struct's `+0x60==0 && +0x5d!=0`. Read live via QMP: `DAT_2002a0ec`'s struct has `+0x60=0` and
`+0x5d=0` in our current emulated environment, so the condition is false and this shortcut is
never taken. **One genuinely lucky finding**: the port-bit condition `FUN_20186a58`/
`FUN_20188d30` actually check (`0xFCFE3200+4` bit `0x40`) already reads **satisfied** in this
machine's current, unmodified default (`0x00c0 & 0x40 != 0`, confirmed live) — meaning, unlike
PWRK's own port-default fix, no register-default correction is needed here; the *only* missing
piece is real hardware `IRQ3`/`IRQ4` delivery itself, exactly parallel to how `IRQ7` needed
`ICR1`/`IRQRR` wiring.

**Not yet done, the concrete next step if this is picked up again**: identify which real IC-7300
pin/signal `IRQ3` (and/or `IRQ4`) is on — not obviously one of the front-panel connector's own
already-identified signals (`FRES`/`P1_0`, `LRXD`/`P6_0`, `PWRK`/`P1_7`, `LTDX`/`P6_1`, per
`notes/ic7300-signal-chain.md`), so this is likely a different board signal entirely; find the
`IRQ3`-equivalent of `pwrk_irq7_config_init` (should exist, same pattern) to confirm its real
`ICR1` sense/edge configuration; then extend `gpio.c`'s external-IRQ front-end the same way
`IRQ7` was done (a second `qemu_irq` output, wired to GIC ID 35, plus a live QOM property to
assert it for testing). `tools/trace_pwrk_wait_advance.py` is ready to re-verify once that
exists — rerun it and watch `civ_state`/`press_active`/`finalized` (it already prints all
three) rather than just PC.

### Follow-up, same day — IRQ3 pin identified (user supplied the RZ/A1H alt-function pin
### table), `gpio.c` extended, and the hypothesis CONFIRMED live: `civ_state` genuinely
### flips `2`→`3`. One further, deeper layer found underneath, not yet closed.

User supplied the RZ/A1H's real alt-function candidate-pin table for `IRQ0`-`IRQ7` (several
candidate pins per IRQ number, as expected for this chip family). Cross-referencing all six
`IRQ3` candidates (`P1_3`/`P1_9`/`P4_11`/`P6_4`/`P6_11`/`P7_11`) against `notes/
ic7300-signal-chain.md`'s already-complete port table picked out **`P7_11`** (`CRXD`/`CBSY`) as
the strongest candidate — it directly parallels this same firmware's own already-confirmed
`P6_13`/`P7_12` pattern (one real net redundantly wired to two CPU pins: one for UART RX, one
for a secondary role), and `CBSY` ("CI-V bus busy") fits `civ_state`'s own name and role exactly.

**Built and wired** (`src/gpio.c`, `src/rz_a1h.c`, `src/rz_a1h.h`): a second `qemu_irq` output
on the same INTC external-IRQ front-end used for `IRQ7`, generic `IRQRR` bit-3 (`IRQ3F`)
handling in the existing read/write ops (no new register block needed — `ICR1`/`IRQRR` already
cover all 8 lines generically), a `P7_11` idle-high reset default (same "unmodeled real input
defaults to its real idle level" reasoning as `P1_6`/`P8_9`/`P1_7`), and a new `civ-bus-busy`
QOM boolean property mirroring `pwrk-pressed`'s own structure exactly (`true`=busy/asserted,
`false`=idle, fires `IRQ3` on the idle-transition rising edge, matching both lines' identical
`0b10` `ICR1` sense-bit programming). Builds clean.

**Confirmed live, isolated from any PWRK press** (parked at the wfi landing point, no button
touched at all): a bare `civ-bus-busy` pulse alone wakes the CPU, and `civ_state`
(`0x2039030f`) is caught at `3` on the very next poll — direct proof `FUN_20186a58` (`IRQ3`'s
real registered handler) ran and did exactly what its own decompile predicted. **The hypothesis
is confirmed, not just plausible.**

**But it doesn't stick** — within a few hundred ms, `civ_state` reads back `2` and PC has
cycled through the wait-for-a-new-press loop back to the identical wfi. Read the real assembly
(not just decompiled C, which had restructured this section confusingly) at `0x20029ba4`-
`0x20029c1c` to get the exact mechanism precisely: the outer loop re-enters the CI-V service
body (`0x20029b60`: `civ_rx_frame_stage_and_dispatch`/`scif1_svc_rx_service`/etc.) as long as
**both** `press_active` (`0x203901ef`) is nonzero **and** `civ_state==3`; once either the
2-second — actually a 200-tick — timeout inside that loop fires, or `civ_state` stops reading
`3`, it falls to `0x20029bbc`, masks `IRQ0`/`3`/`4`/`6`/`7`, and checks `press_active` one more
time: **only if that's `0` does it finalize** (`*0x20390311=1`, the same address as before) —
otherwise it re-arms and loops back to wait for another press. `press_active` is only ever
cleared inside that CI-V loop body, gated on a *third* flag (`*DAT_2002a100`, live address
`0x20390031`) becoming nonzero.

**This third flag is where the trail goes cold for now**: a hex search for every literal-pool
copy of `0x20390031` found the *only* reference to this exact pool slot is the one read-and-
clear site already described (confirmed via `references_to` on the pool address itself, not
just the target) — nothing else in `body.bin` writes it through this same absolute-constant
path. Other hits from the raw-address search (`menu_item_value_set_by_format_type` and others)
turned out to be false leads: they access *different byte offsets* within what's apparently a
small shared multi-field scratch record based at the same address, not this specific field —
a reminder that this technique can't see writers that compute the address via register-relative
offsets from a different base pointer instead of loading the same absolute literal.

**Concrete next step**: decompile `civ_rx_frame_stage_and_dispatch` (`0x2000b258`) and
`scif1_svc_rx_service` (`0x20012854`) directly to see whether either one is the real setter of
`*0x20390031` via a relative-offset path this search couldn't find, or whether finishing this
branch genuinely needs a virtual SCIF1 responder — the same kind of thing the already-built
SCIF3 front-panel responder (`scif.c`) does, but for a different, so-far-unmodeled protocol
handshake. `tools/trace_pwrk_wait_advance.py --civ-busy-pulse` is ready to re-verify once
either lead pans out.

**A real reframing worth checking before chasing that further**: decompiled `civ_rx_frame_stage_
and_dispatch`'s own callee, `civ_dispatch_lookup_validate` (`0x2000b03c`) — this is the actual,
already-identified real CI-V command dispatcher (cross-checked against the IC-7300's own CI-V
manual in an earlier session), and it only acts on *real incoming CI-V protocol bytes*; it
doesn't set `*0x20390031` itself either, and neither does anything else this session found. A
standalone radio with nothing connected to `[REMOTE]`/USB would never generate such bytes — which
raises the real possibility that requiring `*0x20390031` (and therefore `*DAT_2002a104=1`) is the
wrong success criterion for this investigation.

### Follow-up, same day — the reframing above is now CONFIRMED, not just suspected. `civ_state`
### is a real CI-V privilege tier (service/factory-mode-shaped), matching a fact this project
### already independently confirmed in an *entirely different* session months ago, and completely
### unrelated to whether this boot branch has reached a normal working state.

The user pointed out, from real-world radio operating knowledge, that holding the CI-V bus busy
is one known gateway into the IC-7300's service mode at power-on. This project already has that
exact fact on record, fully investigated and closed independently: `notes/kernel-rtos-history.md`
(30th-session "Factory/service mode" section) confirms the real IC-7300 service-mode entry is
front-panel **MENU+FUNCTION held** *and* the **REMOTE/CI-V jack shorted** (`CRXD`/`P6_10` reading
low), checked as a one-shot GPIO level by `boot_check_mode1_combo` — called from
`cold_boot_hw_init`, the *other* boot branch, not `power_state_pwrk_wait_and_bringup` at all. That
specific function isn't what this session's `IRQ3` work touches.

But re-reading this session's own earlier decompiles of `civ_dispatch_lookup_validate` and
`civ_dispatch_invoke_handler` with that fact in mind resolves the open question directly:
`civ_state==3` (`*DAT_2000b230`, the same address as `civ_state`) is a real, distinct CI-V
**privilege tier** in the actual command dispatcher — `civ_dispatch_lookup_validate` requires a
specific permission bit (`puVar9[0] & 0x10`) on a command's table entry to allow it through *while
`civ_state==3`* (a different gate than the ordinary local/remote permission bits 1/2 used
otherwise), and `civ_dispatch_invoke_handler` skips the normal busy/interlock checks (radio-
transmitting, menu-open, etc. — `FUN_2002c588`/`FUN_20066720`/`FUN_2000ac68`/`FUN_20061654`)
entirely once in that state. This is exactly the shape of a real service/factory CI-V mode, not
"link established" or "front panel ready."

**Practical conclusion for this whole thread**: `IRQ3`/`P7_11` (this session's own finding) is the
warm-wake path's mechanism for entering that same elevated CI-V mode — real, correctly modeled,
and empirically confirmed to work — but it was never gating *ordinary* power-on completion.
Re-reading the real assembly at `0x20029ba4`-`0x20029bb8` confirms this directly: even with
`civ_state` staying at its everyday value of `2` (no CI-V-busy ever asserted, service mode never
entered), the function *still* just falls through to the mask-and-loop-back path rather than ever
reaching `*DAT_2002a104=1` — finalize was never on the ordinary path at all, with or without this
session's `IRQ3` fix. **The observed wfi → wake-on-`IRQ7`-or-`IRQ3` → briefly service → `wfi`
cycle is this branch's own correct, permanent, working steady-state** for a radio that woke via
PWRK — a different, event-driven flavor of idle from the auto-power-on branch's RTOS-scheduler-
driven `idle_loop_wfe_spin`, not a boot sequence stuck partway. `*DAT_2002a104` most likely means
something else entirely (a real candidate, not confirmed: "power-off-hold completed", matching the
manual's 2-second-hold-to-power-off) — genuinely a different question from "did this branch boot
successfully," which it now looks like it already does.

**This closes the active PWRK-wait resume point** for practical purposes: both external-IRQ gaps
this branch needed (`IRQ7` for the initial wake/press-cycle, `IRQ3` for the CI-V-privilege-mode
path) are now real, modeled, and live-confirmed; the remaining unknown (exactly what `finalize`
represents) is a genuinely separate, lower-priority question rather than a blocker on this
branch's own basic correctness.

### CORRECTION, same day — a fresh-eyes review (asked for explicitly, on Opus, given only a
### written summary and told to be skeptical and verify everything itself) found the "closed"
### conclusion above rests on a real bug: the ICR1 edge polarity was read backwards. Fixing it
### and re-testing properly gives a MUCH better result than either conclusion this thread
### reached today — this branch genuinely DOES reach the same stable RTOS idle steady-state
### (`idle_loop_wfe_spin`) that the auto-power-on branch does. This was the original question
### this whole thread set out to answer, back at the very start of the day.

**The bug, found and independently re-verified**: `pwrk_irq7_config_init`'s real `ICR1` write
sequence (clear bit14, clear bit15, **set** bit14, clear bit15) leaves the 2-bit sense field at
value `01`, not `10` — a plain bit-ordering arithmetic mistake in this file's own earlier
write-up, not a firmware ambiguity. Independently confirmed against the Renesas reference driver
already vendored in this repo (`scratch/r01an5093ej0170-rza1-swpkg/.../r_switch_driver.c:137`):
`INTC.ICR1 |= 0x0040; // IRQ3 config = '01' = Falling Edge` — the *literal same bit* our own
firmware sets for `IRQ3`, with an explicit vendor comment settling the encoding. **Both `IRQ7`
and `IRQ3` are falling-edge, not rising-edge.** Since `PWRK` is active-low, `IRQ7` genuinely fires
on the *press* (high-to-low), not the release as this file previously stated throughout.

**Why this mattered in practice**: with the (wrong) rising-edge model, `pwrk_irq7_isr` only ever
ran at the instant of *release* — by which point `PPR1` already read "released," so the ISR's own
hold-duration check (`if (bit != 0) break;`) exited on its very first instruction, before the
90ms-equivalent debounce timer could ever matter. `press_active` was therefore observed set to 1
on *every* trial regardless of how long a press lasted — not a firmware property, an artifact of
testing the wrong edge. This directly explains this file's own earlier claim ("a clean release
leaves `press_active` at 1... confirmed empirically, never left 1") — that was real data, just
data about a bug, not about the firmware.

**Also found empirically while re-testing (a second, real gap, distinct from the polarity bug)**:
`FUN_20005d88`'s debounce threshold is measured against `OSTM1`'s live free-running counter
(`0xFCFEC404`, confirmed genuinely advancing during the hold — `ostm.c`'s own real `ptimer`-backed
model, not stuck), scaled to nanoseconds of `QEMU_CLOCK_VIRTUAL` time. Under `-icount shift=auto`,
virtual time paces with instructions *retired*, not real wall-clock seconds — this project has
hit this exact effect from other angles many times before. A tight busy-wait loop retires few
instructions per host-time slice, so the ~90ms-of-virtual-time debounce threshold this specific
ISR uses took **~4.5 real seconds** to elapse in this trial, not 90ms of real time — a real,
reproducible scaling factor for this specific code path, not a hang. (Empirically: `OSTM1_CNT`
grew ~479,000/real-second against this virtual-time-derived counter early in the hold, vs. a
target of 2,880,000 — extrapolating gives ~6s, matching the observed ~4.5s order of magnitude.)

**Fixed** (`src/gpio.c`): both `rza1h_gpio_set_pwrk_pressed()` and `rza1h_gpio_set_civ_bus_busy()`
now raise their respective IRQ on the falling edge (the press / the bus-busy assertion) instead of
the release / bus-idle transition. Plate comments corrected throughout. Also recorded (see the
same fresh-eyes review): `IRQ3`'s real pin (`P7_11`) is now **confirmed**, not just the best of
six analogous candidates — `FUN_20010ea8` (IRQ3's own config function, previously not located)
does the identical 6-register port-mux septet write `pwrk_irq7_config_init` does for `IRQ7`, at
the same `0xFCFE7100` (PBDC) base, port 7's own offset (`+0x1c`), bit `0x800` (bit 11) — an exact
match, and it's called from `power_state_dispatch` (`0x2002b29c`) itself, before either boot
branch splits off.

**Rebuilt and re-tested, corrected edge polarity, holding `pwrk-pressed=true` (not releasing
it)**: `press_active` (`0x203901ef`) reads `0` at t≈4.5s (real time) into the hold — the debounce
window matters, this is not a hang. **PC then leaves `power_state_pwrk_wait_and_bringup`
entirely** and, over a full 60s follow-on trial, lands on `0x200b93ac` (`idle_loop_wfe_spin`, the
exact same address the auto-power-on branch settles into) in 69 of 120 half-second samples — the
clear majority, with the remaining samples scattered across varied, real addresses (genuine RTOS
background activity, not a stuck loop), matching this whole project's own already-established
signature for "reached a genuine, stable, self-sustaining idle steady-state" on the other branch.
**This is the actual, correct answer to the question this whole thread opened with**: yes, the
PWRK-wait branch reaches the same kind of steady state the auto-power-on branch does, once driven
by a correctly-modeled press held long enough for the real debounce to elapse.

One loose end, not chased further: the `finalize` flag (`*0x20390311`, `0x20390311`) never read
`1` in any poll during the 60s trial despite PC clearly having passed through and beyond that
exact code path — most likely a "handle once" signal consumed/cleared by whatever runs right
after (matching this project's own established pattern for other one-shot mode-request flags),
not evidence the write didn't happen; the PC-address confirmation above is the load-bearing
result, not this flag.

**Retraction, for the record**: this file's own immediately-preceding "CLOSED... this loop's cycle
IS the steady state" conclusion was real motivated reasoning — a true-but-unrelated fact
(`civ_state`'s CI-V-privilege-tier role) was used to explain away an observation (`press_active`
never clearing) that actually had a separate, boring root cause (a polarity bug in this project's
own test harness). The `civ_state`/CI-V-privilege-tier finding itself stands and is real; it just
was never the reason `finalize` wasn't being reached. Worth remembering: an explanation that's
independently true is not the same as an explanation that's actually load-bearing for the
observation at hand — this project has hit that exact shape of mistake before (see the
GIC-priority-mask retraction earlier this same day) and hit it again here within hours.

### Follow-up, same day — the shared `idle_loop_wfe_spin` itself examined directly (real
### disassembly, not decompiled C): confirmed a bare `dsb/sev/wfe/wfe/b` idiom with zero
### embedded condition, live-confirmed `CPSR.I=0` (IRQs genuinely enabled) while parked there.

Per the user's own question ("what is that loop waiting for"). The loop body
(`0x200b939c`-`0x200b93ac`) has no polled flag at all — by construction it can only be woken by the
architectural `WFE` wake condition (any pending interrupt, or an explicit `SEV`), not one specific
IRQ. The real "idle vs. run a task" decision happens entirely in the interrupt/RTOS dispatch layer
above this snippet — a normal RTOS context switch loads a different task's saved PC/SP directly, it
doesn't "return through" this loop's own code. Matches this whole project's own already-established
observation (auto-boot branch's ring-pressure-rising-and-draining cycle) exactly.

### Follow-up, same day — a full `-d unimp` survey of both boot branches, prompted by "have we
### detected any unimplemented peripherals being touched before reaching idle". Found a real,
### substantial, previously-uncharacterized gap: the LCD/display controller (`VDC50`+`LVDS`) is
### actively configured by firmware and wasn't modeled at all. Built an MVP for it.

Ran both boot branches with `-d unimp -D <log>` to completion. **Auto-boot branch**: 293
unimplemented-device hits, all during one-time boot config — the log stops growing entirely once
steady idle is reached (confirmed over 15s). **PWRK-wait branch (held)**: only 25 hits before the
first `wfi` (this branch does much less hardware bring-up before parking), but grows to 294 over
the following ~15s once the button is held and finalize fires — and its own tail matches the
auto-boot log's tail **byte-for-byte** (identical `io-e8100000` magic-value sequence,
`0xa0000054`/`0xa0010130`/`0xa00f8000`/`0x84cb34fe`/...) — real, concrete confirmation both
branches converge on the exact same downstream feature/task bring-up code once "boot" completes,
not just the same final PC address.

**Mapped every touched region against `~/Downloads/rza1.svd`**: `spi-status-and-neighbors` =
`SPIBSC0` (boot-flash controller, expected/cosmetic); `io-e8200000` = `SSIF0`/`SSIF1` (DSP audio
link, expected — the DSP itself isn't emulated); `io-e8100000`'s repeated single-register-push
pattern doesn't match anything in this SVD (unidentified, not forced); **`io-fcfe0000`'s 212 hits
resolve almost entirely to `VDC50`+`LVDS`** (33 + 32 distinct registers) — the RZ/A1H's real
LCD/display controller, genuinely being configured (multiple graphics planes `GR0`-`GR3` +
video-input/overlay layers `GR_VIN`/`GR_OIR`, panel timing via `TCON_*`, output format via `OUT_*`)
and previously silently discarded by the generic catch-all.

**Built an MVP** (`rz_a1h.c`): swapped the generic unimplemented-device catch-all for this specific
0x1000-byte block for a plain `add_plain_ram_region()` (same convention as `CPG`) — no
display/timing/compositing behavior modeled, but writes now actually stick instead of being
discarded, so external tools can read back what firmware configured. Confirmed via `-d unimp`: zero
log lines for this range after the swap.

**Built `tools/vdc5_framebuffer_peek.py`**: polls all 6 graphics planes' `GRn_FLM2` (framebuffer
base address)/`FLM3` (stride)/`FLM6` (format+width) — bit-packing confirmed against the vendored
Renesas VDC5 driver source (`scratch/r01an5093ej0170-rza1-swpkg/.../r_vdc_l_register.c`) — and once
any plane's `FLM2` goes nonzero, dumps that guest-RAM region and decodes it to a real PNG using the
real `VDC_GR_FORMAT_*` encoding (RGB565/RGB888/ARGB8888/RGBA8888 implemented; CLUT/YCbCr recognized
but not decoded yet). **Not yet caught**: over up to 120s of real time (a lot of virtual time under
`-icount`) on both boot branches, no plane's `FLM2` ever went nonzero. Found a real, generic VDC5
driver module in the firmware (`FUN_20074a4c` and neighbors — plane position/color-key/enable
setup, matching the vendor reference driver's own structure closely) but this specific function
doesn't set `FLM2` itself; the actual setter, and whether display drawing is gated behind a real
VDC5 vsync/underrun interrupt this project hasn't identified an ID for (which a plain-RAM model
can never generate, so anything waiting on it would block forever), are both open. Concrete next
step: trace `FUN_20074a4c`'s own callers/siblings to find the `FLM2` setter and settle the
interrupt question one way or the other.

### Follow-up, same day — per the user's own request, a per-channel-selectable SCIF bus logger
### added, and a first live capture of real front-panel (`SCIF3`) traffic already found new,
### undecoded content. Full write-up and concrete next steps in
### `notes/front-panel-protocol-handout.md`'s own 2026-09-20 section — this is just the pointer.

`RZA1H_DEBUG=scif<N>` (e.g. `scif3`) now selects one SCIF channel's TX log + one assembled-frame
summary line per complete `0xFE...0xFD` frame; `RZA1H_DEBUG=scif` still means every channel,
backward compatible. TX-only deliberately — the channel-3/5 virtual responders precompute RX
replies directly into guest RAM rather than through `FRDR` byte-by-byte, so a generic RX
accumulator can't see full reply frames the same way. A first capture on a plain auto-boot already
caught two new, real, previously-undecoded outbound frames (type `0x00`, 33-byte payload; type
`0x01`, 1-byte payload) alongside the two already-known handshake frames (`0xF0`/`0xF1`) — see the
notes file for the exact bytes, which of `scif3_driver_pump_tick`'s three real send paths each
likely corresponds to, and the ordered next-session plan (resolve `DAT_200375b4`'s live pointer to
find the payload's real producer; trace the dynamic-type queue `FUN_2003754c` drives).

### Follow-up, 2026-09-20, continued — that plan's step 1 done: the type=0x00 frame's real payload
### buffer resolved live (QMP-only, `xp /1xw`, no GDB) to `0x203dca54`, confirmed via three
### independent `DAT_` symbols aliasing the same address, and two of its fields have real, named
### producers now (renamed in Ghidra + a plate comment at the address). **New, concrete finding**:
### at least one `SCIF3` outbound status field is populated from **`SCIF1`** (service-mode link)
### command handling, not physical-key/encoder input — `scif3_status_svcmode5_flag_set`/`_clear`
### (factory/service-mode active, tied to the already-known `svc_mode5_idle_loop`) and a byte
### written inside `scif1_svc_status_field_switch` (the exact field caught changing live during a
### plain boot). Full detail, plus the still-open `FUN_20006308` real-enqueue-site thread this
### surfaced, in `notes/front-panel-protocol-handout.md`'s own 2026-09-20 follow-up section.

## Status, 2026-09-11, continued — an accidental real-hardware experiment, born directly out of
## the PCB-damage setback above, cross-validates this whole project's foundational RIIC2 modeling
## assumption. **A genuinely valuable finding, not just a mishap.**

**The user's own finding, from the currently-broken probe/repair state**: with `SDA` lacking a
pull-up (or disconnected from the CPU entirely — the current, mid-repair state of the real board),
pressing the radio's power button produces **no activity at all**. A total, silent hang, no visible
sign of life whatsoever.

**This matches, almost exactly, the founding reason this whole `qemu-machine/` device model
exists.** `riic.c`'s own header comment records the *emulated* equivalent of precisely this
failure, found early in this project before `riic.c` had any real behavior: "this project found
busy-waiting forever on a completion interrupt once the channel was still a bare RAM region" —
i.e. with no real EEPROM ever able to ACK a transaction, `body.bin` hung completely, waiting on an
I2C completion interrupt that could never arrive. **Break the real bus on real hardware (no
pull-up, no ACK possible) and the exact same class of failure reproduces**: real firmware really
does treat the early RIIC2/EEPROM transaction as a hard, synchronous, un-skippable dependency, not
something that gets timed out or bypassed — confirming on real silicon what this whole thread has
built its entire model on, not just observed in emulation.

**Practical implication for the still-open ring-overflow question**: this reinforces (doesn't
settle, but reinforces) that the early EEPROM read really does happen on real hardware, really is
a hard boot dependency, and firmware really does block synchronously on its completion, exactly as
modeled — the underlying premise the whole ring-overflow investigation depends on is, once again,
validated against real hardware, not just decompiled code.

## Status, 2026-09-11, continued — the live RIIC2 bus-timing capture attempt hit real physical
## limits: both the EEPROM's own SOIC pins AND its pull-up resistors turned out too small for
## clip probes without support gear this session didn't have, and an attempt at the pull-ups
## caused real PCB damage (a lifted pad/trace, now being repaired with a wire-wrap-wire jumper).
## Full physical-probing finding recorded in `notes/ic7300-hardware.md`'s `IC351` entry.

**Also, while trying to empirically resolve the OLS's physical-pin-to-sigrok-channel mapping
(channels 0-7 showed zero transitions across two capture attempts despite active tapping) --
genuinely inconclusive, not because the technique was wrong, but because the OLS itself dropped
off USB entirely mid-session** (`/dev/ttyACM1` vanished, `lsusb` showed no trace of it) -- most
likely connector/cable flakiness on the analyzer's own USB side, consistent with this exact unit's
already-known age and the separately-reported "dodgy connector" on one probe. Never re-connected
this session to finish that specific check.

**Given both concrete physical obstacles hit in immediate succession (probe-point size, then the
analyzer's own USB connection), paused rather than push further on this specific live-capture
path this session.** Per the user's own explicit tradeoff question, this bus-timing check was
always the "nice to have" cross-check in the JTAG/live-testing plan, not the load-bearing one --
the two JTAG-based tests (breakpoint at the overflow trap; non-halting ring-header poll, if the
debug port supports it) don't touch any SMD component at all, only the already-populated,
board-confirmed debug connector, and carry none of this risk.

**Concrete next steps for whoever picks this up**: (1) once the PCB repair is done and verified
(continuity restored, no bridge to an adjacent net -- see `notes/ic7300-hardware.md`'s own repair
guidance), the two capture/analysis tools (`tools/live_riic2_capture.sh`/`tools/
analyze_riic2_capture.py`) are still fully built and verified, ready to use as-is; (2) before
relying on the OLS again, re-check it's still enumerating cleanly (`lsusb`, `sigrok-cli --scan`)
given the mid-session USB dropout here; (3) the pin-to-channel mapping is still not empirically
confirmed -- redo that check (a wide, all-32-channel low-rate capture while tapping a known
reference is the natural next attempt) once the analyzer's own connection is confirmed stable
again; (4) the JTAG-only tests remain available and lower-risk if the live-capture path continues
to prove difficult -- worth prioritizing once the FT2232H adapter itself arrives, independent of
how this specific thread resolves.

## Status, 2026-09-10, same session, continued — prepared the actual live RIIC2 capture while the
## user checks physical hookup feasibility. Pinned a precise, fresh timing estimate for the scan's
## real onset, bench-characterized the OLS's real RLE/sample-count limits empirically (not
## assumed), and built + fully verified (via a synthetic waveform, not just "should work") two
## ready-to-run tools: the capture script and the decode/analysis script.

**Fresh, current-build timing estimate** (`tools/trace_eeprom_addr_gdbfree.py 20`, re-run on the
current build since so much has changed in the boot path across this whole thread's history): the
dense sequential `0x20`-byte-chunk `FUN_2006cb84` scan starts at real elapsed **t~4.25-4.3s** (since
QEMU/boot start under `-icount shift=auto`) and runs to **~t=5.0s** -- about 0.7-0.8s total
duration, then transitions into the two `0x3df0` `cold_boot_hw_init` stamp-writes. This is the best
available estimate for real hardware's own timing, not a guarantee -- real power-on sequencing
(voltage rail ramp, reset controller behavior) isn't modeled by this emulation at all, so real
timing could differ by whatever that adds. Flagged plainly in the tooling: **expect to iterate
across a few power cycles**, not one perfect shot.

**Bench-characterized the OLS's actual RLE/buffer behavor empirically, on the bench (no radio
needed)**, since the earlier planning session's own theoretical concern (would periodic RLE
reissue overhead limit how much idle time a capture can bridge?) deserved a real measurement, not
an assumption: with the input held idle and RLE on, requesting an oversized sample count, the
device reliably delivered exactly **1,572,864 samples before stopping -- identical across four very
different configured sample rates (100kHz/200kHz/500kHz/2MHz)**. Since the cutoff doesn't scale
with rate, this is a **fixed sample-count ceiling in the driver/protocol**, not a rate-dependent
RLE-efficiency limit -- meaning real-time window covered = ceiling ÷ sample rate, a direct,
useful, quantified tradeoff between edge-timing resolution and how much timing-estimate
uncertainty a single capture can absorb. Chose **1MHz** (yielding a **~1.57s window**, ~1.5x
oversampling of the real ~340kHz signal -- adequate for confirming bus frequency/per-byte timing,
this project's actual question, not for lab-grade edge-jitter measurement) specifically to maximize
margin around the timing estimate's own uncertainty.

**Built and independently verified two tools, not just written and hoped for**:
- `tools/live_riic2_capture.sh` -- sleeps a configurable delay after the user applies power (default
  3.5s, centering the ~1.57s window on the ~4.25-5.0s estimated scan with margin both sides), arms
  the OLS at 1MHz/RLE-on/max-samples, saves a `.sr` session file, and runs a quick post-capture
  check for any decoded I2C activity at all.
- `tools/analyze_riic2_capture.py` -- decodes a saved capture via sigrok's built-in `i2c` protocol
  decoder and extracts real per-byte timing, printed directly against this project's own
  decompiled-register-derived prediction (26,433ns/byte, ~340kHz) as a ratio.

**Verified the analysis script's parsing against the decoder's own real output, not assumed
syntax** -- an early draft guessed at annotation text (`"ADDRESS WRITE"` etc.) that turned out
wrong; caught and fixed by synthesizing a minimal, correct I2C waveform (`START`, address `0xa0`,
`ACK`, data `0x42`, `ACK`, `STOP`) as raw binary samples, feeding it through the exact same
`sigrok-cli`/`i2c` decode command the real script uses, and reading the actual output format
directly (`"40-124 i2c-1: Address write: 50"`, bare `"0"`/`"1"` bit lines needing to be filtered
out, etc.) -- rewrote the regex against real, confirmed text, then re-ran the full pipeline
end-to-end against that synthetic capture and confirmed correct parsing/gap-computation/ratio
output before trusting it on a real, one-shot, hard-to-repeat physical capture.

**Hookup reference** (`notes/ic7300-signal-chain.md`/`notes/ic7300-hardware.md`, already
established, not re-derived): `IC351` (`GT24C128B`), SCL/SDA labeled `ECK`/`EDT` on the schematic
(CPU pins `P1_4`/`P1_5`) -- probe directly at `IC351`'s own SOIC pins (or its pull-up resistors, if
more accessible), not the CPU package (a BGA, not practically probeable). Channel 0 = SCL/`ECK`,
channel 1 = SDA/`EDT`, common GND to the radio's own ground -- must match how the probes are
actually clipped on.

**Not yet done**: the actual physical capture itself (gated on the user's own hookup-feasibility
check, in progress). Once attempted, `tools/analyze_riic2_capture.py` gives a direct, immediate
answer on whether the decompiled-register-derived bit-rate formula holds up against real hardware.

## Status, 2026-09-10, same session, continued — side quest per the user's own request: the
## Openbench Logic Sniffer (OLS) mentioned as candidate tooling was found and bench-tested.
## CONFIRMED WORKING, on its best possible firmware already -- no update needed or even available.
## Ready to use for the planned RIIC2 bus-timing capture whenever that's next picked up.

**Installed `sigrok-cli`/`pulseview` (Arch/CachyOS official repos, `libsigrok` 0.5.2), connected the
device, confirmed clean USB enumeration** (`Microchip Technology, Inc. Open Bench Logic Sniffer`,
VID:PID `04d8:fc92`, `/dev/ttyACM1`) **and a successful protocol identify**: `sigrok-cli --scan`
reports `Open Logic Sniffer v1.01 FPGA version 3.07 with 32 channels`. **Ran a real end-to-end
capture** (2 channels, 1MHz, 1000 samples) -- armed, sampled, transferred over USB, decoded cleanly,
zero errors (flat output expected/correct, nothing was connected to the probes for this test --
this only verifies the pipeline itself, not any real signal).

**Checked directly whether firmware 3.07 is actually current, not assumed**: cross-referenced
against the official `GadgetFactory/OpenBench-Logic-Sniffer` GitHub repo's own `FPGAROM/` directory
-- **3.07 "Demon Core" (released 3/1/2011) is the highest version ever released, nothing
supersedes it** (the project's own upstream changelog stops at 3.06, but the dedicated 3.07 release
blog post and the `.bit` file itself in the repo confirm it exists and is final -- a genuine, if
minor, upstream documentation gap, not evidence 3.07 is unofficial or risky). Per the same release's
own notes, 3.07 is specifically where **"RLE works correctly for all memory depths"** was fixed --
exactly the feature this device was identified as a candidate for. **Confirmed RLE is exposed and
selectable through sigrok's driver right now** (`--show` lists `rle: on, off (current)`).

**Conclusion: no firmware update needed, none exists to update to, don't touch it.** The unit is
already on the best/final firmware for this hardware platform (the OLS project itself has been
dormant since ~2011), already has the RLE capability the planned bus-timing capture needs, and
flashing 15-year-old hardware carries real, avoidable risk for zero possible benefit here. **Ready
to use as-is** for the RIIC2 SCL/SDA capture (candidate test #3 in the section below) whenever
that's next picked up -- no further bring-up work needed.

## Status, 2026-09-10, same session, continued — since the remaining ring-overflow question is now
## hardware-access-gated (see the sections below), planned concrete live/JTAG tests for once real
## hardware access exists, and identified candidate tooling already possibly on hand.

**Per the user's own question ("what kind of JTAG/live testing would give a concrete lead")** --
tiered by cost/risk vs. how decisive an answer it gives, sequenced 1→3→2 (cheapest/independent
first):
1. **Hardware breakpoint at `0x200b93fc`** (the trap's own `b .`) via the already-ordered
   FT2232H-based JTAG adapter + OpenOCD, across many power cycles. This address is a RAM address
   `body.bin` is decompressed to on real boot too (same documented `0x20005000` load point) -- no
   translation needed. Zero perturbation risk in the hoped-for outcome (an unhit breakpoint costs
   nothing); a single hit is decisive positive proof, many clean misses meaningfully strengthens
   the user's own informal "never seen it crash" into something closer to measured confidence.
2. **Non-halting background polling of the ring header** (`0x20420120`: write_idx/read_idx/
   pending/capacity) at a few-ms cadence during a real cold boot, via the same JTAG adapter if its
   ARM debug-port implementation supports memory access without halting the core (a standard
   CoreSight/AHB-AP feature on many ARMv7-A implementations, not guaranteed on this specific
   adapter/core combo -- worth testing feasibility early). This is the single most direct test of
   the *specific* mechanism this thread derived (does `read_idx` really freeze ~30ms while
   `write_idx` keeps pushing? does `pending` ever approach 16?), not just the symptom.
3. **An external, JTAG-independent cross-check of the bus-timing formula itself** -- a logic
   analyzer on RIIC2's SCL/SDA lines to the real EEPROM (`IC351`) during boot, directly measuring
   real per-byte SCL timing against the derived ~340kHz/~26.4us-per-byte formula this whole
   margin argument depends on. Doesn't need JTAG at all, so isn't gated on that hardware arriving.

**Candidate tooling for #3, not yet confirmed in hand -- ask before assuming either way**: the user
has an old Bus Pirate and possibly an old Dangerous Prototypes Openbench Logic Sniffer (OLS)
somewhere, neither confirmed located yet. Checked both directly (not assumed) against our real
~340kHz signal: the Bus Pirate's **text-mode I2C protocol sniffer is not viable at all** (software-
polled, tops out ~70kHz, well under our real bus speed, regardless of burst density) -- but its
separate **raw logic-analyzer/SUMP mode** (up to 1MHz sampling, 4096-sample buffer -- v3-class
hardware) could work for the narrow goal of measuring per-byte timing directly (a ~4ms/~150-byte
capture is plenty for that), with triggering-the-right-moment as the main practical risk given the
scan's own onset timing has shifted by several seconds across this whole thread's boot-path
history. **The OLS, if found, is meaningfully better-suited**: for a 2-channel (SCL/SDA) capture,
16K-sample depth at up to 200Msps-class hardware, and -- if its firmware/sigrok driver supports
run-length-encoded capture (worth checking once located, varies by firmware version) -- idle-time
compression could let a single capture span from power-on through the whole dense scan without
needing a precise trigger at all, since the bus sits idle almost all the time outside actual
transactions. Either device is a candidate; a cheap ~$10 generic logic analyzer (sigrok/PulseView-
compatible, buffer limited by USB streaming rather than onboard memory) remains the fallback if
neither turns up or proves inadequate once tested.

**Not yet built**: no OpenOCD/GDB script or Python polling-tool skeleton for #1/#2 exists yet --
offered to prepare one, not yet requested. Queue this alongside the existing JTAG-hardware-arrival
tracking (project memory) rather than building speculatively before the adapter is confirmed in
hand.

## Status, 2026-09-10, same session, continued — the last flagged loose end (the RIIC2 bit-rate
## fix's "aggregate scan duration only measured 1.44x, not the full 2.19x" wrinkle) chased down and
## resolved. Also a real, honest correction to this same session's own earlier "shift=7 matches
## shift=auto's ~6s" claim — that was a coarse-polling artifact, not a precise measurement.

**Direct new evidence, fine-grained (0.25s poll, not the coarse 3s poll `check_overflow_r0.py`
normally uses)**: with `sleep=off`, real time to the trap scales clearly and reproducibly with
icount shift on this exact build -- **shift=3: ~4.4-4.7s, shift=7: ~2.0-2.1s, shift=10: ~1.4s**,
consistent across repeated runs. This is the same mechanism this session's own earlier round-robin-
overhead work already established (higher shift = more virtual ns per real instruction = fewer
real round-robin passes needed to cross the same virtual-time span) -- now confirmed with a clean,
monotonic, reproducible relationship rather than a single earlier `sleep=off` compression data
point.

**A real correction to this same session's own earlier claim, caught by this finer measurement**:
this session earlier reported `shift=7` (with `sleep=on`, the default) hitting the trap "at the
identical ~6s mark as `shift=auto`" -- that was measured with `check_overflow_r0.py`'s own coarse
3-second poll granularity (which can only report "somewhere between the 3.0s and 6.0s checkpoints"),
not a precise value. **Finer polling shows the real value is ~2.76s, highly reproducible (two
separate runs: 2.762s, 2.763s)** -- genuinely different from `shift=auto`'s own real timing, not
identical as the coarse tool's bucketing made it look. This doesn't change that session's actual
conclusion (real-time compression via `sleep=off` still doesn't affect the overflow outcome, and
that finding used a >10x effect size, robust to this granularity issue) -- but it's a real,
worth-flagging correction: **a 3-second poll cadence is too coarse to distinguish real-time effects
smaller than several seconds apart**, worth remembering before treating two coarse-tool readings as
"identical" in any future session.

**This resolves the original wrinkle, with a more defensible mechanism than the original guess.**
The original hypothesis blamed `-icount shift=auto`'s *own adaptive retuning* specifically. This
session's data shows something broader: real-time-per-unit-of-virtual-time is not a fixed ratio
under `-icount` at all -- it depends on the current shift value, which itself varies (via
auto's own adaptive tuning, responding to whatever the guest's workload looks like at the time) or
is simply subject to ordinary run-to-run variation even when nominally fixed. **Practical
consequence**: a "before/after" real-wall-clock-duration comparison of a single device's own
per-event nominal delay change (like the RIIC2 byte-time formula fix) is fundamentally not a
reliable proxy for the *size* of that nominal change, under `-icount`, regardless of the specific
mechanism -- this is the same class of lesson this project has already hit from multiple other
angles (GDB-pause perturbation, the hotblocks-plugin's own timer-shifting artifact), now confirmed
for real-time/nominal-delay comparisons specifically too. **Not a mystery, not device-model-
specific, and -- per this same session's earlier decisive finding -- of no consequence to the
ring-overflow question itself**, since that mechanism has already been shown to be entirely
virtual-time-domain and insensitive to real-time effects of any kind. This closes out the last
concretely-flagged loose end from the bus-timing-fix thread.

**Where the ring-overflow/GIC-audit thread now stands, overall**: every concretely-testable
candidate raised this session (round-robin batching, icount shift tuning, ring capacity, GIC
priority bit-width, and now this real-time/nominal-delay wrinkle) has been checked and either fixed
(GIC bit-width) or ruled out as an explanation for the emulation/real-hardware divergence. No
further concrete, testable candidate is currently queued -- closing this out as a real, hardware-
access-gated open question (whether real IC-7300 silicon would also overflow this ring) rather than
continuing to search for more emulator-side explanations without a new concrete lead to test.

## Status, 2026-09-10, same session, continued — per the user's own "keep going on that" request:
## audited the ring's 16-slot capacity (confirmed genuinely real) and the GIC priority values
## (found and fixed a real, confirmed-against-Renesas'-own-driver-source model inaccuracy: wrong
## priority-register bit-width) — the fix is real, validated, kept, but empirically does NOT
## change the overflow outcome, exactly as the mechanism predicted before testing.

**Ring capacity, checked not assumed**: traced the producer's own bounds check (`FUN_20187bb4`)
directly -- the "16" isn't a hardcoded immediate in that function at all, it's read from the ring
header's own byte 3 at runtime. Followed that byte back to its real init code (`0x20187f8c`-
`0x20187f96`, real disassembly) which copies it from a **separate static data byte at `0x20336058`**
-- read directly from the image: **`0x10` = 16**. Genuinely real, firmware-compiled, not an
assumption this project made or a value inferred indirectly.

**GIC priority values -- a real, confirmed-against-real-Renesas-source inaccuracy found and
fixed.** This machine's GIC (`rz_a1h.c`) never set `num-priority-bits`, leaving QEMU `arm_gic`'s
own default of **8** significant priority bits. Checked directly against Renesas' own sample
driver already vendored in this repo (`scratch/r01an5093ej0170-rza1-swpkg/.../
r_intc_configure.c`, `R_INTC_SetPriority()`): its own `priority` argument is documented range
**0-31** and gets shifted left by 3 before the real `ICDIPRn` write, with the driver's own comment
stating outright *"Priority[7:3] of ICDIPRn is valid bit"* -- real RZ/A1H silicon implements only
**5** priority bits, not 8. Confirmed `arm_gic.c`'s own `gic_fullprio_mask()`/
`gic_dist_set_priority()` already implement the correct "mask off unimplemented low bits" behavior
generically -- this machine just never enabled it. **Fixed**: `qdev_prop_set_uint32(gic,
"num-priority-bits", 5)` in `rz_a1h.c`.

**Live-verified the fix actually changes what's stored, via a one-off QMP `xp` read of
`GICD_IPRIORITYRn` before/after** (not assumed from the source alone): pre-fix, SGI 0 reads `0xfe`
and nearly every unconfigured interrupt in the system defaults to `0x7f` (a real, previously-
unremarked fact on its own -- almost everything shares one default priority, only RIIC2 (`0x10`),
SGI 0, and OSTM0 (both `0xfe`) are explicitly configured away from it). Post-fix: SGI 0 reads
`0xf0`, defaults read `0x78`, RIIC2 stays `0x10` (already a multiple of 8, unaffected by the
narrower mask). **The relative ordering among all of these is identical before and after** -- RIIC2
highest, defaults in the middle, SGI 0/OSTM0 lowest, in both the 8-bit and 5-bit view -- so this
fix, while real and now silicon-accurate, was never going to be able to change which side wins
GIC arbitration.

**Tested the overflow outcome anyway, per this project's own "test, don't assume" discipline** --
predicting no change is not the same as confirming it. 10 trials of `check_overflow_r0.py`:
**`r0=2` overflow in all 10, no exceptions.** Timing showed some spread (7/10 at the usual ~6.0s
mark, 3/10 at ~9.0s) but with no clean before/after split across the two batches run -- consistent
with this machine's already-documented ordinary `-icount shift=auto` run-to-run jitter, not a new
effect from this fix. Also confirmed no regression in the RIIC2 event stream itself
(`RZA1H_DEBUG=riic`: same ~10,500-event count, same `delay_ns=26433` real byte timing, same log
format as every prior capture). **Kept the fix regardless of the null result on the overflow
question** -- it's a real, validated, now-silicon-accurate correction with zero downside, exactly
the kind of "worth fixing on its own merits even without a big payoff" change this project has
made before (e.g. the RIIC2 START/RESTART/STOP condition-timing fix).

**Where this leaves the audit**: both concrete candidates from the "keep going on that" request are
now checked. Ring capacity: confirmed real, not a divergence source. GIC priority bit-width: found
a real inaccuracy, fixed it, confirmed it doesn't explain the overflow (as predicted, since it
couldn't change relative ordering for these specific values). **Neither of the two most obvious
"is our GIC modeling subtly wrong" candidates explains the emulation/real-hardware divergence** --
the residual mystery narrows further. Remaining un-audited candidates for a future session: the
precise bus-timing formula's own remaining honest wrinkles (the ~1.44x-vs-2.19x aggregate-duration
gap flagged earlier this thread and never fully chased down), or something structural not yet
considered at all.

## Status, 2026-09-10, same session, continued — real-world ground truth arrives: the user has
## never observed the actual IC-7300 crashing from this suspected ring overflow. Chased the most
## concrete testable divergence hypothesis (does our synthetic EEPROM image trigger a validation/
## recovery scan a real, correctly-calibrated EEPROM wouldn't need) -- refuted directly by
## decompile, but a real, separate documentation bug was caught and fixed along the way. The
## user then asked whether attaching GDB changes IRQ behavior -- answered from already-established
## source-level facts, and this session's own virtual-time-domain finding sharpens that tension
## rather than resolving it.

**This is decisive, load-bearing evidence, not a minor data point.** The ring-overflow trap fires
during `cold_boot_hw_init`, on the path every single real cold boot takes -- if this were a real
firmware/hardware fragility, the radio would need to crash/reboot on effectively every power-on, an
extremely conspicuous symptom the user would certainly have already noticed. Combined with
everything above (the mechanism doesn't respond to real-time compression, doesn't respond to icount
shift changes, and the closed per-checkpoint diagnosis already pinned it to QEMU's own round-robin
architecture) -- the balance of evidence now points squarely at **emulation/peripheral-modeling
inaccuracy**, not real firmware fragility, for the "would real hardware overflow" question this
whole thread has carried as its one remaining open item.

**Chased the single most concrete, testable divergence candidate**: this project's virtual EEPROM
image (`riic2_eeprom.img`) is synthetic, built by `tools/build_riic_eeprom_image.py`, not extracted
from a real radio. If `FUN_2006cb84` (the dense-scan source, ~6700+ bytes across 7 chunked reads)
were a checksum/validation-gated *recovery* scan -- reading more, or differently, when content
looks corrupt -- our synthetic image could be tripping a "recovery" code path a real, factory-
calibrated EEPROM's always-valid content would never reach, manufacturing a divergence that has
nothing to do with real GIC/bus timing at all. **Checked directly via decompile, not assumed**:
`FUN_2006cb84`'s own body is a flat, **unconditional** sequence of 7 fixed-size (well, 6 fixed +
one bounded-but-selector-driven, never validation-driven) reads -- no checksum, no branch on
content, nothing resembling "if invalid, read more." The one data-dependent piece is a "which of 8
region-table entries" selector byte, not a retry/recovery gate. **This refutes the hypothesis
cleanly**: a real, correctly-calibrated radio would generate the *identical* volume of I2C traffic
here, every single boot -- this specific scan's size is not an emulation-specific artifact of our
synthetic image.

**A real, separate documentation bug caught (and already independently self-corrected) while
checking this.** Working from this session's own recalled project-memory summary (not the README
itself), decompiled the STI handler (`FUN_2001d9bc`) directly to confirm which higher-level wrapper
pair (`FUN_2001e484`/`FUN_2001dcc4` vs. `FUN_2001e510`/`FUN_2001dd58`) is genuinely read vs. write
-- and momentarily concluded `FUN_2006cb84` might be a *write*, seemingly contradicting its own
established "settings-struct load" description. **Turned out to be chasing an already-fixed bug**:
`README.md` already has this exact correction on record (`FUN_2001e484`/`FUN_2001dcc4` = write,
`FUN_2001e510`/`FUN_2001dd58` = read -- confirmed again here, independently, via the same STI-
handler logic: opcode `0` → `DRT=0xa0` write-address, opcode `≠0` → `DRT=0xa1` read-address) -- the
actual bug was that **this project's own persistent memory file still carried the old, pre-
correction (backwards) labeling**, with no forward pointer to the fix, which is exactly what caused
this session's own momentary confusion. Fixed in memory directly (annotated in place, not silently
rewritten). Doesn't change any device-model behavior (`riic.c` is keyed to the real `0xa0`/`0xa1`
bus values observed, never to these function names) -- purely a documentation/memory-hygiene fix,
but a real lesson: **a stale paraphrase in persistent memory can reintroduce an already-fixed
mistake into a fresh session** -- worth a cross-check against the actual README/history when memory
and a fresh decompile seem to disagree, exactly as happened here.

**Per the user's own follow-up ("since connecting gdb makes it go away, does it somehow make the
irq work in a different way")**: answered from what's already been directly established (three
specific icount-level mechanisms checked against `gdbstub.c`/`cpu-timers.c`/`icount-common.c`
source and all three refuted as the cause -- virtual time genuinely freezes with zero drift during
a GDB pause, no burst-release mechanism exists, and `shift=auto`'s own adaptive tuner freezes both
sides of its comparison together) -- so a read-only GDB poll/resume cycle *should* be virtual-time-
transparent, yet empirically suppresses the overflow. **This session's own real-time-compression
finding (above) sharpens this rather than resolving it**: since the overflow is virtual-time-domain
and GDB's pause is *also* proven virtual-time-transparent, GDB shouldn't be able to touch this
mechanism via any already-checked path -- yet it does. Two candidates remain genuinely unchecked:
a bug in this project's own `gdbrsp.py` breakpoint set/step/restore sequence, or an unexamined
`arm_gic.c` interaction with a stopped-CPU window. Not chased further this session (secondary to
the hardware-fidelity question now that real-world evidence answers the main one) -- flagged for
whoever wants to close out this specific QEMU/tooling curiosity.

**Where this leaves the whole ring-overflow thread**: the "would real hardware overflow" question
now has a real-world answer (no, not observed), and the most concrete "our synthetic data causes
this" hypothesis is refuted. The residual mystery is now narrower and more specific: *something*
about this emulation's own modeling (GIC priority values, ring capacity assumptions, bus-timing
formula, or something not yet identified) makes the margin tighter here than on real silicon.
Not chased further this session -- a real, well-scoped next step for whoever picks this up: audit
the ring's own 16-slot capacity and the GIC priority values actually read live against real
firmware/silicon documentation one more time, now motivated by real-world ground truth rather than
a plausibility argument.

## Status, 2026-09-10, same session, continued — a decisive, direct test REFUTES the whole
## "QEMU's own round-robin real-wall-clock overhead is why SGI0 starves" theory. The ring overflow
## is a virtual-time-domain phenomenon, completely independent of how fast or slow the emulation
## runs in real seconds. Neither of the two follow-up approaches below (icount shift tuning, and
## by direct implication the bigger single-vCPU round-robin-loop patch idea) can fix it, and
## nothing at this project's level can -- this is now genuinely closed.

**Per the user's own follow-up ("do we have other approaches"), evaluated two candidates before
building either**: (1) skip QEMU's multi-vCPU relock/`wait_io` dance in the round-robin loop for
this single-CPU target (a new, bigger, permanent core-QEMU patch -- untried); (2) test whether
`-icount shift=auto`'s own adaptive retuning (vs. a fixed shift) changes the per-event round-robin
pass count (cheap, no new patch). User chose to try both, in order, starting with the cheaper one.

**Testing #2 immediately produced a real, measured effect** -- re-applied `patches/
rr-loop-trace.patch`, ran `tools/trace_rr_loop_overhead.py` (now takes an `icount_value` arg) under
both `shift=auto` and a fixed `shift=7`, then did the same RI->RI per-event join the closed
diagnosis used. **Real result**: fixed `shift=7` needs a median of **4** round-robin passes per
RI->RI event, vs. **10** under `shift=auto` -- a genuine, reproducible >2x reduction in pass count.
But the per-event *real time* gap barely moved (median 127us vs 142us, ~11%) despite passes
dropping by more than half -- meaning each remaining pass, under the higher shift, does
proportionally more real work (bigger icount budget per `tcg_cpu_exec()` call). **This is the key
tell**: the total real-time cost per virtual byte-period looks like it's roughly fixed regardless
of how many discrete passes it's divided into -- exactly what you'd expect if something *other*
than the loop's own per-pass bookkeeping (wait_io/relock/icount-bookkeeping) is the true dominant
real-time cost.

**Ran the actual overflow check under the fixed shift to see if the (real) per-event cost
reduction translated into anything -- it didn't**: `tools/check_overflow_r0.py` (now also takes an
`icount_value` arg) with `shift=7` hits the identical trap, identical `r0=2`, at the identical ~6s
mark as `shift=auto` -- despite genuinely fewer round-robin passes per event. **Then the decisive
test**: added `sleep=off` (fully decoupling virtual time from any real-time pacing QEMU would
otherwise insert to keep the two roughly in sync) -- the whole run now genuinely compresses into
real seconds (trap hit inside the *first* 3s poll window instead of ~6s, confirmed again even more
dramatically with `shift=10,sleep=off`) -- **and the ring still overflows, identical `r0=2`, every
trial.** Making the entire emulation run many times faster in real wall-clock terms changed
*nothing* about whether or when (in virtual time) the overflow happens.

**This directly refutes the working theory this whole sub-thread (and the now-superseded handoff
below) was built on**: SGI0 losing the race to RIIC2 is **not** caused by QEMU's own round-robin
main-loop real-wall-clock dispatch latency being larger than the ~26.4us real hardware gap.  If it
were, compressing real wall-clock time by 12x+ (`sleep=off`) relative to virtual time should have
given SGI0 dramatically more *real* opportunities to interleave per unit of virtual time and
softened or removed the overflow -- it did neither, not even slightly, across every shift value
tried. **What this vindicates instead**: the earlier "real resolution" section further down this
file (no producer burst; `read_idx` freezes for a completely ordinary ~30ms under real GIC-
priority arbitration; the ring's own corrected true margin is ~16-32ms, not the ~1.3s the whole
thread originally assumed) was the *right* framing all along -- this is a **virtual-time-domain**
GIC-priority-arbitration outcome, evaluated identically regardless of real wall-clock speed, not a
real-time-dispatch-latency artifact. The later "sharp user challenge" section that reframed it back
toward round-robin real-time overhead was a reasonable hypothesis to raise and worth having tested
directly (this is exactly that direct test) -- but it's now falsified, cleanly, not just
theoretically doubted.

**Practical fallout for the two candidate approaches**: #2 (icount shift tuning) is answered --
real effect on internal pass count, zero effect on the actual overflow. #1 (the bigger single-
vCPU round-robin-loop patch) was **not built**, on the strength of this same evidence: it targets
the identical class of cost (real per-pass loop overhead) that #2's test just showed has no bearing
on the outcome, so building a new permanent core-QEMU patch to chase it further isn't worth the
maintenance cost this project already weighs against exactly this kind of change (see
`setup.sh`'s own re-clone-and-repatch-every-run design, and this project's existing one permanent
patch, `hw-arm-build.patch`). Flagged to the user rather than silently building it anyway.

**Where this actually leaves the whole ring-overflow thread now**: genuinely closed at the
device-model *and* QEMU-performance level, from three independent angles -- the original closed
per-checkpoint diagnosis, the mechanistic dispatch-count argument against batching (previous
Status section), and now this direct real-time-compression test against the round-robin-overhead
theory itself. **The one remaining open question, unchanged and only reachable with real
hardware**: would a real IC-7300 also overflow this ring given the same firmware/EEPROM-scan
conditions -- a virtual-time-domain question about real GIC arbitration and real bus timing, not
something any further emulator-side change can settle. Tool changes from this investigation
(`icount_value` override params on `check_overflow_r0.py`/`trace_rr_loop_overhead.py`) are kept,
general-purpose, harmless with the default unchanged. `patches/rr-loop-trace.patch` reverted again,
`qemu-src/` back to its normal unpatched build.

## Status, 2026-09-10, new session — the round-robin-batching fix from the handoff just below was
## evaluated (not blindly attempted) and rejected on mechanistic grounds, per the user's own
## choice at a checkpoint; a smaller, real (but expectedly marginal) hot-path cost was cut and
## validated instead. The ring-overflow/round-robin-overhead thread is now believed genuinely
## closed at the device-model level.

**Before implementing, worked through the batching design's actual mechanism rather than building
it first and measuring second (the checkpoint call was explicit: build-and-measure vs. reason-
first).** Conclusion, laid out for the user and confirmed as the basis for their own decision:
the closed diagnosis (see the handoff just below) pins the ~65-90us/event cost to the **outer
round-robin loop itself** (`wait_io`/`relock`/`icount-bookkeeping`, ~10 passes/event) — not to
anything `riic.c` does when arming its own `ptimer`. Firmware's RIIC2 driver is genuinely
interrupt-per-byte (confirmed by decompile, this file's own header comment): each byte needs its
own real `qemu_irq_raise()` -> `cpu_exit()` -> guest ISR dispatch -> DRR read. That's N discrete,
guest-observable dispatch events for N bytes no matter how the device model schedules them —
pre-computing a whole chunk's byte *values* ahead of time (the tractable half of the original
design wrinkle) doesn't reduce how many times the guest must actually be woken, so it can't lower
the round-robin pass *count*, only this file's own microsecond-scale bookkeeping. Reducing
dispatch count without changing guest-visible IRQ timing (the handoff's own fidelity requirement)
or patching QEMU's core loop (already out of scope per the closed diagnosis) isn't achievable at
this level. **User's own choice, given this**: don't build the batching design (agreed it's very
unlikely to survive contact with a real measurement, not worth the build+long-trace cycle to
empirically rediscover that), try one smaller, concrete overhead cut instead.

**The one real (not speculative) hot-path cost actually found**: `riic_schedule_irq_delay()`'s own
permanent diagnostic log line calls `icount_get_raw()`/`g_get_monotonic_time()` as *arguments* to
`rza1h_debug()` — `rza1h_debug()` gates its own body on `RZA1H_DEBUG`, but C evaluates a function's
arguments before the call regardless, so these two calls were being paid on **every** RIIC2
schedule (thousands per dense scan) even with `RZA1H_DEBUG` unset, unlike every other
`rza1h_debug()` call site in this file (whose arguments are plain field reads, not function calls —
not worth the same treatment). Fixed by wrapping the whole call in `if
(rza1h_debug_enabled("riic"))` — restores the "silent unless asked" cost this project's own
`rza1h_debug.h` header comment promises, for this one site.

**Validated, not just built**: rebuilt cleanly (`ninja qemu-system-arm`, only `hw_arm_riic.c.o`
recompiled). `tools/check_overflow_r0.py 3 40` — same trap, same `r0=2`, 3/3, at the same ~6s mark
as an unpatched-in-spirit run (no change expected or found). `RZA1H_DEBUG=riic` capture (8s, 10,500
schedule events) shows byte-identical log format/fields to what's documented throughout
README-history.md — the debug-on path is untouched by this change, exactly as intended.

**Honest sizing of the actual saving, done by arithmetic rather than a wall-clock A/B (the effect
is far below what wall-clock timing in this environment could distinguish from process-launch
noise)**: two now-skipped calls (~tens of ns each) x ~10,500 events in an 8s capture is on the
order of ~1ms of real time saved across the whole window — against the ~65-90us/event round-robin
tax this same window pays (~700ms-1s aggregate for the same event count), this is roughly a
0.1-0.2% cut. **Expected, not a disappointment**: this specific call site was never claimed to be
the round-robin bottleneck (it's riic.c's own bookkeeping, not the main loop's), so a real fix with
no measurable effect on the overflow outcome is exactly what the mechanistic argument above
predicts. Kept anyway — a real, permanent, zero-risk cleanup (this change cannot alter any
guest-visible behavior: the debug-enabled path is byte-for-byte identical, and the debug-disabled
path only skips computing values that were previously discarded unread).

**Where this leaves the whole thread**: the round-robin-per-event overhead is now believed closed
at the device-model level from two independent directions — the prior session's own directly-
measured, per-checkpoint diagnosis (inherent to QEMU's TCG/icount round-robin architecture), and
this session's mechanistic argument for why no device-model-side batching/scheduling trick can
reduce dispatch *count* without either breaking guest-visible timing fidelity or patching QEMU
itself. **The sole remaining genuinely open question on this whole ring-overflow thread stays
exactly what the "real resolution" section further down already named**: whether real IC-7300
hardware would also overflow this ring under the same firmware conditions — undecided without live
JTAG hardware, not resolvable by any further emulator-side change. **Recommendation for whoever
picks this up next**: treat the ring-overflow/round-robin thread as closed pending hardware access;
spend further `qemu-machine/` session time on a different open item (`mmc.c`'s own zero-`ptimer`
gap once SD-card testing becomes active, or another queued item) rather than re-attempting
round-robin-overhead mitigation at this level.

## Status, 2026-09-10, handoff for a fresh session — ATTEMPT THE RIIC2 ROUND-ROBIN-BATCHING FIX
## (superseded by the section just above — read that first; kept below for the full diagnosis
## trail the section above builds directly on)
## (not another diagnosis pass — the diagnosis is already complete and closed, see below)

**Do not re-run the round-robin diagnostic tooling from scratch** — an earlier session in this
same thread already did the full per-event correlation and reached a closed, precise verdict
(README-history.md's "round-robin-loop-overhead investigation is CLOSED" section, plus this same
day's later re-confirmation). Re-deriving it wastes a session; start from the conclusion below.

**The established facts, not to re-derive**:
- QEMU's round-robin TCG main loop costs a median of **~10 outer-loop passes per RIIC2 event**
  (RI→RI dominant case), each paying 5-30us across wait-io/lock-shuffle/icount-bookkeeping stages,
  summing to ~168us — matching the measured ~178-206us real inter-event gap almost exactly.
- Root cause: `cpu_exit()` fires on every interrupt raise (correct, standard QEMU behavior, not a
  bug) forcing a fresh round-robin pass each time, and RIIC2/MTU2's IRQ rate during a dense scan
  is fast enough that ~10 such passes are needed to advance one RIIC byte-period.
- Real hardware's own gap between RIIC2 byte events (**~26.4us**, from the real, firmware-
  programmed bus-timing registers) is **2.5-3.5x smaller** than this emulation's own per-event
  dispatch cost (~65-90us) — a real GIC would deliver a pending interrupt in a handful of cycles;
  this emulation needs a full round-robin pass, tens of microseconds, regardless of how trivial
  the work is. This is why SGI 0 (the job-ring drain IRQ, lowest GIC priority in the system)
  can't interleave into gaps real hardware would leave wide open — not because RIIC2 "stays busy"
  in any real GIC-priority sense (a sharp user challenge this same day corrected that framing),
  but because the *emulator's own* delivery latency is larger than the real hardware window.
- **Why fixing this is worth doing regardless of the eventual "would real hardware also overflow"
  answer**: it removes an emulation-specific confound. If the ring stops overflowing after this
  fix, that's real evidence the overflow was substantially an emulation artifact. If it still
  overflows, that's real evidence the firmware's own ring/timing combination is genuinely fragile
  on real hardware too. Either outcome is a genuine answer instead of a confounded one.
- **Beyond RIIC2**: `mmc.c` (the actual Phase-0 payoff target, still never reached by any traced
  boot path) has zero `ptimer` usage today. This exact problem — real bus-speed pacing colliding
  with round-robin dispatch overhead — will very plausibly resurface there, likely worse (SD
  transfers can be far more event-dense than a 230-chunk EEPROM scan), once real MMCIF timing
  gets added following this project's own established pattern. Understanding/fixing this pattern
  now has payoff beyond just closing out the RIIC2 thread.

**The proposed fix (from the closed diagnosis, never attempted)**: pre-compute a whole in-flight
chunk's worth of RIIC2 byte events and schedule them as fewer, larger `ptimer` waits — landing
every individual byte's own correct nominal virtual-time cost and IRQ, but needing far fewer
separate round-robin-triggering wakeups to deliver them.

**A real design wrinkle to resolve before coding, flagged now so a fresh session doesn't lose time
rediscovering it**: this can't be a uniform "batch N future events" scheme, because RIIC2 **reads**
and **writes** are asymmetric in what's knowable in advance:
- **Reads** (the actually-observed overflow trigger — `FUN_2006cb84`'s settings-struct scan):
  the device model already knows every byte's own value up front (from `riic.c`'s own virtual
  EEPROM backing store) before the guest ever asks for it. This is the tractable case — the whole
  read burst's nominal timing and byte values are fully known in advance, so precomputing a chunk
  of RI events as one larger wait and delivering the individual per-byte IRQs cheaply (without
  each one separately re-triggering a full round-robin `cpu_exit()` cycle) is a real, buildable
  optimization here.
- **Writes**: the *guest* supplies each byte's own value via a live `DRT` register write —
  the device model cannot know byte N+1's value before the guest actually writes it (already
  confirmed this session: `riic.c`'s own write-data-loop is correctly gated on real guest register
  access, not free-running — see README-history.md's EEPROM-write-cycle section). A naive
  "precompute and batch ahead" scheme fundamentally cannot apply to writes without either
  guessing (wrong) or waiting for the guest anyway (no win). **Scope the first attempt to the read
  path only** — it's both the tractable case and the one actually observed triggering the
  overflow; writes can stay exactly as they are.

**Concrete first steps for whoever picks this up**:
1. Read `riic_schedule_irq_delay()`/`riic_schedule_irq()`/`riic_schedule_irq_conditional()` in
   `src/riic.c` (22 call sites total, all funneling through one `ptimer` — a single, well-scoped
   chokepoint) and the `RIIC_READING` phase's own DRR-read handler (`rza1h_riic_read()`'s
   `RIIC_REG_DRR` case) to see exactly where a batched-read design would hook in.
2. Design (on paper/in comments first, this is a real device-model architecture change, not a
   quick patch) a mechanism that, once a read chunk is recognized as in-flight, arms **one**
   `ptimer` for the whole chunk's real duration, then delivers each byte's own RI IRQ at its
   correct virtual-time offset with minimal round-robin re-entry — while remaining byte-for-byte
   indistinguishable to the guest from today's one-`ptimer`-per-byte behavior (same IRQ sequence,
   same timing, same register values at every point the guest can observe).
3. Implement, build, and validate with the exact same regression discipline this whole thread has
   used throughout: `tools/check_overflow_r0.py` (does the trap/r0 outcome change?),
   `RZA1H_DEBUG=riic` log diffing against a pre-fix baseline (do the logged events match 1:1 in
   content/order, only real-time delivery cost should change), and a fresh, fine-grained
   `write_idx`/`read_idx` header trace (the technique from this same day's ring-overflow work) to
   directly confirm whether the ring still overflows post-fix.
4. Either outcome (overflow gone / overflow persists) is a valid, useful, publishable result —
   document it plainly, don't chase a specific "should" answer.

## Status, 2026-09-10, same session, continued — the ring-overflow thread reaches a real
## resolution: there was never a producer burst. Reading `write_idx`/`read_idx` together (not
## just the derived `pending` byte) shows the producer never changes cadence — it's the consumer
## (`read_idx`) that freezes, for **~30ms**, both under `-icount shift=auto` and without it. This
## also overturns a load-bearing assumption from this whole thread's very first session: the
## ring's *real* margin, once the RIIC2 scan is contributing its own pushes (measured combined
## rate ~1-2ms/push, not the assumed ~82ms), is **~16-32ms, not ~1.3 real seconds**. A ~30ms
## consumer stall is completely ordinary under GIC priority preemption — no exceptionally long or
## dense trigger is needed at all. **The GIC-priority mechanism itself was always correct; the
## margin math built on top of it was wrong by ~40x.** This also explains, in hindsight, why the
## EEPROM-write-cycle and no-icount/round-robin-overhead threads earlier this session (both
## honestly tested, neither confirmed) never fully explained it — they were sized against the
## wrong margin.
##
## **Both remaining open items now closed.** (1) The fast-phase push sources resolved, GDB-free:
## PC sampling during the fast phase lands dominantly in `irq_exception_dispatch` — the generic
## GIC dispatch trampoline itself (nesting counter, GIC IAR read, indirect call through a real
## per-IRQ-ID handler table, nested IRQs re-enabled around the call). It doesn't push to the ring
## itself — the fast rate is the aggregate effect of many brief, legitimate RIIC2 interrupts (TI/
## TEI/RI, already independently confirmed to dominate 86.7% of HPPIR samples in this exact
## window) each passing through the same shared trampoline, not one new culprit function. (2)
## **No device-model bug is left standing** — real bus timing, real GIC priorities, and correct
## dispatch code all confirmed individually correct. The 16-slot ring was sized against "~1 push
## per 82ms," an assumption that silently broke once real RIIC2 timing made the scan-phase rate
## ~40-80x denser.
##
## **Corrected, same session, per a sharp user challenge to the "density" framing**: real GIC
## priority arbitration doesn't block a lower-priority interrupt just because something exists
## at higher priority *anywhere in the system* — only while something higher is *currently
## running* or *also pending at that exact instant*. Since each RIIC2 service is genuinely brief
## (~150ns of the ~26.4µs gap, per the earlier per-instruction count), real hardware should have
## ample idle time to interleave SGI 0 in nearly every gap. **The actual mechanism is QEMU's own
## round-robin dispatch latency** — already measured, earlier in this thread, at **~65-90µs per
## scheduled event**, which is 2.5-3.5x *larger* than the entire ~26.4µs gap real hardware would
## offer. A real GIC delivers a pending interrupt in a handful of cycles; this emulation's own
## delivery is gated behind a discrete round-robin pass costing tens of microseconds regardless
## of how trivial the work is. **This pulls the "would real hardware also overflow?" question
## back toward "likely at least partly an emulation-architecture artifact"** — not a clean,
## equally-weighted three-way toss-up anymore, since the QEMU-specific latency floor exceeding
## the real hardware gap is a concrete, mechanistic reason, not just one of several guesses.
## Still not fully resolved without live hardware. Full derivation in README-history.md's newest
## section.
##
## **Two follow-up questions checked, same session.** (1) Why does the real, individually-tiny
## RIIC2 IRQ handling still cause a stall? Confirmed: no single interrupt is slow (real, logged
## spacing of ~26.5us between events, matching the corrected ~340kHz bus formula exactly) — it's
## the sheer *density* of many brief interrupts (one every ~26.5us during an active transfer)
## that leaves SGI 0 (lowest priority) no continuous gap wide enough to slot into, not any one
## handler's own duration. (2) What would real hardware do if this overflow occurred — reboot,
## hang, something else? This firmware already has a real, documented "arm watchdog then spin
## forever" restart idiom used elsewhere (`notes/firmware-update.md`) — but the overflow trap's
## own code doesn't arm the watchdog itself (confirmed via direct listing: a bare `b .`, nothing
## else nearby). Whether a watchdog is already armed by the time this early trap is hit is the
## one genuinely open piece — real, if not fully conclusive, evidence points toward yes (the
## watchdog registers are read and written by the dispatcher that runs *before* `cold_boot_hw_
## init`), making a watchdog-forced reboot the better-supported guess over a silent permanent
## hang, though not proven with this thread's usual rigor. This emulator has no watchdog device
## modeled at all, so it can't test this either way. See README-history.md's newest section for
## the full derivation and the concrete next step (check the real WTCSR/WRCSR unlock semantics
## against the manual, or add a minimal WDT model and test empirically).
##
## **Two more follow-ups, same session, both answered with real, derived numbers, not estimates.**
## (1) Precisely counted (from the real listings, not decompiled C) the per-byte RIIC2 interrupt
## cost: 37 instructions for `irq_exception_dispatch`'s own entry/exit + 24 for the real RI ISR
## (`0x2001dbcc`, resolved via the actual handler-table pointer, not guessed) = **61 instructions
## per byte**. Against the real 26.44us byte-time, that's **~0.6% of available CPU cycles** — over
## 99.4% idle between interrupts, confirming with an actual number that the mechanism was never
## about raw CPU consumption, purely about SGI 0's own bottom-of-the-stack GIC priority losing a
## continuously-repeated race. (2) The CPU clock (Iφ): a first attempt cross-checked a vendor
## sample's `FRQCR` value against this project's confirmed `P0φ=32MHz`, then had to be retracted
## when a second vendor sample showed the same `P0φ` is consistent with more than one `Iφ` (`IFC`
## only scales the CPU core clock, not the shared peripheral dividers). **Then genuinely resolved,
## not just re-assumed**: per the user's own real hardware fact (`P0_2`/`MD_CLK` pulled up →
## clock mode 1, `USB_X1`) and their request for the actual configured values, searched `flash.bin`'s
## own real boot code directly for the `strh` (16-bit store) instructions writing `FRQCR`/`FRQCR2`
## — found them: **`CPG.FRQCR=0x1035`, `CPG.FRQCR2=0x0001`**, this firmware's own real, executed
## values, not inferred from any external sample. Decoded: `IFC=1/1` → **Iφ=384MHz, genuinely
## confirmed** (vindicating the first attempt's number, though not its flawed cross-check
## reasoning). Full clock tree for this exact board: Iφ=384MHz, Bφ=128MHz, P1φ=64MHz, P0φ=32MHz.
## Full derivation across all three passes (assumption → retraction → real confirmation) in
## README-history.md's newest sections.

## Status, 2026-09-10, new session — the deferred heat-map analysis actually run, then a fun
## for-entertainment scif.c experiment, then the session's own biggest open question (the
## dominant non-trap hot spot's "blind spot") chased down and fully RESOLVED with a live memory
## dump: it's `base.dat`'s own real LZSS decompressor, already documented elsewhere in this
## project from a completely different investigation. Verdict on the original heat-map question:
## nothing malicious or anomalous found anywhere — every hot spot is an ordinary, explainable,
## already-understood piece of boot work.

**Ran the tool exactly as the prior session's handoff prescribed.** A 12s capture already showed
the known ring-overflow trap (`0x200b93fc`) at 92.74% of retired instructions (even earlier than
the prior session's own 8s/78.89% smoke test); a 30s capture confirmed it climbs to 97.95%. Per
the handoff's own option (a)/(b) split: used **both** — a 4s pre-trap-only capture for a clean
early-boot picture, and the 30s capture's full data with the trap (and `scif5_wait_hsk1_ready`)
programmatically excluded to get a stable, larger-sample view of "everything else." Both
approaches converged on the same top spots, a good cross-check that the picture is real and not
an artifact of window choice.

**RESOLVED, same session, follow-up per the user's own request ("let's get the memory dump from
qemu for that early boot cluster") — no longer a blind spot at all.** Built `tools/
dump_early_boot_ram.py`: a fully GDB-free, QMP-only (`-S` at reset, `cont`, then tight
`info registers` polling until PC lands in range, then `pmemsave`) capture of guest RAM
`0x20000000`-`0x20004fff`. **Two real QMP gotchas hit and fixed while building it, worth
remembering for any future QMP-scripted tool**: (1) `pmemsave`'s filename argument must be
quoted (`"..."`) — unquoted, the monitor's expression parser tries to evaluate it and chokes on
the first non-hex-digit character ("invalid char 't' in expression"), even though `pmemsave`'s
own `args_type` declares it a plain string; (2) `cont` triggers an asynchronous `RESUME` **event**
on the same QMP socket, interleaved with command replies — a naive one-reply-per-request reader
gets silently offset by one exchange from then on (each subsequent command's reply looks like the
*previous* command's answer) unless events are explicitly skipped while waiting for a reply.

**Disassembling the dump (`arm-none-eabi-objdump`, ARM mode) immediately identified real, valid
code** — and it's a **direct, byte-for-byte match to `unpack_from_flash_to_mem()`**, the real LZSS
decompressor this project already fully reverse-engineered in a completely separate, much earlier
investigation (`notes/decompression-lzss.md`/`notes/base-loader.md`, from the firmware-container-
format work) — same `0xfee`-cursor ring-buffer size, same control-bit literal/match dispatch, same
match length/offset decode, same ring-buffer copy loop, all confirmed line-for-line against the
disassembly. **This isn't a Ghidra coverage gap or a mystery at all**: `body.bin`'s own Ghidra
project correctly starts at `0x20005000` because that's `body.bin`'s real, documented load
address (per `notes/base-loader.md`) — the code below it belongs to `base.dat`, the flash
bootloader, which decompresses the (LZSS-compressed) firmware body directly into RAM at
`0x20005000` before jumping there. This emulation runs that real decompression for real, exactly
as real hardware would. **The heat-map dominance is now fully explained, not just described**: a
byte-at-a-time LZSS decoder unpacking a multi-hundred-KB firmware image is inherently a very large
amount of raw instruction execution — genuinely the single most CPU-expensive phase of the whole
captured boot window, but expected, understood, already-documented work, not a surprise and not a
tooling blind spot. Full disassembly excerpt and the exact match against the documented algorithm
in README-history.md's newest section.

**Everything else resolved cleanly, and none of it is suspicious — all ordinary, boundable
boot-time work**, several of it worth adding to this project's existing named-busy-wait list
(`scif5_wait_hsk1_ready`, the ring-overflow trap, `FUN_2001dcc4`/`FUN_2001dd58`) for future
sessions' own quick recognition:
- **`0x2017c75c` → `FUN_2017c758`**, a plain `memset`-style byte-fill loop (`while(n--) *p++ = c`)
  — ordinary library code, not a peripheral wait, just genuinely CPU-bound work (likely BSS/buffer
  clearing somewhere in early boot).
- **`0x2018648c` → tail of `FUN_20186480`**, a `memcpy`-style word-copy loop whose *second half* —
  a near-identical second loop body reusing the same `r0` value each store instead of reloading —
  was **never auto-disassembled by Ghidra** (shows as raw undefined bytes in the listing, though
  manually decoding the bytes confirms a real `bne` back-branch to `0x2018648c`, i.e. a genuine
  tight loop). This is the **same already-documented Ghidra disassembly bug** this project has
  a known workaround for (see the project-status memory's "Ghidra disassembly-bug workaround"
  note) showing up again, not a new tooling issue.
- **`0x2002b04c` → inside `cold_boot_hw_init`**, a tight 3-instruction spin (`*counter=0; while
  (*counter < 50);`) — a "wait ~50 ticks" delay implemented as a pure CPU busy-poll rather than a
  sleep, presumably incremented by a periodic ISR elsewhere. A new, previously-unnamed member of
  this project's busy-wait family, but unremarkable in kind — same pattern as everything else
  already catalogued.
- **`scif3_driver_pump_tick`/`scif3_frontpanel_identify_handshake`** (`0x200373ac`-`0x200374e3`,
  cluster at `0x2003741c`/`0x200373ac`/`0x200374b0`/`0x200374bc`/`0x200374d4`) — a bounded,
  one-shot boot-time "identify" handshake with the front-panel MCU (IC501) over SCIF3, polling a
  status byte across two small timeout windows (75 and 12 ticks). Also new to this project's named
  list, also unremarkable — a real UART handshake, correctly bounded, not a runaway spin.
- **`FUN_20005dd8`/`FUN_20005d88`** (the `0x20005d88`-`0x20005dec` cluster) — confirmed as one
  single busy-wait, exactly matching this session's own icount-block-splitting hypothesis for why
  the same starting PC kept reappearing with different instruction counts. Mildly interesting on
  its own terms (not concerning): the spin condition is computed via **VFP floating-point math**
  (`VectorUnsignedToFloat` → scale → `VectorFloatToUnsigned` → compare against a threshold) rather
  than plain integer comparison — an unusual implementation choice for a delay/wait primitive,
  worth a curious look in some future session, but not itself a red flag.

**Net verdict on the original question ("check if there's some suspiciously hot spots")**: no —
nothing pathological, no unexpected runaway loop, no lead pointing at hidden/undocumented
functionality. Every hot spot found is a mundane, explainable piece of boot-time init or a already
partially-understood busy-wait. The one genuine actionable finding is the Ghidra RAM-coverage gap
above, which is a *tooling* limitation on this project's own analysis, not a firmware finding.

**Follow-up, same session, per the user's own question ("does the poller expect something else
besides just the ack?")**: traced the full RX FSM (`scif3_frame_rx_statemachine` →
`scif3_frame_dispatch_by_type`) and the handshake's caller, not just the two busy-wait loops.
**The busy-wait itself needs nothing more** — both loops only test status *bits*, and the
existing responder's precomputed state + real-terminator-byte delivery makes the *real* firmware
dispatch code set those bits itself (confirmed by direct decompile, not just re-reading the prior
comments). **But the handshake's own caller, `scif3_frontpanel_init_and_latch_version`, does want
more**: right after the handshake returns, it unconditionally copies `g_scif3_rx_status_buffer`
bytes 1-12 into `g_frontpanel_latched_status` (no comparison, no branch on content — confirmed via
full decompile, so this can't corrupt boot or misroute anything) — bytes 1-3 of which
`ui_version_screen_populate_fields` later formats as "Front CPU: x.xx". Per `scif3_frame_dispatch_
by_type`'s own per-type write-offset logic, those specific bytes are only ever populated by real
frame types 0x01/0x05/0x07/0x0b, not by the generic type-0 frame this project's responder loops
back to satisfy the busy-wait — so those bytes stay unpopulated (almost certainly zero) under the
current model. **Net**: a real, now-documented display-only correctness gap (a future "Front CPU"
version-screen would show garbage/zero, not a real version) — cosmetic only, not currently
reachable by any traced boot path (well before display/UI init), and not blocking. Worth revisiting
only if UI/service-mode work or a later boot stage ever reaches that screen.

**Second follow-up, same session — the shared front-panel status buffer connects directly to the
MENU+FUNCTION service-mode entry combo already fully documented elsewhere in this project**
(`notes/kernel-rtos-history.md`'s "Factory/service mode" section). `boot_check_mode1_combo`/
`_mode5_combo`/`_challenge_response` (called from `cold_boot_hw_init`, right after `scif3_
frontpanel_init_and_latch_version` returns — confirmed via full decompile of `cold_boot_hw_init`)
read the *same* buffer (`DAT_2002b4d8`, offset `0xd` bits 3/4 = MENU+FUNCTION) our responder's
canned ACK only ever partially populates (byte 0 = type, byte 1 = one dummy zero byte — never
offset `0xd`/`0xe`). **Concretely**: this project's own virtual front-panel responder can never
cause the emulated boot to enter `svc_mode1_idle_loop` (real service/factory mode) — the MENU+
FUNCTION bits it would need to see set stay at whatever the buffer's zero-initialized state
already was. Purely informational (nobody's asked to reach service mode via this emulator yet),
but worth having written down given how directly it follows from this session's own trace, and
relevant background if the "run custom code" goal ever wants to explore service-mode entry as a
foothold.

**Third follow-up, same session, purely for fun/entertainment per the user's own explicit
request**: added a real, deliberately arbitrary reply delay (`FRONTPANEL_ACK_DELAY_NS`, `src/
scif.c`, currently 1ms) to the virtual front-panel responder's ack, using the exact same `ptimer`
idiom already established for the channel-5 DSP-link responder's own `DSP_ACK_DELAY_NS`/
`dsp_ack_timer` — arm-on-TX-terminator, precompute-and-deliver-on-fire, same split as that
existing mechanism. **Confirmed harmless to real (unplugged) boot behavior** at both an initial
200µs value and this final 1ms value: 3/3 trials each, identical trap/`r0`/timing to the
synchronous-ack baseline. **A genuinely interesting side effect on the hotblocks heat-map
methodology itself, not on real boot behavior**: a fixed-real-second hotblocks capture (4s) that
previously always showed the SCIF3 pump/handshake cluster instead showed *zero* hits for it,
3/3 reproducible — not because the code stopped running, but because adding one new real
scheduled `ptimer` event shifted which real-second slice of a *profiled* capture that code falls
into (confirmed: the cluster reappears intact, similar total instruction cost, in a longer 12s
capture). A neat, concrete demonstration of the same class of profiling-technique fragility this
project has already documented for GDB-breakpoint perturbation and `-icount shift=auto` skew, now
shown for the hotblocks plugin itself — worth remembering before trusting any single fixed-
duration hotblocks capture as directly comparable to another once anything changes the guest's
own scheduled-timer-event count.

**Concrete next steps for whoever picks this up**: (1) the early-boot "blind spot" is now fully
resolved (see above) — nothing further needed there unless someone wants to actually annotate
`unpack_from_flash_to_mem()` in the separate `icom_loader.rep`/`icom.rep` Ghidra project (out of
scope for this session, which only has `body.bin` open); (2) add the four newly-named busy-waits
above to this file's Directory-layout/reference material alongside the existing three, so a future
heat-map or trace session recognizes them on sight instead of re-deriving them; (3) the
`FUN_20005dd8` VFP-based wait is a loose thread worth a closer look if a future session has spare
curiosity, though not blocking anything; (4) `tools/dump_early_boot_ram.py`'s two QMP gotchas
(quote `pmemsave`'s filename; skip async events like `RESUME` when reading command replies) are
worth remembering for any future QMP-scripted tool in this project.

## Status, 2026-09-10, continued once more, same day — user asked for a firmware CPU-time "heat
## map" (which code churns the most CPU, to spot anything suspiciously hot). Feasibility checked
## and confirmed high-value; a working tool built and verified end-to-end; the actual analysis
## deliberately deferred to a fresh session, per explicit request.

**Feasibility, checked rather than assumed**: QEMU ships a built-in TCG plugin,
`contrib/plugins/hotblocks.c` (not part of the default build, but builds cleanly with one
`ninja` invocation), that counts real per-translation-block execution counts and each block's own
instruction count at the TCG level. This is a genuinely *better* signal than a real-time sampling
profiler for this project's own purposes: under `-icount` (this machine's own default), it counts
actual ARM instructions retired — completely orthogonal to the round-robin-main-loop-overhead/
icount-shift confounds the earlier session in this same day spent real effort untangling (a
wall-clock sampling profiler would inherit exactly those confounds; this doesn't). It also runs
fully inline with normal TCG execution — no `vm_stop()`, no GDB, none of this project's own
long-documented GDB-perturbation risk. It does slow down *host* wall time noticeably (real
per-block bookkeeping overhead), but since `-icount` ties virtual time to instructions retired
rather than wall clock, that shouldn't change any device-visible *behavior*, only how long a run
takes in real terms — not yet stress-tested for a long run, a fair thing for the fresh session to
keep an eye on.

**Built and verified end-to-end**: `tools/hotblocks_profile.py` runs a boot with the plugin
attached, parses its `pc, tcount, icount, ecount` report, computes each block's total retired
instructions (`icount * ecount`), and reports a sorted top-N with each block's % share of the
run — plus the full data to a CSV for later correlation. **One real, reproducible gotcha found
and fixed, not left to bite the fresh session**: without `-d plugin` (or any `-d <category>`)
present on the command line, the plugin's own exit report is silently lost on shutdown — confirmed
0/3 trials produced it without that flag, 3/3 did with it. Not root-caused (a stdio-buffering
interaction is the leading guess) but now a required, hardcoded flag in the tool, not a "nice to
have."

**A real first result already, from the verification run itself, that shapes how the fresh
session should approach this**: in an 8s smoke test, the already-known ring-overflow trap
(`0x200b93fc`, the project's own long-tracked `b .`) accounts for **78.89%** of all retired
instructions — expected, not a new finding (it's a tight, 1-instruction self-branch executed
hundreds of millions of times once boot gets stuck there), but it means a useful heat map needs
to either (a) run for a window short enough that the trap hasn't yet dominated the totals, or
(b) explicitly exclude/expect the already-documented hot spots (the trap, and any of the several
already-named busy-waits like `scif5_wait_hsk1_ready` if reached) and look at what's hot
*besides* them — that's where a genuine surprise would actually show up.

**Deliberately not done this session, per explicit request ("prepare to try it in a new fresh
session")**: resolving any of the smoke test's own top hot addresses to named functions, and any
real, longer analysis run. **Concrete next steps for whoever picks this up**:
1. `ninja -C qemu-src/build contrib/plugins/libhotblocks.so` (once per session, not part of the
   default build).
2. `tools/hotblocks_profile.py <seconds> <top_n>` — pick a duration that's either short enough to
   stay ahead of the known trap (~10-15s), or long enough to get a stable post-trap picture with
   the trap itself excluded from consideration.
3. Resolve the resulting top-N addresses to functions via Ghidra — **one call per address**
   (`mcp__ghidra__inspect`, action=decompile), **not a full function-table export**: this was
   tried and abandoned this session as needlessly expensive (the program has 8,000+ functions;
   the resource-based export alone consumed a large, disproportionate amount of context for a
   task that only ever needs a small number of specific addresses resolved).
4. Aggregate multiple hot addresses mapping to the same function; flag anything genuinely
   surprising — a small/trivial-seeming function eating an outsized share, or a hot spot that
   isn't one of the already-known, already-explained busy-waits this project has documented
   extensively (`scif5_wait_hsk1_ready`, the ring-overflow trap itself, `FUN_2001dcc4`/
   `FUN_2001dd58`'s own busy-wait, etc. — check README-history.md before treating something as
   novel).
Full detail in README-history.md's newest section.

## Status, 2026-09-10, continued once more, same day — checked whether QEMU has pre-existing
## SCIF/UART code worth reusing the way `eeprom_at24c.c` was for RIIC2; found a striking
## register-offset match but a real, well-reasoned decision not to swap

**Per the user's follow-up ("does qemu provide pre-existing code for the serial/uart
peripherals... reasonable to use existing code if applicable")**: checked `qemu-src/hw/char/
sh_serial.c` (QEMU's SH-3/SH-4 SCI/SCIF model) against `scif.c`'s own real RZ/A1H register map.
`scif.c`'s own header comment already ruled out `hw/char/renesas_sci.c` (RX62N's non-FIFO SCI,
wrong register map entirely) in an earlier session, but never checked `sh_serial.c` — a real gap
in that prior check. **Found**: in its SCIF feature mode, `sh_serial.c`'s register offsets
(SMR/BRR/SCR/FTDR/FSR/FRDR/FCR/SPTR/LSR) match `scif.c`'s own real, SVD-confirmed RZ/A1H offsets
*exactly* — unsurprising in hindsight (RZ/A1 was designed as SuperH's ARM successor, keeping
peripheral register compatibility for easy software porting) but a genuine, valuable independent
cross-check that this project's own register map is correct, not previously available.

**Decided not to swap it in, for reasons specific to why this differs from the RIIC case**:
RIIC's swap target (`eeprom_at24c.c`) was a separable *slave* device, orthogonal to the master
controller's own protocol/IRQ timing — the controller needed no behavior change to plug into it.
`sh_serial.c` isn't separable that way; adopting it would mean replacing exactly the parts that
were hard-won here. Concretely: (1) **the IRQ line count itself doesn't match** — RZ/A1H's real
GIC grouping is 4 lines/channel (BRI/ERI/RXI/TXI, confirmed in `rz_a1h.h`), `sh_serial.c` models
5 (adds a separate `tei`, inherited from SH-4's own SCIF); (2) **`sh_serial.c`'s TXI logic is
simpler than what this firmware needs** — it ties the IRQ directly to the enable bit
(`qemu_set_irq(s->txi, val & (1<<7))`) rather than a real completion condition, where `scif.c`
needed three specific, empirically-found fixes (level-vs-edge, a redundant-raise no-op, a
lost-edge artifact) to get a *reliable* signal for this exact firmware — a generic model isn't
validated against that and could easily reintroduce the same class of wedge bug; (3) **no real
baud-rate-accurate TX pacing** in `sh_serial.c` (sends instantly, with its own `// XXX this
blocks entire thread` comment) — `scif.c` is actually more advanced here already (this session's
own earlier fix); (4) the IC-7300-specific virtual responders (front-panel on channel 3, DSP-link
on channel 5) would need to sit on top of whichever base model is used regardless. **One thing
worth remembering for later, not acted on**: `sh_serial.c`'s real 16-byte RX FIFO + receive-
timeout timer is a nicer RX model than what `scif.c` has — nothing currently traced depends on
that behavior, so not an active need, but worth a look if real host-driven RX timeout behavior
ever becomes relevant.

**Process note, worth keeping permanently**: do a quick check of `qemu-src/hw/<category>/` for a
pre-existing model before hand-rolling a new peripheral from scratch — not to default to reusing
it (as this exact check just showed, register-offset compatibility alone doesn't make a swap
worthwhile once IRQ-timing correctness and this project's own already-validated fixes are
weighed), but because it's cheap, sometimes finds a genuinely reusable component (`eeprom_at24c.c`
did turn out worth adopting for RIIC2), and even a "not worth swapping" result is still useful:
an independent cross-check of the register map, and a map of what upstream already gets right or
wrong for the same IP family.

## Status, 2026-09-10, continued once more, same day — the real Tier-2 fix: `riic.c` rewired
## onto a real `I2CBus`/`eeprom_at24c.c` slave device, a genuinely new write-data-loop protocol
## state machine added (decompiled from real firmware, not guessed), a real live regression found
## and fixed along the way, and the fix end-to-end validated live (write, then read back, over a
## fresh transaction) — not just built and hoped for.

**Scope turned out bigger than "swap the backend"**: decompiling the real write driver
(`FUN_2001dcc4`/`FUN_2001da80`/`FUN_2001db50`) found firmware drives a genuine multi-byte TI/TEI
producer-consumer loop for write-data bytes that `riic.c`'s controller state machine never
modeled at all (it only ever chained through the 2 address bytes, then assumed a restart-to-read)
— wiring in a real EEPROM backend alone wouldn't have fixed the write bug on its own. Built it
anyway, per explicit direction to do both the backend swap and the protocol fix in one pass.

**What's real, now**: `riic.c`'s DRT/DRR handlers now drive a real `I2CBus` via
`i2c_start_transfer()`/`i2c_send()`/`i2c_recv()`/`i2c_end_transfer()`, with a real
`hw/nvram/eeprom_at24c.c` slave (`at24c_eeprom_init_rom()`, 16KB, 2-byte addressing, address 0x50
— confirmed via decompile: `FUN_2001d9bc` writes `DRT=0xa0`/`0xa1`) seeded from the same
`-global rza1h-riic.image=` file as before. A new write-data-loop state (reusing
`RIIC_WAIT_RESTART`) accepts further DRT writes as real write-data bytes, chained via a new
TEI-then-conditional-TI mechanism (`riic_schedule_irq_conditional()`/`riic_event_fire()`'s own
comments have the full derivation) that discretely approximates real TDRE/TEND per-byte hardware
semantics without this device needing to know firmware's own byte count in advance.

**Two real bugs found live while building this, neither purely theoretical**:
1. `ptimer_transaction_begin: Assertion '!s->in_transaction' failed` — the ptimer callback
   (`riic_event_fire()`) can't itself call `ptimer_transaction_begin` on the same timer (it's
   already mid-transaction when invoked). Fixed with a `QEMUBH` (`riic_ti_offer_bh`), deferring
   the follow-up schedule outside the firing callback — exactly the pattern `ptimer_trigger()`'s
   own upstream comment recommends ("Use a bottom-half routine to avoid reentrancy issues").
2. **A real regression, caught by directly comparing a 60s trace against the pre-session
   baseline, not assumed fixed just because it built and booted**: the BH itself is async and can
   run *after* the guest has already responded to the same TEI with a real CR2=RS — since only
   one event can be in flight at a time by this device's own design, an unconditional BH would
   silently clobber the just-scheduled, now-load-bearing restart STI with a stale TI offer.
   Confirmed live: this parked every read transaction's restart, stalling the whole channel (and
   the boot the ring-overflow trap itself depends on) in an idle WFE loop after well under a
   second of activity, instead of the many further seconds of dense traffic the unmodified
   baseline shows. Fixed by re-checking `phase == RIIC_WAIT_RESTART` inside the BH too, not just
   at `riic_event_fire()`'s own raise-time check. **Re-verified against the baseline after the
   fix**: 12,746 log lines / 1,260 distinct timestamps / zero anomalies over 40s, matching the
   baseline's 12,491 lines / 1,299 timestamps almost exactly, and the known ring-overflow trap is
   still reached at the same ~10-15s mark, confirmed unchanged over a full 70s window — this fix
   doesn't touch that thread's own mechanism or timing at all.

**The actual write path validated live, end-to-end, not just "builds and doesn't crash"**: built
`tools/test_riic_eeprom_write.py` — since no traced boot path naturally reaches the one *known*
real EEPROM write (it happens later in `cold_boot_hw_init` than the ring-overflow trap boot
currently halts at), this drives RIIC2's real registers directly over GDB instead (same
bypass-firmware spirit as `force_call_fup.py`), timed to run once the CPU is confirmed stuck at
the trap (real time still advancing, firmware provably never touching RIIC2 again — confirmed the
safest possible moment, not just assumed). Wrote 8 real bytes to a fresh offset, then read them
back over a separate, fresh transaction: **PASS — genuinely persisted and read back correctly**.
One test-harness-only gotcha hit and fixed along the way (not a `riic.c` bug): the trap is reached
mid-transaction, not at a clean boundary, so the channel needs an explicit forced-to-idle
(`CR2=SP`) before the test's own `CR2=ST` — without it, every test byte silently fed into
whatever real transaction was already in flight instead of the test's own intended address.

**Bonus, found by the same longer regression trace, not chased further this session**: a natural
(un-injected) 40-55s free-running capture reached the *real*, previously-never-observed
`0x3df0` EEPROM activity live for the first time (both a read and the start of a second
transaction at that exact offset) — RIIC2 traffic keeps flowing from interrupt context long after
the foreground CPU is stuck at the ring-overflow trap (consistent with, not contradicting, the
already-closed GIC-priority mechanism: the trap doesn't disable IRQs). Genuinely new territory
this project has never seen live before, but a fresh lead for a future session, not investigated
further here — this session's own scope was the write-path fix.

Full derivation, including the exact decompiled write-driver trace and every log excerpt, in
README-history.md's newest section.

## Status, 2026-09-10, continued once more, same day — user asked whether we can A/B-test our
## own hand-rolled EEPROM model against QEMU's real, built-in one. Built and ran a differential
## test; it immediately found a real bug (EEPROM writes are silently dropped, never persisted),
## independent of anything about the round-robin-loop or GIC-priority threads above.

**Scoped what "A/B test" can actually mean here first.** `riic.c` models the RIIC *host
controller* register/IRQ protocol (CR2/SR2/DRT/DRR/STI/TI/TEI/RI/SPI) — Renesas-specific, no
QEMU-built-in equivalent exists or could exist generically, so that half stays hand-written no
matter what. What *is* comparable is the EEPROM *data-plane* underneath it (address counter,
auto-increment/wrap, read, write) — currently a hand-rolled byte array baked directly into
`riic.c`, versus QEMU's own real, generic 24Cxx slave model (`hw/nvram/eeprom_at24c.c`, already
noted as unused-by-this-project in the prior Status section above). A full live A/B (rewiring
`riic.c` to drive a real `I2CBus` + `at24c_eeprom_init_rom()` slave instead of its own array) is
a genuine refactor — still not attempted, still "deserves its own session" per the prior note.
**What's cheap and was actually done instead**: a pure-Python differential test
(`tools/eeprom_ab_diff_test.py`, new) that ports both sides' real logic faithfully — QEMU's own
`at24c_eeprom_send()`/`recv()`/`event()` (read directly from `qemu-src/hw/nvram/eeprom_at24c.c`)
and `riic.c`'s own actual behavior (read directly from `src/riic.c`) — and runs the same scripted
transactions against both, without touching the live boot or the working system at all.

**Results, run directly, not just reasoned about:**
- **Reads match exactly**, including the already-RE-derived "dummy read after switching to
  receive mode" quirk (one discarded `recv()` before the real data) — a genuine, independent
  confirmation that this project's own earlier reverse-engineering of that quirk (see the "Ghidra
  fix applied" section further down) was real protocol behavior, not a misreading, since it falls
  naturally out of driving a real reference EEPROM model the same way.
- **Address wraparound differs beyond the real EEPROM's own 0x4000-byte size**: the reference
  model wraps (`% rsize`, real 24Cxx hardware behavior) while `riic.c`'s `mem_addr` is a bare
  `uint16_t` that only wraps at 0x10000 and reads 0 past the backing file's own EOF in between.
  **Not currently consequential** — every traced real scan (`FUN_2006cb84`'s own ~0x0420-0x1fe0/
  0x3ac0-0x3e80 ranges) stays well under 0x4000 — but a real, latent discrepancy if anything ever
  addresses higher.
- **Real bug found, not latent**: `riic.c` **silently drops every EEPROM write** — confirmed by
  tracing the code, not just the test (`src/riic.c`'s DRT-write `switch` only ever consumes
  address bytes; any DRT write once the address phase is done falls to the `default:` case, which
  just logs "unexpected DRT write" and does nothing; separately, `s->image_fd` is opened
  `O_RDONLY`, so persistence couldn't happen even if the state machine tried). **This directly
  matters**: this project already traced a real EEPROM *write* during cold boot (`cold_boot_hw_
  init` stamping `"SX3765 V0.9H-000"` at offset `0x3df0`, see "Ghidra fix applied" below) — on
  real hardware or against QEMU's own reference model, a later read of that offset would see the
  freshly-written string; against `riic.c` as it stands today, it would still see whatever the
  static backing image already had there. No traced boot path currently reads that offset back
  within the same session, so this hasn't caused an observed discrepancy yet — but it's a real,
  confirmed correctness gap, not a hypothetical one.

**FIXED, same day, later in this session — see the newer Status section above**: option (b), the
fuller `I2CBus`/`eeprom_at24c.c` refactor, chosen and built, plus the write-data-loop protocol
state machine that turned out to also be needed (not just a backend swap) — validated live, write
then read-back over a fresh transaction, genuinely persisted. Full test output and methodology
for *this* section's own findings (the ones that motivated the fix) still in README-history.md's
relevant section from earlier in the day.

## Status, 2026-09-10, continued in the same day — the QEMU round-robin-main-loop-overhead
## investigation (prepared, not run, by the prior session) is CLOSED. Verdict: real mechanism
## found and localized, but it's inherent to QEMU's own single-threaded TCG round-robin + icount
## architecture, not a bug in this project's device models and not fixable at this project's
## level without patching core QEMU. This reframes the whole ring-overflow thread's real-world
## relevance one more notch: it's now understood, with real measured numbers (not a plausibility
## argument), to be substantially an emulation-architecture artifact.

**Ran the prepared tooling exactly as instructed** (`tools/apply_rr_loop_trace.sh`, then
`tools/trace_rr_loop_overhead.py 15`) and did the actual per-event correlation join the prior
session flagged as the missing piece. **Result**: every one of 10,261 RIIC schedule-irq events in
a 15s capture happens inside an iteration that *does* call `tcg_cpu_exec` (never one of the
~half of iterations with no exec at all) — but that exec slice's own duration (median ~28us) is
much smaller than the real measured gap between consecutive same-type events (RI→RI, the dominant
case: median ~178-181us). **The real answer**: a median of ~10 outer-loop iterations elapse
between consecutive RI events, not one — and summing every checkpoint-to-checkpoint span across
those ~10 in-between iterations (wait-io + relock + icount-bookkeeping + exec, each iteration
paying its own 5-30us) reproduces the measured gap almost exactly (~168us predicted vs.
~178-206us measured). **So the per-event cost isn't one fixed cost paid once — it's roughly a
dozen small round-robin main-loop passes needed to advance virtual time by one RIIC byte-period.**

**Chased why ~half the iterations are "empty" (no exec) — and the obvious first guess (genuine
CPU halt/WFI, with icount's warp-to-next-deadline path not yet catching up) was checked directly
and refuted**: added one line logging `cpu->halted`/`cpu_can_run()`/`cpu_work_list_empty()` to the
same temporary patch — **`cpu->halted` reads 0 in 99.87% of samples** (298,582/298,960 in a fresh
6s capture). The real explanation, found by reading `tcg-accel-ops-rr.c` itself: a
`qatomic_load_acquire(&cpu->exit_request)` check can `break` the inner loop before
`tcg_cpu_exec()` even when the CPU is fully runnable — `cpu_exit()` is the standard way a device
model asks a running vCPU to stop promptly so a freshly-raised interrupt gets serviced without
delay, and at RIIC2/MTU2's IRQ rate during this scan (~1 every 180us) that alone explains the
observed alternating full/empty iteration pattern.

**Verdict**: `cpu_exit()`-per-interrupt-raise is correct, standard QEMU behavior, and the
lock-shuffle/icount-bookkeeping stages measured directly this session are each already lean
(single-digit-to-tens of microseconds). The multiplier comes from *needing* ~10 such passes per
device event during an IRQ-dense scan, not from any one pass being slow — this is inherent to
this accelerator's architecture, not a project-level bug. **One real, not-yet-attempted idea for
whoever wants to actually claw this back** (a genuine design change, not a same-session tack-on):
since RIIC2's byte-at-a-time reads are already known to be one driver-level "chunk" transaction,
a model-side optimization that pre-computes a whole chunk's worth of byte events as fewer, larger
`ptimer` waits (while still landing each byte's own nominal virtual-time cost and IRQ correctly)
could reduce the number of real round-robin re-dispatches without changing anything the firmware
itself observes. Not attempted, not even sketched in code — flagged as an idea. Full derivation,
including the exact join methodology and numbers, in README-history.md's newest section.

**`qemu-src/` reverted clean** (`git -C qemu-src checkout -- accel/tcg/tcg-accel-ops-rr.c`,
rebuilt, confirmed pristine) — same "investigated, then left unpatched" convention as
`irq-mask-trace.patch`. `patches/rr-loop-trace.patch` itself is untouched and reusable via
`tools/apply_rr_loop_trace.sh` for a future re-run if this thread gets picked back up.

**Also this session, per the user's own question: how does `riic.c` compare to how other QEMU
projects build I2C emulation?** Checked both the vendored `qemu-src/` tree already in this repo
and external sources. **Real finding**: QEMU has a generic, standard I2C bus framework
(`hw/i2c/core.c`'s `I2CBus`/`i2c_slave_send()`/`recv()`) plus an existing, ready-made 24Cxx-family
EEPROM slave model (`hw/nvram/eeprom_at24c.c`, with real address/page-rollover handling, optional
`BlockBackend` persistence, and an `init_rom` property for exactly the "seed with a fixed ROM
image" role `tools/build_riic_eeprom_image.py` was built from scratch to fill) — every real
in-tree I2C controller (`aspeed_i2c.c`, `designware_i2c.c`, `imx_i2c.c`, confirmed via external
search too) plugs into that same bus/slave split; `riic.c` doesn't use any of it, keeping the
virtual EEPROM as a hand-rolled byte array baked directly into the host-controller model. **Not a
one-sided miss, though**: none of those real in-tree controllers checked model real bit-rate-
accurate SCL timing via `ptimer` the way `riic.c` now does — most are instant-transfer FIFO
models, so this project's own timing realism is ahead of the QEMU-mainline norm, not behind it.
**Practical takeaway, not acted on**: adopting the real `I2CBus`/`eeprom_at24c.c` framework would
be a genuine, worthwhile architectural cleanup (free persistence/init-ROM handling, standard
idiom) but is optional, not blocking, and deserves its own deliberate session — a real refactor,
not a drive-by change. Full detail and sources in README-history.md's newest section.

**Actual next step for whoever picks this up**: the round-robin-overhead thread is closed and the
ring-overflow trap's mechanism was already fully closed earlier this same day (GIC-priority-
starvation, see the next Status section down) — there is no further open sub-question on *why*
the trap fires. The real choice now is strategic, not diagnostic: (1) treat the overflow as an
accepted, understood emulation-timing limitation and find some way past it that doesn't touch
real firmware (the not-yet-attempted RIIC2-chunk-batching idea above is the most concrete lever on
the table, but is a real design task, not a quick fix); or (2) set this specific trap aside for
now and reassess whether `body.bin`'s own MMCIF driver — this whole `qemu-machine/` effort's
actual Phase-0 payoff, still never reached by any traced boot path — could be approached
differently (e.g. a targeted `force_call_fup.py`-style direct call into SD-card code, bypassing
the parts of cold boot that don't matter for validating `mmc.c` itself, rather than insisting on
a fully organic boot through this trap first). Neither has been decided yet — a real conversation
with the user, not a foregone conclusion from this session's own findings.

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

**Also fixed, same session, per the user's own follow-up question**: checked `riic.c`'s RIIC2
bit-rate timing directly against the real GT24C128B datasheet and the RZ/A1H hardware manual —
found the formula only ever implemented one of five real `SCLE`/`NFE`/`CKS`-dependent variants,
and (compounding it) `FER` was resetting to 0 instead of its real hardware default (which has
`SCLE=1`/`NFE=1`) — together making RIIC2 run **~2.19× faster per event** than real hardware
(~744kHz modeled vs. a real, correctly-decoded ~340kHz). Fixed both; confirmed live (mean
inter-event gap for the same dense EEPROM scan: 2249ns→3238ns) and confirmed no regression (the
known ring-overflow trap still hits, same `r0=2`, same PC, 5/5 trials, now ~1-1.5s later as
expected). One honest wrinkle: the *aggregate* scan-duration change measured only ~1.44×, not the
full 2.19× the per-event formula change implies — likely `-icount shift=auto`'s own adaptive
retuning absorbing part of it (see the still-open GDB/icount tension above), not fully chased
down. Full derivation in README-history.md's newest section.

**Second RIIC timing bug, per real user pushback ("would I2C really be slow enough to cause
this?")**: checked directly against the manual's own start/restart/stop condition timing diagrams
(§18.12) — `STI`/`SPI` were charged the *same full 9-cycle byte time* as `TI`/`TEI`/`RI`, when the
manual's own diagrams show these conditions cost only 1-3 SCL periods, not 9. Fixed
(`riic_condition_time_ns()`, new); confirmed no regression (5/5 trials, same trap). Honestly
small effect on the *currently* dominant scan specifically (it's RI-dominated — 32 real
byte-transfers per chunk vs. 2 condition events), but a real, distinct, now-corrected bug in its
own right.

**Bigger finding, worth its own flag**: back-of-envelope math on the *previously-documented*
"557 transactions, ~4.8 real seconds" measurement (an earlier, prior-day session) doesn't add
up — even generously, corrected per-event timing predicts ~100ms for that many transactions, not
~4.8s. Checked directly rather than assumed: that specific scan **does not appear anywhere in a
fresh 60-second GDB-free capture** of the current build — only 279 total address-resolution
events total, none resembling 557 distinct random-address transactions. Boot has changed too
much since that measurement (DMAC/HSK1/SCIF-RIIC fixes all landed after) for it to still
describe today's build. **But the general finding survives, freshly re-confirmed, not just
assumed**: re-ran `tools/trace_irq_frequency.py` — RIIC2's TEI still dominates 86.7% of HPPIR
samples during the backlog window, matching the original magnitude. What's changed is *which*
RIIC2 activity is responsible: the already-traced `FUN_2006cb84` settings-struct load (found
earlier this same session), not a separate, still-unidentified dense scan. Real numbers, not a
hand-wave, on whether ~340kHz I2C is "slow enough": `FUN_2006cb84`'s own scan measures ~0.8-0.9
real seconds at the corrected bus speed — comparable in order of magnitude to the ~1.3 real
seconds (16 slots × ~82ms) a queue this size can absorb before overflowing. The mechanism holds
up under checked arithmetic; it just isn't the specific scan previously credited with it. Full
derivation in README-history.md's newest section.

**Two more sharp user questions, both checked directly, together pinning the ~4× gap down to
QEMU's own main-loop overhead**: (1) how long would SGI0 need to be unavailable, given the
message rate? Simple math: 16 slots × ~82ms ≈ **1.31 real seconds** cumulative. (2) How long does
the real RIIC ISR take to process? Decompiled the actual firmware handlers directly — all tiny,
straight-line, no loops; genuinely fast on real hardware, not the bottleneck. (3) Does the
handler (or its caller) busy-wait? Not the ISR — but `FUN_2001dcc4`/`FUN_2001dd58` (arms one
32-byte chunk transaction) end with a real polling loop on the transaction's own phase byte,
calling into a genuine ITRON-style syscall wrapper each iteration. **Checked live** (a
structural, not timing, question — GDB's known perturbation matters far less here):
`tools/trace_riic_busywait_probe.py`, two bounded THUMB-mode breakpoints (fast-fail path vs. the
real `svc 0x0`) — **150/150 hits landed on the real SVC, zero on the fast-fail path**: the
busy-wait genuinely traps into the kernel every iteration, not a disguised spin. (Real side
finding: `LR` was *not* reliable for identifying the caller at this depth — `in_kernel_context()`
overwrites it via its own internal `bl` and never restores it on return, only `PC` — the real
caller sits on the stack, not in `LR`, at this call depth.)

**But that still doesn't explain the ~4× gap on its own — continued into the QEMU-main-loop
hypothesis using data already on hand.** Added a microsecond-precision host timestamp alongside
the existing `icount_get_raw()` instrumentation in `riic_schedule_irq_delay()` (kept
permanently, not reverted — the user's own call, and it's what found this). Broken down by IRQ
type, for events where the guest did little real work: `STI`'s requested delay is ~4× shorter
than `RI`/`TI`/`TEI`'s (this session's own condition-timing fix), yet its real measured cost is
essentially the *same* (~67µs, right in their ~65-90µs range) — not ~4× shorter as it would be if
real time scaled with the requested delay. **A real, roughly fixed ~65-90µs cost per scheduled
event, largely independent of that event's own nominal timing value.** `TCG_KICK_PERIOD` (100ms)
checked and ruled out directly — two orders of magnitude too coarse. This is very likely inherent
to this emulator's own round-robin TCG main-loop architecture (exiting/re-entering `cpu_exec()`,
BQL reacquisition, icount bookkeeping, GIC IRQ delivery) — not pinned to one exact QEMU function
yet, but the magnitude and independence from the requested delay are directly measured, not
inferred. Explains, in hindsight, why the condition-timing fix barely moved the measured
duration: it reduced a *nominal* value that was never the dominant real cost to begin with. Full
derivation, including the per-IRQ-type breakdown table, in README-history.md's newest section.

**Superseded pick-list, kept only for its own historical trail**: this section originally listed
three unweighed candidate fixes for the ring overflow (raise SGI0's own GIC priority, grow the
ring's capacity, reduce RIIC2's IRQ density). Two of those (raise SGI0 priority, grow the ring)
are now understood to be the *wrong class* of fix entirely — both require changing the REAL
FIRMWARE's own configuration/data layout, which this project can't do and wouldn't want to even
if it could: the whole point is testing real, unmodified IC-7300 firmware, not a patched version
of it. The third (reduce RIIC2's IRQ density) was tried (real bus-speed pacing, this session) and
confirmed insufficient alone. **What actually explains most of the overflow, found this same
session by directly measuring rather than assuming**: real, correctly-modeled RIIC2 bus timing
predicts `FUN_2006cb84`'s scan takes ~215ms — safely under the ~1.31s (16 slots × ~82ms)
threshold needed to overflow the ring. The *actual* measured ~830-910ms is ~4× longer, and that
gap was traced (same session) to a real, ~65-90µs-per-scheduled-device-event cost inherent to
QEMU's own round-robin TCG main loop — independent of the requested ptimer delay, confirmed via
direct `icount`+host-timestamp correlation, not the device model or the real firmware. **This
reframes the whole remedy question**: the ring overflow is very likely mostly (or entirely) an
*emulation* artifact, not something the real IC-7300 would necessarily hit — so the real fix
isn't a firmware-shaped workaround at all, it's finding and reducing that QEMU-architecture
overhead directly.

**RESOLVED, same day (continued session) — see the new Status section at the top of this file**:
this profiling was run, the ~65-90us/event cost was localized and its fixability verdict decided
(inherent to QEMU's round-robin+icount architecture). The paragraphs below are kept for their own
derivation trail (what was prepared and why) but are no longer the active next step.

~~**NEXT SESSION — prepared and ready to run, not yet executed**~~: profile exactly where the
~65-90µs/event cost goes inside QEMU's round-robin main loop (`accel/tcg/tcg-accel-ops-rr.c`'s
`rr_cpu_thread_fn()`). This matters well beyond RIIC2: `mmc.c` (the actual Phase-0 target) has
**zero** `ptimer` usage today — no real SD-bus-speed pacing yet — so this exact problem is
currently dormant there, not fixed, and will very plausibly resurface (likely worse, given SD
transfers can be far more event-dense than a 230-chunk EEPROM scan) the moment real MMCIF timing
gets added, following this project's own established pattern of adding real timing to each
peripheral in turn. Fixing the general QEMU-overhead cause now is much higher leverage than
patching around RIIC2's own specific case.

**What's ready to go, built and verified this session** (patch applies cleanly, builds cleanly,
already smoke-tested and shown to produce real, useful data — see README-history.md's newest
section for a first look at what it already revealed):
1. `qemu-machine/tools/apply_rr_loop_trace.sh` — applies `patches/rr-loop-trace.patch` (a
   temporary diagnostic to `accel/tcg/tcg-accel-ops-rr.c`, same style/precedent as
   `irq-mask-trace.patch`) and rebuilds. **Not applied by default** — same convention as the
   IRQ-mask patch, run this first. `git apply --reverse` to drop it once this investigation
   concludes.
2. Logs a host-side, GDB-free microsecond timestamp (`RZA1H_RR_TRACE=1`) at 6 checkpoints per
   outer-loop iteration: `loop_top` → `after_wait_io` (brackets `rr_wait_io_event()`) →
   `after_relock` (brackets the `bql_unlock`/`replay_mutex_lock`/`bql_lock` "lock shuffle") →
   `after_icount_bookkeeping` (brackets `icount_account_warp_timer()`/`icount_handle_deadline()`)
   → `before_tcg_cpu_exec` → `after_tcg_cpu_exec` (brackets the actual guest-execution slice).
3. `qemu-machine/tools/trace_rr_loop_overhead.py` — runs a free boot with both `RZA1H_RR_TRACE=1`
   and `RZA1H_DEBUG=riic` together (both GDB-free, safe to combine), parses both logs, and
   reports per-checkpoint-span statistics plus the full outer-loop-iteration total.
4. `riic.c`'s own permanent `icount_get_raw()`/host-microsecond-timestamp instrumentation (kept
   from earlier this session) is the other half of the correlation — this new trace answers
   *where in the QEMU loop* the time goes; `riic.c`'s own log answers *which RIIC2 phase
   transition* each loop iteration corresponds to.

**Concrete next steps for whoever picks this up**:
1. Run `tools/apply_rr_loop_trace.sh`, then `tools/trace_rr_loop_overhead.py 15` (or longer — the
   smoke test this session used only 8s and mostly missed the dense `FUN_2006cb84` scan window;
   run long enough to cover it, `t≈4-7s` per `check_overflow_r0.py`'s own established timing).
2. The smoke test already found two concrete leads worth chasing first: (a) a surprisingly large
   fraction of outer-loop iterations have **no** `tcg_cpu_exec` call in them at all (roughly half,
   in the smoke test) — worth understanding why, since each such "empty" iteration still pays the
   full lock-shuffle + icount-bookkeeping cost for no guest-execution benefit; (b) the
   `before_tcg_cpu_exec`→`after_tcg_cpu_exec` span (the actual guest-execution slice) has an
   extremely wide spread (median ~3µs, but a tail up to tens of milliseconds) — understanding
   what drives a slice long vs. short would directly explain a lot of the per-event variance.
3. Actually correlate specific loop iterations (by host timestamp) against specific `riic.c`
   "schedule irq=..." log lines — the current tool prints both but doesn't yet join them
   per-event; that join is the concrete missing piece to attribute the ~65-90µs specifically to
   RIIC2's own events rather than general boot activity.
4. Once localized: decide whether it's fixable (e.g. unnecessary work happening on every
   iteration regardless of whether anything's due) or fundamentally inherent to this
   single-threaded round-robin architecture — that answer determines whether a real fix or a
   documented, clearly-labeled compensation is the realistic path forward.

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
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR/STI/TI/TEI/RI/SPI protocol, real bit-rate-generator-paced timing — **formula corrected 2026-09-10** to implement all 5 real SCLE/NFE/CKS-dependent variants (manual §18.3.12/13), not just the SCLE=0 case, and to reset `FER`/`BRL`/`BRH`/`MR1` to their real hardware power-on defaults; confirmed against the real firmware-programmed register values (`CKS=1`⇒IICφ=16MHz, `FER` left at its real `SCLE=1,NFE=1` reset default) — real rate ≈340kHz, comfortably inside the GT24C128B datasheet's own min/max windows at either supported voltage; START/RESTART/STOP conditions also corrected (`riic_condition_time_ns()`) to their real, much-shorter §18.12 timing instead of a full byte time. **EEPROM data-plane rebuilt 2026-09-10 (same day, later)**: replaced a hand-rolled, read-only byte array (found via this project's own A/B differential test to silently drop every write and wrap at the wrong address-space size) with a real `hw/i2c/core.c` `I2CBus` + `hw/nvram/eeprom_at24c.c` slave (16KB, 2-byte addressing, real 7-bit address 0x50 — confirmed via decompile), plus a genuinely new write-data-loop protocol state machine (decompiled from the real firmware write driver, `FUN_2001dcc4`/`FUN_2001da80`/`FUN_2001db50`) that the old model never had at all — live-validated end to end (write then read-back over a fresh transaction, `tools/test_riic_eeprom_write.py`). See README.md's Status section and README-history.md for the full derivation (including a live regression found and fixed along the way) — only RIIC2 exercised by any traced boot path so far, the real physical EEPROM `IC351`/`GT24C128B` (note: not the same thing as the diode matrix in `notes/diode-matrix.md`, which is a separate, GPIO-scanned resistor array — an earlier session's own label here conflated the two) |
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI IRQ per channel, now real baud-rate-accurate pacing (`FSR.TDFE`/`TEND`, `PCLK`=32MHz, see Status above — 2026-09-10). Real RXI backing two virtual responders: a front-panel one on channel 3, and a DSP-link one on channel 5 (the latter triggered by a second, tiny MMIO region at `0xFCFE3120` on the channel-5 instance only, not by SCIF registers, and now paced by a real (placeholder) delay too — see Status above). **Per-channel bus logger added 2026-09-20**: `RZA1H_DEBUG=scif<N>` (e.g. `scif3` for the front panel) logs just that channel's TX bytes plus one assembled-frame summary line per complete `0xFE...0xFD` frame; `RZA1H_DEBUG=scif` still means every channel. TX-only for now — see `notes/front-panel-protocol-handout.md`'s 2026-09-20 section for why, and for a first real captured-traffic analysis already in progress |
| MMCIF (SD/MMC host) | `mmc.c` | Real command/response/data protocol + virtual SD card, validated standalone — `body.bin`'s own driver not yet reached by any traced boot path |
| DMAC (DMA controller) | `dmac.c` | Real channel 0 only (edge `DMAINT0`/GIC ID 41, real `address_space_read()`/`address_space_write()` transfer, `ptimer`-based one-shot completion) — confirmed load-bearing 2026-09-09, **completion-delay race fixed 2026-09-10** (see Status above — 1000ns raced the firmware's own next instruction under `-icount`, raised to 100us). Every other channel/register still plain storage |
| RSPI2 (Serial Peripheral Interface ch.2) | `rspi2.c` | Minimal — `SPSR2`'s TX-ready bit always set, `SPDR2` writes logged only, no real transaction timing or completion IRQ — **confirmed load-bearing 2026-09-09**, unblocks `rspi2_transmit`'s own busy-wait, see Status above |
| VDC50 (LCD/display controller) + LVDS | `rz_a1h.c`'s `add_plain_ram_region()` | Added 2026-09-20 (found via `-d unimp`: firmware genuinely configures multiple graphics planes during boot, previously silently discarded). Plain storage only, no display/timing/compositing behavior modeled — but unlike the generic catch-all it was carved out of, writes now stick, so `tools/vdc5_framebuffer_peek.py` can read back `GRn_FLM2`/`FLM3`/`FLM6` and decode a real framebuffer once firmware points one at real content (not yet observed within 120s on either boot branch — see Status above for the open "gated on an unmodeled VDC5 interrupt?" question) |

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
