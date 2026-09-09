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

## Status, 2026-09-10 — DMAC completion race fixed; boot reliably reaches a job-ring-overflow
## trap right after it; that trap's full mechanism is now traced end to end, one open question
## left. This section is deliberately a tight current-state summary, not the narrative — for the
## full derivation, README-history.md's final five 2026-09-10 sections cover the ring-overflow
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
## **Open resume point**: what specifically holds `CPSR.I=1` that long. This is now a
## static-analysis question — find a `disableIRQinterrupts()`/`cpsid i` call site (in
## `cold_boot_hw_init`-era code reached around this boot depth) whose matching re-enable isn't
## guaranteed to run promptly — not a live-tracing one: live tracing has been shown to suppress
## this specific effect at multiple levels (a breakpoint on `id0`'s entry; even plain extra
## memory-read polling beyond 1-2 words per sample), so budget any further live confirmation
## carefully and re-verify the phenomenon still reproduces before trusting a "clean" run.
##
## Two wrong leads were chased and retracted the same session they came up, not left standing —
## an SVC-dispatch misattribution (the address was actually the generic IRQ vector) and a
## `sdcard_file_rpc_dispatch_task` misattribution (alive and doing real work, genuinely, but not
## the source of this specific burst). See README-history.md if the exact reasoning ever matters.

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
| GPIO/port registers | `gpio.c` | Real (masked set/clear, `PNOT` toggle, live `PPR` pin levels) — `P1_6`/`PDV` (power-fail detector) defaults high, see Status above |
| L2C (PL310 cache controller) | `l2c.c` | Real (`CACHE_ID`/`CACHE_TYPE`/`REG7` self-clear semantics) |
| CPG | `rz_a1h.c`'s `add_plain_ram_region()` | Plain storage, no behavior — nothing traced needs more yet |
| MTU2 | `mtu2.c` | Real channel 3's `TGI3A` (GIC 154), channel 4's `TGI4A`/`TGI4C` (GIC 159/161), and two more purely-polled compare-match events sharing one status byte (`0xFCFF0305` bits 2/0, targets `0x30c`/`0x308`, no GIC ID — host-wall-clock deadlines, not a live counter) — **all five confirmed load-bearing** (ch3 unblocks `cold_boot_hw_init`'s task-readiness wait — see `mtu2_ch3_periodic_housekeeping_tick` in Status above — ch4 unblocks `dsp_boot_handshake`, the fourth unblocks `scif5_cmd_transmit_now`, the fifth unblocks a DMA-descriptor-setup routine). Every other channel/register/event still plain storage (`regs[]` passthrough). **`MTU2_FREQ_HZ` confirmed real at 32MHz** (P0φ/1, same real clock as OSTM — the original 25MHz was an empirical placeholder, corrected once `-icount` made a real-clock re-check necessary) |
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
