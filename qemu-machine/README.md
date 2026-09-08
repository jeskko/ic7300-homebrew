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

## Status, 2026-09-08 — boot clears the entire cold-boot branch gate; the active blocker is a
## real-but-known SCIF3 front-panel handshake with no virtual front panel to answer it

**Confirmed, solid:**
- A custom QEMU machine (`rz-a1h`) builds cleanly against real QEMU v11.1.1 source (pinned,
  vendored checkout under `qemu-src/`, gitignored — `setup.sh` recreates it) and boots real,
  unmodified v1.42 firmware: `base.dat`'s traced sequence runs, the body decompresses, real GIC
  IRQ delivery works (OSTM0 is `body.bin`'s real tick source — GIC ID 134, `CMP`=32000), and
  execution reaches deep into real, previously-unreached `cold_boot_hw_init`-era code — the
  furthest this emulator has ever gotten.
- **With `tools/build_riic_eeprom_image.py`'s output supplied as RIIC2's backing image** (see
  "Running it" below), `FUN_2002b29c`'s entire cold-boot-vs-power-state branch decision clears
  for the first time — every EEPROM signature check and the real GPIO power-good gate all
  resolve correctly (see README-history.md's most recent two sections for the full multi-bug
  trace: a level- vs. edge-triggered GIC bug, a dummy-first-byte RIIC2 read-shift, and a GPIO
  `P1_6`/`PDV` power-fail-detector default, all found and fixed the same day).

**Active resume point — the concrete next step:** boot now stops at
`scif3_frontpanel_identify_handshake` (`0x20037424`-`0x200374e3`), a real, already-documented
one-shot SCIF3 handshake with the physical front-panel MCU (`IC501`) at cold boot — sends an
outbound `0xF0`-type frame via `scif3_send_frame` (`0x20037214`) and blocks (75-count retry
loop) waiting for a reply. `scif.c` currently has "no IRQ line wired to the GIC yet" and models
TX only — nothing plays the front panel's role on the RX side, so this handshake can only ever
time out, and confirmed via 60+ real seconds of polling that it does **not** resolve on its
own (a genuine stall, not a slow-but-graceful timeout). `sdcard_file_rpc_dispatch_task`'s own
struct fields (`[+0x20]`/`[+0x28]` at the live address `DAT_200ba174` resolves to) are still
zero at this point — the task-creation payoff is closer than it's ever been but not yet
reached.

**What building a fix needs** (protocol detail already fully documented, not re-derivation):
- Framing: 33-byte packets, `0xFE`/`0xFD` preamble/terminator — same convention as CI-V
  (`SCIF0`) and the service-mode link (`SCIF1`), one shared driver template.
- The reply lands in `g_scif3_rx_status_buffer` (`0x203dcab6`) via the same
  `scif3_frame_rx_statemachine`/`scif3_frame_dispatch_by_type` machinery ordinary frames use;
  bytes 1-12 get copied into `g_frontpanel_latched_status` (`0x203dca96`), and bytes 1-3 of
  *that* are what the version-info screen formats as `<digit>.<digit><digit>` (matches the
  `"SX3765 Vx.xx-xxx"` format already found in the EEPROM signature strings this same session
  — a real, satisfying convergence, not confirmed to be more than a shared naming convention
  across the product's MCUs).
- Full derivation: `notes/front-panel-firmware.md`'s "Resolved... how the main CPU gets the
  front-panel version" entry, and `notes/front-panel-protocol-handout.md`'s "What's already
  solid about the `SCIF3` wire protocol" section — both already have everything needed to build
  a minimal responder; this is a `qemu-machine/` implementation task, not a research one.
- **What doesn't exist yet and needs building**: `scif.c` has zero RX-injection capability at
  all (TX-only, matching its own "deliberately minimal" scope from when it was built). A
  virtual front-panel responder needs `scif.c` (or a small companion device) to (1) recognize a
  complete outbound `0xF0`-type frame on the TX side, and (2) inject a correctly-framed reply
  back so `scif3_frame_rx_statemachine` sees it as a real incoming byte stream. Matches this
  project's own permissive-peripheral philosophy elsewhere (`mmc.c`'s virtual SD card,
  `riic.c`'s virtual EEPROM) — a plausible canned reply is enough, real front-panel-MCU
  protocol fidelity isn't needed to unblock boot.
- **Open question to settle first**: does boot actually *need* this handshake to succeed (or at
  least to be seen trying and giving up) before proceeding to task creation, or is task
  creation gated on something reachable only after this regardless? Worth a quick check (e.g.
  does the 75-count retry loop's own timeout path, once actually reached, lead anywhere new)
  before committing to building a full responder.

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
| SCIF0-7 (UART) | `scif.c` | TX-only, no IRQ wired yet — see "Active resume point" above for what SCIF3 specifically still needs |
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
4. ~~SCIF UART output~~ — done (TX-only; SCIF3 RX/IRQ support is the active item 5 work, see
   Status above).
5. **SD-card/VFS testing (`sdk/roadmap.md`'s Phase 0 payoff)** — the active thread. `mmc.c`
   built and validated standalone; `riic.c` built and validated end-to-end against a real
   natural boot; `FUN_2002b29c`'s entire cold-boot branch gate now clears. **Currently blocked
   on**: a virtual front-panel SCIF3 responder (see "Active resume point" above) — once past
   that (or once it's confirmed not actually gating task creation), the original question —
   does the SD-card update flow reach MMCIF against a *properly* kernel-created task, and
   would the whole chain accept and boot custom firmware entirely offline — becomes directly
   retestable with the existing `force_call_fup.py`/`test_fup_scheduling.py` tooling, no
   further RTOS-internals work needed.
