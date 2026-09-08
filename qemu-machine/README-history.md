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
