# `qemu-machine/` — full session-by-session narrative

Full chronological derivation and evidence trail behind [README.md](README.md), which carries
only the current-state summary (confirmed facts, the peripheral status table, and the active
resume point). Sections below are in the order they happened. Addresses, register values, hex
offsets, function names, and cross-references are preserved verbatim from the original notes.

## First pass — the migration's motivating question, and the real off-by-32 GIC bug

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
whatever calls this function next, presumably a GIC-enable helper. Not annotated in Ghidra at
first -- this address range was a never-before-disassembled gap (the same class of gap
documented in the project memory's Ghidra-tooling section, not an ARM/Thumb mode bug this
time: needs disassembly applied at all, in ARM mode, confirmed by both the objdump result and
Ghidra's own decompile of the freshly-`functions.create`d stub failing with "bad instruction
data"). A fix request was queued in `scratch/armthumb_fix_requests.txt`
(`0x200b939c 0x60 arm`) for the user to run via the Script Manager, per the established
workflow in `tools/README.md` -- since done; the region is now annotated
(`ostm0_tick_arm_and_get_irq_id` at `0x200b93b0`, `idle_loop_wfe_spin` at `0x200b939c`). Its
caller (presumably a GIC-enable helper taking the returned `134`) still has no static xref --
a loose end, not blocking.

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
periodic timer interrupt does wake the `WFE`-parked CPU and gets correctly taken -- a real
`qdev_get_gpio_in()` off-by-32 bug and the dynamic proof (`tools/test_irq.py`,
`tools/gdbrsp.py`). Cross-checked with a negative control: without the fix, or without any
timer ever firing, the CPU reliably stays parked at the exact same PC (`0x200b93ac`) across
repeated real-time waits; with the fix, both a manually-armed OSTM0 and (more tellingly) a
completely untouched boot leave that PC within ~1-2 real seconds every time.

## SCIF UART output (extension-roadmap item 4, done 2026-09-08)

Eight real SCIF instances (`scif.c`), one per real hardware channel, with real known roles
per `notes/ic7300-signal-chain.md`: SCIF0 is the CI-V UART, SCIF1 the service/calibration
link, SCIF3 the front-panel link, SCIF5 the DSP link. No `emu/peripherals/` Python original
exists for this one — genuinely new work, not a port. Deliberately minimal (matching this
item's own scope): no baud-accurate transmit pacing, no IRQ line wired to the GIC yet, just
enough real register modeling (masked FIFO-status bits always reporting "ready" so a
polling-based driver never blocks) to make transmitted bytes observable. Every transmitted
byte is always logged (`-d unimp`, `rza1h-scif<N>: TX ..`) regardless of whether a real
chardev is attached, and each channel also takes an optional real backend via
`serial_hd(i)` — pass e.g. `-serial stdio -serial null -serial null -serial pty` positionally
for channels 0-3 the normal QEMU way.

**One real scare chasing this down, worth recording so it isn't re-investigated**: a handful
of post-SCIF boot snapshots landed at a PC that looked like an early-boot hang in IRQ mode
(e.g. `0x200051c0`, `CPSR` mode `0x12`) — genuinely alarming on first sight, distinct from
every prior snapshot this session. Traced with `LR` (the IRQ-banked `r14`, holds the
interrupted return address): it pointed straight back at `0x200b93ac`, the idle loop itself.
**Not a regression** — OSTM0's real 64us tick period (`CMP`=32000 at `ostm.c`'s 500MHz) means
the CPU spends a healthy fraction of real time actually inside the ISR, so a plain 2-second
wall-clock sample landing mid-ISR is statistically expected, not a hang; confirmed by
continuing another 200ms and seeing a normal idle-loop PC again. SCIF's presence didn't cause
this — it was already possible before, just never observed in this session's smaller sample
of earlier snapshots. **Lesson for later sessions**: don't conclude "stuck" from a single
snapshot showing IRQ-mode CPSR near the start of RAM -- read `LR` first, it settles whether
this is a real hang or just an ordinary ISR-in-progress snapshot.

## MMC/SD host controller (extension-roadmap item 5, first pass, 2026-09-08)

`mmc.c` -- a real, protocol-level model of the RZ/A1H's MMCIF (MMC Host Interface), the one
hardware block servicing the 4-bit SD bus this schematic wires up (`SD_CMD`/`SD_CLK`/
`SD_D0`-`D3`/`SD_WP` on `P4_8`-`P4_14`, per `notes/ic7300-signal-chain.md`). This is the
`sdk/roadmap.md` Phase 0 payoff item -- a fully-offline way to test whether a custom
`body.bin` gets accepted and boots, without JTAG -- and the first peripheral in this
directory whose bit-level layout came from the real Renesas hardware manual (Chapter 51,
pages 51-1 to 51-42) rather than `~/Downloads/rza1.svd` (which lists this peripheral's
register names/offsets but zero bit fields, unlike every other peripheral here).

**What's implemented**: the real register set (`CE_CMD_SETH`/`SETL`, `CE_ARG`,
`CE_BLOCK_SET`, `CE_RESP0`-`3`, `CE_INT`, `CE_HOST_STS1`/`2`, `CE_DETECT`, etc., all at their
real bit positions) plus a permissive virtual SD card sharing the same device (real MMCIF
host-controller behavior and a fake-but-protocol-correct card are simplest to model
together here, see the file's own comment) -- `CMD0`/`CMD8`/`CMD55`+`ACMD41`/`CMD2`/`CMD3`/
`CMD9`/`CMD7`/`CMD16`/`CMD17`/`CMD18`/`CMD24`/`CMD12`/`CMD13`, always reporting SDHC
(high-capacity, so read/write command arguments are block indices, not byte addresses) and
a card permanently "inserted". A real, documented MMCIF-specific quirk worth remembering:
`CE_CMD_SETH`'s own name is backwards from its address -- it holds the notional register's
*upper* 16 bits despite living at the *lower* offset (`+0x00`, with `CE_CMD_SETL` at
`+0x02`) -- and writing it is what the manual says triggers command transmission, regardless
of whether software writes it as its own 16-bit store or as half of a combined 32-bit write.
Backed by a flat raw disk image via the `"image"` device property (`-global
rza1h-mmc.image=/path/to/file.img`) -- reads with no image configured (or past its end)
synthesize zero blocks rather than faulting.

**Validated standalone**, the same methodology this session already used for GIC/OSTM0:
`tools/test_mmc.py` drives a full real identification sequence against a small test image
with known per-block content and confirms every response, then single-block, multi-block
(with auto-`CMD12`), and write-then-read-back all round-trip correctly.

**Not yet found: where body.bin's own SD driver actually is.** A direct whole-image search
for this peripheral's base address (`0xE804C800`) as a literal 32-bit word, and a Ghidra
`references_to` check, both came back with **zero hits** — a real, currently-unexplained
negative result. Either the driver code sits in a not-yet-disassembled region (this
project's own established recurring pattern — see the OSTM0-arming-code find) or it loads
the base via a split `MOVW`/`MOVT` pair a literal-word search can't find, or (least likely,
given the schematic's own native 4-bit-bus wiring) SD access doesn't go through this
controller at all. Confirmed empirically too: running an untouched boot for 10 real seconds
with `-d unimp` produced zero accesses to this device at all — SD-card access isn't part of
the boot path this project has already traced (matches `notes/firmware-update.md`: the
update flow is reached via the SD menu, `sd_menu_dispatch_task` case `0xb`, an on-demand
user action, not anything boot-time).

**One real scare while testing this, resolved — not a regression, a lesson worth keeping.**
After adding `mmc.c`, a couple of `tools/test_irq.py` runs showed the CPU still parked in the
idle loop after arming OSTM0 -- looked exactly like the escape-rate regressing. Built
`tools/trial_irq.py` (manages the QEMU process directly via `subprocess.Popen`, sidestepping
this session's own repeated shell pkill/pgrep self-match footguns) to measure the *rate*
across repeated trials rather than reacting to single samples: ~50% escaped with `mmc.c`
present, ~65% with it temporarily `#if 0`'d out and rebuilt -- indistinguishable given the
sample size, and both a world away from the guaranteed-0% rate before real IRQ delivery
worked at all (the very first fix). **Reframed, this makes complete sense**: once
a real periodic tick genuinely drives real scheduling, the system spends real wall-clock time
actually *running its normal workload*, not just idling — a coarse sample landing on "doing
real work" versus "idle" a roughly a coin flip's worth of the time is expected behavior for a
working scheduler, not evidence of anything wrong. **Lesson**: a single boot-snapshot
comparison is not a reliable regression test once real interrupt-driven scheduling is
involved -- use `tools/trial_irq.py`'s repeated-trial approach (or at minimum several
independent fresh-boot trials) before concluding a change regressed IRQ delivery.

## Forcing `firmware_update_main` directly, and where it actually gets stuck (2026-09-08, same day)

**`firmware_update_main` (`0x20025ae4`) turns out to take no arguments at all** — confirmed by
decompile: it opens a fixed global path (`DAT_200264a0`), not a caller-supplied one. That
global has **zero static writers anywhere in `body.bin`** — initially as unexplained as the
missing MMCIF xrefs — but this session found the real reason: it's populated at runtime by the
generic SD-card file-browser/selection UI (a separate subsystem, deliberately not traced or
driven here), using a path built from a folder name and whatever file the user picked from a
list. **Found the real expected convention directly from Icom's own published manual**
(section 15, "Updating the firmware", `IC-7300_ENG_FM_12b.pdf`): *"Copy the downloaded
firmware data into the IC-7300 folder on an SD card"* — i.e. `\IC-7300\<original filename>` at
the SD card root, browsed and picked by name, not a single hardcoded path.

Since `firmware_update_main` is a pure function of that one global, driving it doesn't
require reverse-engineering the file-browser UI at all: **`tools/force_call_fup.py`**
writes a path string directly into the RAM `DAT_200264a0` already points to (confirmed via
`tools/icom_fw`'s own decompressor that this address is genuinely past the end of the static
image — real writable RAM, not a Ghidra gap) and jumps the CPU straight to the function's
entry point over GDB, self-verified via single-stepping through the real prologue before
switching to real-time execution. **`tools/build_sdcard.py`** (a thin `mkfs.vfat` +
`mtools` wrapper, no loopback mount needed) builds a real FAT16 image with a repacked
container (round-tripped through `tools/icom_fw`'s existing `unpack`/`pack`, per
`sdk/roadmap.md`'s own Phase 0 recipe) placed at exactly that path.

**Real, concrete progress, but not yet reaching MMC.** Single-stepping the forced call
confirmed genuine execution through the real prologue, the internal `FUN_20037604` (a
one-shot lock check, not a hang), and into `FUN_200bc5f4` — which turns out to *post* an
"open" request onto an internal ring buffer (`file_rpc_post_command`, real command ID `0xf`)
consumed by a **separate, already-running service task**,
`sdcard_file_rpc_dispatch_task` (`0x200b9c00` — this project's own past retraction of an
earlier "CI-V dispatcher" guess for this same function, see `notes/sd-card-filesystem-
security.md`, turned out right: it's a generic internal file-RPC service, and its real
consumer role is exactly what's exercised here). The caller then blocks in a genuine,
uncapped wait (`FUN_200b9af8` with an infinite-timeout parameter) for that task to respond.
**With both a blank image and a real, valid FAT16 image containing the update file at the
documented path, the result was identical**: the CPU keeps legitimately executing (context
switches to a different, real stack observed; no fault/abort mode ever seen) but never
reaches any MMCIF register access, and `firmware_update_main` never returns, within a
10-second window either way. Since the *image content* provably didn't change the outcome,
the blocker sits upstream of any actual filesystem/card access — most likely (at the time,
before this was resolved -- see below) in how cleanly a hand-hijacked context (the idle
task's registers overwritten via GDB, not a properly FreeRTOS-created task) interoperates
with this RTOS's own semaphore/queue/task-scheduling internals.

**Left as a genuinely open, well-scoped next thread** rather than guessed further: (1) does
`sdcard_file_rpc_dispatch_task` actually run and receive the posted command at all (would
need real RTOS-internals visibility — task list/TCB state, not just PC/CPSR snapshots — to
tell blocked-forever-on-a-real-precondition apart from never-scheduled apart from
silently-misrouted); (2) whether hijacking a properly-idle, genuinely-blocked task context is
fundamentally sound for this RTOS's synchronization primitives, or whether reaching this deep
into a live multitasking system requires either creating a *real* new task (going through the
kernel's own task-creation API instead of repurposing an existing context) or finding a
lower-risk injection point closer to where a real button-press would land.

## The above resolved: `sdcard_file_rpc_dispatch_task` (and ~10 sibling feature tasks) never
## get created at all in this emulation — a real RIIC2/I2C peripheral-modeling gap, not an
## RTOS-hijacked-context problem (2026-09-08, next session)

Settled item (1) above directly with real breakpoints instead of a full TCB-struct reverse-
engineering effort (a from-scratch derivation looked expensive and uncertain — see
`notes/kernel-rtos-history.md`'s "chased `pvPortMalloc`" section for why this project already
backed off that path once). Added real software-breakpoint support to `gdbrsp.py`
(`set_breakpoint`/`remove_breakpoint`, `Z0`/`z0` packets — generic in QEMU's TCG accel, not
ARM-specific, confirmed against `gdbstub/{gdbstub,system}.c` in the vendored checkout) and
built **`tools/test_fup_scheduling.py`**: breakpoints `sdcard_file_rpc_dispatch_task`'s own
outer-wait-return point (`0x200b9d34`) and its real per-command-handler dispatch (`0x200b9da0`,
the `blx r12`), run a baseline (no forced call) and the forced-call scenario back to back on
fresh boots, and compare.

**Neither breakpoint ever fires in either trial**, and neither does a third, deliberately
weaker one added when that came back silent (`0x200b9e30`, fires the instant the task's wait
call returns *at all*, matched command or not). But `file_rpc_post_command`'s own entry and its
internal signal call (`FUN_20186e68`, the same handle `sdcard_file_rpc_dispatch_task` blocks on
via `FUN_20186de4`) **do** fire, twice, with the real command ID (`0xf`) — confirming the post
genuinely happens in this exact automated run, not just in the earlier manually-single-stepped
session. The signal call's own handle argument reads `0x0`. Reading the shared control struct
directly (`DAT_200ba174`/`DAT_200bbc50` both resolve to the same live address, `0x203907f0` —
confirming poster and consumer really do share one struct) showed why: **`[+0x20]` and
`[+0x28]`, the two wait/signal handles this whole mechanism depends on, are still zero even
after 30 real seconds of idle-loop-reaching, untouched boot** — they're never initialized at
all, on a completely stock boot, forced call or not.

Traced why. Those fields are set by `sdcard_file_rpc_dispatch_task`'s own init/creation
function (`FUN_200b995c`, which also creates the task itself via `itron_act_tsk`) — one of
**~80 unconditional calls inside `system_mode_request_dispatch`** (`0x2002a6b8`), which runs
inside `cold_boot_mode_dispatch`'s (`0x2002b1c8`) own loop. `cold_boot_mode_dispatch` is only
entered from one place: `FUN_2002b29c`'s `if (bVar8==1 && *pcVar4=='\0')` branch — and a global
flag this function sets as its very first instruction (`*DAT_2002b500`, "have I been entered")
still reads back `0` after 30s, so **`cold_boot_mode_dispatch` — and therefore the *entire*
feature-task-creation pass (`sdcard_file_rpc_dispatch_task`, `sd_menu_dispatch_task`, both
`audio_buffer_task`s, the RTTY/voice pollers, etc. — essentially every task in
`notes/kernel-rtos.md`'s catalog except the low-level kernel-internal ones) — never runs at
all** in this QEMU machine. This isn't specific to the forced-call scenario; a completely
untouched boot shows the exact same thing.

Localized the actual stall with four more waypoint breakpoints across `FUN_2002b29c`'s own
body (called once, early, confirmed reachable — `notes/diode-matrix.md`'s 14th session had
already independently established `FUN_2002b29c` is "reachable only via message-dispatch,
genuinely once per real hardware boot", a clean cross-validation of this same call site found
independently here by dynamic tracing rather than static analysis): every pass reaches the
call site right after `riic2_driver_init` (`0x2002b2e0`, calling `FUN_2002b274`) and never
comes back. `FUN_2002b274` → `FUN_2001e510` → **`FUN_2001dd58`** is the real stall — a classic
"post a request, then busy-wait `while (*pcVar1 != '\0') FUN_20062c1c();` for a completion
flag some interrupt handler is supposed to clear" pattern, driving an I2C2/RIIC2 read
(`*DAT_2001e6d0 = 0x58` — a real, plausible 7-bit I2C slave address) right after
`riic2_driver_init` registers 6 real RIIC2 interrupt handlers (event IDs `0xcf`/`0xcd`/`0xce`/
`0xd1`/`0xd0`/`0xd2` — already documented in `notes/memory-map.md` as RIIC2's own IDs,
205–210). Since `qemu-machine/src/rz_a1h.c`'s RIIC0-2 were still `add_plain_ram_region()` —
inert storage, zero interrupt-generating behavior (unlike GPIO/L2C/OSTM/MMCIF, which all got
real behavior earlier this project) — the completion interrupt this busy-wait needs never
fired, and the task stayed parked here permanently. **Tempting connection flagged rather than
asserted at the time**: this read happens on RIIC2, the diode-matrix EEPROM's own documented
controller (`notes/memory-map.md`), at the very start of cold boot, into a global
(`DAT_2002b504`) whose top bit feeds directly into `FUN_2002b29c`'s own branch decision —
shaped exactly like a `region_code` read, though a later session found the real gate is a
signature-string comparison, not `region_code` itself (see below).

**Net effect: `force_call_fup.py`'s original "hijacked-context" hypothesis from the section
above is superseded, not confirmed.** The reason nothing beyond `file_rpc_post_command` was
ever observed has nothing to do with hijacking the idle task's context — `sdcard_file_rpc_
dispatch_task` (along with essentially the whole feature-task set) simply doesn't exist yet in
this emulated boot, full stop, regardless of how the call into `firmware_update_main` is
made. This is a genuine peripheral-modeling gap in `qemu-machine/` itself (RIIC needs at least
enough real interrupt-completion behavior to unblock this one boot-time read), the same class
of fix every other peripheral here has already needed, not a deeper RTOS-semantics question.

**Deliberately not attempted that session**: implementing real RIIC register/interrupt
behavior. The real RZ/A1H hardware manual (`/data/misc/icom/7300/doc/REN_r01uh0403ej0600_
rz_a1h_MAT_20210129-2931443.pdf`, Section 18 "I²C Bus Interface") documents the
named interrupt sources (`INTRIICTMOI`/`INTRIICALII`/`INTRIICSTI`/`INTRIICSPI`/`INTRIICNAKI`/
`INTRIICRI`/`INTRIICTEI`/`INTRIICTI`) and the `RIICnCR1`/`CR2`/`MR1-3`/`FER`/`SER`/`IER`/`SR1`/
`SR2`/`SAR0-2`/`BRL`/`BRH`/`DRT`/`DRR` register set (offsets also in `~/Downloads/rza1.svd`, but
— like MMCIF before this — with zero bit-field detail, manual needed for real semantics), but
which of `riic2_driver_init`'s 6 registered handlers actually clears `sdcard_file_rpc_dispatch_
task`'s struct's completion flag, and via what exact `CR2`/`SR2` bit sequence, wasn't traced —
a real, MMCIF-`mmc.c`-sized follow-on task, not a quick guess-and-check fix.

## `riic.c` built, real bug found and fixed (level- vs. edge-triggered), RIIC2 no longer the
## blocker -- boot progresses further, to a new and different stall (2026-09-08, third session)

Built `src/riic.c`: real `CR1`/`CR2`/`MR1-3`/`FER`/`SER`/`IER`/`SR1`/`SR2`/`SAR0-2`/`BRL`/`BRH`/
`DRT`/`DRR` register storage plus the real CR2(ST/RS/SP)-driven protocol sequence derived by
decompiling all 6 of `riic2_driver_init`'s registered handlers (address-phase write, restart,
read-phase, completion — see the file's own header comment for the full per-source derivation,
each tied to a real named interrupt confirmed against `scratch/r01an5093ej0170-rza1-swpkg`'s
`r_intc.h`: `INTIICTEI2`/`RI2`/`TI2`/`SPI2`/`STI2`/`NAKI2` = IDs 205-210). Backed by a flat
byte-addressable virtual EEPROM via the same `"image"` property convention `mmc.c` already
established. All 3 RIIC channels wired with real behavior in `rz_a1h.c` (only RIIC2 confirmed
exercised by any traced boot path so far); `RZA1H_GIC_NUM_IRQ` bumped from 192 to 224 to cover
the highest real ID this adds (`INTIICNAKI2` = 210).

**A real, general bug found and fixed getting this working, worth remembering beyond this one
device**: the first version used `qemu_irq_pulse()` for every step, copying `ostm.c`'s own
pattern — but reading the GIC's own `ICFGR` bits back live (once RIIC2's IDs were actually
enabled) showed all 6 configured **level**-triggered, unlike OSTM0's **edge**-triggered ID.
QEMU's `arm_gic` only latches a level-sensitive SPI as pending while the line is actually
observed high; a pulse (raise-then-immediately-lower, synchronously, within one host call)
never gives the CPU a chance to sample it, so the interrupt was silently and totally dropped
every time — confirmed directly: a real `CR2=ST` write during natural boot reached the device
and its timer fired `qemu_irq_pulse` (both logged), then genuinely nothing further happened, no
ISR ever ran. This is the *exact same class* of gotcha this project already hit once with
OSTM0's own `ICFGR` (see `rz_a1h.c`'s comment on that) — re-confirms the lesson generally:
always check a real interrupt's actual configured trigger mode before assuming a pattern
copied from a working device is safe. Fixed by switching to genuine level-sensitive semantics
throughout: `qemu_irq_raise()` and leave a line asserted until the specific register access
that represents real hardware auto-clearing it (or the driver's own explicit `SR2` write-0)
happens — full mapping of which access clears which source in the file's own comment. Once
fixed, validated two ways: a from-scratch manual protocol drive over raw GDB (mirroring
`test_mmc.py`'s own standalone-validation methodology) correctly returned a byte from a test
virtual-EEPROM image at the exact requested offset; and, far more importantly, **real natural
boot activity now genuinely completes this exact I2C transaction end to end** (confirmed via
`-d unimp` tracing every register access during an untouched boot — the full address-phase,
restart, single-byte read, and stop sequence, byte-for-byte matching the statically-derived
protocol, with zero "unexpected access" warnings).

**Net effect on the original blocker**: fixed. `notes/kernel-rtos-history.md`'s
`cold_boot_mode_dispatch`-entry flag (`*DAT_2002b500`) turned out to be a shared, multi-purpose
global also written by `FUN_2002b29c` itself for an unrelated reason (a wrong attribution made
mid-session and corrected here, not left standing) — the *reliable* signal is still
`sdcard_file_rpc_dispatch_task`'s own control struct fields (`[+0x20]`/`[+0x28]`, see the
resolved section above), and RIIC2's own busy-wait is conclusively no longer where execution
gets stuck (confirmed via the same waypoint-breakpoint methodology as before: every one of
`FUN_2002b29c`'s internal call sites up to and past `FUN_2002b274` now gets reached and
returns from, repeatedly, where it previously never returned once).

**But this alone doesn't reach the original payoff** — those two struct fields are *still* zero
after a full untouched boot. Traced why, cleanly localized this time: `FUN_2002b29c` (the real
top-level cold-boot/power-state entry dispatcher, see its own plate comment) computes a branch
decision (`cold_boot_mode_dispatch` — the path that eventually creates the whole feature-task
set — vs. `FUN_20029ca4`, "the power-state main loop with watchdog-kick sequences") partly from
**the actual byte value RIIC2 delivers**, not just from whether the read completes. This
emulator's virtual EEPROM is an empty, permissive placeholder (no real captured dump exists),
so it answers with `0x00` for everything — and with that value, `FUN_2002b29c` reproducibly
picks the *other* branch. Confirmed via `-d unimp` tracing a full natural boot: execution now
reaches a genuinely new, previously-unreached region (`FUN_20029ca4`'s own body, `0x20029914`-
`0x20029de7`) and stops cleanly at a real RZ/A1H watchdog-unlock sequence (the documented
`0xA518`/`0x5AB1`/`0xA51E` magic write-protect codes) immediately followed by `wfi` — a new,
different, and *correctly* modeled stopping point (this machine has no watchdog-timer device
yet, so nothing ever wakes the `wfi`), not a bug in this session's own work. A second, larger
(17-byte) RIIC2 read was also observed starting at a different memory offset (`0x3e80`, vs. the
first read's `0x3e00`) — presumably deeper cold-boot-specific EEPROM content this same
emulated read path also serves, later identified (see below).

**Follow-up, same day, `FUN_20029224`/`FUN_20029270`/`FUN_200291d8` now traced**: all 3 turn
out to read EEPROM signature/version strings via the same `FUN_2001e510` getter and `memcmp`
them against real reference strings sitting in ROM (`FUN_2017c81e`) — genuinely ROM-embedded
ground truth, no physical EEPROM dump needed for these specific values:
- `FUN_20029224`: the 16-byte `0x3e80` signature vs. `"SX3765 V4.81-000"` (`DAT_2002a08c` →
  `0x2018d796`) — the *current native* format version, distinct from the 4 legacy-compat
  signatures `FUN_20024738`'s own table already documented (`notes/eeprom-catalogue.md`'s
  entry for `0x3e80`): `"SX3765 V3.41-000"`/`"V4.41-000"`/`"V4.61-000"`/`"V4.71-000"`
  (`DAT_20024a28`'s table, confirmed by reading it directly).
- `FUN_20029270`: the same signature's first 7 bytes vs. `"Partial"` (`DAT_2002a098` →
  `0x2018d77f`).
- `FUN_200291d8`: a *third*, previously-unseen 16-byte EEPROM parameter, ID `0x3fc0` (added to
  `notes/eeprom-catalogue.md`), vs. `"SX3765 V0.30-000"` (`DAT_2002a088` → `0x2018d786`).

Built a test image supplying all of `0xff`@`0x3e00`, the real `"SX3765 V4.81-000"`@`0x3e80`,
and the real `"SX3765 V0.30-000"`@`0x3fc0` (confirmed via direct `pread()` tracing that `riic.c`
genuinely delivers all three correctly, byte for byte) — **still no change at the time**: boot
reached the exact same `FUN_20029ca4`/`wfi` stopping point. A follow-up breakpoint at the
branch-decision instruction (`0x2002b540`) **never fired at all** with this image, unlike
before — meaning `FUN_2002b29c` was taking a different internal path before even reaching that
decision point. Resolved later the same day (see below) — turned out to be a real second
`riic.c` bug in the test data, not a deeper code-path mystery.

## The 0x2002b540 mystery resolved, a real second riic.c bug found, and boot reaches real
## cold-boot code for the first time — GPIO's power-good pin turns out to be the actual gate
## (2026-09-08, same day, continued)

Traced why `0x2002b540` stopped firing even with the "correct" `0x3e80`/`0x3fc0` signatures in
place: `FUN_2002b29c` has an **earlier, unconditional short-circuit** at `0x2002b450`
(`cmp r4,#1; beq 0x2002b540; b 0x2002b54c`) — if `bVar8` (`r4`) isn't exactly `1`, execution
jumps *straight* to `FUN_20029ca4`, skipping the `0x2002b540` check (and the `0x2002b524` flag
it reads) entirely. `bVar8` reads `2` with plain zeroed EEPROM content, confirming `bVar2`
(from the 3-function signature chain) was still forcing it — traced further and found why:
`FUN_20029224` returned `-1` (mismatch) even against a byte-for-byte-correct `"SX3765
V4.81-000"` test image.

**Found a second real `riic.c` bug causing that mismatch.** `FUN_2001dbcc` (the real `RI`
handler) reads `DRR` even on its very first "arm" firing (state != 5) — `ldrb r0,[r1,#0]` at
`0x2001dbc4`-ish, immediately followed by `mov r0,#5` — a **dead store the decompiler dropped
from the visible C entirely**, hiding a genuine register access. This is a real, documented
"dummy read after switching to receive mode" pattern common to many I2C-master IP blocks, not
an Icom firmware bug — but it means the byte the driver actually *keeps* for destination
position 0 comes from `mem_addr + 1`, not `mem_addr`. Confirmed directly: breakpointed the
exact `FUN_2017c81e` (`memcmp`) call site inside `FUN_20029224` and read both compared buffers
live — the delivered bytes came back as `"X3765 V4.81-000\0"` (shifted left one, trailing
garbage) against a byte-perfect `"SX3765 V4.81-000"` written at the nominal offset. Rebuilt the
test image with every signature shifted by one byte (`0x3e01`/`0x3e81`/`0x3fc1` instead of
`0x3e00`/`0x3e80`/`0x3fc0`) — the same live buffer dump now matches byte-for-byte, and
`FUN_20029224` returns `0` (match). Documented in `riic.c`'s own comment as a real, permanent
finding, not fixed away — this device already serves the dummy read correctly (any real DRR
access gets a real byte); the earlier test images were simply built on a wrong assumption
about which offset holds "byte 0" from the driver's perspective. (`tools/build_riic_eeprom_
image.py` now builds this exact corrected image, see README.md's current Status section.)

**With the corrected signatures, `bVar2` genuinely goes false and `bVar8` becomes `1`** — the
first real success on this decision chain. But `0x2002b540`'s own check (`*DAT_2002b524 ==
'\0'`) *still* failed: traced its own gate, `(*(ushort *)(DAT_2002b528 + 4) & 0x40) == 0`,
where `DAT_2002b528` resolves to `0xFCFE3200` — the real GPIO port-pin-read (`PPR`) register
base — and `+4` is `PPR1`, bit `0x40` (bit 6) is **`P1_6`, real signal name `PDV`: the output of
`IC361` (`NJU7704F3`), the board's own power-fail/brownout detector, wired directly into the
CPU** (`notes/ic7300-hardware.md`/`notes/ic7300-signal-chain.md`). **Independently
cross-confirmed against a completely separate, already-existing finding**: `notes/firmware-
update.md`'s own trace of `main_idle_loop`'s watchdog-reset mechanism already established this
exact same bit's polarity from a totally different angle — low means brownout/fault, high
means supply healthy — and both mechanisms need it high for normal operation. `gpio.c`'s
`pin_level[]` defaulted every pin to `0` (a reasonable general default for genuinely unmodeled
inputs), which reads as a *permanent brownout condition* no real boot ever observes. Fixed with
a narrow, well-justified exception: `rza1h_gpio_reset()` now sets `P1_6` high by construction
(a real board's supply is healthy by the time firmware runs at all — this isn't board-specific
data like the EEPROM's content, it's a hardware-guaranteed truth in the absence of an actual
fault this emulator has no way to simulate anyway).

**Net effect: boot progresses dramatically further, past every previously-seen stopping point.**
Confirmed via extended polling (60+ real seconds): the CPU now cycles through real,
previously-unreached `cold_boot_hw_init`-era code (`0x20037xxx` region) instead of parking
forever in the watchdog/`wfi` sequence. The new stopping point is itself a real, clean,
**already-identified** mechanism, not a mystery: `scif3_frontpanel_identify_handshake`
(`0x20037424`-`0x200374e3`, a name from earlier front-panel-thread work, not newly guessed) — a
genuine one-shot SCIF3 handshake with the physical front-panel MCU at cold boot, bounded by a
75-count retry loop. Since `scif.c` (this project's own real SCIF UART model) has "no IRQ line
wired to the GIC yet" and nothing plays the front panel's own role on the RX side, this
handshake can only ever time out. **Unlike the watchdog/`wfi` stop, this one did *not* resolve
on its own after 60 real seconds** — genuinely stuck here, not a graceful one-shot timeout that
was simply slow. `sdcard_file_rpc_dispatch_task`'s own struct fields are still zero at this
point — the actual payoff is closer than it's ever been (past the entire cold-boot branch
gate) but not yet reached. See README.md's current Status section for the concrete next step.

## 2026-09-09: SCIF3 TXI made real (three bugs, two of them a repeated pointer-indirection
## gotcha), then a genuinely deeper blocker found underneath

Picking up exactly where the previous section left off: `scif3_frontpanel_identify_handshake`
stuck in its 75-count retry loop, `scif.c` having no TX-completion IRQ modeled at all. Before
committing to a full virtual front-panel RX responder, the Status section's own flagged
question ("does the timeout path lead anywhere new") got a real answer via live testing.

**First real check — is the retry counter genuinely stuck, or just slow?** Booted with a GDB
stub, used `qemu-machine/tools/gdbrsp.py` (interrupt+read+continue in a loop) to poll the
counter `scif3_frontpanel_identify_handshake`'s own do-while checks (`*pbVar2 < 0x4b` in the
decompile). First attempt read the literal address `0x200375c0` directly and saw a constant
value across 20+ real seconds — looked damning, but this was the **first instance of a real
methodology mistake this session made twice**: `DAT_200375c0` is itself a *pointer variable*
(its stored value is the real counter's address, not the counter itself), exactly the same
shape `DAT_2003758c` and several other globals in this driver already have. Re-read correctly
(dereference first): the real counter, at whatever address the pointer held that boot
(`0x203fc60f` that run), was genuinely `0` and genuinely never advanced across the same 20+
seconds — with real OSTM0-driven scheduler activity visibly happening around it (PC repeatedly
landing in the real IRQ vector/dispatcher region). A cheap forced-write test (`gdbrsp.py`'s
`write_memory`, bumping the counter directly to `0x4b` then `0x0c`) confirmed the *shape* of
the timeout path is real and reachable — PC did move past the function's first loop into a
second one — but that second loop turned out to share the exact same underlying blocker
(`FUN_200374e4`'s busy check unconditionally returns "busy" while status bit `0x02` is set),
so this shortcut didn't avoid the real investigation, just located it precisely.

**Root cause, once traced**: `scif3_driver_pump_tick`'s very first line is
`if ((*DAT_20037588 & 2) != 0) return;` — a busy flag. The only code anywhere in the image that
clears it is inside the ISR `scif3_send_frame` itself explicitly arms via a confirmed generic
`gic_enable_irq(id)` helper (`FUN_200b8308`, confirmed by its own trivial decompile:
`*(GICD_base + (id>>5)*4 + 0x100) = 1<<(id&0x1f)` — exactly `GICD_ISENABLERn` semantics) called
with the literal `0xec` (236). Since `scif.c` had zero IRQ output at all, that ISR could never
run, and the driver became a permanent no-op after its very first send. This is a genuinely
deeper root cause than the Status section's previous framing ("no virtual front panel for the
RX side") — the real first blocker is TX *completion*, not the reply.

**Deriving TXI3's real GIC ID independently, and getting an unplanned cross-check**: `0xec`=236
already told us the ID directly, but to build the fix properly (and to generalize to all 8 SCIF
channels, matching this project's own "wire it once, correctly, for all instances" habit from
`riic.c`), the ID was independently re-derived from `~/Downloads/rza1.svd`'s `ICDISR6`/`ICDISR7`
register fields, using the exact same "register-index×32+bit" formula that already gave OSTM0
its confirmed-correct ID 134 (`ICDISR4`, `OSTM0TINT` at bit 6 → 4×32+6=134). `ICDISR7`'s own
fields give SCIF-n's group as `BRIn=221+4n, ERIn=222+4n, RXIn=223+4n, TXIn=224+4n` — TXI3 lands
on exactly 236, a genuine two-independent-derivations-agree confirmation, not a coincidence
chased backwards from the answer.

**Bug 1 — not wired at all.** Added `qemu_irq irq` to `RZA1HScifState`, `sysbus_init_irq()` in
`rza1h_scif_init()`, and wired each of the 8 channels in `rz_a1h.c`'s SCIF creation loop to
`qdev_get_gpio_in(gic, RZA1H_SCIF_TXI_BASE0 + i*RZA1H_SCIF_TXI_STRIDE - RZA1H_GIC_NUM_INTERNAL)`
(`RZA1H_GIC_NUM_IRQ` raised from 224 to 256 to cover TXI7=252). First version fired the IRQ with
a bare `qemu_irq_pulse()` on every FTDR write, copying `ostm.c`'s pattern.

**Bug 2 — pulse vs. level, the exact bug `riic.c`'s own file comment already documents.**
Live-checked TXI3's real configured trigger mode (`GICD_ICFGR14` bits 8-9 = `0` = level;
confirmed enabled via `ISENABLER7` bit 12 set) before assuming pulse was safe — per this
project's own established discipline (`riic.c`'s comment: "always check a real interrupt's
actual configured trigger mode ... don't just copy the pattern from a working device"). Fixed
first pass: raise on SCR's TIE bit going 0→1 (since this model's FSR always reports
TDFE/TEND=1, enabling TIE alone makes the real hardware condition true immediately), lower when
TIE clears. This did NOT fully fix it — single-stepping through one ISR call showed real
progress (the frame's own send-index advanced, a byte reached FTDR) but free-running for 20+
more seconds showed zero further advancement. Root cause: `qemu_set_irq` treats calling
`raise()` while the line is *already* high as a no-op (no edge, no event forwarded to
`arm_gic`) — fine for a one-shot handshake bit, but the real multi-byte-frame ISR
(`FUN_20036e34`) re-arms the *same* already-high TIE-gated condition on every subsequent byte
without ever clearing TIE in between, so only the very first byte of any frame ever actually
got delivered as a real interrupt. Fixed by switching to explicit `qemu_irq_lower()`+
`qemu_irq_raise()` pairs on every TIE-enabling write (SCR and FTDR both), forcing a genuine
transition every time — the exact fix shape `riic.c`'s own DRT-write cases already established
for the same class of bug.

**Bug 3 — an emulator-only artifact, found by live register-write tracing, not more
single-stepping.** Still stuck after bug 2's fix. GDB single-stepping kept giving a misleading
picture (a lone successful invocation, no clear evidence of anything after) — the tool that
actually resolved this was booting with `-d unimp -D <logfile>` and adding one temporary
`qemu_log_mask` line logging every SCIF3 SCR write, then just reading the resulting real
register-write trace in order:
```
SCR 0->0x20->0x30->0x70->0x78   (RE/TE/RIE/REIE bring-up)
TX fe                            (preamble, direct write in scif3_send_frame)
SCR 0x78->0xf8                   (TIE on)
TX f0                            (type byte, sent via the ISR's first real invocation)
TX fd                            (terminator, sent via the ISR's second real invocation)
SCR 0xf8->0x78                   (TIE off -- the ISR's own terminal cleanup)
```
This trace alone proved the whole 3-byte frame really did go out over a genuinely
interrupt-driven path, both ISR re-entries fired, and the terminal cleanup ran — directly
contradicting what looked like a still-stuck live GDB read moments later (`DAT_20037588 & 2`
still appeared set). That contradiction was the second instance of **the same pointer-
indirection mistake as the counter earlier**: `DAT_20037588` is *also* a pointer variable (its
stored value, e.g. `0x203902d3` that boot, is the real status byte's address), not the byte
itself. Dereferencing it properly showed `0xa0` — busy bit genuinely clear, only the
"waiting for a reply" bit still set, exactly as expected after a real, complete send. The
actual bug behind the earlier appearance of a stall (before this correction) was real too,
just different: the ISR's own FSR-clearing write (`*(ushort*)(FTDR_ptr+4) &= 0xff9f`, real
per-byte bookkeeping, not a deliberate interrupt acknowledgement) was routed through an FSR
write-handler that unconditionally called `qemu_irq_lower()` — undoing the FTDR-write case's
own just-issued raise before the CPU running the same synchronous host call ever had a chance
to sample it. Real hardware would never lose an edge this way (register writes take real time
there); fixed by making the FSR write handler a true no-op again, as it originally was before
this pass introduced the interference.

**End state, this thread**: TXI3 is real, verified end-to-end via the register trace above.
`scif3_driver_pump_tick`'s busy flag correctly clears after a real send. But the *outer* wait
loop (the 75-count retry bound, a *different* counter from the busy flag) still never advances
— confirmed again via 40+ continuous real seconds post-fix. That counter (the same
`DAT_200375c0`-pointed-to address from this section's opening) has zero writers anywhere in the
whole SCIF3 driver per `references_to` — it isn't SCIF-specific at all. Best lead: OSTM0's own
confirmed real per-tick handler, `irq_context_switch_id86` (`0x200059b4`, a genuine FreeRTOS
context-switch sequence, not a minor peripheral ISR), calls three still-undecoded helpers on
every tick — `FUN_200b93f8` (currently just `bx lr`, an empty stub — worth understanding *why*
before dismissing it), `FUN_200b9400`, `FUN_2018849c` — one of which plausibly increments a
real `xTickCount`-equivalent this address reads. Not chased further this session — this is
real FreeRTOS-internals tracing, the exact kind of work this whole thread's payoff question
(SD-card update flow reaching MMCIF against a properly-created task) explicitly hoped to avoid
needing. See README.md's Status section for the concrete resume point.

**Methodology lessons worth keeping, both already-partially-known patterns that bit twice as
hard as expected this session**:
- The pointer-indirection mistake (reading a `DAT_*` global's own address instead of
  dereferencing it first) produced a *plausible, confident-looking* wrong answer both times
  ("stuck forever") rather than an obviously-broken one — it only got caught by independently
  cross-checking against a different signal (a forced-write test moving PC in the first case; a
  live register-write trace in the second). When a `DAT_*` symbol's decompile usage looks like
  `pbVar = DAT_X; *pbVar = ...` rather than `*DAT_X = ...` directly, it's a pointer variable —
  check this before trusting a raw memory read against its literal address.
- Live single-instruction-level GDB stepping and a coarser real-time register-write trace
  (`-d unimp`) each answered a question the other one couldn't: stepping proved the ISR's
  *logic* was correct in isolation (state genuinely advances byte-to-byte), while the trace
  proved what actually happened *in real free-running time order* across multiple separate
  interrupt entries — the trace was what actually resolved bug 3, after stepping alone gave a
  falsely reassuring picture of a single successful call with no visibility into what came next.

## 2026-09-09, continued: the RTOS-tick-counter lead is a red herring; a real front-panel RX
## responder is what actually unblocks the handshake, and boot reaches real task activation

Picking up immediately after the previous section's TXI work, with the concrete next step
being "find what increments the RTOS tick/delay counter the identify handshake's own timeout
polls". A `gdbrsp.py` hardware watchpoint (`Z2`/`z2`, added this session — QEMU's TCG gdbstub
implements these generically, not ARM-specific) settled this fast and conclusively: both the
handshake's outer counter and its own `subcnt` field get **zero writes across 120 continuous
real seconds**, even with confirmed real OSTM0/scheduler activity happening the whole time.
Chasing the tick-counter mechanism further (OSTM0's per-tick handler, `irq_context_switch_id86`,
calls several genuinely empty stub functions — `FUN_200b93f8`, `FUN_200b9400` — that looked like
plausible leads) would have been wasted effort: the counter isn't broken, nothing upstream of it
ever needed to run, because the handshake's *primary* exit path (a real reply from the front
panel, clearing its busy flag directly) was always the real fix needed — the fallback timeout
was never going to matter once that existed. Pivoted to building the virtual front-panel RX
responder the original plan (before the tick-counter detour) had already scoped.

**Protocol groundwork, confirmed live before writing any responder code**: `scif3_frame_
rx_statemachine` (found at `0x20036c68`; the raw disassembly alone left the exact register-
tracking ambiguous, so a real Ghidra function was created at that address and fully decompiled
before trusting any of it) polls real FDR/FRDR/FSR/LSR/SCR registers directly, not a software
ring buffer as an earlier, less careful reading of the raw listing had suggested. `scif3_frame_
dispatch_by_type` needs only a first-content-byte of 0xF0 (or 0xF1) to clear the handshake's
busy flag — no real reply payload needed at all for the identify case specifically.

**First responder design, and why it didn't survive contact with real testing**: feed a canned
3-byte 0xFE/0xF0/0xFD reply through the exact same `frdr`/`rx_pending` path a real chardev byte
would use, one byte at a time, each driving its own real RXI — mirroring how a real front panel
would actually behave. This required getting *exactly* one byte visible per real interrupt, and
every mechanism tried failed the same way, confirmed via live single-step traces each time:
- Synchronous delivery (make the next byte available immediately, within the same FRDR-read
  handler that consumed the previous one) let the guest's own drain loop (`while (FDR & 0x1f)
  { byte = FRDR; }`, keeping only the *last* value read) consume all 3 queued bytes in one pass
  before the per-byte frame-assembly logic below the loop ever got to look at any of them
  individually -- only the trailing 0xFD's value was ever examined, with no 0xFE ever seen to
  mark a frame in progress, so it was silently dropped as an incomplete frame.
- Switching to a `QEMUBH` (`qemu_bh_schedule`) did not fix this. A live register-write trace
  (`-d unimp -D <logfile>`, the tool that ended up resolving every stage of this investigation)
  showed the next byte still becoming visible *before* the guest's own next FDR poll -- a
  scheduled BH can still run interleaved within the same guest loop that scheduled it, at least
  in this project's single-threaded TCG configuration.
- A real `QEMUTimer` (even armed for just 1ns) looked like it had fixed the interleaving, based
  on the same kind of register-write trace showing clean alternation. But a careful, fully fresh
  single-step trace of an actual live exchange (breakpoint at the state machine's entry, then
  single-stepping the *first* real invocation from a clean `-S` halt) showed all 3 bytes already
  consumed by the time that very first RXI was serviced -- this build's TCG event loop processes
  pending timers more eagerly than "the guest's own next handful of instructions" reliably
  survives, and no timer delay short of a real, much-larger-than-instruction-count gap closes
  that window. Concluded this class of fix isn't reliably achievable in this environment and
  stopped trying to out-race the drain loop.

**Second, final design — precompute the end state instead of racing to deliver it**: rather
than feeding bytes through frdr fast enough, directly write the *result* a real byte-at-a-time
exchange would have produced straight into guest RAM (via `address_space_write()`/`address_
space_read()`, reading the firmware's own live pointer *values* first, never a hardcoded
address -- the frame buffer, descriptor, and status fields are all reached through pointer
*variables* at fixed literal-pool addresses, the exact same shape as the pointer-indirection
gotcha the previous TXI session hit twice), then deliver *only* the genuinely-necessary
terminator byte (0xFD) through the completely normal FRDR/RXI path. With the frame buffer's
type byte, the descriptor's byte count, and the status "frame in progress" bit all already set
as if 0xFE and 0xF0 had each been processed by a real earlier call, `scif3_frame_rx_
statemachine`'s own real logic sees byte-count >= 2 and the frame-in-progress bit already
clear on this one real call, and genuinely calls `scif3_frame_dispatch_by_type` -- no timing
race of any kind, since there's only ever one byte in flight. Also fixed, same pass: RXI is
level-triggered exactly like TXI, and nothing was ever lowering it once a byte was consumed
(the raise-only-to-re-arm pattern elsewhere never needed an explicit lower before) -- caused a
genuine interrupt storm (this same ISR re-entering continuously with FDR reading 0 and nothing
to do) until a `qemu_irq_lower()` was added to the FRDR-read handler.

**First real payoff, live-confirmed**: the identify handshake's busy flag (bit 0x80) cleared
for the first time ever. But immediately surfaced a second bug of the same general shape: the
real ACK sets status bits 0x60, and bit 0x40 of that same byte is exactly what `scif3_driver_
pump_tick` reads as "send a 0xF1 keepalive ping" -- with no real serial timing anywhere in this
model to pace it, ACKing the resulting 0xF1 frame the same way (the first responder version
ACKed both 0xF0 and 0xF1) re-armed the same bit right back, producing a genuine, unbounded
fe/f1/fd ping-pong (confirmed: tens of thousands of frames logged in the first dozen real
seconds). Fixed by simply never ACKing type 0xF1 at all -- real hardware's own ping is
presumably paced by something this project hasn't needed to find yet (a real timer, or a front
panel that just doesn't reply instantly); not ACKing it sidesteps modeling that pacing entirely,
since `pump_tick` clears its own ping-request bit before sending and nothing else ever sets it
again once the responder stops re-arming it.

**A second, distinct layer surfaced immediately after, confirmed live**: with the identify
handshake's first wait loop now resolving for real, boot reached the handshake's own *second*
wait loop for the first time -- a genuinely different blocking condition (status bit 0x04, set
only by an ordinary type-0x00-0x1F frame's own successful dispatch), read directly off that
loop's own disassembly rather than assumed. Confirmed via a real, previously-never-transmitted
33-byte outbound status/data frame (type 0x00) appearing in the trace log for the first time in
this whole project's history. Extended the responder to ACK any outbound type 0x00-0x1F the
same general way (a 1-byte dummy-payload echo, matching just enough of `scif3_frame_dispatch_
by_type`'s own length-validity check to be accepted) -- resolved cleanly, no new loop, and a
second, different frame type (0x01, also new) went out and got ACKed right after.

**The actual payoff**: boot then reached `cold_boot_hw_init`'s own `itron_act_tsk` call --
a real ITRON RTOS task activation, confirmed via a full decompile of `cold_boot_hw_init` and a
direct listing cross-check of the live PC. This is the single biggest milestone this whole
`qemu-machine/` thread has existed to reach, and it fell out directly from getting the SCIF3
handshake genuinely right rather than working around it. See README.md's Status section for
the concrete new blocker found immediately past this point (a task-readiness counter,
different from and unrelated to the SCIF3 one this section opened with, that doesn't advance
yet either) and the next resume point.

**Methodology notes worth carrying forward, both earned the hard way this session**:
- GDB single-stepping an isolated interrupt entry proved a genuinely *misleading* signal more
  than once here -- it can show one invocation's logic working correctly in complete isolation
  while hiding a real problem in what happens immediately afterward (an interleaved next
  delivery, an interrupt storm). A live, full-sequence `-d unimp` register-write trace was what
  actually resolved every one of this session's real bugs; when the two disagree, trust the
  full trace, not the isolated step.
- A live hardware watchpoint (`Z2`/`z2`, generic in QEMU's TCG gdbstub) is a fast, conclusive
  way to answer "does anything write here at all" for a dynamically-allocated RAM target that
  static `references_to` analysis can't usefully search (no fixed literal address to look for).
  Reach for it before spending real effort tracing a counter's *hypothetical* writer.

## The SLV5 hypothesis retired, the real readiness-counter writer traced end to end: MTU2
## channel 3's TGI3A (GIC ID 154) — confirmed armed by the guest, confirmed never fired

Picking up the previous section's "Active resume point" directly: before building anything
against the `0xE8100000` (SLV5) hypothesis, checked it live first, per this project's own
established discipline of testing a hypothesis before spending build effort on it.

**SLV5 hypothesis retired, empirically.** Booted the existing `flash.bin`/`riic2_eeprom.img`
combination with `-d unimp,guest_errors -D /tmp/unimp_trace.log` for 22 continuous real
seconds (no GDB, just a free-running boot) and grepped the full trace for any access to the
`io-e8100000`/`io-e8000000`/`io-e8030000`/`io-e8200000` unimplemented-device regions
(`rz_a1h.c`'s own names for the SLV5-area catch-alls): **zero hits, across the whole window**.
Only `spi-status-and-neighbors`, `io-fcfe0000`, `rza1h-scif3`, and `gic_dist_writeb` ever
appear. `thunk_FUN_2007ea68`/`slv5_periph_connect_disconnect_handler` is simply never reached
this early in boot — it cannot be today's blocker, whatever else it turns out to be later.
Recorded here so a future session doesn't re-open this lead without new evidence.

**Traced the readiness counter's real writer instead, via Ghidra's `references_to` on the
counter's own dereferenced RAM address (`0x2039076c`, confirmed still the same fixed value
this session — it comes from a literal-pool constant baked into the static image at
`DAT_2002b4ec`, not a genuinely per-boot-random address as the previous session's phrasing
cautiously assumed; worth re-verifying live each time regardless, since nothing rules out a
future build changing it).** `references_to 0x2039076c` returns exactly the two RAM-struct
touch points inside `cold_boot_hw_init` already known (reset to 0 at `0x2002b048`, the
busy-wait read at `0x2002b04c`/`LAB_2002b04c`, and the post-loop reset to 0 at `0x2002b0e8`)
**plus a completely separate pair**: a read at `0x200b7a7c` and a write at `0x200b7aa0`,
both inside `FUN_200b7910` — a generic multi-rate software-timer tick handler (increments a
base-rate pair of 16-bit fields unconditionally every call, then cascades to slower-rate
fields and countdown timers every 2/4/8/200 calls via bitmasking `bVar3`, a classic ITRON-style
shared system-tick housekeeping routine). This is the real writer.

**`FUN_200b7910` is only ever called from one place**: `FUN_20005b98`, which calls it every
other invocation (`(bVar1+1 & 1) == 0`) alongside `ssif0_bring_up_and_pump`/
`ssif1_bring_up_and_pump` and a `+8000` accumulator bump. `FUN_20005b98` itself has exactly one
static reference anywhere in the image: as the `handler_ptr` argument to
`register_event_handler(0x9a, FUN_20005b98)` inside `FUN_20005c08`. `register_event_handler`
is the same confirmed-generic id-indexed dispatch-table primitive documented in the
`register_event_handler` decompile's own comment (49+ call sites project-wide, `event_id` is a
plain array index bounds-checked against `*DAT_200b9684`) — and `FUN_20005c08` immediately
follows registration with `FUN_200b83d0(0x9a,0x10)` (priority), `FUN_200b8244(0x9a,1)`
(trigger-mode-shaped), and `FUN_200b8308(0x9a)`, which is the exact same
`*(GICD_base + (id>>5)*4 + 0x100) = 1<<(id&0x1f)` `GICD_ISENABLERn`-shaped helper the SCIF3 TXI
IRQ investigation already confirmed (`README-history.md`'s "SCIF3 TXI made real" section) — so
`event_id` here is a **plain absolute GIC ID**, not an ITRON-abstract event number: **GIC ID
0x9a = 154**.

**Identified GIC ID 154 independently, via the same SVD-derived "register-index×32+bit"
formula already cross-checked twice** (OSTM0=134 from `ICDISR4`/`OSTM0TINT` bit 6, SCIF3
TXI3/RXI3=236/235 from `ICDISR7`'s `TXIn=224+4n`/`RXIn=223+4n` fields):
`ICDISR4` (register index 4, covering absolute IDs 128-159) has bit 26 named **`TGI3A`** —
`4*32+26 = 154`, an exact match. **`TGI3A` is MTU2 (Multi-Function Timer Pulse Unit 2) channel
3's Timer-General-Interrupt-A, the compare-match-A interrupt** — confirmed against
`~/Downloads/rza1.svd`'s own `MTU2` peripheral block (base `0xFCFF0000`) and its channel-3
register offsets (`TCR_3`=0x200, `TMDR_3`=0x202, `TIORH_3`/`TIORL_3`=0x204/0x205,
`TIER_3`=0x208, `TCNT_3`=0x210, `TGRA_3`=0x218 — all 16-bit-spaced, matching the real MTU2
extended-channel layout).

**Found `FUN_20005c08`'s own MTU2 channel-3 setup, via `references_to` on those exact SVD
offset addresses** (`0xFCFF0200`/`0xFCFF0218` etc., not the earlier, misleading single hit on
the bare peripheral base `0xFCFF0000` alone — that one turned out to belong to a *different*,
post-loop reconfiguration of MTU2 **channel 0**, `FUN_200b5b64`, called only after
`cold_boot_hw_init`'s busy-wait already exits, and is not this counter's blocker): `iVar1 =
DAT_20005e1c` (the MTU2 base) with `TCR_3=0` (prescaler Pφ/1, undivided, no TCR-driven
auto-clear), `TMDR_3=0`, `TIORH_3=TIORL_3=0`, `TCNT_3` reset to 0, **`TGRA_3=8000`** (the real
compare-match period), and `TIER_3`'s enable bit set via `FUN_20360adc(iVar1+0x208,1,0)` —
a genuine, real periodic hardware-timer configuration, not a guess. `FUN_20005c08` itself has
exactly two static callers project-wide: inside `select_active_slot_resources` (`0x20062c64`,
called from `0x2003bb6c`) and inside the CI-V-retraction/firmware-update retry loop
(`README-history.md`'s earlier session, `notes/`'s own Fup_AutoEnd tracing) — the former reads
like an ordinary per-boot "pick the active configuration slot" step, not something exclusive to
firmware-update mode, consistent with what live testing confirmed next.

**Live-confirmed, not just statically inferred**: booted with `-S`, let it free-run 20 real
seconds, then read `GICD_ISENABLER4` (`0xE8201110`) and `GICD_ISPENDR4` (`0xE8201210`)
directly over `gdbrsp.py` while confirming the CPU was genuinely parked at the busy-wait
(`PC=0x2002b04c`, exactly `LAB_2002b04c`) with the counter still reading `0`:

```
PC after 20s: 0x2002b04c
readiness counter *0x2039076c (low byte): 0
GICD_ISENABLER4: 0x4000040  bit26 (ID 154) set: True   (bit 6 = ID 134 = OSTM0, also set)
GICD_ISPENDR4:   0x80       bit26 (ID 154) set: False
```

**This is the real, now fully pinpointed blocker, not a hypothesis**: the guest itself has
already armed GIC ID 154 exactly as the trace above predicts (`ISENABLER4` bit 26 = 1) by the
time it reaches the busy-wait — `select_active_slot_resources` (or whatever the real normal-
boot caller turns out to be) does run before `cold_boot_hw_init`'s task-readiness wait, so the
registration path is not in question. But `ISPENDR4` bit 26 staying `0` across the whole 20
seconds means MTU2 has never once asserted this line — expected, since `rz_a1h.c` currently
maps the whole MTU2 region via `add_plain_ram_region()` (plain storage, zero behavior, per the
Confirmed-peripherals table) — there is no real counter running behind it at all, so a real
compare-match event can never occur.

**Next step, not yet built**: a real `src/mtu2.c`, mirroring `ostm.c`'s already-proven
real-QEMU-timer + real-IRQ pattern, for at minimum MTU2 channel 3 — a free-running 16-bit
`TCNT_3` counted against a real `QEMUTimer`, comparing against `TGRA_3` (`8000`, though don't
hardcode it — read the guest's own configured value the same way `ostm.c` reads `OSTM0.CMP`),
raising GIC ID 154 on compare-match, respecting `TIER_3`'s enable bit the way `ostm.c` already
gates on its own control register, and — per this project's own hard-earned discipline from
the SCIF3 TXI bugs — checking the interrupt's real configured trigger mode
(`GICD_ICFGR` bits for ID 154) and re-arm semantics live before assuming a bare
`qemu_irq_pulse()`/`qemu_set_irq()` pattern is safe, rather than copying another device's
pattern on faith. Once wired, retest with the same `gdbrsp.py` poll used here: watch
`*0x2039076c` actually climb past `0x32` and `cold_boot_hw_init` fall through past the
busy-wait into `FUN_200b5b64`/`FUN_200b5be0`/`FUN_200b5ea4`/`FUN_200b5f38` and beyond. See
`README.md`'s Status section for the current resume point.

## `src/mtu2.c` built and confirmed load-bearing; boot progresses dramatically further; the
## very next blocker already identified as DMAC channel 0 (GIC ID 41)

Picking up the previous section's "Next step" directly, checked the exact trigger mode live
first (this project's own established discipline) rather than assuming a pattern from
`ostm.c`: booted with `-S`, read `GICD_ICFGR9` (`0xE8201C24`, the register covering IDs
144-159 at 2 bits/ID — ID 154's bits are 20-21) and got back `0b00` = **level**, not edge.
Also read MTU2 channel 3's live register state directly at this point to cross-check the
static decompile rather than trust it blindly: `TCR_3=0x00`, `TIER_3=0x0d` (bit 0/`TGIEA` set,
the one this device gates on — bits 2/3 also set, unrelated), `TGRA_3=0x1f40` (`8000`, an
exact match for the decompile), `TSTR=0xc1` (bit 6/`CST3` set — channel 3 genuinely running).
`TSTR`'s real bit layout also confirmed against the SVD directly (channels 3/4 use bits 6/7,
not 3/4 — a real, documented MTU2 numbering quirk, not assumed from the channel number).

**Built `src/mtu2.c`** mirroring `ostm.c`'s real-`ptimer`-plus-real-IRQ pattern, scoped to
channel 3 only (every other register/channel stays a plain byte-array passthrough, matching
what `add_plain_ram_region()` already did for the whole module) — level IRQ held while
`TSR_3`'s `TGFA` bit is set and `TIER_3`'s `TGIEA` is enabled, write-0-to-clear semantics on
`TSR_3` (a naive plain store would let software "set" `TGFA` by writing 1, wedging the level
line permanently high the first time `TGIEA` is enabled — the real MTU2 protocol is the
opposite), and the channel's own `TSTR` `CST3` bit gating whether the backing `ptimer` runs at
all — handling either write ordering (`TSTR` before or after `TCR_3`/`TGRA_3` configuration)
since the exact order wasn't traced. Wired into `rz_a1h.c` in place of the old
`add_plain_ram_region()` call, IRQ connected to GIC ID 154 the same
`qdev_get_gpio_in(gic, id - RZA1H_GIC_NUM_INTERNAL)` way every other device in this file
already does. Compiled clean on the first attempt.

**Live-tested immediately, same `gdbrsp.py` poll used to find the blocker — confirmed
load-bearing, not just "builds and doesn't crash":**

```
t~2s:  PC=0x200051ec
t~4s:  PC=0x200b5f28
...
counter samples across a 20s free-run: 0xd8 -> 0x8e -> 0x63 -> 0x1f (wrapping repeatedly)
```

The readiness counter, static at `0` on every single prior boot (this session's own earlier
evidence and the previous session's watchpoint both agree on that), now climbs continuously.
PC moves through a wide, changing spread of addresses (`0x20005xxx`, `0x200b5xxx`,
`0x2018xxx`) rather than sitting at one address — real, active execution, confirmed by
sampling 15 times over 30 real seconds and finding 4 distinct PCs, not 1. This is the biggest
confirmed forward-progress jump since `itron_act_tsk` itself.

**The very next blocker, already identified via the identical method, not yet built against:**
of the 15 samples above, 12 land on `0x200b5f28` — inside `FUN_200b5ea4`, the third of the
four calls `cold_boot_hw_init` makes right after the now-resolved busy-wait
(`FUN_200b5b64(); FUN_200b5be0(); FUN_200b5ea4(); FUN_200b5f38();`). Its own decompile shows
a nested busy-wait on two flag bytes (`*(char*)(puVar1-0x12)` then `*(char*)(puVar1-0x13)`,
`puVar1 = DAT_200b6318 = 0x20390700` in this build — the same general `0x2039xxxx` state-block
neighborhood as the just-resolved counter, though a different, unrelated field). `references_to`
on both flag addresses (`0x203906ed`/`0x203906ee`) traces the writer to `FUN_200b5be0` — the
call immediately *before* `FUN_200b5ea4` — which itself calls
`register_event_handler(0x29, FUN_200b5b90)` followed by the same generic
`FUN_200b8308(0x29)` GIC-enable helper already confirmed `GICD_ISENABLERn`-shaped: **GIC ID
41 (`0x29`)**. Identified via the same "`ICDISRn` register-index×32+bit" SVD formula yet
again (register index 1, bit 9): **`DMAINT0`, the RZ/A1H's DMA controller channel 0
completion interrupt** — a peripheral this project has never modeled in any form (not in the
Confirmed-peripherals table at all, currently whatever generic unimplemented-device
catch-all its address range happens to fall under).

**Not yet live-confirmed the way MTU2 was** — no GIC register read has been done yet to check
whether ID 41 is actually armed/pending by the time this new busy-wait is reached, unlike the
`ISENABLER4`/`ISPENDR4` read that turned the MTU2 lead from a hypothesis into a certainty
before any code was written. That check is the natural next step, per this session's own
now-twice-proven discipline (test live before building, the same discipline that retired the
SLV5 lead earlier in this same session). See `README.md`'s Status section for the current
resume point.

## `src/dmac.c` built and confirmed load-bearing too; boot reaches `dsp_boot_handshake` --
## a whole further stage (SCIF5 DSP-link bring-up), the furthest ever

Picked up the previous section's "not yet live-confirmed" directly. Booted with `-S`, let it
run to the same parked state, and read the GIC registers for ID 41 the same way MTU2's ID 154
were read: `GICD_ISENABLER1` (`0xE8201104`) bit 9 = **1** (armed by the guest), `GICD_ISPENDR1`
(`0xE8201204`) bit 9 = **0** (never fired) — the identical armed-but-never-fired shape MTU2
had, confirming this is a real, live blocker and not a dead end. `GICD_ICFGR2` (`0xE8201C08`,
covering IDs 32-47, 2 bits/ID — ID 41's bits are 18-19) reads `0b10` = **edge**, not level —
the opposite of MTU2's `TGI3A`, so this device gets `ostm.c`'s simpler `qemu_irq_pulse()`
pattern instead of `mtu2.c`'s raise-and-hold one.

**Traced the exact call reaching this point, disassembly-first (not decompile-first, after
`references_to` on the two flag bytes led there) to get real byte-offset math right**:
`cold_boot_hw_init` calls `FUN_200b5b64(); FUN_200b5be0(); FUN_200b5ea4(); FUN_200b5f38();` in
that order (function *addresses* aren't in call order — `FUN_200b5be0` sits at a lower address
than parts of `FUN_200b5ea4`'s own helper chain, which briefly looked like it might belong to
`FUN_200b5be0` until the real function boundaries were checked directly against a fresh
disassembly listing, the same "watch out for Ghidra's decompile-by-address landing outside
the queried range" caution from earlier in this file). `FUN_200b5be0` configures DMAC channel
0's `CHCFG_0` (`0x00222160`)/`CHITVL_0`/`CHEXT_0`/`DCTRL_0_7`, sets `CHCTRL_0 |= 0x62`
(channel enable), registers `FUN_200b5b90` as GIC ID 41's ISR, then clears both flag bytes
(`0x203906ed`/`0x203906ee`) to 0 right before a tail-branch out.

`FUN_200b5ea4` (the busy-wait's own containing function) actually has **two** separate
busy-waits on the same flag (`0x203906ee`), not one — the first (nested with the second flag,
`0x203906ed`) is already satisfied immediately (both cleared to 0 by `FUN_200b5be0` moments
earlier), so it falls straight through to `FUN_200b5cdc()` (builds what looks like a
scatter-gather descriptor list — a loop over up to 12 entries, each an appended
`(flag-derived-word, size)` pair, via a shared "append one descriptor" helper,
`FUN_200b5c60`) and then `FUN_200b5dc0()`, which is the real DMA-arming function: a
cache-clean loop (`mcr p15,0,r0,cr7,cr10,1` + `dmb sy` over a buffer range — real cache
maintenance ahead of a DMA transfer, consistent with L2C already being modeled for real in
this project) followed by the actual `N0SA_0`/`N0DA_0`/`N0TB_0` writes (source = a RAM buffer
address read from a fixed literal, `0x20415340` in this build; dest = a GPIO-region address,
`0xFCFE3108`, inside `RZA1H_GPIO_BASE`'s own claimed range — a real DMA-out-through-a-GPIO-port
pattern, plausible for driving a parallel bus like the front-panel LCD without per-byte CPU
involvement, though not confirmed further this session) and finally sets `0x203906ee = 1`,
arming the **second** busy-wait — the one this session's live GIC read above was actually
parked at.

**The ISR itself, `FUN_200b5b90`, was also fully disassembled**: touches MTU2 *channel 0*'s
`TSTR`/`TSR` bits (an unrelated cross-subsystem side effect, real per the disassembly, not
interpreted further), re-applies the same `CHCTRL_0 |= 0x62` `FUN_200b5be0` used, then clears
`0x203906ee` back to 0 and tail-branches to a shared ISR epilogue (`0x200b0f68`) — confirming
this ISR is the one and only thing that can ever resolve the busy-wait, exactly the same shape
as MTU2's tick handler.

**Built `src/dmac.c`** for DMAC channel 0 only (every other channel/register stays a plain
byte-array passthrough, same scoping choice as `mtu2.c`): performs the real transfer via
`address_space_read()`/`address_space_write()` (the same real-pointer-values approach
`scif.c`'s virtual front-panel responder already established) as soon as `N0TB_0` — the last
of the three "arm" registers, confirmed via the live disassembly order above — is written,
firing the edge IRQ from a short one-shot `QEMUTimer` rather than synchronously (real DMA is
asynchronous). Wired into `rz_a1h.c` the same overlap-mapped way every other device inside the
`io-e8200000` unimplemented-device catch-all already is. Compiled clean on the first attempt,
same as `mtu2.c`.

**Retested with multiple free-running trials, not just one** — this project's own established
discipline (`tools/trial_irq.py`'s whole reason for existing: "a single boot snapshot isn't a
reliable regression test once real interrupt-driven scheduling is involved," per this file's
very first section). A first single 30-second sample showed the DMAINT0 busy-wait's own PC
(`0x200b5f28`) no longer dominant — but three more independent 20-second trials right after
showed real run-to-run variance (one trial's PC samples were still mostly `0x200b5f28`,
another had zero occurrences of it at all) that could have looked alarming taken in isolation.
A longer, single 50-second trial with every sample printed (not just a `Counter` summary)
settled it: `0x200b5f28` never appeared even once in that run, and PC spent the whole window
oscillating between `dsp_boot_handshake` (see below) and the periodic MTU2/DMAC/OSTM ISR
cluster (`0x20005xxx`) — the variance across trials is genuinely just *how long it takes to
first reach and arm the DMA transfer* (dependent on how many periodic ticks other code needs
first), not whether the fix resolves it once armed. Confirmed load-bearing, not a fluke.

**Boot now reaches `dsp_boot_handshake`** — a name already established in a prior session
(not freshly discovered this session), confirming this really is new, further territory:
`cold_boot_hw_init`'s own later lines (already visible in this file's very first `cold_boot_
hw_init` decompile, further down than anything reached before now) call
`scif5_dsp_link_driver_init(); scif5_wait_hsk1_ready(); dsp_boot_handshake();` — part of the
SCIF5 DSP-link bring-up, never reached by any traced boot path before this session.

**The next blocker, only lightly traced (deliberately not chased further this session):**
`dsp_boot_handshake` calls `scif5_bitrev_transmit_word()` (bit-reverses a 32-bit word and
transmits it -- its own register-offset pattern, `iVar4 + -0x2e0` written with values like
`0x10000`/`0xa0000000`, looks more like an RSPI-shaped control sequence than a plain SCIF one,
not reconciled with its own name this session) then busy-waits on a status byte
(`0x203906ba` in this build) only that same function's tail can plausibly clear. It ends by
enabling **GIC ID 0x9f (159)** via the same generic `FUN_200b8308` helper used for every other
interrupt in this file -- not yet identified against the SVD, not yet live-checked the way
DMAC/MTU2 were. This is a different subsystem from DMAC/MTU2 (a real DSP-link handshake
protocol, per this project's own signal-chain notes) and, per the front-panel handshake's own
precedent (`scif.c`'s `rza1h_scif3_frontpanel_ack`), may need a genuine virtual responder
rather than just a timer-backed device -- judged worth its own dedicated investigation rather
than a quick continuation of this one. See `README.md`'s Status section for the current
resume point.

## MTU2 channel 4 (`TGI4A` and `TGI4C`) built; a real early-fire bug found and fixed; the next
## stall turns out to be a different, deeper class of problem -- a generic RTOS job-queue
## overflow, not a missing peripheral

Picked up the previous section's GIC 0x9f lead directly. Identified it the same way as every
other interrupt in this project: `0x9f` = 159 = register index 4, bit 31 in the "`ICDISRn`
register-index*32+bit" SVD formula -- **`TGI4A`**, MTU2 channel 4's own compare-match-A event.
`scif5_bitrev_transmit_word`'s own `iVar4`-relative writes (the ones that looked RSPI-shaped)
turned out to be an unrelated GPIO-area side effect (base `0xFCFE3400`, confirmed via its own
literal) -- the real timing mechanism this function arms is genuinely MTU2:
`*(short*)(TCR_3_base+0x1c) = *(short*)(TCR_3_base+0x12) + 0x200` resolves (once the byte math
is done carefully, the same "don't trust decompile pointer arithmetic over raw byte offsets"
discipline from earlier this session) to `TGRA_4 = TCNT_4 + 0x200` -- a one-shot relative-delay
arm, not a periodic tick.

**`scif5_dsp_link_driver_init`'s own pre-existing file comment** (written 2026-08-29, a prior
session, well before any of today's work) already documented the wider mechanism this taps
into, and it was right: a ring buffer drained via channel 4's compare-match-**C** event too
(GIC ID 161, confirmed via the same formula: register index 5, bit 1 -- `TGI4C`,
`scif5_ring_pop_and_send`), with compare-match-A (159) as `scif5_ring_underrun_handler`'s own
"ring now empty" signal. Extended `mtu2.c` to cover channel 4's TGI4A, generalizing the
existing channel-3 event-tracking code into a small per-event struct (register offsets, which
TIER/TSR bit, whether it arms on the `TSTR` transition or only on an explicit compare-register
write) rather than hardcoding channel-shaped logic a second time.

**A real bug, found and fixed before declaring this working -- not caught by static reasoning,
only by live testing**: the first TGI4A implementation mirrored channel 3's arming policy
exactly (auto-rearm whenever `TSTR`'s matching `CSTn` bit transitions 0->1, using whatever the
compare register already holds). Multiple free-running trials showed inconsistent results
(sometimes `dsp_boot_handshake`'s busy-wait cleared quickly, sometimes it stayed stuck for the
whole trial) -- read live GIC state at one such stuck point and found something that shouldn't
be possible if the fix were simply "not yet reached": `GICD_ISPENDR4` bit 31 (TGI4A) was
already **set** while the CPU was still parked at the much-earlier DMAINT0 busy-wait, and
`TSR_4`/`TIER_4` already showed real MTU2 activity. The only explanation: `CST4` (channel 4's
own `TSTR` start bit) goes high very early in boot -- alongside `CST3`, in the same write --
long before `scif5_bitrev_transmit_word` ever runs to give `TGRA_4` a real value. Auto-arming
on that early transition fired TGI4A with whatever stale (zero) `TGRA_4` the reset state held,
asserting the level line and leaving `TSR_4` spuriously set far too early -- a genuine,
confirmed-live bug, not a hypothesis. **Fixed by making TGI4A arm only on an explicit
`TGRA_4`/`TCR_4` write while already running**, never on the `TSTR` transition -- matching
what the real trigger (`scif5_bitrev_transmit_word`/`scif5_ring_underrun_handler`, both issue
exactly such a write) actually is. Kept `TGI4C` on the transition-based arming policy, since
live testing confirmed the opposite for it: `TGRC_4` stays at its reset value (`0x0000`)
through the entire traced path -- nothing ever writes it before `TSTR` starts the channel, so
a real periodic drain tick can only be coming from the free-running-wraparound case, the same
one channel 3's own `TGI3A` already relies on.

**Retested with multiple trials after the fix -- `dsp_boot_handshake` resolves reliably now,
no more of the early-fire inconsistency.** Boot progresses into `dsp_cmd_table_init` (already
named/known from an earlier, 2026-08-29 session) and its own call to `dsp_param_sync_tick()`,
which pushes roughly 23 parameter words onto the SCIF5 ring meant to be drained by TGI4C.

**Built TGI4C too** (channel 4's compare-match-C event, same file, same generalized per-event
struct, periodic/transition-arming like channel 3) -- compiled clean, wired to GIC 161.
**This alone did not fully resolve the next stall.** Multiple post-fix trials still reached a
stable, reproducible parked PC (`0x200b93fc`, an unconditional `do {} while(true)` -- a real
overflow-protection halt, not a hardware wait) within 10-25 real seconds, exactly as before
TGI4C existed.

**Traced the real cause precisely instead of guessing further** -- captured the exact register
state at the halt (polling until `PC == 0x200b93fc`, then reading `r0`/`LR` immediately, no
separate breakpoint needed since a `Z0` breakpoint's own overhead shifted timing enough to
miss the window across several attempts -- a real, minor addition to this session's
timing-sensitivity lessons): `r0 == 2`, `LR` matching the call site inside `FUN_20187bb4`, a
generic fixed-capacity ring-push helper used by (per its 4 call sites, all inside a tight
`0x20186c**` address range that also contains the confirmed-real ITRON wait primitive
`FUN_20186de4` from an earlier session's `test_fup_scheduling.py`) what looks like a **generic
software-timer-expiry dispatch table**, not anything DSP-specific. Traced the ring's own base
address (`0x20420120`) and its "wake the consumer" call (`FUN_20187b8c`) all the way down:
it writes `0x10000` to `*(some_base + 0xf00)`, where `some_base` (`DAT_20187bac`) resolves to
**a plain RAM address** (`0x20336024`), not any peripheral register -- confirmed via direct
listing, not assumed. Whatever is supposed to notice this flag and drain the ring is a pure
software/RTOS construct, not something waiting on any interrupt this project could model.

**This is a genuinely different class of problem than everything else this session fixed**,
and is judged worth its own dedicated investigation rather than a further quick continuation:
it points directly at this project's own long-standing open fallback hypothesis (first raised
earlier the same day, before any of today's MTU2/DMAC peripheral work) -- whether this
emulator's context-switch mechanism genuinely handles multiple concurrently-scheduled tasks
correctly. Every milestone before today was still effectively single-tasked; today's real
peripheral work is what finally unlocked enough concurrent activity (DSP link, parameter sync,
whatever else posts to this same generic dispatch table) to actually reach and expose this
question directly, rather than it staying purely theoretical. The natural next step: trace
which task is meant to drain this specific queue and directly test (the same way this
project already tested `sys_monitor_task_entry`'s own context-switch path) whether it's ever
actually dispatched. See `README.md`'s Status section for the current resume point.

## The scheduler hypothesis was wrong -- corrected by testing it, not assumed: the real
## overflow cause was this session's own MTU2 tick rate, fixed by slowing it down

Picked up the previous section's own suggested next step directly -- but rather than diving
straight into "trace which task drains this queue" (a large, open-ended investigation), first
tried the cheapest thing that could disprove the scheduler hypothesis outright: a breakpoint
on the ring-push call itself (`0x20187bb4`), logging every single hit's caller (`LR`) and
arguments, not just the final overflow. A genuine "consumer never runs" scheduling bug would
show either a single slow trickle of pushes from one starved producer, or an eventual burst
once *something* finally got a chance to run; a rate mismatch would show a perfectly healthy,
steady producer outrunning its consumer regardless.

**Result, across 1080+ hits over 89 real seconds: exactly one caller, never once different**
(`LR=0x20186c67`, `r0=0x20415c60`, `r1=1` on literally every hit) -- a single software timer,
inside `FUN_20186c4c` (the timer-ID-indexed dispatch helper this project's own earlier session
had already partly traced), expiring and re-arming on a steady, regular real-world cadence.
Not a burst, not a scattered set of different producers -- one clean, periodic, always-
identical signal. **More tellingly: with the breakpoint active, the overflow never happened
at all across the full 89-second run** -- a stark contrast to every unbreakpointed run, which
reliably hit it within 10-26 real seconds. A real context-switch/scheduling defect has no
reason to care about a debugger pausing execution on one specific, otherwise-healthy producer
call; a rate mismatch between that producer and its consumer would disappear exactly this way
once anything -- a breakpoint's own overhead included -- slows the producer down relative to
the consumer's own real pace.

**This directly retracts this file's own previous section's framing.** The "generic RTOS
job-queue overflow... points at this project's long-standing open question about concurrent
task scheduling" conclusion was a reasonable hypothesis given the project's history, but
wrong -- and only caught because it got tested live instead of accepted and escalated into a
much larger scheduler investigation. Worth being explicit about this rather than quietly
revising the record: a real check (arguably one call cheaper than the deep static tracing
that preceded it) settled the question in minutes.

**Traced the real cause to this session's own earlier work.** `FUN_20186c4c`'s single steady
producer is, per this project's own established understanding, exactly the kind of software-
timer-expiry event `FUN_200b7910` (MTU2 channel 3's periodic housekeeping tick, this session's
very first fix) drives via its own cascading /2/4/8/200 countdown-timer bookkeeping. Channel
3's `MTU2_FREQ_HZ` (500 MHz -- copied verbatim from `ostm.c`'s own "fast for testing, not
real-clock-accurate" constant, chosen purely for wall-clock testing speed, not correctness)
was very likely driving that cascade -- and therefore this exact software-timer-expiry
producer -- faster than any real RZ/A1H's own MTU2 clock ever would, filling a small,
fixed-16-slot queue faster than whatever real-hardware-paced consumer normally drains it.

**Confirmed by direct experiment, not left as plausible reasoning**: lowered `MTU2_FREQ_HZ`
from 500 MHz to 25 MHz (a deliberately conservative slowdown, not derived from any documented
real RZ/A1H clock value -- picked purely to test the hypothesis cheaply) and reran the same
free-running trial pattern used throughout this session. **Six independent trials (four 40-
second runs, two 90-second runs -- 340 real seconds of cumulative boot time) produced zero
recurrences** of the overflow, where the 500 MHz build reliably hit it within 10-26 real
seconds on every prior run this session. Boot now progresses further than any prior state
this session reached, into `scif5_send_and_wait_reply` (already named from an earlier
session's DSP-comms tracing) -- a real, synchronous DSP command/reply round-trip, several
genuine stages past where the overflow used to occur.

**A generalizable lesson for this project going forward, not just a one-off fix**: any device
this project adds with a "fast for testing, not real-clock-accurate" frequency constant
(`ostm.c`'s `OSTM_FREQ_HZ`, now `mtu2.c`'s `MTU2_FREQ_HZ`) is a latent candidate for exactly
this class of bug -- correct in isolation, but capable of producing real, reproducible
downstream symptoms in *other*, unrelated firmware code that happens to share the same
virtual clock and carries implicit real-time-relative assumptions (a fixed-capacity queue
sized for a real hardware rate, in this case). A future blocker that "looks like" a
scheduling or concurrency defect is worth checking against this cheaply first (does slowing
down a recently-added fast peripheral clock change the symptom?) before escalating into a
much larger scheduler-correctness investigation. See `README.md`'s Status section for the
current resume point.
