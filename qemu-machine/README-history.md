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

## Status as of the SCIF5 DSP-comms session, 2026-09-09 (superseded by README.md's current
## Status -- kept here verbatim for the narrative trail)

## Status, 2026-09-09 — the RTOS job-queue overflow is resolved, and the previous resume
## point's own hypothesis (a genuine scheduler/context-switch bug) turned out to be wrong:
## live testing traced it to this project's own MTU2 tick rate being too fast, fixed by
## slowing it down; boot now reaches a real synchronous DSP command/reply exchange

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

**The previous resume point's own hypothesis was wrong — corrected by further live testing,
not assumed.** The generic ring-push overflow (`FUN_20187bb4`/`FUN_200b93fc`) looked, from a
first pass, like it pointed at a genuine emulator scheduling defect (the "wake the consumer"
call writes a plain RAM flag, not a peripheral register — see README-history.md's previous
section). Testing that directly instead of accepting it: breakpointed the ring-push call
itself and captured **every single hit's caller** across a full run, not just the final
overflow. Result: one single, always-identical producer (same `LR`, same arguments) firing at
a steady real-world rate — never a multi-source burst, never a different caller. More
tellingly, **adding the breakpoint itself (which briefly pauses the guest on every hit) made
the overflow stop happening entirely** across a full 90-second run. A real scheduling defect
would not care about that kind of pause; a **rate mismatch between one specific producer and
a small fixed-capacity queue** would disappear exactly like this once something slows the
producer down.

That pointed straight at this session's own `mtu2.c`: its `MTU2_FREQ_HZ` (500 MHz, the same
"fast for testing" constant `ostm.c` uses, chosen for wall-clock testing convenience, not
correctness) was very likely driving the underlying software-timer-expiry scan that feeds
this exact producer faster than a real chip's own MTU2 clock ever would, filling the queue
faster than its real-hardware-paced consumer could keep up. **Confirmed by direct experiment,
not just plausible reasoning**: lowered `MTU2_FREQ_HZ` to 25 MHz and reran — 6 independent
trials (four 40s runs, two 90s runs, 340 real seconds of boot time total) with **zero
recurrences** of the overflow, where every prior run (with the 500 MHz constant) reliably hit
it within 10-26 real seconds. Boot now progresses further than ever, reaching
`scif5_send_and_wait_reply`'s own busy-wait — a genuine, already-named (from an earlier
session) synchronous DSP command/reply round-trip, several real stages past the overflow
point.

**Worth carrying forward as its own methodology lesson**: this project's own first read of
the overflow (previous resume point) reached for "this must be the long-standing open
scheduler question" — a real, legitimate hypothesis given the project's history, but wrong
here, and only correctable by testing it directly (the breakpoint-hit-tracing above) rather
than building on it. The actual, much more mundane cause was this session's own earlier
choice of an unrealistically fast peripheral clock having a real downstream effect on
unrelated firmware code that happened to share the same virtual clock. Every device this
project adds with a "fast for testing, not real-clock-accurate" frequency constant
(`ostm.c`'s `OSTM_FREQ_HZ`, now `mtu2.c`'s `MTU2_FREQ_HZ`) is a candidate for this same class
of issue if a future blocker looks scheduling-shaped -- worth checking before escalating to a
"is the scheduler broken" investigation.

**Active resume point:** `scif5_send_and_wait_reply` (already named/known from an earlier
session — the DSP's real synchronous command/reply API, 14 call sites project-wide) busy-waits
on a "reply-ready" flag (`*(DAT_200b1c84+3)`) after arming a retry timer
(`scif5_arm_retry_timer`). Not yet traced this session — the natural next step is the same
playbook used throughout today: find what's supposed to clear that flag (a real SCIF5 RX
event, per this function's own already-written comment) and confirm it live before building
anything.

## SCIF5 reply-ready deadlock resolved: a virtual DSP responder, and a real ordering-race bug
## found and fixed getting there (2026-09-09, continuing straight on from the resume point above)

Picked up the resume point directly: traced `scif5_send_and_wait_reply`'s busy-wait
(`*(DAT_200b1c84+3)`, called the "reply-ready flag" below) live before building anything, per
this session's own established discipline.

**Live-traced the real clearer, and caught a real decompiler artifact doing it.**
`scif5_arm_retry_timer`'s own decompile-derived comment already named `scif5_rx_isr` as the
clearer, but that function's own decompile (from an earlier session, 2026-08-29) doesn't show
any write to offset `+3` at all -- comparing it line-by-line against the raw disassembly
listing found why: a genuine dead-store-elision artifact silently dropped `strb r0,[r4,#0x3]`
(the actual clear) from the pseudocode entirely, because the decompiler's own points-to
analysis didn't realize `r4` (`DAT_200b2b34 - 0x13`) and `DAT_200b1c84`'s own value
(`0x203906b8`) are the *same* struct, just accessed via two different base+offset
combinations 0x13 apart (confirmed: `DAT_200b2b34`'s static value is exactly
`struct_base + 0x13`). With that resolved, the real struct layout is: `+0x2`/`+0x3` two
separate busy flags, `+0xc` the RX byte count, `+0x13` the 4-byte raw RX buffer, `+0x18` the
decoded reply word. `scif5_rx_isr` clears `+0x3` only after collecting 4 real bytes over
SCIF5's RX path and `rbit`-reversing them into `+0x18` -- with no virtual DSP ever replying,
this can never happen. Live-confirmed the deadlock directly too: a 90-second free-run poll
(GIC state, both busy flags, and the `+3` flag itself, all read every 5 seconds) showed the
flag going to `1` around 35 seconds in and then never once clearing again -- genuinely,
permanently stuck, not slow.

**First design (TX-triggered) worked once, then failed the very next trial -- a real ordering
race, not a fluke.** Mirroring the channel-3 front-panel responder's own proven shape: track
outbound `FTDR` writes on channel 5 (no framing needed, every SCIF5 command is a bare 4-byte
`rbit`-reversed word), and once 4 bytes have gone out, synthesize a reply using the same
precompute-3-bytes-then-deliver-1-real-byte-through-FRDR/RXI technique the channel-3
responder established (worked out the exact bytes by hand: raw word `0x00000004`, `rbit`'d by
`scif5_rx_isr`'s own real code into `0x20000000` -- top nibble 2, `scif5_classify_reply`'s
"trivial ack" class, chosen as a universal reply since some callers separately check for a
more specific class and this only affects their own retry/error handling, never this
busy-wait). First live trial: the flag never stuck once in 90 seconds -- looked solved.
**Second, independent trial: stuck again, in the exact same place, within 35 seconds** --
this project's own established multi-trial discipline catching a real bug a single success
would have missed. Root cause, found by reading the raw instruction order rather than
re-guessing: the reply-ready flag isn't actually set busy (`=1`) until `scif5_arm_retry_timer`
runs -- *after* the caller's own TX already returned -- so a synchronous TX-triggered ack can
land *before* that `=1` write and get silently overwritten by it, with nothing left to clear
it again for that exchange. This project has hit this exact class of problem before (the
channel-3 responder's own file comment) and already knows a delay-based fix doesn't reliably
work in this single-threaded TCG build (a `QEMUTimer` runs "far more eagerly than the guest's
own next few instructions guarantees").

**Real fix: hook a write that's provably sequenced after the busy-flag set, by real
instruction order, not by timing.** `scif5_arm_retry_timer`'s very next steps after setting
the flag are a real 5-write arm sequence to a genuine, previously-unmodeled hardware register
at `0xFCFE3120` -- reached two independent ways in the firmware (`DAT_200b1c98+0x120` and,
from the *TX helper's own separate* arm sequence, `DAT_200b1c8c-0x2e0` -- same physical
address, a real confirmation this is one genuine register). Its last write is always one of
two large sentinel constants (`0x10000000`/`0x40000000`), cleanly distinct from every other
write that lands there (the small `<=0x10000` values every sequence's earlier writes use, and
the TX helper's own differently-sentineled `0xa0000000` completion). Implemented as a second,
small MMIO region (4 bytes, exactly at `0xFCFE3120`) mapped only on the channel-5 SCIF
instance -- `sysbus_init_mmio` called a second time in `rza1h_scif_init` (channel isn't known
that early, so all 8 instances get the region created; `rz_a1h.c` only maps it for `i==5`) --
whose write handler fires the same ack helper only on those two sentinel values. Since this
write is the literal next instruction after the busy-flag set, in the same function, this is
race-free by construction: no scheduling assumption needed, just real, confirmed instruction
order.

**Confirmed load-bearing across 2 independent 90-second free-run trials with the fixed
design**: the reply-ready flag never stuck once in either trial (every prior design, TX-
triggered included, stuck permanently within 35 seconds on its failing runs). Both trials
progressed to the *same* new frontier, not a random spread of addresses -- a strong signal
this is a real, reproducible stage boundary rather than noise: `scif5_cmd_transmit_now`'s own
busy-wait (traced via `LR`, since PC alone kept landing inside a tiny, ubiquitous
load-mask-shift register-field-read helper called from many places) on the shared ring-active
flag (`DAT_200b1cac`) -- the same flag `scif5_send_and_wait_reply` itself also waits on at its
own entry, and the one `shared_job_ring_dispatch`'s case-1 resolution clears only once the
background ring (case 1/2/3/4 job queue, the same one `dsp_param_sync_tick`'s ~23-word
parameter stream and the earlier-diagnosed job-queue-overflow both involve) reaches genuinely
empty (write pointer == read pointer). Not yet traced this session whether/why that ring
isn't reaching empty -- the natural next step, same playbook as always: confirm live (is the
ring's write pointer still advancing faster than its read pointer, and if so from what
producer) before building anything.

## SCIF5 ring-drain and RSPI2 blockers found and fixed; boot reaches previously-unanalyzed code
## for the first time (2026-09-09, continuing straight on from the SCIF5-responder session)

Picked up the resume point directly: `scif5_cmd_transmit_now`'s busy-wait on the shared
ring-active flag (`DAT_200b1cac`), not yet traced.

**Live-traced the real gate, and it wasn't the ring-active flag at all.** Reading
`scif5_cmd_transmit_now`'s decompile more carefully: it has *two* gates, checked in order --
`*pcVar1` (`DAT_200b1cac`, ring-active) only second. The *first*, `*pcVar2 != 0` (`DAT_200b1cb0`
's target), guards a real hardware-register poll (`FUN_20360b24` on `DAT_200b1cb4`'s target,
bit 2 of `0xFCFF0305`) -- and a live 90-second `set_watchpoint` on that exact address (this
project's own established technique for "is this genuinely never written, not just rarely")
confirmed zero writes across the whole run. Traced the arm/clear pair by reading raw
disassembly around the two other write sites `references_to` found (Ghidra's decompile again
represented the arming call, `FUN_200b7ae0`, as if inlined into `scif5_cmd_transmit_now` with
no visible call, rather than showing it as a separate function -- confirmed by direct listing,
not assumed): `scif5_cmd_transmit_now` itself arms this compare (a real, deliberate 640-tick
rate-limiter on direct SCIF5 sends) as its own last step after every successful transmit,
computing a relative target from a *widely-referenced* free-running counter (`0xFCFF0306` --
read from many unrelated subsystems, not just SCIF5, strongly suggesting a real, shared system
timer) and clearing the status bit to await a genuine hardware compare-match that this
project's plain-storage MTU2 passthrough can never produce.

**Fixed in `mtu2.c`** (the same 1KB-window device already covering channels 3/4, confirmed via
`RZA1H_MTU2_SIZE`/`0x400` that `0xFCFF0305`/`0xFCFF030c` genuinely fall inside its own mapped
region): a fourth compare-match event, modeled as a host-wall-clock deadline (sidesteps 16-bit
counter wraparound entirely) rather than a live counter -- the same "fixed one-shot period, not
the guest's real relative delay" simplification already established for TGI4A. Purely polled,
no IRQ needed (none was found registered anywhere near it).

**Confirmed load-bearing live**: `scif5_cmd_transmit_now`'s own busy-wait resolved. Boot
progressed to a **third, distinct blocker** the very same free-run trial: `shared_job_ring_
dispatch`'s case 3 (a real, previously-untriggered job-type-3 "RSPI2 transmit" ring entry, only
reachable once the ring could drain far enough) calls `rspi2_transmit` directly, whose own
internal busy-wait (`SPSR2` bit 6, TX-ready) blocked next -- `rspi2_transmit` itself was already
fully confirmed and documented by an earlier session (2026-08-29: `SPCR2`=`0xE800D800`,
`SPSR2`=`0xE800D803`, `SPDR2`=`0xE800D804`, `SPCMD2`=`0xE800D820`), just never built, the exact
same "real, previously-unmodeled peripheral" shape every blocker this session has had.

**Built `rspi2.c`**, minimal (same permissive philosophy as `mmc.c`'s virtual SD card and
`scif.c`'s TX-always-logged FTDR): SPSR2's TX-ready bit always reads set, SPDR2 writes are
logged only, no real transaction timing or the completion IRQ (GIC `0xa2`/162) modeled --
`shared_job_ring_dispatch`'s own case 3 doesn't wait for that completion event either, so
unblocking `rspi2_transmit`'s internal poll is everything this stage needs. Wired into
`rz_a1h.c` (overlap-mapped inside the existing "io-e8000000" catch-all, no IRQ connected) and
the build patch/setup.sh's symlink list.

**Confirmed load-bearing across 2 independent trials**: both reached genuinely new ground --
`0x200600a8`-area code with **no existing Ghidra function symbol at all**, the first time this
project has traced execution into previously entirely unanalyzed firmware. A quick decompile of
its containing block (`0x2005ff1c`) shows DMA-descriptor-shaped setup (three chained `0x240`-
byte transfers, `FUN_20360b0c`-style DMAC channel kicks) -- very plausibly graphics/display DMA,
consistent with `itron_act_tsk`'s already-identified target this whole thread reached earlier
this session (`ui_graphics_lifecycle_task`, which creates the real EGL window/pixmap surfaces).
Not yet traced further this session -- genuinely new territory, appropriately left for a fresh
investigation rather than rushed. See `README.md`'s Status section for the concrete resume
point.

## Status as of the MTU2/RSPI2 session (superseded by README.md's current Status -- kept here
## verbatim for the narrative trail)

## Status, 2026-09-09 — two more real, previously-unmodeled peripherals found and fixed
## (a rate-limiter compare-match in MTU2, and RSPI channel 2); boot reaches previously
## entirely unanalyzed firmware for the first time -- likely graphics/display DMA setup

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

**Confirmed, solid, this session — continuing straight on from the SCIF5-responder work above:
a virtual SCIF5 DSP responder resolved `scif5_send_and_wait_reply`'s reply-ready deadlock (full
derivation in `scif.c`'s own file comment and README-history.md), and two further real,
previously-unmodeled peripherals were found and fixed getting past the next two blockers:**
- **`scif5_cmd_transmit_now`'s own busy-wait turned out to gate on a different flag than the
  prior resume point assumed** — not the shared ring-active flag, but a real hardware
  compare-match (`0xFCFF0305` bit 2) that a live 90-second `set_watchpoint` confirmed is never
  written at all. Fixed in `mtu2.c` (a fourth compare event, modeled as a host-wall-clock
  deadline rather than a live counter — sidesteps 16-bit wraparound entirely).
- **Boot then reached a real, previously-untriggered RSPI2 (job-type-3) ring entry** —
  `rspi2_transmit`'s own busy-wait (`SPSR2` bit 6), a function already fully documented by an
  earlier session (2026-08-29) but never built. Fixed with a new, minimal `rspi2.c` (TX-ready
  always set, TX bytes logged only — same permissive philosophy as `mmc.c`'s virtual SD card).
- **Confirmed load-bearing across 2 independent trials**: both reached genuinely new ground —
  `0x200600a8`-area code with **no existing Ghidra function symbol at all**, the first time
  this project has traced execution into previously entirely unanalyzed firmware. A quick look
  at its containing block shows DMA-descriptor-shaped setup (three chained transfers), very
  plausibly graphics/display DMA — consistent with `itron_act_tsk`'s already-identified target
  (`ui_graphics_lifecycle_task`, which creates the real EGL surfaces).

**Active resume point:** a busy-wait in the newly-reached, still-unnamed `0x200600a8`-area code
— not yet traced this session. Same playbook as always: decompile/name the containing function,
confirm live what the loop is actually waiting on, before building anything.

## A fifth MTU2 compare event found and fixed (same shared status byte, a different bit); boot
## reaches a real, pre-existing generic ring-overflow trap in genuinely new territory
## (2026-09-09, continuing straight on)

Picked up the resume point directly: decompiled/read the raw listing of the still-unnamed
`0x2006003c`-area function (the one containing `0x200600a8`) in full, rather than just the one
block already seen.

**The three "near-identical" wait blocks aren't identical -- one has an inverted branch
condition, a real difference easy to miss skimming.** Blocks 1 and 3 skip their own retry-poll
when a status halfword's bit `0x200` is *clear* (`beq`); block 2 -- the one execution actually
reaches -- does the opposite (`bne`), entering the retry-poll precisely when that bit is
*clear*. Traced block 2's own retry target (`FUN_20360b24` on a literal-pool address) to
`0xFCFF0305` bit 0 -- the *same* status byte `mtu2.c`'s fourth event (bit 2) already covers,
paired with a *different* compare-target register (`0xFCFF0308`, not `0x30c`). Confirmed live
(free-run poll, this project's own established technique) that bit 0 genuinely never sets on
its own, the same shape every blocker this session has had.

Traced the arming side too (`references_to` found exactly 2 call sites, both via a shared
helper): both pass the identical literal period `0x7d00`/32000 -- unlike the fourth event, this
one's real requested period is fully known, so it's honored exactly at this device's own
already-established `MTU2_FREQ_HZ` (32000/25MHz = 1.28ms) instead of approximated. Confirms
this is a genuinely shared, multi-subsystem software-timeout facility (one counter, several
independent compare/status channels) -- this caller isn't SCIF5/DSP-comms related at all
(reached via the DMA-descriptor-setup routine from the previous section, very plausibly
graphics/display).

**Built and confirmed live.** Across 2 independent trials, boot now progresses well past this
point into previously never-reached code, and hits a **real, pre-existing generic
overflow-protection trap** (`0x200b93fc`, an unconditional infinite loop taking an error code
in `r0` -- already documented by an earlier session as backing a *different* ring's own
overflow protection, `FUN_20187bb4`/`FUN_201877e4`, "used well beyond just this one ring").

**Applied this session's own established diagnostic technique** (breakpoint the generic
ring-push helper itself, capture every hit's caller and ring pointer across a real run) before
assuming anything: found one single, extremely steady producer (LR `0x20186c67`, ~12 pushes/sec
across 1092 hits in 90 breakpoint-slowed seconds, zero overflows) feeding a *different* ring
(`0x20415c60`) than the one that actually overflowed in an un-breakpointed trial (`0x20420120`)
-- the breakpoint overhead itself likely delays reaching whatever triggers the second ring's own
overflow, matching this project's own prior finding that pausing execution on every hit changes
timing enough to mask a real race. A follow-up free-run poll of `0x20420120`'s own header bytes
(read/write index, a capacity of 16, and what looks like a third, separate "pending" counter)
showed values cycling in the 5-15 range between 5-second samples, always caught with read==write
-- consistent with a fast burst-then-drain producer/consumer pair that only occasionally loses
the race, not a steady rate mismatch like the earlier-diagnosed overflow. `FUN_20186c4c` (the
caller of both the observed producer and, presumably, whatever feeds `0x20420120`) turned out to
be a generic "broadcast one event to N registered subscriber rings" dispatcher, not something
ring-specific -- genuinely new, not-yet-mapped infrastructure.

**Active resume point:** root cause not yet found for the `0x20420120` ring's own overflow --
same playbook as always, but this one needs a more targeted trace (breakpoint filtered to this
specific ring pointer, or a tighter polling interval around the observed ~40-second mark) rather
than the broad producer-trace already tried, which caught the wrong ring's traffic. See
README.md's Status section for the concrete next step.

## The `0x20420120` ring's own struct/producer/consumer fully derived; overflow reproduced twice
## via a new low-perturbation tracing technique with exact numbers; the DMAC-cascade hypothesis
## it pointed at tested live and came back negative (2026-09-09, a fresh session picking up the
## resume point above)

Picked up the resume point directly: static re-derivation first (Ghidra), live confirmation
before building anything, per this thread's own established discipline throughout.

**Full struct derivation, this time from the actual producer/consumer code rather than just
polled bytes.** `0x20420120` is a single global work-queue struct: byte 0 = write index
(producer-owned), byte 1 = read index (consumer-owned, written back only once per drain call,
not per entry), byte 2 = pending count (LDREX/STREX-atomic, incremented by the producer /
decremented by the consumer once per drained entry), byte 3 = capacity (16, matching the prior
session's own live-polled observation exactly), then 16 * 8-byte entries (4-byte job-object
pointer + 4-byte payload word) starting at offset 4 -- total struct size `0x84`, which lines up
exactly with a second, sibling table (a per-channel job-object pointer array) sitting
immediately adjacent at `0x204201a4`, a real structural confirmation, not a coincidence.
- **Producer**: `FUN_20187bb4`, called from exactly one site, `FUN_20186c4c`
  (`0x20186c62`, return address `0x20186c66`/LR `0x20186c67`) -- **this is the *exact* LR the
  prior session's own push-helper breakpoint already caught.** That session's own conclusion
  ("feeding a *different* ring, `0x20415c60`") was a misread, corrected this session: the value
  it captured is r0/param_1, an opaque per-channel job-object pointer *stored as data* in the
  queue entry, not a destination ring pointer -- `FUN_20187bb4` has no ring-pointer parameter at
  all, it always targets this one hardcoded queue. So the earlier session's breakpointed producer
  was *already* the right one; the "wrong ring" belief, not the breakpoint's own target, was the
  error.
- **Consumer**: `FUN_20187ae4`, drains while pending != 0, called from **`irq_context_switch_id0`**
  (`0x20005960`) -- an already-existing, already-named, already-resolved (2026-08-30,
  `notes/kernel-rtos.md`) real GIC-ID-0 (an SGI, architecturally always software-generated, never
  a hardware peripheral line) context-switch handler, structurally identical to `swi_handler`'s
  own scheduler logic. So the queue drains *only* on this specific context-switch event, not on
  a dedicated per-push doorbell or a fixed periodic tick of its own.
- **Overflow trap**: `FUN_200b93fc` (the pre-existing, already-documented unconditional infinite
  loop), called directly from inside `FUN_20187bb4` with `r0=2` when the pending-count check
  fails -- confirmed live, see below.
- A separate, unrelated per-object ring shape (`FUN_201877e4`, called via a completely different
  path, `FUN_20187ae4`'s own type-1 dispatch case) was found and fully read while tracing this,
  then ruled out as unrelated to `0x20420120` -- noted here only so a future session doesn't
  re-investigate it as a lead; it uses `r0=3`, not the `2` the overflow trap actually reported.

**Root-caused a real self-inflicted tooling bug before any of the above could be tested live**:
the very first attempt at a new trace script used `pkill -f qemu-system-arm` to clean up a
previous QEMU instance before launching a fresh one -- and it killed its own parent shell
instead, immediately and silently (`pkill -f` matches full command lines by default, and the
shell invoking that exact command line contains the string `qemu-system-arm` as a substring of
*itself*, from the pattern argument). Every symptom matched a hard, early kill with no output at
all, across several failed attempts, before the actual cause was traced (rather than guessed) by
noticing the one successful run was the one invocation that happened to omit the `pkill` prefix.
This is the *exact* footgun `tools/trial_irq.py`'s own file comment already named and worked
around (`subprocess.Popen` process management instead of shell backgrounding) -- worth a harder
flag for any *new* script written against this project: never `pkill -f` a pattern that appears
in the invoking command line itself, and prefer targeted PID-based cleanup (or none, since a
fresh QEMU listens on a fresh `-gdb tcp::1234` and a stale one just fails to bind) over a broad
`pkill -f`.

**New technique, built to avoid the exact masking this thread already hit twice**: a live
per-call breakpoint (on the push helper, or a data watchpoint on the ring's own header bytes)
fires at the same frequency as the thing under investigation and reliably prevented the overflow
from manifesting at all in the prior session's own 90-second, 1092-hit trial. `tools/
trace_job_ring_overflow.py` (new) instead free-runs the whole boot and only pauses on a fixed
**wall-clock** cadence (`interrupt()`/read/`cont()`, every 0.25s), independent of any specific
guest code path -- a uniform stall that (unlike a targeted breakpoint) slows the producer and
consumer by the same proportion rather than desynchronizing them. A single breakpoint on the
overflow trap itself is included too, since it only ever fires once, at the moment of genuine
interest, and stops everything anyway.

**Confirmed live, twice, with real numbers -- this reproduces under gentle wall-clock polling,
unlike every breakpoint-based attempt before it:**
- Trial 1 (100s run): `t=51.47s` pending=1 (write_idx=15, read_idx=14, nearly caught up) →
  `t=51.84s` **OVERFLOW** (`r0=0x2`, `lr=0x20187c29` -- confirmed to be the return address
  right after `FUN_20187bb4`'s own internal call to `FUN_200b93fc(2)`, exactly matching the
  static derivation above), header at the moment of the trap: write_idx=11, read_idx=11,
  pending=16 -- a structurally *consistent* full-buffer state (write index having wrapped
  exactly back around to equal read index with every slot occupied), not obviously corrupted
  index arithmetic.
- Trial 2 (60s run, coarse-then-fine polling: 0.5s until 47s, then 0.02s -- the fine window
  never actually got exercised, because the real event happened earlier): `t=42.940s`
  pending=14 (write_idx=4, read_idx=6, already wrapped) → `t=43.56s` **OVERFLOW**, same `r0=0x2`,
  same `lr=0x20187c29`.
- **Both trials show the identical shape**: the queue sits at 0 (or very close to it) for tens
  of seconds of real boot time, then goes from near-empty to completely full and overflowing
  within well under half a second -- a genuine sudden **burst**, not a slow steady leak like the
  earlier MTU2-clock-rate overflow this same session already fixed. This confirms and sharpens
  the prior session's own "burst-then-drain, not steady" read of the coarse 5-second polling
  data into hard numbers.
- Both trials also independently caught the CPU at the identical `pc=0x20005258 lr=0x20005248`
  right in this critical window -- a genuine, never-function-bounded code region (the classic
  "empty function list doesn't mean no code" gap this project has hit several times before) that
  reads structurally like a generic vectored-IRQ dispatch stub (an indirect `blx r2` through a
  per-ID handler-pointer table, in the same unbounded gap between `reset_handler` and
  `kernel_start` that also houses the other low-level exception trampolines) -- i.e., an
  interrupt was actively being dispatched at both polled moments, though not yet pinned to a
  specific ID.

**Tested the most concrete hypothesis this pointed at, live -- came back negative, a real,
useful result, not just an unswept gap.** The newly-reached DMA-descriptor-setup region
(`FUN_2005ff1c`, the "three chained 0x240-byte transfers" a prior session had characterized as
"very plausibly graphics/display DMA, DMAC channel kicks") was the obvious first suspect: if
each of those three transfers completes near-instantly and each fan-out-notifies several waiter
tasks via the kernel's own event-flag mechanism (traced below), that alone could plausibly
produce a same-instant burst in the teens. Added temporary `fprintf`-based host-side
instrumentation to `dmac.c`'s own completion path (`git checkout`-reverted immediately after,
zero trace left in the tracked source) and reran: **DMAC channel 0 fired exactly once in a full
60-second run** -- and it was the *already-known*, already-documented early-boot transfer
(`dst=0xfcfe3108`, matching `dmac.c`'s own file comment's "a real RAM buffer -> a GPIO-region
destination address" verbatim), not the graphics-region one at all.
**Followed up by actually reading `FUN_2005ff1c` itself properly** (the prior session's own
"DMAC channel kicks" characterization was never independently re-derived until now): it doesn't
touch real DMAC MMIO (`0xE8200000`-range) anywhere. It only populates a *software* descriptor
table (three 0x240-byte source/dest/size records) and calls `FUN_20360b0c` -- decompiled and
confirmed to be a **generic single-bit bitfield setter** (`*addr = (*addr & ~mask) | (value <<
shift)`), not a DMA-kick function at all. **This retracts the prior session's own "DMAC channel
kicks" characterization** -- whatever real hardware mechanism (if any) eventually acts on these
three software descriptors is still unidentified, and it demonstrably isn't `dmac.c`'s modeled
channel 0. The DMAC-cascade hypothesis is closed as tested and ruled out, not abandoned
unswept.

**Active resume point:** the real producer of the burst is still unidentified. Two concrete
leads, neither yet tried: (1) trace what actually *consumes* `FUN_2005ff1c`'s three
"descriptor ready" bit-sets -- that downstream consumer, not DMAC hardware, is now the leading
suspect for triggering a chain of kernel event-flag signals (`FUN_2007e3cc`, confirmed this
session to be ITRON's own `iset_flg`-shaped primitive -- set/clear up to 4 waiter tasks per call,
reached only indirectly via a syscall dispatch table, so a fan-out search from here needs the
*runtime* waiter list, not a static one); (2) pin down which GIC ID the recurring
`pc=0x20005258` vectored-dispatch stub was actually servicing at the two captured moments -- a
few back-to-back ultra-fine (`~10ms`) wall-clock polls bracketing a fresh overflow, comparing
successive LR/PC pairs, would show directly whether it's one ID retriggering rapidly or several
different ones firing in a tight cluster, without needing a risky direct GIC IAR read from
outside the CPU's own execution. `tools/trace_job_ring_overflow.py`'s coarse/fine two-phase
polling (`[seconds] [poll_interval] [fine_start] [fine_interval]`) already supports narrowing in
once a fresh trial's own approximate overflow time is known from a first coarse pass.

## Followed the CPSID/CPSIE lead live -- a real, useful correction after a second trial
## contradicted the first (2026-09-09, same session, immediately following)

Read `FUN_2005ff1c`'s actual caller context properly (it had never been done -- the prior
session's "graphics/display DMA" label was itself a guess, never verified): its two callers,
`FUN_200605e4` and `FUN_200605fc`, both tail-jump into a shared body at `0x2006003c` containing
the ~20 MTU2-rate-limiter-gated retry loops already known from the fifth-compare-event work
earlier this session -- and **that whole body is bracketed by a real `cpsid i` (`0x20060040`)
and `cpsie i` (`0x200604bc`)**, i.e. IRQs (including GIC ID 0, the job-ring's only drain
trigger) are architecturally incapable of firing for its entire span. `FUN_200605fc` itself is
called from exactly one site: **`cold_boot_hw_init`** (`0x2002b0b8`) -- so this isn't graphics
DMA at all, it's a call embedded directly in the same master boot-init function this whole
project thread has centered on all along; the "graphics/display" label is retracted alongside
the earlier "DMAC channel kicks" one.

Built `tools/trace_irq_mask_window.py`: three low-frequency breakpoints (the `cpsid`/`cpsie`
addresses, plus the overflow trap), each hit only pausing long enough to log a timestamp and
step past it -- deliberately avoiding the per-push-frequency masking problem the ring's own
producer/consumer already demonstrated twice.

**Trial 1 (first run) looked like a clean confirmation**: `CPSID` at t=34.267s (pending=0) →
a second, back-to-back `CPSID` at t=34.514s (pending=0, no intervening `CPSIE` -- a genuine,
reproducible oddity, see below) → `CPSIE` at t=34.764s (pending=0, masked window measured at
exactly 0.2500s) → **overflow at t=35.310s, only 0.55s later**. Read in isolation, this is a
compelling story: nothing could push during the mask, `pending` was still 0 right as IRQs came
back, then a burst followed almost immediately after.

**Trial 2, run to get a second, independent data point (this project's own established
discipline -- a single snapshot isn't a reliable regression test once real interrupt-driven
scheduling is involved), directly contradicted that reading.** The identical `CPSID`/`CPSID`/
`CPSIE`/`CPSIE` pattern occurred again, at almost exactly the same wall-clock offset (t=34.42s,
0.246s spacing this time -- matching trial 1's 0.247s closely enough that this specific
double-event is clearly a real, deterministic-ish part of normal boot, not noise; its own cause
is still unexplained and not worth chasing further right now). This time a fourth breakpoint,
armed reactively right at the `CPSIE` hit, watched `irq_context_switch_id0`'s own entry
(`0x20005960`, the ring's only drain trigger) to directly measure how often the consumer
actually runs afterward. **It fired 142 times over the next ~30 seconds** (roughly once every
210ms) before the overflow finally happened at t=65.078s -- thirty seconds after the `cpsie`,
not 0.55s. **This flatly contradicts trial 1's apparent correlation**: the consumer was running
reliably and often the whole time, so total starvation-by-masking isn't the general
explanation, and this specific `cpsid`/`cpsie` window isn't a reliable trigger either -- trial
1's close timing was most likely coincidental, not causal. Retracting that reading here rather
than letting it stand uncorrected.

**What this second trial actually establishes, and it's a real sharpening of the question, not
a dead end**: since the drain trigger fires reliably (~every 200ms) and *still* an overflow
eventually happens, the real question isn't "why doesn't the consumer run" (it does) -- it's
"what makes upwards of 14-16 entries land inside a single one of those ~200ms gaps, at some
unpredictable point roughly 30-65+ seconds into this boot phase." `FUN_20187ae4` (the consumer)
has no per-call drain cap -- its own loop runs `while (pending != 0)`, so every one of those 142
calls, if it ran with anything queued, would have fully emptied the ring -- reinforcing that
this is genuinely a single-gap burst-exceeds-capacity event, consistent with (and now better
explaining) the very first coarse-polling trials' own numbers (near-empty to full within well
under half a second).

**Active resume point, corrected and sharpened**: the real burst producer is still
unidentified, and the `cpsid`/`cpsie` lead is downgraded from "likely cause" to "an interesting,
reproducible boot-sequence detail, not yet shown to be causal." The two leads from the previous
section are still the live ones (what consumes `FUN_2005ff1c`'s "descriptor ready" bit-sets;
whether other GIC IDs besides 0 are also active in the same window) -- now reframed around a
sharper question: given the consumer demonstrably runs every ~200ms, what could make ~16
independent things happen inside one such gap. `tools/trace_irq_mask_window.py` (the id0-hit
counting logic in particular) is directly reusable for testing whichever new hypothesis comes
next -- arm the id0-entry watch and correlate its hit-count/spacing against the ring's own
`pending` byte read at the same moments, rather than assuming a specific fixed code location is
the trigger.

## Two more angles opened in parallel, same session: a datasheet-grounded clock-realism finding,
## and a reactive producer-capture tracer (2026-09-09, continuing straight on)

**Real OSTM clock frequency finally tracked down (was previously undocumented anywhere in this
project) -- worth acting on, but not a quick fix.** `ostm.c`'s `OSTM_FREQ_HZ` (500MHz) has
always been an explicitly-flagged "fast for testing" placeholder, same as `mtu2.c`'s original
value before that got tuned. The real RZ/A1H hardware manual (`REN_r01uh0403ej0600_rz_a1h_MAT_
20210129-2931443.pdf`, Section 11.3.2 + Table 6.3) documents OSTM's real count clock as `P0φ`,
25.00-33.33MHz depending on clock mode -- a real, citable number, not a guess, and confirms
`mtu2.c`'s own already-tuned 25MHz sits in the *same* real clock domain (MTU2's count clock is
also `P0φ`, just via a divider), though `mtu2.c`'s own comment is honest that 25MHz was chosen
empirically to fix a bug, not derived from this manual section.
**Important nuance, reasoned through before acting on this (not yet tested live)**: naively
lowering `OSTM_FREQ_HZ` to 33.33MHz alone could plausibly make bursts like this session's *worse*,
not better. QEMU's `ptimer`-backed devices (all of this project's timers) are paced against real
*wall-clock* time by default (`QEMU_CLOCK_VIRTUAL` without `-icount` advances 1:1 with real host
time) -- so a *slower*, more realistic `OSTM_FREQ_HZ` makes the real, wall-clock-measured gap
between ticks *longer*, not shorter, while TCG keeps executing guest code as fast as the host
possibly can in between. Unthrottled TCG on a modern host almost certainly executes far more
guest instructions per real millisecond than the real ~400MHz-class silicon this firmware
targets -- so a longer, more realistic inter-tick gap could let the guest CPU burst through
*more* real work (and therefore more queue pushes) before the next tick, not less. The
architecturally-correct fix for this whole class of mismatch is QEMU's `-icount` (instruction-
count-paced virtual time, throttling guest execution to a chosen real-hardware-equivalent rate)
used *together with* realistic clock constants -- not a realistic clock constant alone. This is a
bigger, more invasive change (affects every timer-paced device in the machine, and would need
the same kind of live re-validation `mtu2.c`'s own 25MHz tuning already needed once) than
anything tried so far on this thread, not attempted yet.

**`tools/trace_job_ring_producer.py`** (new): the natural extension of `trace_irq_mask_window.py`'s
own reactive-arming trick, applied to the *producer* side instead of the consumer -- cheap
wall-clock polling of `pending` detects a run-up, and only then arms a breakpoint on
`FUN_20186c4c` (the broadcast dispatcher, chosen over `FUN_20187bb4` one level down because the
latter has exactly one static caller so its own LR can't distinguish sources) to capture caller
LR + payload + channel number for each push during the actual burst, without perturbing the
whole boot. First two trials (100s, 130s) didn't reproduce an overflow at all -- confirms the
burst is genuinely rare/probabilistic on this specific timescale, consistent with everything
found so far (normal small pending=1 blips drain cleanly within ~0.25s every 10-30s throughout
both runs; nothing pathological seen outside of an actual overflow). More trials needed to
actually catch a live burst with the producer watch armed.

**A third producer-capture trial (2026-09-09, continuing) settled that this approach has hit a
real methodological wall, independent of the clock question**: the overflow hit at t=78.141s,
but the immediately preceding poll (t=77.918s, 0.223s earlier) showed `pending=0` -- fully
drained. The *entire* burst (0 to 16, overflow) happened inside that 0.223s gap, likely much
less once polling overhead is subtracted -- too fast for 100ms-granularity reactive polling to
ever see an intermediate value and arm the producer breakpoint in time. Since a *continuously*-
armed breakpoint/watchpoint on this path already independently proved (twice) to mask the bug by
desynchronizing producer and consumer, "catch it in the act with a breakpoint" is now a dead end
with the tools available -- reinforcing the clock-realism angle as the more promising direction.

**Real crystal frequency confirmed off the actual schematic, closing the one gap the datasheet
research left open** (user-supplied, read directly off the IC-7300 service manual's crystal/
oscillator table): `X301` (48.000 MHz) connects to the main CPU's pins 108/109, labeled
`USB_X1`/`USB_X2` on Icom's own schematic -- this is an exact, unambiguous match for the RZ/A1H
manual's **clock mode 1** (48MHz on `USB_X1`, PLL x32), which the manual documents as giving a
**fixed P0φ = 32.00MHz**, not the 25.00-33.33MHz range clock mode 0 would have left open. Real,
hardware-confirmed, not a guess. Elegant cross-check: `32000 / 32,000,000 Hz = exactly 1.000ms`
-- OSTM0's own real tick period would be a clean, obviously-deliberate 1ms RTOS tick, strongly
reinforcing that this is the right number (Icom's own engineers very likely chose `CMP=32000`
specifically to land on that round figure). The other four crystals were also identified and
ruled out as irrelevant to this question: `X621`/6MHz feeds the USB hub (`TUSB2046`), `X661`/
12MHz feeds the USB audio codec (`PCM2901`), `X901`/12.288MHz feeds the DSP, `X1201`/41.344MHz
feeds the FPGA -- none of these are the main CPU's own P0φ domain.

## The fix, confirmed live across two independent full-length trials: real OSTM_FREQ_HZ +
## `-icount shift=auto` together (2026-09-09, continuing straight on)

Tested the two variables separately, per this project's own "one change at a time, confirm
live" discipline, rather than assuming the combined reasoning was right without checking.

**Step 1: `OSTM_FREQ_HZ` alone, no `-icount`.** Changed to the confirmed-real 32,000,000 (from
the 500MHz placeholder). Rebuilt, ran a trial: boot proceeded at a normal wall-clock pace (no
new hangs, no regressions to any already-working milestone), but the overflow still happened
(t=44.18s) -- a clean negative result, exactly matching the live-reasoned prediction that the
constant alone isn't enough while `QEMU_CLOCK_VIRTUAL` stays tied 1:1 to real host wall-clock
time (unthrottled TCG can still burst through unrealistic amounts of guest work inside any
given real-time gap, regardless of how realistic that gap's own *length* is).

**Step 2: added `-icount shift=auto`** (QEMU's instruction-count-paced virtual time, throttling
guest execution to a chosen rate rather than letting TCG run at full host speed) on top of the
same real `OSTM_FREQ_HZ`. Added as an opt-in `ICOUNT` env var to `tools/
trace_job_ring_overflow.py` for testing rather than hardcoding it, so the tool stays useful for
comparison. **Two independent trials, 130s and 160s, both completed their full duration with
zero overflows** -- a real result, not a fluke: every previous non-icount trial (this whole
session, roughly a dozen across every tool built) hit the overflow somewhere in the 35-78s
range, so two clean full-length runs well past that window is strong evidence, matching this
project's own established multi-trial confirmation bar. Added a heartbeat print (every 15s
regardless of `pending` changes) to both trials specifically to rule out the boring false
positive (boot silently stalling somewhere new instead of genuinely progressing) -- PC kept
advancing and the ring kept showing its normal healthy small blip-then-drain pattern throughout
both full runs, not a stall.

**This is the fix.** `OSTM_FREQ_HZ` is being kept at the real, schematic-confirmed 32,000,000
(reverting to 500MHz would just be reintroducing a known-wrong placeholder for no reason).
`-icount shift=auto` is not yet wired in as this machine's own default launch flag anywhere
(README.md's "Running it" section needs updating, and it's worth checking whether it should
just always be passed) -- and per the schematic-research angle opened the same session, the
other timer-paced peripherals (`mtu2.c`'s 25MHz, `dmac.c`'s arbitrary 1000ns completion delay,
`scif.c`/`riic.c`/`rspi2.c`'s own clock assumptions) haven't been re-validated under `-icount`
yet either -- each was tuned/chosen against the old, unthrottled timing model, so any of them
could plausibly need a similar real-value correction now that the machine's overall timing
philosophy has changed. Not yet done, flagged as the natural next step.

## A real, deeper blocker found on a longer trial: DMAC's own completion wait now genuinely stalls
## under `-icount` -- the ring-overflow fix itself still stands, this is a separate, further issue
## (2026-09-09, continuing straight on)

Wired `-icount shift=auto` in as an actual default (`tools/qemu_launch.py`, a new shared launch
helper replacing every tool script's own copy-pasted launch line) and pushed toward the original
SD-card/VFS goal with a longer, 300s exploration trial (the 130s/160s trials that confirmed the
ring-overflow fix didn't run long enough to reach this). **Real finding, not a false alarm**:
by t=135s the trial had been parked at the exact same `pc=0x200b5f28`/`lr=0x200b5f24` for every
single 15-second heartbeat since t=14.5s -- confirmed via `ps` that the QEMU process was at 91%
CPU (genuinely spinning, not idle/blocked), and via Ghidra that this address is *inside*
`FUN_200b5ea4` -- the exact, already-known `DMAINT0`-gated busy-wait `dmac.c`'s own real channel-0
model was built specifically to unblock (see `dmac.c`'s own file comment and this file's much
earlier "DMAC channel 0 built" section). This session's separate `MTU2_FREQ_HZ` fix (below) was
ruled out as the cause -- it doesn't gate this particular wait at all.

**Diagnosis, not yet fixed**: `ostm.c` and `mtu2.c` both use QEMU's higher-level `ptimer` API
(built to integrate correctly with `-icount`'s deadline model), while `dmac.c` is the *only*
device in this machine using a raw `QEMUTimer` (`timer_new_ns(QEMU_CLOCK_VIRTUAL, ...)`/
`timer_mod`) directly for its one-shot completion delay (`DMAC_COMPLETE_DELAY_NS`, 1000ns). A
raw `QEMUTimer` not integrated with icount's own deadline-checking mechanism is a well-known
category of icount pitfall (a tight spin-loop translation block can fail to yield control back
to QEMU's main loop often enough for such a timer's callback to actually fire) -- a strong,
plausible, but not yet confirmed-by-fix hypothesis. Not yet tested: switching `dmac.c` to
`ptimer` the same way `ostm.c`/`mtu2.c` already work, or otherwise making its completion timer
icount-aware.

**Important, so this doesn't read as a contradiction of the earlier fix**: the original
`0x20420120` job-ring overflow this whole thread chased is still genuinely fixed -- both
confirming trials (130s/160s) were real, and nothing here changes that result. This is a
*further*, *deeper* blocker on the same boot path, only reachable *because* the ring-overflow
fix cleared the way to it, found by simply running longer once that fix was in place. Active
resume point for continuing toward the original SD-card/VFS goal: fix `dmac.c`'s icount
interaction (most likely: port it to `ptimer`) before the next long exploration trial.

## MTU2's own real clock confirmed too (2026-09-09, same session): both channels 3 and 4 run
## undivided off the same real P0φ, 32.00MHz -- same clean 1ms period as OSTM

A background research agent found the real prescaler firmware configures for MTU2 channels 3
and 4, closing the gap `mtu2.c`'s own comment had left open (25MHz was chosen empirically, not
derived). Direct disassembly of the already-known channel-3/4 init functions
(`FUN_20005c08`/`FUN_20005cac`) shows both write their own `TCR` register as a literal `0x00` --
decoded against the RZ/A1H manual's Table 10.9 (the channel-3/4-specific `TPSC` encoding,
distinct from channels 0-2's own table), `TPSC=000` means "count on P0φ/1", i.e. no division at
all. Combined with P0φ's own real, schematic-confirmed value, both channels' real rate is
**32,000,000 Hz** -- identical to OSTM0's own real rate, and giving the same clean 1.000ms
period for the already-modeled fourth/fifth compare events (whose own real requested period,
`0x7d00`/32000, was already known exactly -- this just corrects what frequency it's honored
against). `MTU2_FREQ_HZ` changed from 25000000 to 32000000; rebuilt cleanly. Not yet
live-tested in isolation (the DMAC/icount blocker above was found first, on the very next trial
after this change went in, but is confirmed unrelated) -- re-test once `dmac.c`'s own icount
issue is resolved, since a full clean long trial isn't currently reachable to confirm this one
either way.

## The DMAC/icount fix doesn't generalize: a fixed `-icount shift=1` clears DMAC's own stall but
## a *different* busy-wait then stalls in its place -- looks like a broader icount-cooperation
## issue with this machine's busy-wait-heavy boot style, not a single device's bug (2026-09-09)

Ported `dmac.c` to `ptimer` (matching `ostm.c`/`mtu2.c`) as diagnosed above. **Tested, and it
alone did not fix the stall**: rebuilt, reran under the same `-icount shift=auto` default --
still parked at the identical `FUN_200b5ea4` busy-wait for multiple consecutive 15s heartbeats,
91% CPU, same as before the port. The `ptimer`-vs-raw-`QEMUTimer` diagnosis was wrong, or at
least insufficient on its own.

**Tried a fixed, small `-icount shift=1` instead of `auto`** (the auto-tuner's own dynamic
slice-sizing was the next suspect -- a tight, no-I/O spin loop might look "compute-bound" to it
and get an abnormally large slice size, meaning abnormally infrequent real exits back to QEMU's
main loop where timers actually get serviced). **This did clear the DMAC stall** -- a fresh
trial reached a different, later PC (`0x200b48f8`) within 15s where the previous config was
still stuck at `0x200b5f28` after 90+ seconds. Ran a full 160s confirming trial to be sure.

**But that trial found the exact same *shape* of problem recur, just relocated**: parked at
`0x200b48f8` for 10 consecutive 15s heartbeats (135 real seconds), 91% CPU, genuinely stuck --
confirmed via Ghidra to be inside `scif5_wait_hsk1_ready` (`0x200b48e4`), an entirely different,
already-known function (the DSP-link handshake wait right before `dsp_boot_handshake()` in
`cold_boot_hw_init`'s own sequence) -- not DMAC-related at all, and not something either the
`ptimer` port or the shift change touches.

**This changes the read on the whole DMAC finding**: it's very likely not a DMAC-specific bug,
or even specifically a raw-`QEMUTimer`-vs-`ptimer` issue -- both fixes tried so far have each
"solved" one specific stall only to have a *different* busy-wait somewhere later in the same
boot sequence get stuck in its place, under either `-icount` configuration tried. This looks
like a broader, more general icount-cooperation problem with this machine's own boot code style
(many sequential busy-wait loops, none of which stalled at all under the *old*, un-throttled
timing model -- confirmed earlier this session that boot reaches well past both of these exact
points, into the ring-overflow region, in under 44-78s with real clocks and no icount at all).
**Not yet root-caused.** The original `0x20420120` ring-overflow fix itself is unaffected by any
of this (its own confirming trials happened to not run long enough to reach either of these
later stalls) and still stands as confirmed.

**Active resume point, reframed**: `-icount`, as currently configured (with either `shift=auto`
or a small fixed shift), is not yet a reliable way to reach deep boot milestones reliably --
each configuration tried relocates rather than eliminates a stalling busy-wait somewhere in
`cold_boot_hw_init`'s own long, sequential call chain. This needs either genuine QEMU icount
internals research (why does a slice boundary / timer check seemingly never happen for *some*
tight busy-wait loops under either shift setting tried), or a different overall strategy for
long-running boot tests than blanket `-icount`. Not resolved this session -- flagged honestly
rather than claimed fixed.

## Handoff prep for a new session: one more diagnostic on the DMAC/icount stall, real progress
## but a genuine, honestly-unresolved puzzle -- not closed out (2026-09-09, continuing straight on)

Before handing this off, did one more cheap, targeted diagnostic to sharpen the resume point
rather than leave it at "not yet root-caused."

**Confirmed via temporary debug instrumentation (added to `dmac.c`'s completion path, reverted
immediately after -- zero trace left in tracked source) that the completion mechanism itself
works under `-icount shift=auto`**: reproduced the original stall scenario (default launch, the
now-`ptimer`-based `dmac.c`) with `DMAC_DEBUG_LOG` capturing both the "armed" and "complete"
events. The completion callback fired only ~67 real microseconds after being armed -- the
one-shot delay resolves essentially instantly, and `qemu_irq_pulse()` executes right after.
**This rules out "the ptimer callback never fires" as the mechanism** -- a real, previously
untested fact, not an assumption.

**Then checked the next link with a live GDB breakpoint on the real DMAINT0 ISR
(`FUN_200b5b90`, already confirmed by an earlier session)**: built `tools/trace_dmac_isr.py`
(two low-frequency breakpoints -- the ISR entry, and `FUN_200b5ea4`'s own wait-loop check,
the latter disarmed after its first hit to avoid reintroducing per-push-frequency masking).
**The ISR genuinely was entered** -- hit at `t=3.731s`, with the wait-loop check itself reached
at `t=3.937s`, ~0.2s later. In this specific 40-second trial, **the stall did not reproduce at
all** -- no overflow trap, no further unexpected stops, for the entire remaining ~36 seconds.

**A genuine puzzle, checked immediately rather than left dangling**: this diagnostic run's ISR
hit at `t≈3.7s` is far earlier than every wall-clock-polling trial's own observed stall onset
(consistently `t≈14-16s` in every prior trial that hit this exact address, `0x200b5f28`).
`references_to` on `FUN_200b5ea4` confirms **exactly one real caller** (`0x2002b060`, inside
`cold_boot_hw_init`, as expected) -- ruling out "two different invocations, one that completes
fast and a later one that stalls" outright. This is the *same single* call, just reached at a
very different real wall-clock time depending on what observation technique preceded it.

**Refined hypothesis, better supported now that the alternative is ruled out**: something about
the *accumulated* difference between this diagnostic's own two low-frequency breakpoints (set
once, mostly idle) and the wall-clock-polling trials' own continuous `interrupt()`/`read()`/
`cont()` cycle (every 0.25-1s from `t=0` onward) changes how fast boot reaches this point *and*
what happens once it does -- plausibly because `-icount shift=auto` is a *stateful, adaptive*
mechanism (it tunes its own shift value based on observed workload over time), so the specific
sequence of pauses/observations *before* reaching this code could leave the auto-tuner in a
different internal state by the time the DMA arm+wait actually runs, even though the wait's own
logic and the DMAC device model are identical either way. Not confirmed -- QEMU's own `-icount`
auto-tuning implementation (vendored under `qemu-src/`) would need reading to verify this
mechanism exists as described, but it's a concrete, checkable claim, not a vague gesture.

**Concrete next steps for a fresh session, in the order they're cheapest to try**:
1. Read QEMU's own vendored `-icount shift=auto` implementation (search `qemu-src/system/` and
   `qemu-src/accel/tcg/` for `icount`-related adaptive-shift logic) to confirm or refute the
   "stateful auto-tuner" mechanism above -- this is the most direct way to explain why observation
   method changes the outcome for the *identical* single code path.
2. Reproduce with a technique that observes without touching GDB's breakpoint/pause machinery at
   all -- e.g. QEMU's own `-d int,exec` trace flags to a log file (already used elsewhere in this
   project's history for exactly this "observe without perturbing" need), or QMP monitor commands
   instead of the GDB stub -- to see whether the stall reproduces under an entirely different,
   non-GDB observation method, or whether *any* form of pausing avoids it (which would further
   support the stateful-auto-tuner theory over a GDB-specific one).
3. Try a **fixed** (non-auto) shift value tuned specifically to still be large enough to avoid
   the earlier `scif5_wait_hsk1_ready` stall `shift=1` caused (README-history.md's prior section)
   -- if a stateful auto-tuner really is the mechanism, a fixed shift removes the state-dependency
   entirely and should make the outcome reproducible one way or the other, rather than varying by
   observation method.
4. If genuinely stuck after all of those: read `qemu-src/system/cpu-timers.c`/`accel/tcg/cpu-
   exec.c` (icount budget/deadline handling) for how a hard IRQ signaled mid-slice actually gets
   serviced -- the point where this stops being project-specific and starts being real QEMU
   internals research, per the prior session's own assessment.

`tools/trace_dmac_isr.py` (new) is kept as a reusable diagnostic, and `references_to` on
`FUN_200b5ea4` (or any other suspected multi-invocation site) is worth reaching for early before
assuming a stall is code-path-specific rather than timing/state-dependent.

## The "DMAC/icount stall" was never a real DMA or `-icount` bug -- it's a GDB-remote-stub
## reliability artifact, confirmed by removing GDB from the loop entirely (2026-09-09, new session)

Picked up exactly where the prior session's own "concrete next steps" list left off, in order.
Step 1 (read QEMU's own `-icount shift=auto` implementation): confirmed straight from
`qemu-src/accel/tcg/icount-common.c` that the adaptive tuner genuinely is stateful --
`icount_adjust()` fires every real 1000ms off `QEMU_CLOCK_VIRTUAL_RT` (a clock that keeps
advancing in real time even while the vCPU is `vm_stop()`ped) and ratchets `icount_time_shift`
up/down based on drift between executed-icount-as-ns and real elapsed time, capped at
`MAX_ICOUNT_SHIFT` (10). Also found, from `qemu-src/accel/tcg/tcg-accel-ops-icount.c`, that
`icount_get_limit()` converts the nearest pending `QEMU_CLOCK_VIRTUAL` timer deadline into an
*instruction budget* via `icount_round()` (dividing by `2^shift`) -- confirming the theory was at
least mechanically plausible. This made the stateful-auto-tuner theory look like the answer.

**Live testing immediately complicated that picture, then overturned it entirely.** A new
diagnostic (`tools/trace_dmac_icount_shift.py`) layered the *exact* periodic `interrupt()`/
`cont()` wall-clock cadence the original stall-observing scripts use on top of light breakpoints,
sustained for a full 240s (well past the 130-300s range where the stall was previously found) --
clean, healthy progress the whole time, no stall. This directly refuted "periodic GDB pausing
alone causes it" (step 2 of the prior session's plan, in spirit). A follow-up genuinely
hands-off free run (`tools/trace_dmac_hands_off.py`, zero GDB interaction after the initial
continue, checked only via external `ps`) ran clean for 347s too -- but `ps` CPU% alone can't
distinguish "still making real progress" from "pinned in the wait the whole time", so this wasn't
decisive on its own.

**The decisive test bracketed the wait's own natural exit instead of guessing at proxies.**
Reading `FUN_200b5ea4`'s raw listing found the real structure: two internal waits, not one --
loop 1 (`0x200b5f00`-`0x14`, waiting for a *shared* busy flag to go idle before starting) then an
arm step (`FUN_200b5dc0`, which itself sets that flag busy) then loop 2 (`0x200b5f28`-`30`, the
actual completion wait, falling straight through to the function's real return at `0x200b5f34`
once it clears). `tools/trace_dmac_wait_completion.py`/`_completion2.py` set exactly one (then
two) breakpoints at these natural exits, nothing at or near the loops themselves, and free-ran
otherwise untouched. **Two full 500s trials: `LOOP1_EXIT` (`0x200b5f18`) fired quickly (t=3.8s,
confirming the transfer gets armed just fine -- not a shared-resource deadlock), but
`DMAC_WAIT_RETURN` (`0x200b5f34`) never fired in either trial.** This looked like rock-solid
confirmation that the *specific* arm-to-completion path was the real, narrow blocker -- Case B
from a two-case framework (Case A: stuck waiting for a *prior* user of the shared flag; Case B:
own transfer armed, completion never comes back) laid out mid-session while explaining the
mechanism to the user.

**Then a completely independent verification method (host-side-only instrumentation, zero GDB
involvement) directly contradicted that "confirmation".** Added temporary `fprintf`-based debug
logging straight into `dmac.c`'s own `rza1h_dmac_write()`/`rza1h_dmac_ch0_complete()` (guarded by
`DMAC_DEBUG_LOG`, same disposable-instrumentation pattern earlier sessions already used and
reverted for this exact device) -- logging the arm write, the ptimer firing, the IRQ pulse, and
any guest write back to `CHCTRL_0` (which only the real ISR, `FUN_200b5b90`, ever writes, so
seeing it is direct proof the *guest's own ISR code* executed, not just that the host's ptimer
callback fired). **Ran completely free of any gdbstub at all** (no `-S`, no `-gdb` in the QEMU
invocation whatsoever) -- the whole arm -> ptimer-fire -> IRQ-pulse -> guest-ISR-runs-and-acks
chain completed in **under 1 millisecond of real time**, reproduced identically across 8 separate
trials. This includes trials that individually re-added every variable suspected of mattering --
`-gdb` present but unconnected, `-S` resumed via QMP instead of GDB, a GDB client connected and
disconconnecting immediately, a real breakpoint hit-and-removed mid-execution (`LOOP1_EXIT`
itself) -- **every single one of these still completed instantly**. The only configuration that
ever failed to show completion was the *exact* original diagnostic: GDB attached, both
`LOOP1_EXIT` and `DMAC_WAIT_RETURN` armed as breakpoints from the start, `LOOP1_EXIT` hit and
disarmed, then blocking on `DMAC_WAIT_RETURN`. Forcing a `interrupt()` mid-run under that exact
configuration reported the CPU pinned at `0x200b5f28` (the loop's own check instruction) --
*directly contradicting* the independent, GDB-free device-model log, which by then had already
proven the guest's ISR ran and the flag was cleared.

**Ruled out a retry-loop explanation for the log's own three `CHCTRL_0` writes before accepting
this conclusion**, per this project's own established discipline of checking before asserting:
`cold_boot_hw_init`'s decompile (the real caller context) shows `FUN_200b5be0(); FUN_200b5ea4();
FUN_200b5f38();` called in strict, single-pass straight-line sequence -- `FUN_200b5ea4` is called
exactly once, no outer loop that could explain repeated arming. The three `CHCTRL_0` writes are
real: one from the ISR's own `|=0x62` ack (confirmed in its decompile), the other two most likely
from the two immediately-following sibling transfers' own analogous channel-control writes
(`FUN_200b5f38`/`FUN_200b60dc`, already known from `references_to` to share this same flag) --
not independently confirmed in detail, but not needed to be, since the single-call-site fact
alone already rules out "this is just a hot retry loop with a coincidentally-revisited PC".

**Conclusion, stated as plainly as the evidence supports**: the actual DMAC hardware/software
completion chain works correctly and fast under `-icount shift=auto`, with or without GDB
attached, with or without breakpoints set. What does *not* work reliably is QEMU's own GDB
remote-serial-protocol stub, specifically for this address pattern (a breakpoint sitting at the
literal fall-through target of a tight polling loop's own conditional branch) under `-icount`:
either the breakpoint silently never traps, or a forced `interrupt()`'s register snapshot reports
a stale/incorrect PC, or some combination -- not yet narrowed further, and the root QEMU-internals
cause (something in gdbstub's interaction with icount-paced translation-block execution/
invalidation near this address) is still genuinely open. But it no longer matters for this
project's actual goal: **this specific busy-wait is not a real blocker**, and every trial this
session actually reached, watched via GDB, that appeared to "never complete" was very likely
seeing this same artifact, not a real hang.

**A humbling implication, stated honestly rather than glossed over**: the *original* diagnosis
that motivated porting `dmac.c` from a raw `QEMUTimer` to `ptimer` (this file's earlier "FIXED,
2026-09-09" section) almost certainly used the same GDB-based observation method that's now shown
to be unreliable here. This does **not** mean that port was wrong or should be reverted -- it's
still a real architectural improvement, consistent with `ostm.c`/`mtu2.c`'s own pattern, and
`dmac.c`'s temporary debug instrumentation was reverted from *this* (already-`ptimer`-based)
version, so nothing here re-tests the old raw-`QEMUTimer` version directly. But it does mean the
specific claim "the raw `QEMUTimer`'s callback never fired... a real `-icount` bug" should be read
with real skepticism now, not treated as settled -- it's plausible the old version would have
shown the identical "instant completion, GDB-free" result if it had ever been tested that way.
Not going back to re-verify the old version; flagging the uncertainty honestly is enough.

**Durable methodology lesson for this whole project, worth internalizing broadly, not just for
DMAC**: a "stall" that only manifests when observed via GDB breakpoints/forced-interrupts, and
disappears under a genuinely independent, host-side-only verification (device-model logging via
`fprintf`, or `-d unimp` for a device that already logs its own real commands, as `mmc.c` turns
out to already do for every real SD command via `qemu_log_mask(LOG_UNIMP, ...)`) should be
treated as suspect *by default* from now on, before spending further effort on a device model or
`-icount` explanation. This project's whole GDB-based tooling (`gdbrsp.py`, every `trace_*.py`
script) is fast and has been reliable for plenty of other findings this project has made (SCIF,
RIIC, MTU2, the earlier ring-overflow diagnosis, etc.) -- this isn't a blanket indictment of the
approach -- but it is now a confirmed, real failure mode specifically under `-icount shift=auto`
that any future "this busy-wait never clears" finding should be cross-checked against before being
trusted. **Concrete next step, in progress as this section is being written**: a genuinely
GDB-free long free run (no `-S`, no `-gdb`, no QMP polling at all -- the true baseline none of
this session's earlier "hands-off" tests actually were, since they all used `qemu_launch.py`'s
fixed `-S -gdb tcp::1234` args under the hood) to see how much further boot naturally progresses
now that this false blocker is understood, watching in particular for real MMCIF/SD-card activity
(`mmc.c` already logs every real command via `qemu_log_mask(LOG_UNIMP, ...)`, so `-d unimp` alone
is enough to see it -- no new instrumentation needed there).

## `src/rza1h_debug.h`: the GDB-stub lesson made into a permanent tool, plus what a genuinely
## GDB-free 15-minute free run actually shows (2026-09-09, same session, continuing straight on)

**A failed measurement attempt worth recording so it isn't retried**: before landing on the
"bracket the wait's natural exit" technique that actually resolved the DMAC finding above, this
session first tried `tools/measure_dmac_wait_throughput.py` -- comparing real elapsed time for N
consecutive GDB single-steps (`step()`) inside the DMAC wait loop against N steps of ordinary ISR
code, hoping to directly measure "is this specific loop's real per-instruction cost
disproportionate". The result was uninformative, not just wrong: both regions measured
~82,000 microseconds *per single step*, a 1.00x ratio -- meaning the measurement was entirely
dominated by GDB single-step round-trip overhead (already large under this icount config,
apparently independent of what instruction actually executes), swamping any real difference by
several orders of magnitude. Kept as a cautionary, not deleted -- any future temptation to
single-step-time a suspected slow region under `-icount` should budget for this ~80ms/step floor
first, or use a completely different technique (breakpoint hit-rate over a fixed wall-clock
window, not raw per-instruction stepping).

**The actual resolution technique, once found, was clean**: reading `FUN_200b5ea4`'s raw listing
found it has two internal waits, not one (loop 1: is the shared slot idle; loop 2: has my own
transfer completed), each with a natural exit PC reached only on genuine completion. Bracketing
those two exits with exactly two breakpoints, nothing at or near the loops themselves, and
free-running otherwise untouched, at first looked like clean confirmation of a narrow blocker
(loop 1 cleared in 3.8s, loop 2 never fired in two full 500s trials) -- until independent,
completely GDB-free verification (temporary `fprintf` logging directly in `dmac.c`'s own
callbacks, reverted after use) proved the whole chain actually completes in under 1ms, 8/8
trials, including trials that reintroduced every suspected variable one at a time. The full
derivation is the "GDB-remote-stub reliability artifact" section above; this note is just
recording the two techniques that led there, in the order they were actually tried, for whoever
picks this up next.

**`src/rza1h_debug.h` built, same session, directly motivated by that lesson** (per the user's
own framing: "would it be a reasonable idea to add debug option for our implemented peripherals
... would get cheap-ish info what's happening without needing to have gdb" -- yes, and today is
the proof). A small, permanent, project-owned helper (`rza1h_debug(dev_name, fmt, ...)`, gated by
one `RZA1H_DEBUG=<comma-separated names>|all` env var, silent when unset), deliberately *not*
built on QEMU's own `-d`/`qemu_log_mask()` mechanism -- `mmc.c`'s pre-existing real
command-dispatch logging already (mis)used `LOG_UNIMP` for this, which works but pollutes that
category's otherwise-clean "genuinely unimplemented access" signal (directly relied on earlier
this same session to answer "does anything touch display/VDC5 hardware at all" -- a routine
peripheral trace mixed into the same category would have made that check meaningfully less
trustworthy). A dedicated mechanism also needs no QEMU source patching, avoiding a second patch
to rebase against future QEMU version bumps on top of the existing `patches/hw-arm-build.patch`.
Wired into `dmac.c` (arm/complete), `mtu2.c` (all three compare-match ticks, both software-timer
arm/expire events), `riic.c` (START/RESTART/STOP conditions, DRT phase transitions), `scif.c`
(TX/RX bytes, both virtual responders' canned acks), `rspi2.c` (TX bytes), and `mmc.c` (real SD
command dispatch, migrated off its old `LOG_UNIMP` misuse, same for `scif.c`/`riic.c`/`rspi2.c`'s
own pre-existing `LOG_UNIMP` calls). `gpio.c`/`l2c.c`/`spi_boot.c` deliberately left untouched --
plain-storage/self-clearing devices with no real protocol state machine worth tracing, and adding
calls there would just be noise against this tool's own stated design principle. One real build
gotcha hit along the way: `MTU2_SWTIMER_B_ONESHOT_NS`'s computed type didn't match `PRId64` under
`G_GNUC_PRINTF`'s strict format checking on this host (`long` vs `long long`, a real, if
mundane, type-checking mismatch) -- fixed by an explicit `(long long)` cast plus a literal
`%lld`, not by fighting the platform's own `inttypes.h` macros.

**Immediately validated the new tool is useful, not just plausible in theory.** A genuinely
GDB-free 15-minute (900s) free run with only `-d unimp,guest_errors` (no `RZA1H_DEBUG`) showed
the process alive and steady (~102% CPU) the whole time, but **zero new logged activity for 840
straight seconds** after an initial burst -- ambiguous on its own (could be a healthy steady
state touching only already-real-modeled devices, which `-d unimp` can't see at all by design;
could equally be a genuinely different stall). A second run with `RZA1H_DEBUG=all` immediately
resolved the ambiguity in the *reassuring* direction: `mtu2.c`'s ch3 (`TGI3A`)/ch4 (`TGI4C`)
compare-match events fire continuously, alternating at a steady ~964 combined events/second --
matching the free-running 16-bit-wraparound math almost exactly (`0x10000` counts / 32,000,000 Hz
≈ 2.048ms per channel's own wraparound ≈ 488Hz/channel, ×2 channels ≈ 976Hz, close enough to the
measured 964Hz that this isn't a coincidence). **This is direct, independent-of-GDB proof the
system is genuinely alive and actively running a real, ongoing RTOS-scheduler-tick mechanism
throughout the stretch the `unimp` trace alone made look quiet/frozen -- not evidence of a new
stall.** A follow-up run scoped to the rarer channels only (`RZA1H_DEBUG=dmac,riic,scif,rspi2`,
excluding the now-understood, extremely chatty `mtu2`/`ostm` ticks) was run for the same full
900s to see whether anything *else* interesting happens during this stretch (a later DMAC
transfer, RIIC/SCIF activity, etc.).

**Result: fully decisive, not ambiguous.** The log held at exactly 142 lines for the entire 900
real seconds -- literally zero new `dmac`/`riic`/`scif`/`rspi2` activity anywhere past the
initial boot burst (the RIIC2 EEPROM reads, the SCIF3 front-panel identify/keepalive handshake,
and one DMAC transfer, all landing within the first ~3-8 real seconds). Combined with `mtu2.c`'s
continuous, steady ~964Hz combined tick rate confirmed in the same window, this settles the
question cleanly: **the system reaches a genuine, stable RTOS idle loop -- alive and actively
scheduling (the tick keeps running), but making no further forward progress of any kind for a
full 15 real minutes.** Not a new blocker -- a firmware that has legitimately finished everything
it can do without an external trigger (a front-panel button, an SD-card insert event, a specific
menu action) that a purely passive free-running boot will never generate on its own. **Concrete
next step for whoever picks this up next**: `force_call_fup.py`'s "force a direct call into
`firmware_update_main`" approach (already built, not yet retested against this now-much-further,
now-correctly-understood boot state) is the right next move, not a longer or differently-scoped
free run -- the passive-observation avenue is now closed out with a real, well-supported answer,
not abandoned for lack of one.

## `force_call_fup.py` retested -- same session, immediately following. A real methodology bug
## found and fixed along the way, then a decisive (if disappointing) result: the SD-card path's
## real blocker is the *already-diagnosed* 2026-09-08 gate, not anything from today's DMAC thread

**First retest attempt found a genuine bug in the retest itself, not a firmware issue.** Ran
`force_call_fup.py` exactly as written against a freshly-`-S`-launched instance (no prior free
run) -- got a real Prefetch Abort (`PC` pinned at the ARM exception vector `0x0000000C`,
`CPSR` mode bits reading Abort mode). Traced why before concluding anything about the firmware:
`force_call_fup.py` hijacks directly from **cold reset** (`SP=00000000` in the "before" trace --
no real startup code has ever run, so no stack has ever been set up), whereas the original
2026-09-08 investigation's own wording ("the idle task's registers overwritten via GDB") implies
it hijacked an *already-running* context with a real stack. Confirmed this is exactly the
difference: a small wrapper that frees the CPU to run naturally for 15s first (same live GDB
connection throughout -- dropping and reconnecting mid-`continue` under `-icount` was found,
separately, to corrupt the gdbstub's state for the next client, a second real gotcha this
session) before doing the identical hijack sequence, using the real stack pointer already in
place, produces no crash at all.

**With that fixed, and with a real FAT16 SD-card image attached** (`tools/build_sdcard.py`,
matching the documented `\IC-7300\<filename>` convention this project's manual research already
established) **, the retest reproduces the exact qualitative behavior the original 2026-09-08
session described, almost word for word**: the CPU keeps legitimately executing, cycling through
varied real code addresses across genuine context switches (not stuck in one tight loop), but
never reaches any MMCIF register access (confirmed independently via `RZA1H_DEBUG=mmc,dmac,riic,
scif,rspi2` -- zero new activity in the log after the hijack, in a 15s trial) and
`firmware_update_main` never returns. A direct memory read of `sdcard_file_rpc_dispatch_task`'s
own control struct (`0x203907f0+0x20`/`+0x28`, the exact fields the 2026-09-08 session already
identified as the task-creation tell) confirms this isn't just a `force_call_fup.py`-specific
symptom: **both fields are still zero even after 120 real seconds of genuinely untouched, natural
free-running boot** -- ruling out "just needs more real time" (today's whole session's other
running theme) before accepting this conclusion.

**First conclusion (immediately superseded, kept here rather than deleted -- a real, honest
correction, not a clean derivation)**: attributed this to `FUN_2002b29c`'s branch decision (the
2026-09-08 session's own diagnosis) never routing to `cold_boot_mode_dispatch`. **Wrong** --
`cold_boot_mode_dispatch`'s own decompile (`0x2002b1c8`) shows `cold_boot_hw_init()` called as
its very first, plain synchronous statement, and `cold_boot_hw_init` demonstrably DOES run (all
of today's DMAC/MTU2/RIIC/SCIF/RSPI2 findings happen inside it, confirmed via `references_to`
on its real entry point `0x2002afc0` -- the caller is `0x2002b1d8`, squarely inside
`cold_boot_mode_dispatch`'s own address range). So `cold_boot_mode_dispatch`, and therefore
`FUN_2002b29c`'s gate, has already been entered and passed. `system_mode_request_dispatch()`
(which creates `sdcard_file_rpc_dispatch_task`) is only reached AFTER `cold_boot_hw_init()`
returns -- and something inside `cold_boot_hw_init`'s own remaining body, not `FUN_2002b29c`, is
the real blocker.

**Precisely localized it, same session, before handing off.** Built
`tools/trace_cold_boot_hw_init_tail.py`, a waypoint-breakpoint sweep across `cold_boot_hw_init`'s
own remaining call sequence (the same technique the 2026-09-08 session used successfully to find
`FUN_2001dd58` inside `riic2_driver_init`) -- two smoke tests (15s, 60s) got zero hits at all,
including the very first waypoint. Rather than trust that at face value (this session's whole
point is that GDB observation can itself be unreliable under `-icount`), cross-checked with a
completely GDB-free `RZA1H_DEBUG=dmac,riic,scif,rspi2` run: **only one DMAC arm+complete event
total**, matching exactly `FUN_200b5ea4`'s own already-confirmed transfer -- meaning
`FUN_200b5f38` (`cold_boot_hw_init`'s very next call after `FUN_200b5ea4`, at `0x2002b064`) never
reaches its own `N0TB_0` write at all. Decompiling `FUN_200b5f38` confirms why this is plausible:
it's structurally identical to `FUN_200b5ea4` (same two-loop arm/wait shape) and, confirmed via
listing (not assumed), uses the **literal identical struct base** `0x203906EC` -- the exact same
shared "channel busy" flag (`0x203906EE`/`0x203906ED`) `FUN_200b5ea4` already uses. Since the
debug log shows no second arm event, `FUN_200b5f38` is stuck in *its own* loop 1 (the
shared-slot-idle check, `0x200b5fa4`-`0x200b5fac`) -- the flag `FUN_200b5ea4`'s own ISR already
clears is somehow still non-zero by the time this sibling checks it.

**This is likely a real, different bug -- not simply a repeat of the GDB-stub artifact**, since
the finding came from a completely GDB-free run. Built `tools/trace_fun200b5f38_wait.py`,
bracketing `FUN_200b5f38`'s own loop-1-exit (`0x200b5fbc`) and real return (`0x200b5fd8`) with
exactly two breakpoints -- the same near-zero-perturbation technique that worked cleanly on
`FUN_200b5ea4` -- ready to run, **not yet run to a conclusion**, per explicit instruction to
prepare this for a fresh session rather than solve it now. See that script's own module comment
for the concrete follow-up questions once it narrows further (whether `FUN_200b5ea4`'s ISR
genuinely completes before `FUN_200b5f38` starts checking, direct reads of the two flag bytes
rather than a breakpoint-dependent snapshot, and whether `0x203906ED` -- the second flag, less
carefully traced than `0x203906EE` this session -- is the actual culprit).

**Net effect on the "passive idle loop" finding above**: still accurate as an *observation*
(`mtu2.c` ticking continuously, no new `dmac`/`riic`/`scif`/`rspi2` activity for 900s), but the
*explanation* is now precise rather than vague -- not "the firmware finished everything and is
waiting for an external trigger", and not `FUN_2002b29c`'s gate either, but specifically
`FUN_200b5f38`'s own stuck loop 1, three call frames deep inside `cold_boot_hw_init`. **Today's
entire DMAC/MTU2/ring-overflow/GDB-stub thread remains a genuinely separate, already-resolved
concern** -- fixing it was real, necessary progress (it's what let boot reach far enough to find
this next, deeper blocker at all) -- but this specific bug is new territory, not that thread
recurring. **Concrete next step for whoever picks this up next**: run
`tools/trace_fun200b5f38_wait.py` first (cheapest, most direct); if `LOOP1_EXIT` never fires,
read `0x203906EE`/`0x203906ED` directly to see the actual stuck value(s), then trace who else (if
anyone) touches those two addresses between `FUN_200b5ea4`'s ISR clearing them and
`FUN_200b5f38`'s own check -- a `references_to` sweep on `0x203906ED` specifically hasn't been
done yet this session and is a natural place to start.

## 2026-09-10 session: the DMAC/icount stall really was a real bug after all -- a precise, self-inflicted completion race

Picked up exactly where the prior session's handoff left off: `tools/trace_fun200b5f38_wait.py`,
built but deliberately not run to a conclusion. Ran it -- TIMEOUT, neither `LOOP1_EXIT`
(0x200b5fbc) nor `OWN_RETURN` (0x200b5fd8) ever fired in a 300s free-running trial, matching the
prior session's own prediction ("stuck in loop 1"). Followed the prepared next step exactly: a
new tool, `tools/read_fun200b5f38_flags.py`, free-runs past early boot then periodically
`interrupt()`s and reads `r15`/the two flag bytes directly (no breakpoint at all, deliberately,
to stay clear of the already-documented gdbstub-artifact pattern for breakpoints at a tight
loop's own natural exit).

**First real surprise**: PC sat at `0x200b5f28` and `0x20187b9e`, never once in `FUN_200b5f38`'s
own loop 1 range (`0x200b5fa4`-`0x200b5fac`) across 5 samples spanning 20-40s. Checked what
function actually contains `0x200b5f28` via `functions.get` (not assumed) -- it's `FUN_200b5ea4`
itself (bounds `0x200b5ea4`-`0x200b5f37`), specifically its *own* loop 2 (the post-arm completion
wait). **This falsifies the prior session's whole "stuck in FUN_200b5f38's loop 1" diagnosis**:
the CPU never even reaches `FUN_200b5f38` at all, because the *preceding* call
(`FUN_200b5ea4`, "resolved" the session before) itself never returns. The prior session's own
tooling (`trace_dmac_isr.py`, the host-instrumentation check) had confirmed the DMAC model's
*internal* chain (arm -> ptimer -> pulse -> ISR entry) runs fast and reliably -- but never
independently re-checked the actual RAM flag the firmware itself polls, after the fact, via a
path with zero breakpoint involvement anywhere near the code in question. That gap is exactly
what this session's `read_fun200b5f38_flags.py` closed.

**Cross-checked with an entirely different, gdbstub-free path before trusting it**: wrote
`tools/qmp_read_mem.py`, a tiny client for QMP's own `human-monitor-command` -> `xp` (physical
memory examine) -- a completely separate QEMU code path from `-s -gdb`'s remote-serial stub.
Launched QEMU with only `-qmp unix:...,server,nowait` (no `-S`, no `-gdb`, no GDB client
anywhere in the process at all) and read `0x203906EE` repeatedly, 5s apart, across two
independent boots: **every single sample, both boots, read exactly `0x01`**, never once `0x00`.
Cross-checked `RZA1H_DEBUG=dmac`'s own log at the same time: it clearly shows one clean
"armed"/"complete, pulsing DMAINT0" pair, so the *device model* really does fire -- the flag
being permanently stuck is a fact about what happens after, not about the model's own internal
chain. This directly contradicts the prior session's "RESOLVED... completes under 1ms every
trial, 8/8" conclusion for this exact busy-wait; that finding didn't hold up to a completely
independent verification path.

**Precisely localized the mechanism by reading the actual code, not by re-running a breakpoint
near the loop** (a live breakpoint-based attempt was tried first here too, `trace_dmac_race.py`
-- discarded, not trusted: it showed hundreds of identical hits at a suspiciously exact ~123ms
cadence, always reporting the same value, which pattern-matches the already-documented gdbstub
artifact class rather than real forward progress; also turned out to be catching a *different*
recurring caller entirely -- `references_to` on the shared low-level arm routine, `FUN_200b5dc0`,
shows 6 real call sites, not the one this session assumed). Reading the raw listing directly
instead: `FUN_200b5dc0` (the shared low-level "arm" helper all 6 callers use) writes `N0TB_0`
(`0x200b5e14`, the exact write `dmac.c`'s model treats as "start the completion ptimer") and,
only **one ARM instruction later** (`0x200b5e1c`, `mov r0,#1; strb r0,[r2,#2]`), marks its own
control struct busy -- the very flag `FUN_200b5ea4`/`FUN_200b5f38` both poll to decide whether to
keep waiting.

**Confirmed the race with a clean, gdbstub-free experiment rather than a breakpoint at the
suspect addresses**: `dmac.c`'s completion ptimer was still set to its original 1000ns delay.
Under `-icount`, that's short enough that the ptimer callback -- and the guest ISR it triggers,
which correctly clears the same flag -- can run to completion inside the single-instruction gap
between the arm write and the firmware's own busy=1 write. The firmware's delayed busy=1 then
silently overwrites an already-correct completion, permanently (the transfer is genuinely done,
so nothing will ever clear the flag again). Tested by raising `DMAC_COMPLETE_DELAY_NS` alone (no
other change) and re-reading via the same 100%-QMP-only method: at 10ms, flag reads `0x00`
cleanly; dialed back down to confirm a smaller value still works, 100us also reads `0x00`
cleanly, 5/5 trials combined across both values, and (bonus) `RZA1H_DEBUG=dmac`'s log now shows
**two** clean arm/complete pairs (`count=356` then `count=1296`) -- meaning `FUN_200b5f38`'s own
transfer completes too, for the first time. Kept 100us as the committed fix (smaller than 10ms,
still comfortably clear of the race). This is a real, self-inflicted emulation race (real DMA
physically cannot outrun the CPU's own very next instruction the way a sub-microsecond model
under `-icount` can) -- not a GDB artifact, and not the same bug as the original "never fires at
all" `QEMUTimer`-under-icount issue the `ptimer` port fixed. Very likely a genuine regression
introduced *by* that same `ptimer` port (2026-09-09): the original raw-`QEMUTimer` version
predates `-icount` entirely in this project's own timeline, so the race window may simply never
have existed before `-icount` and the `ptimer` port coexisted.

**With the fix in, boot progresses well past this point for the first time since the `ptimer`
port -- straight into the already-known `0x200b93fc` job-ring-overflow trap.** Confirmed via
`functions`/`inspect.listing`: that address is a literal `b 0x200b93fc` (self-branch, a
deliberate infinite trap), not a transient poll location. Reproduced 3/3 fresh trials (all with
`-icount shift=auto`, matching this project's own recommended default), each landing on the trap
somewhere in the ~20-45s boot-time window. **This means the ring-overflow fix's own prior
"confirmed clean, zero overflows across two 130s/160s trials" finding needs a real caveat**: by
this project's own timeline, DMAC channel 0 only ever *actually delivered a real, working
completion* either (a) before `-icount` existed at all, or (b) never, once the `ptimer` port
introduced this race -- meaning no prior "clean" ring-overflow trial ever ran with a genuinely
working DMAC channel 0 *and* `-icount` *and* the OSTM/MTU2 real-clock fix all active together
until this session's fix went in. Whether this trap-hit is the *same* overflow mechanism
re-surfacing under a now-different boot timing profile, or something new the DMAC fix's own
changed scheduling exposed, is genuinely open -- flagged honestly, not solved this session.

**Concrete next step for whoever picks this up next**: trace the `0x200b93fc` ring-overflow
trap fresh, now that it's reliably reproducible (3/3) with a real, working DMAC channel 0 in the
mix for the first time. `tools/trace_job_ring_overflow.py` (wall-clock-cadence polling, already
proven not to mask this class of bug by desynchronizing producer/consumer) is the right starting
tool -- but re-derive the producer/consumer/timing fresh rather than assuming last session's
`0x20420120`-ring analysis still applies unchanged, since the DMAC fix demonstrably changes
timing this deep in boot. `tools/qmp_read_mem.py` (this session, new) is the generally-useful
takeaway tool going forward: any future "this never seems to clear" finding should get a fully
GDB-free QMP cross-check before being trusted, the same way this session's own correction to the
prior "RESOLVED" DMAC finding depended on exactly that.

## 2026-09-10 session, continued: re-derived the `0x200b93fc` ring-overflow trap fresh (now that DMAC actually works) -- same ring, same mechanism, one real new lead

Picked up the resume point left right after the DMAC completion-race fix: `0x200b93fc` (the
already-known job-ring-overflow trap) is now reliably reachable, so re-ran
`tools/trace_job_ring_overflow.py` (unchanged, still targets the `0x20420120` ring from the
2026-09-09 analysis) rather than assuming that prior analysis still applies unchanged.

**Re-confirmed, not just assumed, that this is the same ring/mechanism as before**: reproduced
the overflow trap live (default coarse 0.25s wall-clock polling, no breakpoint near the hot
path), `r0` (the trap's own error code) reads `0x2` and `lr` reads `0x20187c29` -- both match
the prior session's own static derivation exactly (`FUN_20187bb4`'s own overflow-error path,
the `0x20420120` ring's producer). Header samples show the identical burst shape already
documented: `pending` sits at 0-1 for tens of seconds, then jumps to `pending=4` within one
0.25s-polling gap, then fully overflows (`pending=16`) well under half a second later.

**New methodology finding, worth remembering broadly**: tried narrowing in with the tool's own
`fine_start`/`fine_interval` feature (switching to 0.02s polling shortly before the expected
overflow window) -- **this suppressed the overflow entirely** (50s run, zero overflows) even
though the exact same finer-grained technique is just `interrupt()`+`read_memory()`, no
breakpoint at all. This generalizes the project's own prior finding (a breakpoint on the hot
push path masks this bug) one step further: even a *non-breakpoint*, low-overhead periodic
interrupt is enough perturbation to prevent this specific burst, if frequent enough. Coarse
(0.25s) polling was the sweet spot that still let it reproduce.

**Traced why `FUN_20186c4c` (the queue's real producer-side caller) can plausibly receive a
genuine multi-task burst, rather than assuming a single hot loop**: `FUN_20186c4c` itself pushes
exactly one item per call (no loop) -- indexes a per-channel target-function table
(`DAT_20186c70`) by a channel argument and calls `FUN_20187bb4` once. Its own single caller,
`FUN_20186fb4`, is a generic "post a message to a channel" primitive: if not in kernel context,
it invokes `software_interrupt(0)` (an SVC) instead of pushing directly. Only 2 real *direct*
(kernel-context) call sites exist for `FUN_20186fb4` itself (neither yet named/analyzed), but
the SVC path means **any user-mode task anywhere in the system** can reach the same push via a
software interrupt, not just those 2 sites -- a genuinely generic OS primitive, not a narrow one.

**Confirmed this SVC path directly, not just inferred it**: one header sample right at the
burst's own onset (`t=40.654s`, `pending` jumping from 0 to 4) caught `pc=0x200051ec`. That
address was undefined bytes in Ghidra (no disassembly-context ever applied there -- the
project's own known ARM/Thumb Ghidra bug, see the "Known Ghidra/tooling gotchas" entry in
`../.claude/projects/.../icom-ic7300-re-project.md`), so cross-checked directly against
`arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x20005000` on `scratch/unpacked/142/
body.bin`: decodes cleanly and gaplessly as real ARM code from `0x200051c0` (`cps #19` --
switch to SVC mode, the literal entry of an exception vector) through a real `rfeia sp!`
return at `0x20005274`, with a syscall-number-indexed jump table dispatch through `0x204201d0`
in the middle. This is the SVC/software-interrupt exception dispatcher itself -- directly
confirms `FUN_20186fb4`'s `software_interrupt(0)` call really does re-enter the kernel through
here, and that syscall handler registration table is genuinely a many-caller, whole-OS-wide
mechanism (matches "very plausibly graphics/display" activity, already speculated by an earlier
session, being a plausible burst source at this exact boot phase). Filed a fix request in
`scratch/armthumb_fix_requests.txt` (`0x200051c0 0xb8 arm`) so a real Ghidra function can be
created here, plus an `Analysis` bookmark pointing back to this section.

**Not yet done, honest resume point for whoever picks this up next**: this reframes the bug as
"a real multi-task message burst, from a genuinely generic whole-OS syscall path, occasionally
exceeding a fixed 16-slot queue" rather than either a consumer-starvation or an icount-artifact
theory -- but *which* task(s) fire the actual burst, and why specifically around the ~20-45s
boot mark, is still open. Once the ARM-mode fix above is applied (needs the user's own GUI
action, per this project's established Ghidra workaround), the natural next step is identifying
what's scheduled/active right at the burst's own onset -- e.g. a live watch on which tasks are
running via the context-switch path, or checking what boot milestone typically lands around
this exact wall-clock mark (display/EGL surface setup was the leading guess before this
session, still unconfirmed). Given this session's own finding that *any* added observation
(even non-breakpoint polling, fine-grained enough) suppresses the burst, favor the same
low-perturbation coarse-polling technique already proven to work here over anything tighter.
