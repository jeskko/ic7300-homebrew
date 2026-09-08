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

## Status, 2026-09-09 — SCIF3 TXI now real and verified end-to-end; the active blocker moved one
## level deeper, to an unidentified RTOS tick/delay counter the handshake's own timeout polls

**Confirmed, solid, foundational (from the prior session, still true):**
- A custom QEMU machine (`rz-a1h`) builds cleanly against real QEMU v11.1.1 source (pinned,
  vendored checkout under `qemu-src/`, gitignored — `setup.sh` recreates it) and boots real,
  unmodified v1.42 firmware: `base.dat`'s traced sequence runs, the body decompresses, real GIC
  IRQ delivery works (OSTM0 is `body.bin`'s real tick source — GIC ID 134, `CMP`=32000), and
  execution reaches deep into real, previously-unreached `cold_boot_hw_init`-era code.
- **With `tools/build_riic_eeprom_image.py`'s output supplied as RIIC2's backing image** (see
  "Running it" below), `FUN_2002b29c`'s entire cold-boot-vs-power-state branch decision clears —
  every EEPROM signature check and the real GPIO power-good gate all resolve correctly (see
  README-history.md for the full multi-bug trace behind this).

**Confirmed, solid, this session (newest first):**
- **SCIF3's TXI (transmit-complete) interrupt is now real and verified working end-to-end**
  (`scif.c`, `rz_a1h.c`/`rz_a1h.h`). The "quick check" the previous Status entry flagged (does
  the identify handshake's own 75-count timeout path lead anywhere once actually reached) led
  somewhere deeper instead: `scif3_driver_pump_tick`'s very first line (`if (bit 0x02) return;`)
  was a permanent no-op after the *first-ever* SCIF3 send, because the only code that ever
  clears that busy bit is the ISR for the interrupt `scif3_send_frame` explicitly arms
  (confirmed via a real `0xec`=236 literal argument to a confirmed generic `gic_enable_irq(id)`
  helper) — and `scif.c`'s TX side had "no IRQ line wired to the GIC yet" (the previous
  paragraph's own words). Fixed and **live-verified via a register-write trace** (`-d unimp`
  shows the real sequence: `SCR 0x78→0xf8` (TIE on) → `TX fe/f0/fd` → `SCR 0xf8→0x78` (TIE off,
  the ISR's own terminal cleanup) — the identify request's real 3-byte frame now goes out over a
  genuinely interrupt-driven path, and the driver's busy flag correctly clears afterward.
  Three real bugs found and fixed getting here, all documented in `scif.c`'s own comments:
  1. TXI3's GIC ID (236) derived from `~/Downloads/rza1.svd`'s ICDISR6/7 fields (formula:
     register-index×32+bit, the same one that already gave OSTM0 its ID 134) — independently
     cross-checked against that `0xec` firmware literal, a genuine two-derivations-agree
     confirmation, not just SVD-reading.
  2. TXI3 is level-triggered (confirmed live via `GICD_ICFGR14`/`ISENABLER7`) — the *exact* class
     of bug `riic.c`'s own file comment already documents (a bare pulse is silently dropped by
     `arm_gic` for a level-sensitive SPI) hit twice over: first as the obvious pulse-vs-level
     issue, second as a subtler one where `qemu_irq_raise()` while already-high is a silent
     `qemu_set_irq` no-op (fine for one byte, but the real multi-byte-frame ISR re-primes TIE on
     every subsequent byte without an intervening clear) — fixed the same way `riic.c` already
     had to: explicit `qemu_irq_lower()`+`qemu_irq_raise()` pairs, not a bare raise.
  3. The ISR's own FSR-clearing write (real-hardware bookkeeping, immediately after every FTDR
     byte) was routed to an FSR write-handler that unconditionally lowered the just-raised line
     again before the CPU ever got a chance to sample it — an emulator-only artifact (nothing
     happens fast enough on real hardware to lose a genuine edge this way). Fixed by making FSR
     writes a true no-op, as they originally were before this pass added interference.
  A live-tracing methodology note worth keeping: two of this session's dead ends came from
  **the exact same pointer-indirection gotcha hitting twice** — `DAT_20037588` (the driver's
  "busy flags" byte) and `DAT_200375c0` (the handshake's own retry counter) are both themselves
  *pointer variables* holding the real target's address, not the target itself; reading the
  literal address directly (instead of dereferencing it first) gave a plausible-looking but
  completely wrong "stuck forever" signal for a long stretch of this session before the mistake
  was caught by cross-checking against the ISR's own confirmed real writes.
- **New blocker found, one level deeper than TX completion**: with TXI now real, the identify
  handshake's own outer wait loop (`scif3_frontpanel_identify_handshake`'s 75-count retry bound)
  still never advances — confirmed via 40+ continuous real seconds of execution *with* real
  OSTM0-driven scheduler activity visibly happening around it (PC repeatedly visits the real IRQ
  vector/dispatcher). The polled counter (`*DAT_200375c0`'s dereferenced target, a per-boot RAM
  address, e.g. `0x203fc60f` this run) has **zero writers anywhere in the SCIF3 driver code** —
  it is not SCIF-specific at all, almost certainly a generic RTOS tick/delay counter that some
  other, not-yet-traced piece of kernel code increments. OSTM0's own confirmed real per-tick
  handler (`irq_context_switch_id86`, `0x200059b4` — a genuine FreeRTOS context-switch sequence)
  calls several still-unidentified sub-functions on every tick (`FUN_200b93f8` — currently just
  `bx lr`, an empty stub — `FUN_200b9400`, `FUN_2018849c`); the real tick-count increment (if one
  of these is it) hasn't been confirmed. This is real FreeRTOS-internals tracing, the exact kind
  of work the SD-card payoff question already hoped to avoid needing ("no further RTOS-internals
  work needed" was the previous Status entry's own hoped-for outcome) — a genuinely new, deeper
  checkpoint, not a quick follow-up.

**Active resume point — the concrete next step:** find what increments the RTOS tick/delay
counter `scif3_frontpanel_identify_handshake`'s own outer 75-count loop polls (a per-boot RAM
address, `*DAT_200375c0`'s dereferenced target — read it live each boot, don't assume last
session's `0x203fc60f`). Given it has zero writers anywhere in the SCIF3 driver itself, the
real candidates are OSTM0's own confirmed per-tick handler's still-unidentified callees:
`FUN_200b93f8` (`0x200b93f8`, currently just `bx lr` — an empty stub, oddly, worth checking
*why* it's empty before assuming it's irrelevant), `FUN_200b9400`, `FUN_2018849c`, called from
`irq_context_switch_id86` (`0x200059b4`). This is real FreeRTOS-internals tracing (find the
real `xTickCount`-equivalent and confirm this address is it, or find the real mechanism if it
isn't a global tick count at all but a per-task/per-driver delay field written through some
other indirect path) — treat it as its own dedicated tracing session, not a quick follow-up.
Two live-tracing tools worth reusing directly: `-d unimp -D <logfile>` on the QEMU command line
(catches every `scif.c` register write with a timestamp-free but correctly-ordered trace — this
is what resolved the TXI investigation once GDB single-stepping alone gave a misleading picture,
see README-history.md) and always dereference `DAT_*`-named globals in this driver's own
literal-pool style before trusting a read against them (see the pointer-indirection gotcha in
the "Confirmed, solid, this session" bullets above) — both were essential this session and will
likely be again. Once this counter is understood, the SCIF3 front-panel-handshake work from the
prior Status entry (building a virtual front-panel RX responder, or confirming the handshake's
own natural timeout path is enough without one) is still the next layer after that — nothing
in this session invalidates that plan, it just wasn't reached yet.

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
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI (transmit-complete) IRQ per channel — verified end-to-end against SCIF3's real identify-handshake frame. RXI still not wired (no traced boot path needs it yet) |
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
4. ~~SCIF UART output~~ — done: TX plus a real, level-triggered per-channel TXI IRQ (2026-09-09),
   verified end-to-end against SCIF3's real identify-handshake frame. RXI still not wired (no
   traced boot path needs it yet).
5. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — the active thread. `mmc.c`
   built and validated standalone; `riic.c` built and validated end-to-end against a real
   natural boot; `FUN_2002b29c`'s entire cold-boot branch gate now clears; SCIF3 TX now
   genuinely completes over a real interrupt-driven path. **Currently blocked on**: an
   unidentified RTOS tick/delay counter (see "Active resume point" above) — once found, the
   already-scoped SCIF3 front-panel-handshake work (a virtual RX responder, or confirming the
   handshake's own natural timeout is enough without one) is the next layer, and after that the
   original question — does the SD-card update flow reach MMCIF against a *properly*
   kernel-created task, and would the whole chain accept and boot custom firmware entirely
   offline — becomes directly retestable with the existing
   `force_call_fup.py`/`test_fup_scheduling.py` tooling.
