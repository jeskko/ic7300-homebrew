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

## Status, 2026-09-09 — the SCIF3 front-panel handshake fully resolves for the first time ever;
## boot reaches real ITRON task activation; the active blocker is a new, so-far-unadvancing
## task-readiness counter one step past it

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

**Active resume point — the concrete next step:** `cold_boot_hw_init`, right after activating
the new task, busy-waits on a task-readiness counter (`*DAT_2002b4ec`'s dereferenced target,
`0x2039076c` this run — a per-boot address, don't hardcode it) bounded at `0x32` (50) — this
is a genuinely new counter, unrelated to the SCIF3 one above (different address, different
containing function, found immediately after task activation, not inside the SCIF3 driver at
all). Confirmed via live polling that it stays at `0` for 20+ continuous real seconds even with
the task already activated. Two real leads, neither chased yet: (1) find what task
`itron_act_tsk`'s own argument (`DAT_2002b4e8`, a task descriptor) identifies — `notes/kernel-
rtos.md`'s existing `itron_act_tsk` call-site table (11 sites, already cross-checked in earlier
sessions) may already have this one; if the activated task's own entry point is known, trace
what it does before touching this counter, since that's presumably the real writer; (2) check
whether the newly-activated task is actually being *scheduled* at all under this emulator's
context-switch mechanism (`irq_context_switch_id86`/`id0`, already confirmed real and working
for the existing `sys_monitor_task_entry` task) — a real but so-far-unconfirmed possibility is
that multi-task scheduling itself has a gap this project hasn't exercised yet, since every
prior milestone reached was still effectively single-tasked. Two tools worth reusing directly:
`gdbrsp.py`'s new watchpoint support (fast, conclusive "does anything write here at all" answers
— faster than manual `references_to` sweeps for a dynamically-allocated RAM target); and a live
`-d unimp` register trace whenever a fix looks right in isolation but the overall state still
doesn't budge, per the methodology point above.

## Confirmed peripherals

| Device | File | Status |
|---|---|---|
| GIC (Distributor + CPU I/F) | `rz_a1h.c` (QEMU's own `arm_gic`) | Real, working — see the off-by-32 `qdev_get_gpio_in` bug in README-history.md |
| OSTM0 / OSTM1 | `ostm.c` | Real timer + real IRQ. OSTM0 is `body.bin`'s real tick source (ID 134, `CMP`=32000) |
| SPI boot status | `spi_boot.c` | Real — the one register `base.dat`'s SPI-ready poll needs |
| GPIO/port registers | `gpio.c` | Real (masked set/clear, `PNOT` toggle, live `PPR` pin levels) — `P1_6`/`PDV` (power-fail detector) defaults high, see Status above |
| L2C (PL310 cache controller) | `l2c.c` | Real (`CACHE_ID`/`CACHE_TYPE`/`REG7` self-clear semantics) |
| CPG, MTU2 | `rz_a1h.c`'s `add_plain_ram_region()` | Plain storage, no behavior — nothing traced needs more yet |
| RIIC0-2 (I2C) | `riic.c` | Real CR2/SR2/DRT/DRR protocol + virtual EEPROM (only RIIC2 exercised by any traced boot path so far — the diode-matrix EEPROM, `IC351`/`GT24C128B`) |
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI IRQ per channel. Real RXI on channel 3 too, backing a virtual front-panel responder (SCIF3 only) — see Status above |
| MMCIF (SD/MMC host) | `mmc.c` | Real command/response/data protocol + virtual SD card, validated standalone — `body.bin`'s own driver not yet reached by any traced boot path |

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
   (real behavior), `add_plain_ram_region()` for CPG/MTU2 (plain storage, nothing traced needs
   more yet).
4. ~~SCIF UART output~~ — done: TX plus real per-channel TXI, real RXI + a virtual front-panel
   responder on channel 3 (2026-09-09). The SCIF3 front-panel handshake now fully resolves for
   the first time ever, and boot reaches real ITRON task activation as a direct result.
5. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — the active thread. `mmc.c`
   built and validated standalone; `riic.c` built and validated end-to-end against a real
   natural boot; `FUN_2002b29c`'s entire cold-boot branch gate now clears; the SCIF3
   front-panel handshake now genuinely completes; boot reaches real `itron_act_tsk` task
   activation. **Currently blocked on**: a new task-readiness counter that doesn't advance yet
   (see "Active resume point" above) — once past that, the original question — does the
   SD-card update flow reach MMCIF against a *properly* kernel-created task, and would the
   whole chain accept and boot custom firmware entirely offline — becomes directly retestable
   with the existing `force_call_fup.py`/`test_fup_scheduling.py` tooling.
