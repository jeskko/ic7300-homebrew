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

## Status, 2026-09-09 — MTU2 channel 4 (both TGI4A and TGI4C) built and confirmed load-bearing:
## `dsp_boot_handshake` now resolves for real; boot reaches a genuinely new class of blocker --
## a generic RTOS job-queue overflow whose trigger is a pure in-RAM flag, not a missing
## peripheral, pointing at this project's long-standing open question about concurrent
## task scheduling

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

**Confirmed, solid, this session (newest first) — the SCIF3 front-panel handshake now
completes for real, and boot reaches genuine ITRON task activation for the first time ever:**
- **`cold_boot_hw_init` progresses all the way to `itron_act_tsk` — a real RTOS task
  activation** (`0x2002b038`-area, confirmed live via a decompile + direct listing match), the
  single biggest milestone this whole `qemu-machine/` thread exists to reach. Getting there
  needed a real, working virtual front-panel responder (`scif.c`'s `rza1h_scif3_frontpanel_ack`)
  — the SCIF3 RX side, previously entirely unmodeled.
- **RXI (receive-data-full) wired for the first time**, alongside TXI, gated to channel 3
  (the confirmed front-panel link) — GIC ID 235, from the same SVD-derived formula as TXI3's
  236, already enabled by firmware alongside it (confirmed live, no extra arming needed).
- **The responder itself went through two real designs before landing on one that works**,
  full derivation (three separate real bugs, each confirmed via live tracing, not guessed) in
  `scif.c`'s own file comment and README-history.md's newest section — short version: naive
  byte-by-byte delivery through the real FRDR/RXI path (mirroring a real chardev byte) could not
  be made reliable against `scif3_frame_rx_statemachine`'s own drain loop no matter which QEMU
  deferral primitive backed it (synchronous, `QEMUBH`, and even a real `QEMUTimer` all let the
  guest's drain loop consume multiple queued bytes in one pass, confirmed via single-step
  traces each time); the design that actually works precomputes the *end state* a real
  byte-at-a-time exchange would have reached (frame buffer, byte count, status flags — all
  written directly via `address_space_write()`, using live-read real firmware pointer *values*,
  never hardcoded addresses) and delivers only the one genuinely necessary terminator byte
  through the real path. **A second, independent real bug** then surfaced once the identify
  ACK worked: it sets status bits that include the exact bit `scif3_driver_pump_tick` reads as
  "send a keepalive ping", and ACKing that resulting ping the same way re-armed it right back —
  a genuine, unbounded fe/f1/fd loop (confirmed: tens of thousands of frames in the first dozen
  real seconds) — fixed by simply never ACKing that specific frame type.
- **A second layer needing the same treatment was found and fixed the same session**: the
  handshake's own *second* wait loop (right after the first) blocks on a different signal
  (status bit 0x04, set only by an ordinary type-0x00-0x1F frame's own successful dispatch) —
  confirmed live via a real, previously-never-transmitted 33-byte status/data frame. The
  responder now ACKs any outbound type 0x00-0x1F the same general way (a 1-byte dummy-payload
  echo), which resolved this layer too, cleanly (no further loops observed).
- **The RTOS-tick/delay-counter investigation from the previous Status entry turned out to be a
  red herring, not the real blocker** — confirmed via a live hardware watchpoint (`gdbrsp.py`
  gained `set_watchpoint`/`remove_watchpoint`, `Z2`/`z2`, this session) showing the polled
  counter genuinely never gets a single write across 120 continuous real seconds, even post-fix.
  The real blocker the whole time was the missing front-panel RX responder above; once that
  existed, the handshake resolved via its *primary* exit path (a real reply, clearing its busy
  flag directly) well before its fallback timeout counter would ever have mattered. Chasing that
  counter further would have been wasted effort — worth remembering generally: a counter that
  never advances is evidence of "something upstream never runs", not necessarily evidence that
  the counter itself is what needs fixing.
- **A methodology point worth real emphasis**: getting the responder right needed cycling
  through several plausible-looking "fixed" states that live testing then disproved — GDB
  single-stepping alone gave a *misleadingly reassuring* picture more than once this session
  (showed one invocation succeeding in isolation, hid what happened immediately after); a real
  `-d unimp -D <logfile>` register-write trace, read in full sequence, is what actually
  resolved each case. Don't trust an isolated single-step trace's "it worked" over a full
  real-time trace's "and then what" when the two disagree.

**The activated task's identity is already known — no re-derivation needed.** `itron_act_tsk`'s
own argument here (`DAT_2002b4e8`) reads `0x2019889c` in the static image (confirmed directly),
an exact match for an existing row in `notes/kernel-rtos.md`'s task catalog: caller
`cold_boot_hw_init`, descriptor `0x2019889c` → **`ui_graphics_lifecycle_task`** (renamed from
`FUN_2007ef5c`), already ✅ **fully resolved** in that catalog — the master graphics lifecycle
task, which calls `graphics_stack_startup_egl_openvg`, creates the real 480×272 on-screen EGL
window surface (the touchscreen's actual resolution) plus a 960×552 off-screen EGL pixmap
surface, then runs a 2-state init/present-frame loop. Not one of the catalog's two genuinely
open identities (`kernel_start`'s mystery task, `thunk_FUN_2007ea68`'s dynamic activation).

**The SLV5 (`0xE8100000`) hypothesis from the previous resume point is now retired, checked
live before any build effort went into it**: a full 22-second `-d unimp,guest_errors` boot
trace shows zero accesses anywhere in the `0xE8100000`-area regions — `thunk_FUN_2007ea68`/
`slv5_periph_connect_disconnect_handler` is simply never reached this early, so it cannot be
today's blocker. See README-history.md's newest section for the trace.

**Active resume point — the concrete next step, now a fully traced, live-confirmed root
cause rather than a hypothesis:** `cold_boot_hw_init`'s task-readiness busy-wait
(`*0x2039076c` — a fixed literal-pool address in this build, confirmed unchanged this session,
bounded at `0x32`/50) is incremented by a generic multi-rate system-tick handler
(`FUN_200b7910`) that only ever runs as the registered ISR for **GIC ID 154** — cross-derived
two independent ways (the same "`ICDISRn` register-index×32+bit" SVD formula already
confirmed for OSTM0=134 and SCIF3 TXI/RXI=236/235, plus the literal `0x9a` argument to the
same `GICD_ISENABLERn`-shaped helper SCIF3's TXI fix already confirmed) as **MTU2 (Multi-
Function Timer Pulse Unit 2) channel 3's `TGI3A`** compare-match-A interrupt. Live-confirmed
directly, not guessed: with the CPU genuinely parked at the busy-wait (`PC=0x2002b04c`) after
20 real seconds, `GICD_ISENABLER4` (`0xE8201110`) reads back bit 26 **set** — the guest has
already armed this exact interrupt — while `GICD_ISPENDR4` (`0xE8201210`) reads bit 26
**clear** the whole time: MTU2 has never once asserted it, because `rz_a1h.c` currently maps
the whole MTU2 region via `add_plain_ram_region()` (plain storage, no behavior at all — see
the Confirmed-peripherals table). The guest's own real configuration for this channel is also
already traced (via `references_to` on the SVD's real per-channel register offsets, not the
peripheral's bare base address): `TCR_3=0` (prescaler Pφ/1, undivided), `TCNT_3` reset to 0,
**`TGRA_3=8000`** (the real compare-match period, don't hardcode — confirm live each build),
`TIER_3`'s enable bit set.

**`src/mtu2.c` is now built, wired, and confirmed load-bearing against a real boot** — a real
channel-3 timer (level IRQ, write-0-to-clear `TSR_3` semantics, everything derived above),
mirroring `ostm.c`'s proven pattern. Live-tested immediately after building it (same
`gdbrsp.py` poll used to find the blocker): the readiness counter, static at `0` for 20+
seconds on every prior boot, now climbs continuously (`0xd8` → `0x8e` → `0x63` → `0x1f`,
wrapping past its 1-byte width many times over) and **PC moves dramatically past the former
busy-wait**, visiting a wide, changing spread of addresses across `0x20005xxx`/`0x200b5xxx`/
`0x2018xxx` — real, active multi-region execution, not a single new parked address. This is
confirmed, not inferred: the fix genuinely unblocks `cold_boot_hw_init` and boot progresses
further than any previous session reached.

**`src/dmac.c` is now also built, wired, and confirmed load-bearing.** Live-checked GIC ID 41
first (per this session's own now-established discipline): armed by the guest
(`GICD_ISENABLER1` bit 9 = 1), never pending (`GICD_ISPENDR1` bit 9 = 0) — the same
armed-but-never-fired shape MTU2 had — and confirmed **edge**-triggered (`GICD_ICFGR2` bits
18-19), unlike MTU2's level `TGI3A`. Traced the exact register sequence live too (`N0SA_0`/
`N0DA_0`/`N0TB_0` — DMAC channel 0's source/dest/count — written in that order, source a RAM
buffer, dest a GPIO-region address) via `references_to` on the SVD's real per-offset
addresses, the same method used for MTU2's `TGRA_3`. Built `dmac.c` for channel 0 only
(real transfer via `address_space_read()`/`address_space_write()`, edge IRQ on completion —
full derivation in README-history.md's newest section) and retested with multiple free-running
trials (not just one snapshot — this project's own established discipline, since real
interrupt-driven scheduling makes a single boot non-representative): **the DMAINT0 busy-wait
now resolves in every trial**, run-to-run timing varying only in *when* it clears, never
whether. Boot then reaches **`dsp_boot_handshake`** (already-identified/named from an earlier
session) — a whole further stage, part of the SCIF5 DSP-link bring-up
(`scif5_dsp_link_driver_init()` → `scif5_wait_hsk1_ready()` → `dsp_boot_handshake()`), never
reached before this session.

**GIC ID 0x9f (159) identified and built: `mtu2.c` extended to cover MTU2 channel 4 too.**
Same SVD formula, register index 4 bit 31: **TGI4A**, channel 4's own compare-match-A event
— not RSPI as the register-access shape briefly suggested; `scif5_bitrev_transmit_word`'s
`iVar4`-relative writes turned out to be an unrelated GPIO-area side effect (base
`0xFCFE3400`), while the actual timing mechanism is genuinely MTU2. `scif5_dsp_link_driver_
init`'s own pre-existing file comment (written 2026-08-29, before this session) already
documented the wider mechanism: a ring buffer drained via channel 4's compare-match-**C**
event too (GIC ID 161, **TGI4C**) — both now built.

**A real bug found and fixed before either was confirmed working**: TGI4A's first version
auto-armed on `TSTR`'s `CST4` bit going high (mirroring channel 3's own correct pattern) —
but live testing showed `CST4` goes high early in boot, well before `TGRA_4` is ever
meaningfully written, so this fired the IRQ far too early with a stale register value. Fixed
by making TGI4A arm only on an explicit `TGRA_4`/`TCR_4` write while already running (the real
trigger point), never on the `TSTR` transition itself — `TGI4C` keeps the transition-based
arming (its own real trigger, confirmed live: `TGRC_4` never gets written before `TSTR`
starts channel 4, only afterward).

**Confirmed via multiple trials**: `dsp_boot_handshake`'s own busy-wait (`*0x203906ba`)
now resolves reliably once TGI4A works correctly. Boot progresses into `dsp_cmd_table_init` →
`dsp_param_sync_tick()` (both already-named/known from an earlier session) — pushing ~23
parameter words onto a ring buffer meant to be drained by TGI4C.

**Active resume point — TGI4C alone did not fully resolve the next stall, and the real cause
turned out to be a different, deeper class of problem.** A generic fixed-capacity ring-push
helper (`FUN_20187bb4`, reused far beyond the DSP link — its 4 call sites sit inside what
looks like a generic software-timer-expiry dispatch table) hits its own overflow protection
(`FUN_200b93fc`, an unconditional infinite loop) — confirmed live via register capture at the
exact halt (`r0=2`, matching this specific overflow site, not the sibling `FUN_201877e4`
one). Traced the "wake the consumer" call the pusher makes (`FUN_20187b8c`) all the way down:
it writes to a **plain RAM address** (`0x20336f24`, not any peripheral register) — meaning
whatever is supposed to notice this flag and drain the ring is a **pure software/RTOS
construct**, not something waiting on any interrupt this project could model. This points
directly at this project's own long-standing open fallback hypothesis (first raised
2026-09-09 morning, before any of today's MTU2/DMAC work): whether this emulator's
context-switch mechanism genuinely handles multiple concurrently-scheduled tasks correctly —
today's peripheral work has, for the first time, unlocked enough real concurrent activity to
actually reach and expose this question directly, rather than it staying purely theoretical.
Judged worth its own dedicated investigation (tracing which task is supposed to drain this
queue and whether it's ever actually dispatched) rather than a quick continuation.

## Confirmed peripherals

| Device | File | Status |
|---|---|---|
| GIC (Distributor + CPU I/F) | `rz_a1h.c` (QEMU's own `arm_gic`) | Real, working — see the off-by-32 `qdev_get_gpio_in` bug in README-history.md |
| OSTM0 / OSTM1 | `ostm.c` | Real timer + real IRQ. OSTM0 is `body.bin`'s real tick source (ID 134, `CMP`=32000) |
| SPI boot status | `spi_boot.c` | Real — the one register `base.dat`'s SPI-ready poll needs |
| GPIO/port registers | `gpio.c` | Real (masked set/clear, `PNOT` toggle, live `PPR` pin levels) — `P1_6`/`PDV` (power-fail detector) defaults high, see Status above |
| L2C (PL310 cache controller) | `l2c.c` | Real (`CACHE_ID`/`CACHE_TYPE`/`REG7` self-clear semantics) |
| CPG | `rz_a1h.c`'s `add_plain_ram_region()` | Plain storage, no behavior — nothing traced needs more yet |
| MTU2 | `mtu2.c` | Real channel 3's `TGI3A` (GIC 154) and channel 4's `TGI4A`/`TGI4C` (GIC 159/161) — **all three confirmed load-bearing 2026-09-09** (ch3 unblocks `cold_boot_hw_init`'s task-readiness wait, ch4 unblocks `dsp_boot_handshake`), see Status above. Every other channel/register/event still plain storage (`regs[]` passthrough) |
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR protocol + virtual EEPROM (only RIIC2 exercised by any traced boot path so far — the diode-matrix EEPROM, `IC351`/`GT24C128B`) |
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI IRQ per channel. Real RXI on channel 3 too, backing a virtual front-panel responder (SCIF3 only) — see Status above |
| MMCIF (SD/MMC host) | `mmc.c` | Real command/response/data protocol + virtual SD card, validated standalone — `body.bin`'s own driver not yet reached by any traced boot path |
| DMAC (DMA controller) | `dmac.c` | Real channel 0 only (edge `DMAINT0`/GIC ID 41, real `address_space_read()`/`address_space_write()` transfer) — **confirmed load-bearing 2026-09-09**, unblocks the busy-wait right after MTU2's, see Status above. Every other channel/register still plain storage |

## Directory layout

- **`src/`** — our own C sources (tracked in git): `rz_a1h.h`/`rz_a1h.c` (shared addresses,
  the machine itself), plus one file per peripheral in the table above. `ostm.c`, `gpio.c`,
  `l2c.c` are direct C ports of the matching `emu/peripherals/*.py` module; `scif.c`, `mmc.c`,
  `riic.c` are genuinely new work (no Python original).
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
    -qmp unix:/tmp/qemu.sock,server,nowait   # or -s -S for GDB
```

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
   (real behavior), `riic.c` (real), `mtu2.c` channel 3 + channel 4 (both `TGI4A`/`TGI4C`) and
   `dmac.c` channel 0 (real, 2026-09-09 — all confirmed load-bearing, see Status above);
   `add_plain_ram_region()` still covers CPG (plain storage, nothing traced needs more) and
   everything in MTU2/DMAC outside their modeled channels/events.
4. ~~SCIF UART output~~ — done: TX plus real per-channel TXI, real RXI + a virtual front-panel
   responder on channel 3 (2026-09-09). The SCIF3 front-panel handshake now fully resolves for
   the first time ever, and boot reaches real ITRON task activation as a direct result.
5. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — the active thread. `mmc.c`
   built and validated standalone; `riic.c` built and validated end-to-end against a real
   natural boot; `FUN_2002b29c`'s entire cold-boot branch gate now clears; the SCIF3
   front-panel handshake now genuinely completes; boot reaches real `itron_act_tsk` task
   activation; **`mtu2.c` (channels 3 and 4) and `dmac.c` together clear cold_boot_hw_init's
   whole task-readiness-wait cluster and `dsp_boot_handshake`'s own wait, reaching
   `dsp_cmd_table_init`/`dsp_param_sync_tick` — the furthest any session has reached**.
   **Currently blocked on**: a generic RTOS job-queue overflow whose drain mechanism is a
   pure in-RAM flag, not a peripheral — see "Active resume point" above. This is a genuinely
   different class of problem (probable task-scheduling gap, not a missing device) and is
   judged worth its own dedicated investigation. Once past it, the original question — does the
   SD-card update flow reach MMCIF against a *properly* kernel-created task, and would the
   whole chain accept and boot custom firmware entirely offline — becomes directly retestable
   with the existing `force_call_fup.py`/`test_fup_scheduling.py` tooling.
