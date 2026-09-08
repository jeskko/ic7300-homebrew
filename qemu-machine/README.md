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

## Status, 2026-09-08 — real GIC IRQ delivery confirmed working, and body.bin's real tick
## source identified (OSTM0, ID 134, CMP=32000) as a side effect of fixing it

**The migration's actual motivating question is now answered, with a real bug found and
fixed getting there.** `rz_a1h.c` wired `OSTM0`/`OSTM1`'s IRQ outputs straight to
`qdev_get_gpio_in(gic, 134)`/`qdev_get_gpio_in(gic, 135)`, treating those as absolute GIC
interrupt IDs. They aren't: `arm_gic`'s own `gic_set_irq()` offsets every external
`qdev_get_gpio_in()` index by `GIC_INTERNAL` (32, the SGI+PPI count) to get the absolute ID
-- confirmed by cross-checking `hw/arm/fsl-imx6.c`'s own IRQ `#define`s (used directly as
gpio-in indices there) against `hw/intc/arm_gic.c`. So this machine's OSTM0/OSTM1 lines were
actually wired to absolute IDs 166/167, not 134/135 -- silently never delivering to the CPU
no matter how correctly the distributor/CPU-interface/`ICFGR` were configured. Found via a
manual test (`tools/test_irq.py`, see below) that force-armed OSTM0 and watched
`GICD_ISPENDR4` (the pending-register word covering IDs 128-159) never show the bit set.
Fixed by subtracting `RZA1H_GIC_NUM_INTERNAL` (32) in `rz_a1h.c`; full derivation in that
file's own comment and `rz_a1h.h`'s new constant.

**After the fix, a completely untouched boot (zero manual GDB writes at all) reaches deep,
active code past the idle loop on its own** -- confirmed via `tools/test_irq.py`'s own
negative-control mode and a follow-up register dump: `GICD_CTLR`/`GICC_CTLR`/`GICC_PMR` are
all firmware-enabled, `GICD_ISENABLER4` has bit 6 (ID 134) set, and `OSTM0.CMP` reads back as
**32000** with `TE=1` (running) -- entirely `body.bin`'s own doing, not anything this test
script wrote. This also **answers Extension-roadmap item 2** ("find what really arms
`body.bin`'s tick source"): it's genuinely **OSTM0**, not OSTM1 (which is real too, but only
ever polled for its free-running `CNT` by `FUN_2002b878`, confirmed in earlier sessions --
never armed with a real `CMP`/interrupt). Direct `arm-none-eabi-objdump` disassembly of the
live RAM image (ARM mode, matching the surrounding idle loop) at `0x200b93b0`-`0x200b93e0`
shows the exact arming sequence: `mov r2, #32000` / `str r2, [r0]` (writing `OSTM0.CMP`),
`strb r1, [r0, #0x14]` (writing `OSTM0.TS` to start it), then `mov r0, #134` right before
`bx lr` -- the literal interrupt ID (Renesas's own `INTC_ID_OSTM0TINT`) being handed back to
whatever calls this function next, presumably a GIC-enable helper. **Not yet annotated in
Ghidra** -- this address range is a never-before-disassembled gap (the same class of gap
documented in the project memory's Ghidra-tooling section, not an ARM/Thumb mode bug this
time: needs disassembly applied at all, in ARM mode, confirmed by both the objdump result and
Ghidra's own decompile of the freshly-`functions.create`d stub failing with "bad instruction
data"). A fix request is queued in `scratch/armthumb_fix_requests.txt`
(`0x200b939c 0x60 arm`) for the user to run via the Script Manager, per the established
workflow in `tools/README.md`.

**The dynamic-testing tooling gap (GDB's Python API being unreliable) is also closed**:
built `tools/gdbrsp.py`, a from-scratch ~150-line client speaking GDB's remote-serial
protocol directly over a raw socket (no `gdb` process, no Python-API threading) --
`g`/`G`/`m`/`M` for registers/memory, `c`/`s`/interrupt/`?` for execution control. Used by
`tools/test_irq.py` (arms OSTM0 + the GIC manually, or just watches an unmodified boot) to
drive the whole IRQ-delivery investigation end-to-end, reliably, this session. One real
non-QEMU-specific gotcha documented in `gdbrsp.py` itself: sending the RSP interrupt byte
(`0x03`) to an *already-stopped* target gets no reply at all (only a *running* target
answers it) -- query current state with `?` (`status()`) instead when that's unknown, which
always answers either way.

**Confirmed, solid** (unchanged from the first pass):
- A custom QEMU machine (`rz-a1h`) builds cleanly against real QEMU v11.1.1 source and boots.
- Real, unmodified v1.42 firmware genuinely boots on it: `base.dat`'s traced sequence runs,
  the body decompresses, and execution reaches deep into `body.bin` — confirmed by hitting
  **the exact same real addresses independently found via the Unicorn emulator and manual
  Ghidra/objdump analysis** (`FUN_2002b878`'s `OSTM1`/`PPR1` busy-wait, then the real `WFE`
  wait at `0x200b93ac`), a strong cross-validation that both emulation paths agree.
- Real `arm_gic` (ARM GIC Distributor + CPU Interface) and two real `OSTM` timer devices are
  wired at the real hardware addresses, replacing every peripheral this slice doesn't model
  with QEMU's own generic `unimplemented-device` (the same "hit it, then build it" role
  `emu/peripherals/stub.py` plays in the Unicorn version).
- **Two real bugs found and fixed getting this far** (both documented in the source, not just
  here): `rz_a1h.c` initially never created a backing memory region for the flash address before
  calling `rom_add_file_fixed()` — that function only *registers* a blob to be copied in later,
  it doesn't allocate memory, so the very first instruction fetch faulted immediately. And
  `ostm.c`'s first `CNT` model was bounded by `CMP` (an "elapsed fraction of the current
  period" reading) — wrong for `FUN_2002b878`'s real polling loop, which starts `OSTM1`
  without ever writing `CMP` and polls `CNT` against a large, unrelated software threshold;
  fixed by making `CNT` a genuinely unbounded, monotonically increasing counter (matching what
  `emu/peripherals/ostm.py`'s own Python model already did, for the same reason).
- A third, QEMU-machine-registration-specific gotcha, also worth remembering: a plain
  `DEFINE_MACHINE("rz-a1h", ...)` genuinely registers a real QOM type (confirmed via
  `qom-list-types implements=machine`) that is still completely invisible to `-M help`/`-M
  rz-a1h` on the ARM target — every real ARM/AArch64 board in this QEMU version uses
  `DEFINE_MACHINE_ARM()` (or the older manual-`TypeInfo` equivalent with
  `arm_machine_interfaces[]`) instead, because ARM's machine lookup walks a target-specific
  QOM base type (`TYPE_TARGET_ARM_MACHINE`), not plain `TYPE_MACHINE`. See
  `include/hw/arm/machines-qom.h` in the vendored checkout.

**Now conclusively validated** (2026-09-08, second pass, same day): a real GIC-delivered
periodic timer interrupt does wake the `WFE`-parked CPU and gets correctly taken -- see the
Status section above for the fix (a real `qdev_get_gpio_in()` off-by-32 bug) and the dynamic
proof (`tools/test_irq.py`, `tools/gdbrsp.py`). Cross-checked with a negative control: without
the fix, or without any timer ever firing, the CPU reliably stays parked at the exact same PC
(`0x200b93ac`) across repeated real-time waits; with the fix, both a manually-armed OSTM0 and
(more tellingly) a completely untouched boot leave that PC within ~1-2 real seconds every
time.

## Directory layout

- **`src/`** — our own C sources (tracked in git): `rz_a1h.h` (shared addresses, all
  already-confirmed ground truth carried over from `emu/board.py`/`emu/peripherals/`, not new
  derivation), `rz_a1h.c` (the machine), `ostm.c` (real timer + real IRQ — see its own file
  comment for the full `CNT`-semantics story), `spi_boot.c` (the one real peripheral needed to
  get past `base.dat`'s own SPI-ready poll, direct C port of `emu/peripherals/spi_boot.py`).
- **`patches/hw-arm-build.patch`** — the one small diff (`hw/arm/Kconfig` + `hw/arm/meson.build`)
  that registers our files in a pinned, vendored QEMU checkout. Kept as a patch rather than a
  fork since this is private and pinned, not meant to be upstreamed.
- **`setup.sh`** — idempotent: clones QEMU `v11.1.1` (shallow) into `qemu-src/` if missing,
  applies the patch, symlinks `src/*.c` in, configures (`arm-softmmu` only), builds.
- **`tools/build_flash.py`** — thin wrapper around the already-existing, already-tested
  `emu/flash_image.py` (no reimplementation of the container-offset-correction logic) —
  produces the flat flash image `rz_a1h.c` loads via `-kernel`.
- Gitignored: `qemu-src/` (recreated by `setup.sh`), `flash.bin` (recreated by `build_flash.py`).

## Running it

```
qemu-machine/setup.sh                                  # one-time (or after a source edit)
emu/.venv/bin/python3 qemu-machine/tools/build_flash.py # produces qemu-machine/flash.bin
qemu-machine/qemu-src/build/qemu-system-arm -M rz-a1h -nographic \
    -kernel qemu-machine/flash.bin -serial none -monitor none \
    -qmp unix:/tmp/qemu.sock,server,nowait   # or -s -S for GDB
```

Inspecting live state: `tools/gdbrsp.py`'s raw GDB remote-serial client (`c`/`s`/`?`/`g`/`G`/
`m`/`M`) is now the reliable way to both inspect and drive execution (registers, arbitrary
memory-mapped-register reads/writes, continue/interrupt) — see `tools/test_irq.py` for a
worked example. QMP's `human-monitor-command` → `info registers` also still works for a
read-only spot-check.

## Extension roadmap

Items 1 and 2 from the original plan are both **done** (2026-09-08, second pass) — see the
Status section above: the GDB reliability gap turned out to be `gdb`'s own Python API, not
QEMU's gdbstub (a raw RSP client is completely reliable), and body.bin's real tick source is
OSTM0 (GIC ID 134, `CMP`=32000), confirmed both dynamically (register read-back on an
untouched boot) and statically (direct disassembly of the arming code at `0x200b93b0`).

1. ~~Root-cause the GDB scripting reliability gap~~ — done, `tools/gdbrsp.py`.
2. ~~Find what really arms `body.bin`'s tick source~~ — done, OSTM0/ID 134/CMP=32000. Not yet
   annotated in Ghidra (never-before-disassembled region, ARM mode; fix request queued in
   `scratch/armthumb_fix_requests.txt` for the user to run) — worth finishing so this has a
   real function name and can be cross-referenced from `notes/kernel-rtos.md`, but doesn't
   block anything else here.
3. Port `emu/peripherals/{gpio,cpg,l2c,mtu2,riic}.py` to C devices here — mechanical (each is
   already a tiny `read`/`write` pair), deliberately deferred so this slice stayed focused.
4. Now that (1)+(2) are done: SCIF UART output, then the same downstream roadmap
   `emu/README.md` already lists (SD-card/VFS testing for `sdk/roadmap.md`'s Phase 0, the big
   payoff).
