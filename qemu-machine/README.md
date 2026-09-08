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

## Status, 2026-09-08 — first slice built and boots real firmware; IRQ delivery not yet
## conclusively validated

**Confirmed, solid**:
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

**Not yet conclusively validated**: whether a real GIC-delivered periodic timer interrupt
actually wakes the `WFE`-parked CPU and gets correctly taken by `body.bin`'s own ISR — the
actual point of this whole migration. Manually arming `OSTM0` via GDB (writing `CMP`/`TS`
directly) to test this in isolation, decoupled from the separate open question of whether
`body.bin` itself ever arms a real tick source by the time it reaches the `WFE` wait, did not
produce a clear result this session — GDB's Python scripting against QEMU's remote-serial
stub proved unreliable here (`continue`/`interrupt` interactions that should block
synchronously often didn't, and some register snapshots looked stale across reconnects in
ways not yet root-caused). This is a **tooling reliability gap in this session's own test
methodology, not a demonstrated problem with QEMU's interrupt delivery** — genuinely
inconclusive, not a negative result. Revisit with a more robust driver (see Next steps).

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

Inspecting live state: QMP's `human-monitor-command` → `info registers` worked reliably this
session; GDB's remote stub (`-s -S`, `target remote :1234`) did not, for the reasons above —
prefer QMP for now until that's sorted out.

## Extension roadmap

1. **Root-cause the GDB scripting reliability gap**, or replace it with a more robust driver
   (raw GDB remote-serial-protocol client in Python, avoiding `gdb`'s own Python API entirely;
   or drive everything through QMP, which was reliable). This blocks conclusively answering
   this migration's actual motivating question.
2. **Find what really arms `body.bin`'s tick source** — a real RE question, not an emulator
   gap: by the time execution reaches the `WFE` wait, has `body.bin` armed `OSTM0` (or some
   other peripheral not yet modeled here) for periodic operation? Tracing this pins down
   whether the manual-arm test in (1) is even testing the right device.
3. Port `emu/peripherals/{gpio,cpg,l2c,mtu2,riic}.py` to C devices here — mechanical (each is
   already a tiny `read`/`write` pair), deliberately deferred so this slice stayed focused.
4. Once (1)+(2) land: SCIF UART output, then the same downstream roadmap `emu/README.md`
   already lists (SD-card/VFS testing for `sdk/roadmap.md`'s Phase 0, the big payoff).
