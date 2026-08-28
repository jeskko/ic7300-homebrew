# Main-CPU kernel/RTOS

Traced from `body.bin` (rebased to `0x20005000`, see [[base-loader]]),
starting from `kernel_start` (`0x20005290`, called from
`reset_handler`) through `swi_handler`, the SWI/SVC exception vector.

## Confirmed: a real, ITRON-shaped preemptive kernel

- **Call convention**: kernel calls check the caller's current ARM mode
  (`get_cpsr_mode`, `0x20184bb8` — `mrs r0,cpsr; and r0,r0,#0x1f`). If in
  User mode (`0x10`), trap via `software_interrupt(0)`; if already in a
  privileged mode, call the kernel function directly — skips trap overhead
  for kernel-to-kernel calls. Every traced wrapper (`itron_act_tsk` and
  its siblings) follows this exact pattern.
- **SWI(0) is the general kernel-call trap.** A separate SWI-immediate
  indexed table exists (base+bound both resolve through
  `0x20184850`, `*(uint*)0x20184850 == 1`) but has only **one** real
  entry (`0x200b940c`) — a narrow special case, not the main mechanism.
  Almost everything goes through SWI #0, which internally decides the
  actual operation some other way (not yet pinned down — likely a
  function-code value passed in a register/stack slot before the trap).
- **`swi_handler` (`0x20005050`+8*4... entry at `0x200056dc`,
  ARM-mode — see below) implements a full context switch**, not just a
  syscall dispatch:
  - Compares a pointer pair at `0x20005954` (current vs. next task
    context — a scheduler ready-queue pointer pair)
  - `coproc_moveto_Context_ID(...)` — writes ARM CP15's Context ID
    (ASID) register — **confirms per-task memory-context tagging**, i.e.
    genuine preemptive multitasking with MMU-level task isolation, not a
    cooperative/single-context loop
  - Reads/masks/writes `Coprocessor_Access_Control` around the VFP/NEON
    enable bits (`0xf00000` = CP10/CP11) — **lazy FPU context save**:
    only saves VFP state if the outgoing task actually used it
  - Ends with an indirect call through the new task's saved context —
    the actual switch-to-next-task jump
- Confirms exception vectors execute in ARM state even where Thumb code
  exists elsewhere in the binary (architectural guarantee, not
  implementation choice) — relevant if more vector-adjacent code needs
  the same ARM-vs-Thumb disassembly-context fix `reset_handler` and
  `swi_handler` both needed (see [[base-loader]]).

## RTOS identity: CONFIRMED — FreeRTOS, Renesas RZ/A1H port, with real customizations

Resolved by direct comparison against
`scratch/r01an5093ej0170-rza1-swpkg/` (Renesas application note
R01AN5093, "RZ/A1H Software Package"), which bundles a full FreeRTOS
port at `gcc/RZA1H_SoftwarePackage/RZA1H_Sample/src/freertos/portable/gcc/arm_ca9_rza1h/`
(`portasm.S`, `port.c`, `portmacro.h`). Multiple independent, exact
matches against our decompiled `swi_handler`:

- **GIC register addresses**: the port's `INTC_ICCIAR_ADDR = 0xE820200C`
  / `INTC_ICCEOIR_ADDR = 0xE8202010` fall exactly inside the
  `0xE8200000`–`0xE822FFFF` I/O region already in [[memory-map]].
- **Lazy FPU/VFP save**: the port's `ulPortTaskHasFPUContext`-gated
  `VPUSH{D0-D15}`/`VPUSH{D16-D31}`/`FMRXNE FPSCR` sequence matches our
  handler's `Coprocessor_Access_Control`-gated VFP save exactly.
- **Overall shape**: `portSAVE_CONTEXT` → `vTaskSwitchContext()` →
  `portRESTORE_CONTEXT` matches our handler's structure (compare
  current-vs-next TCB pointer, then jump into the new task).
- **`portAPSR_USER_MODE == 0x10`** in `port.c` is the exact constant
  `get_cpsr_mode`'s callers compare against.

**Icom customization identified, not stock behavior:** in the reference
port, the `0x10`/User-mode check happens exactly once, as a startup
assertion in `xPortStartScheduler()` (`configASSERT(ulAPSR !=
portAPSR_USER_MODE)`) — ordinary FreeRTOS API calls (`xQueueSend`,
`vTaskDelay`, etc.) are plain C function calls, no trap involved. Our
firmware instead repeats the "check mode, `SWI(0)` if User /
call-direct if privileged" pattern across multiple distinct wrapper
functions (`itron_act_tsk` and its siblings — misleading names now,
kept for continuity, should probably be renamed once the real FreeRTOS
task-API correspondence is confirmed). Combined with the
`coproc_moveto_Context_ID` (ASID) tagging found in `swi_handler`, which
the stock port also doesn't do: **Icom appears to have extended this
base port with real unprivileged user-mode task support and per-task
MMU memory-context isolation** — a genuine, non-trivial modification
beyond the Renesas reference sample, not something to assume away.

**Version caveat — the bundled package is newer than what Icom likely
used.** `scratch/r01an5093ej0170-rza1-swpkg/` (from
https://www.renesas.com/en/software-tool/rza1h-freertos-software-package)
bundles **FreeRTOS Kernel v10.6.1** (`manifest.yml`/`History.txt`,
released August 2023). The IC-7300 first shipped ~2016, roughly 7 years
earlier — likely built against FreeRTOS ~8.x/9.x for this port, not
v10.6.x. Concretely: `History.txt` shows v10.6.0 (July 2023) introduced a
"new MPU wrapper" with opaque kernel-object handles, **task context saved
in the TCB instead of on the task's own stack**, and syscalls running on
a **separate privileged-only stack**. Our `swi_handler` does none of
that — it pushes context onto the *current* stack, the older/classic
`portSAVE_CONTEXT` style — which is actually corroborating evidence
Icom's build predates that overhaul, not a reason to doubt the match.
**Trust the stable, hardware-fixed and long-unchanged parts of this
comparison (GIC addresses, overall save/switch/restore shape, VFP lazy
save) — don't assume exact struct layouts/handle representations match
without finding an older, contemporaneous package.**

Now that the base is identified, `scratch/r01an5093ej0170-rza1-swpkg/`'s
`tasks.c`/`queue.c`/`semphr.h`/etc. give real FreeRTOS symbol names and
struct layouts to match against — much faster than guessing from
scratch. Worth re-deriving `PTR_DAT_20005954`'s exact role (currently
read as a pointer *pair*, whereas stock `pxCurrentTCB` is a single
pointer — possibly Icom added a small before/after snapshot struct
around the `vTaskSwitchContext()` call) before renaming further kernel
globals, rather than assuming a 1:1 stock layout.

## Prior (superseded) note: RTOS not found by name, despite a thorough string search

Searched (all case-insensitive, whole `body.bin`): `TOPPERS`, `ITRON`,
`T-Kernel`, `TRON`, `eSOL` — **zero hits** for any of them as an isolated
RTOS-name string. `Copyright` and `kernel` string hits all turned out to
be font-library false positives (Unicode glyph names in a `post` table,
X11/BDF font-property names like `POINT_SIZE`/`PIXEL_SIZE`) — nothing
RTOS-related.

**One concrete, actionable finding instead: `"RENESAS RZ/A1 SD Driver
Ver4.01"`** at `0x201812e0` — Icom used Renesas's own official reference
SD-card driver (part of a Renesas RZ/A1 BSP/FIT software package) as the
low-level driver behind the SD-card update flow ([[firmware-update]]).
This is the *only* versioned/named component string found anywhere in the
image. Renesas distributes these BSP/FIT packages publicly (often
permissively licensed) — **worth searching Renesas's downloads/GitHub for
a RZ/A1H software package containing this exact driver version** for
direct source-level comparison. Whether that package also bundles a named
RTOS component is unknown — worth checking if pursued.

Also found nearby (same general area, `0x200fdd90`): a string referencing
`"Renesas Electronics"` and an **OpenVG** (Khronos 2D vector graphics)
component — relevant to the display/GUI subsystem, not the kernel;
noting here since it turned up in the same search pass.

## Open questions
1. ~~How SWI(0) actually selects which kernel operation to run~~ —
   resolved: it isn't a syscall-number dispatch at all, `SWI(0)` is
   FreeRTOS's yield/reschedule trap. The `itron_act_tsk`-named wrappers
   are Icom's own "elevate to privileged mode, call this FreeRTOS API
   function directly" trampolines, not per-syscall dispatch — rename
   candidates once individual wrapped APIs are identified.
2. ~~Whether the Renesas RZ/A1 BSP/FIT package reveals the bundled
   RTOS~~ — resolved, see above: FreeRTOS, confirmed by direct source
   comparison.
3. No static task list recoverable — task control blocks are
   runtime-populated (confirmed blank/`0xff` in the static image at the
   one candidate pointer checked, `0x203907c4`). With FreeRTOS confirmed,
   `xTaskCreate` call sites (real signature now known from
   `scratch/r01an5093ej0170-rza1-swpkg/.../freertos/tasks.c`) are a much
   more promising way to enumerate tasks than the table-search approach
   tried earlier.
4. New: confirm `PTR_DAT_20005954`'s exact role/layout against
   `pxCurrentTCB`'s real usage in `tasks.c` before renaming further
   kernel globals — see "RTOS identity" section above.
5. New: `itron_act_tsk` and siblings are misleadingly named now that
   ITRON isn't the right frame — worth renaming once each one's wrapped
   FreeRTOS API is identified by comparing call-site argument shapes
   against `task.h`/`queue.h`/`semphr.h` signatures.

## Chased `pvPortMalloc`, didn't find it — but mapped the ITRON trampoline table and a real static pool (14th session)

User's idea: [[multi-cpu-images]]'s `chunk4`/`chunk5-tail` destination-table mystery involves a
dynamically-populated RAM pointer — if `pvPortMalloc` (or Icom's equivalent) could be found and traced
statically, it might resolve that without needing JTAG. Found the reference implementation
(`heap_5_renesas.c`, in `scratch/r01an5093ej0170-rza1-swpkg/`) has real Renesas customizations
(`R_OS_InitMemManager()` lazy-init, an `xDesiredBlock` allocation-hint global not in stock FreeRTOS) — good
anchors in principle. **Did not find a general-purpose heap allocator matching it.** Recording what was
actually found, since it's a real (if partial) map of adjacent territory:

**All 19 `itron_act_tsk`-sibling trampolines identified and decompiled** (the full cluster sharing
`in_kernel_context`'s call pattern, `0x20186d0c`-`0x201871d0`). Most (11 of 19) are **unconditional stubs**
— they return a fixed error code (`0x82` in most cases) whenever called from kernel context, with no real
implementation at all in this build; only 8 have a genuine inner function. Of those 8:
- `itron_act_tsk` → `FUN_201888f4`: **confirmed real task-activation**, using a **static, compile-time
  task descriptor** (`0x2033605c` = `{entry_point=0x201871f0 (Thumb), priority=2, flags=1,
  stack_size=800}`) and a small fixed-size ID→TCB lookup table (`DAT_20188980`), not dynamic allocation.
- `FUN_20188650` (wrapped by `FUN_20186da4`, also called directly from kernel bootstrap): lazy-init a
  buffer described by a small config struct (count + buffer pointer), format it, return the buffer pointer
  as a handle — reads as queue/message-buffer creation (`cre_mbf`-shaped), not a general allocator.
- `FUN_20188726` (wrapped by `FUN_20186e98`): same shape as `FUN_20188650`, not yet distinguished further.
- `FUN_20188a54` (wrapped by `FUN_2018713c`, 3 params — the best malloc-shape candidate before checking):
  turned out to be a **callback/wait-object registration** primitive (checks a global "system ready" flag,
  writes a type tag + two stored params into a small record) — not memory allocation.
- `FUN_20186c18`, `FUN_201880c8`, `FUN_20186ce8`/`FUN_20187696` (message-buffer receive-shaped, via
  `FUN_20186f08`) — not examined in enough depth to rule in or out, lowest-priority remaining candidates.

**Found a real, static memory pool, just not its allocator.** Traced the kernel bootstrap
(`kernel_start` → `FUN_20005298` → `FUN_20186d2c` → `FUN_20188574`, the one-time lazy-init routine matching
`R_OS_InitMemManager`'s "if not yet initialised" shape) to `FUN_201876f0` — a genuine **region-descriptor
setup** call (checks 8-byte alignment, writes an end-of-region marker) matching `vPortDefineHeapRegions`'s
per-region logic, called with **static, compile-time constants**: base `0x20416198`, size `0x9f88`
(40,840 bytes). Unlike the chunk4/5 destination table, **this address is a real constant in the image, not
a runtime-populated placeholder** — a genuinely useful, concrete fact. But `references_to` on both the raw
address and the global holding it turn up **nothing else** — no allocator function anywhere in the analyzed
code touches this pool again after its one-time setup. Either the actual allocate/free calls reference it
indirectly (an ID/handle-based lookup, matching real μITRON `get_blk(ID mplid, ...)` semantics — plausible
given task activation above already uses ID-indexed tables, not raw addresses) or they're simply not among
the 19 trampolines checked here.

**Honest conclusion**: this firmware's ITRON-flavored kernel-object layer (tasks, and apparently
queues/message-buffers) leans on **static, compile-time-configured descriptors**, not a general-purpose
runtime heap — consistent with normal real-time embedded practice, but meaning it likely **isn't** what
populates `multi-cpu-images`' chunk4/5 destination-pointer table. That table's populator is probably
application-level code with its own buffer management (or a genuinely different allocator not among these
19 candidates), not this kernel bootstrap layer. **Doesn't (yet) let live/JTAG analysis be postponed** for
that specific question, but the static pool address/size and the trampoline map are solid, reusable facts
for whoever picks this up next — particularly if a stronger malloc-candidate turns up (e.g. via `libpng`'s
required user-malloc callback, confirmed present via `"libpng version 1.6.12"` in strings — PNG decoding
*must* call some allocator, and it would show many more call sites than these narrow kernel-bootstrap
paths, worth checking before further guessing here).
