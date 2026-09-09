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

## Status, 2026-09-09 — a virtual SCIF5 DSP responder resolves the reply-ready deadlock (a
## real ordering-race bug found and fixed getting there); boot now reaches a new busy-wait on
## the shared DSP-comms ring's own "still active" flag

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

**Confirmed, solid, this session — the SCIF5 DSP reply-ready deadlock is resolved via a
virtual DSP responder, and boot reaches a new, further busy-wait:**
- **`scif5_send_and_wait_reply`'s "reply-ready" flag (`*(DAT_200b1c84+3)`) was confirmed
  genuinely, permanently stuck** (a 90-second free-run poll never once saw it clear). Its real
  clearer, `scif5_rx_isr`, was mis-represented by an earlier session's own decompile — a real
  dead-store-elision artifact silently dropped the actual clearing instruction, caught by
  comparing the raw disassembly listing against the pseudocode line by line — and needs 4 real
  bytes over SCIF5's RX path (an `rbit`-reversed reply word), which no virtual DSP ever sends.
- **A virtual channel-5 DSP responder now supplies that missing input**, mirroring the
  channel-3 front-panel responder's own "precompute the end state, deliver only the genuinely
  necessary last byte through the real FRDR/RXI path" technique — a universal class-2
  ("trivial ack") reply, matching this project's established permissive-peripheral philosophy.
- **The first design (triggered off a complete 4-byte TX) worked in one live trial, then got
  stuck again in the very next one** — a real ordering race: the reply-ready flag isn't set
  busy until *after* the caller's TX returns (inside `scif5_arm_retry_timer`), so a
  synchronous TX-triggered ack can land before that write and get silently overwritten by it.
  Fixed by hooking a write provably sequenced *after* the busy-flag set instead — a genuine,
  previously-unmodeled hardware register at `0xFCFE3120` that `scif5_arm_retry_timer` itself
  reprograms as its very next step, reached two independent ways in the firmware (a real
  confirmation it's one register, not two), modeled as a second, tiny MMIO region mapped only
  on the channel-5 SCIF instance. Race-free by construction (real instruction order, not
  timing) — **confirmed load-bearing across 2 independent 90-second trials**, both reaching
  the same new frontier.

Full derivation (the decompiler artifact, the byte-level `rbit` arithmetic, the race and its
fix) in `scif.c`'s own file comment and README-history.md's newest section.

**Active resume point:** both confirming trials progressed to `scif5_cmd_transmit_now`'s own
busy-wait (traced via `LR`, since PC alone kept landing inside a tiny, ubiquitous
register-field-read helper called from many places) on the shared ring-active flag
(`DAT_200b1cac`) — the same flag `scif5_send_and_wait_reply` itself waits on at its own entry,
cleared only once the background DSP-comms ring (the same one `dsp_param_sync_tick`'s ~23-word
parameter stream and the earlier-diagnosed job-queue overflow both involve) reaches genuinely
empty. Not yet traced this session whether/why that ring isn't reaching empty — same playbook
as always: confirm live (is the write pointer still outrunning the read pointer, and from what
producer) before building anything.

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
| SCIF0-7 (UART) | `scif.c` | TX with real, level-triggered TXI IRQ per channel. Real RXI backing two virtual responders: a front-panel one on channel 3, and a DSP-link one on channel 5 (the latter triggered by a second, tiny MMIO region at `0xFCFE3120` on the channel-5 instance only, not by SCIF registers — see Status above) |
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
4. ~~SCIF UART output~~ — done: TX plus real per-channel TXI, real RXI + two virtual
   responders (a front-panel one on channel 3, a DSP-link one on channel 5, 2026-09-09). The
   SCIF3 front-panel handshake fully resolves, boot reaches real ITRON task activation, and the
   SCIF5 `scif5_send_and_wait_reply` reply-ready deadlock is resolved too.
5. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — the active thread. `mmc.c`
   built and validated standalone; `riic.c` built and validated end-to-end against a real
   natural boot; `FUN_2002b29c`'s entire cold-boot branch gate now clears; the SCIF3
   front-panel handshake now genuinely completes; boot reaches real `itron_act_tsk` task
   activation; **`mtu2.c` (channels 3 and 4) and `dmac.c` together clear cold_boot_hw_init's
   whole task-readiness-wait cluster and `dsp_boot_handshake`'s own wait**; a generic RTOS
   job-queue overflow that followed turned out to be a self-inflicted timing artifact
   (`mtu2.c`'s own tick rate outrunning an unrelated queue's real-hardware-paced consumer,
   not a scheduler bug), fixed by tuning that rate; **a virtual SCIF5 DSP responder now
   resolves `scif5_send_and_wait_reply`'s own reply-ready deadlock too** (2026-09-09, see
   Status above for the real ordering-race bug found and fixed getting there). **Currently
   blocked on**: `scif5_cmd_transmit_now`'s busy-wait on the shared DSP-comms ring's own
   "still active" flag, not yet traced — see "Active resume point" above. Once past it, the
   original question — does the SD-card update flow reach MMCIF against a *properly*
   kernel-created task, and would the whole chain accept and boot custom firmware entirely
   offline — becomes directly retestable with the existing `force_call_fup.py`/
   `test_fup_scheduling.py` tooling.
