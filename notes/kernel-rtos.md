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

## Found it: the general-purpose `malloc`/`free`/`realloc`, via `libpng` (15th session)

Chased the `libpng` lead from the previous session's dead end. Found the string references for two known
libpng messages (`"a background color must be supplied..."`, `"Can't set both read_data_fn..."`) landing
in code matching `png_set_background`/`png_set_read_fn`/`png_set_write_fn` exactly (fixed struct offsets
`0x1a0`=io_ptr, `0x198`/`0x19c`=read/write_data_fn, `700`=error_fn — a `png_error()`-shaped function
confirmed at `FUN_200d3e14`, complete with libpng's `"#nnnnnn "` error-code-prefix parsing convention).
Traced from there to `FUN_200d494c` (`png_create_read_struct`, allocates a `0x468`-byte struct, sets
`zbuf_size`/`flags` fields) → `FUN_200ce4d0` (`png_create_struct_2`, builds a template on the stack, then
calls the real allocator) → `FUN_200d488c` (`png_malloc`-shaped: allocate-or-`png_error("Out of memory")`)
→ `FUN_200d4678` (**`png_malloc_base`, confirmed exactly**: checks a user-supplied `malloc_fn` at struct
offset `0x3b4` first, PNG_USER_MEM_SUPPORTED-style, falls back to a platform default) → **`FUN_20186230`,
the platform default allocator**.

**`FUN_20186230` is a real, general-purpose `malloc()`.** Lazy-initializes a free list spanning a genuine
**static heap region, `0x20587b64`-`0x205dcb60` (347,132 bytes / ~339 KB)** on first call, then searches it
via `FUN_20186bc8` — a classic first-fit-with-split free-list allocator (block header = `{size, next}`,
8-byte-aligned). **Confirmed general-purpose, not PNG-specific**: 14 call sites total, spread across
completely unrelated code — a large cluster at `0x2007a000`-`0x2007e000`, another at `0x20144xxx`, plus
internal use from `realloc`. Also found its siblings, completing the full triad:
- **`free()` = `FUN_20184b5c`** — textbook address-ordered free-list insertion with coalescing (merges with
  both the previous and next block if adjacent). Its free-list-head global (`DAT_20184bb4`) and `malloc`'s
  (`DAT_20186278`) are two separate literal-pool copies of the *same* address (`0x20390b00`, verified by
  reading both) — correctly shared state, not a bug.
- **`realloc()` = `FUN_20187228`** — found via a call site right next to the `itron_act_tsk` trampoline
  table from the previous session (`0x20187266`); handles the `realloc(NULL, size) == malloc(size)` case
  explicitly by calling `FUN_20186230` directly.

**Does this resolve the [[multi-cpu-images]] chunk4/5 destination-table mystery?** Checked directly:
**no** — `0x2018d224` (that table's address) falls well outside this heap's bounds (`0x20587b64`-
`0x205dcb60`), so it isn't backed by this allocator. That specific structure is still some other kind of
static/reserved buffer, not explained by finding this malloc. But this is still a major, reusable find:
**any future "is this a dynamically-allocated buffer" question in this firmware now has a real answer to
check against** — read the candidate pointer, see if it falls in `0x20587b64`-`0x205dcb60`, and if so this
whole call chain (`FUN_20186230`/`FUN_20186bc8`/`FUN_20184b5c`/`FUN_20187228`) is exactly what produced and
manages it.

## Systematic search for remaining ARM/Thumb disassembly-context bugs (17th session)

User's idea: rather than stumbling on these one at a time, use Ghidra's own analysis-time error markers to
enumerate every candidate at once, then check which are still actually broken before asking for any GUI
fixes. Ghidra auto-creates a bookmark (`type: Error, category: Bad Instruction`) every time its analyzer
hits a disassembly conflict — `annotate.list_bookmarks` surfaces these directly, no guessing needed.

**Enumerated all of them**: 139 total `Bad Instruction` bookmarks across the whole program. 130 carry the
exact phrase `"possibly due to inconsistent context"` — literally Ghidra's own diagnosis of the ARM/Thumb
context-propagation bug already known from `reset_handler`/`swi_handler`/`base_dat_reset_vector_target`
(see [[base-loader]]). Clustering these 130 by address proximity (gap > `0x800` = new cluster) collapses
them to **23 distinct regions** — a single confused function generates many bookmarks, one per flow into
it, not one bug per bookmark. The remaining 9 are 4×`"Unable to resolve constructor"`, 1×`"non-existing
memory"`, 4×`"conflicting instruction/data"`.

**Checked all 32 candidates (23 clusters + 9 others) against `objdump` ground truth, not just the bookmark
list.** This mattered: a bookmark only records that a conflict happened *during* analysis, not that the
*current* disassembly is still wrong — Ghidra can (and mostly did) self-resolve to the correct
interpretation despite the historical conflict. **31 of 32 already match `objdump`'s ARM decode exactly** —
these are stale markers, not live bugs, and not worth any further attention.

**Found exactly one still-genuinely-broken spot**: `0x200054c8`-`0x200054e7` (32 bytes), the source of the
`"non-existing memory"` bookmark at `0x200054d6`. Sits immediately after `reset_handler`'s own `bx lr`
(`0x200054c4`) — a separate function, not `reset_handler` itself. Ghidra currently shows this span as
undefined bytes followed by one badly-misdecoded instruction (`bl 0x20d074da` — a branch target far outside
the entire 10 MB RAM window, an immediate tell). The real ARM decode (verified via `objdump`, both as
straight ARM and cross-checked against forced-Thumb decoding of the same bytes to make sure ARM was
genuinely the better fit) is a small, complete, sensible routine:
```
mrs  r1, CPSR
cps  #0x1f          ; switch to System mode
mov  sp, r0          ; set System-mode stack pointer (from param in r0)
msr  CPSR_c, r1      ; restore original mode
isb  sy
bx   lr
cps  #0x10          ; switch to User mode
bx   lr
```
**A per-CPU-mode stack-pointer setter** — sets up the System-mode SP (and has a User-mode entry point
too), classic early-boot stack initialization, called from somewhere near `reset_handler`/`mmu_init_body`.
Disassembly correctly resynchronizes on its own right at `0x200054e8` (`cps #0x10`, matches `objdump`
exactly), so the broken span is exactly these 32 bytes, nothing more. **Worth the user forcing ARM
disassembly over `0x200054c8`-`0x200054e7` via the GUI** — small, cheap fix, and it's genuine boot-sequence
detail (which mode gets its own stack, and from where) not yet in [[base-loader]] or here.

**Honest bottom line**: this systematic sweep mostly closes out the "how many more ARM/Thumb bugs are
lurking" question rather than opening up new mysteries — 138 of 139 flagged spots are already fine. One
small, real fix remains, and it's a nice, self-contained boot-sequence detail rather than anything touching
the open `chunk4`/`chunk5`/DSP mysteries elsewhere in this project.

## Fixed and confirmed: `set_sys_mode_stack_pointer` / `enter_user_mode` (18th session)

User forced ARM re-disassembly over `0x200054c8`-`0x200054e7` via the GUI. Confirmed clean: every
instruction now matches the `objdump` prediction from the previous session exactly, no more undefined
bytes or garbled branch.

**One small refinement**: `0x200054c8` (`0x00086060`) itself is not part of the new function — it's a
literal-pool constant belonging to the *preceding* function (`FUN_20005458`, a VFP-register-clear/FPSCR-
mask routine that loads it via `ldr r3,[0x200054c8]` as an FPSCR mask), which merely happens to also decode
as a valid-looking `andeq r6,r8,r0,rrx` instruction by coincidence — classic "literal pool sitting right
after a function's `bx lr`" artifact, harmless but worth not misreading as real code. The genuine function
starts at `0x200054cc`.

**Named and typed both halves**:
- **`set_sys_mode_stack_pointer(void *sp_value)`** (`0x200054cc`) — switches to System mode, sets the
  System-mode banked SP to `sp_value` (8-byte aligned), restores the caller's original mode, ISB. Real
  early-boot stack initialization.
- **`enter_user_mode(void)`** (`0x200054e8`, previously `FUN_200054e8`) — switches to User mode and
  returns. **Found its real caller**: `FUN_20186d58` — already known from the very start of this file as
  one of the three calls `FUN_20005298` makes right after `itron_act_tsk` activates the first task
  (`FUN_20186d2c(); itron_act_tsk(...); FUN_20186d58();`). Decompiling `FUN_20186d58` shows it checks the
  current CPU mode and a "scheduler started" flag, and calls `enter_user_mode` specifically when already in
  System mode with that flag clear — **this is the scheduler-start privilege drop**, the actual moment
  execution transitions from privileged boot code into the first task. Ties `set_sys_mode_stack_pointer`/
  `enter_user_mode` directly into the already-documented early-boot call chain rather than leaving them as
  an isolated fix.

Both functions commented (PLATE) and left renamed/typed in the live Ghidra project for future sessions.

## Boot-chain completeness audit (19th session) — traced through to the real first task, one gap identified

User asked "how complete is our understanding of the boot process" while chasing what eventually brings
the DSP out of reset (see [[ic7300-signal-chain]]'s `DRESD` finding). Walked the whole chain end-to-end
against the live Ghidra project rather than trusting prior notes at face value — found and fixed one
analysis artifact, and disassembled a genuinely never-before-analyzed function (the first task's actual
body). Full chain, current state:

1. **Boot ROM / `base.dat`** (flash, XIP) — ✅ solid, see [[base-loader]]: MMU setup, SPI flash bring-up,
   LZSS-decompress the chosen body variant to RAM at `0x20005000`, `VBAR` repoint, jump in.
2. **`body.bin`'s own vector table → `reset_handler`** — ✅ solid: cache/branch-predictor cleanup,
   `mmu_init_body()`, `FUN_2002b878()` (a port-register read/modify/write sequence, not yet individually
   named — minor, low-priority gap), then two indirect calls resolved this session:
   `DAT_200050c4` → `thunk_FUN_200b8690` (a tiny thunk running `FUN_200b848c()`/`FUN_200b8630()` — some
   pre-kernel hardware bring-up, not decompiled further; low priority) and `DAT_200050c8` → **`kernel_start`
   itself** (`0x20005290`) — confirms `reset_handler`'s last act really is handing off into the sequence
   below, closing what was an open item as of last check. Never returns.
3. **`kernel_start` (`0x20005290`)** — ✅ now fully re-verified against the raw ARM listing (it's plain
   ARM code, `blx` calls throughout, cleanly disassembled — no ARM/Thumb confusion here). **Found and
   fixed a Ghidra artifact**: a spurious second `Function` object, `FUN_20005298`, existed starting at
   `kernel_start`'s *second instruction* — not a real function at all, just an analyzer mis-split (PLATE-
   commented at `0x20005298` explaining this, `kernel_start` itself cross-references it). The real,
   single, unbroken sequence, with real literal values pulled directly from the listing (previously only
   named, never dumped):
   ```
   sp = *0x200052b4                  ; = 0x205DCC60 -- the initial kernel boot stack pointer
   run_ctors_and_start_kernel()        ; walks a C++ static-ctor table (0x2017cd14-0x2017cd18,
                                        ; {arg0,arg1,arg2,fnptr} per entry) -- confirms a real
                                        ; C-runtime-init step DOES exist, answering an until-now
                                        ; unasked question ("is there a ctor/BSS-init stage at all?")
   FUN_20186d2c()                       ; kernel/RTOS bootstrap (static heap-region setup, etc.
                                        ; -- already documented above, 14th session)
   r0 = *0x200052b0                     ; = 0x203907C4 -- the first task's ID/descriptor slot,
                                        ; runtime-populated (confirmed blank/0xff in the static
                                        ; image, per this file's "Open questions" #3)
   itron_act_tsk(r0, 0)                  ; activate the first task
   FUN_20186d58()                         ; scheduler-start dispatch -> enter_user_mode() (18th
                                          ; session, privilege drop into the first task)
   b .                                    ; halt loop, unreachable once the first SWI context
                                          ; switch fires
   ```
   **No BSS-zero step was found separately** — worth flagging honestly rather than assuming: either it's
   folded into the ctor-table walk (plausible, some `libgcc`/startup variants do this), or `body.bin`'s
   decompressed image already has BSS pre-zeroed as shipped (this codebase's whole image comes from a
   single LZSS-decompressed blob, not a separate flash-to-RAM copy + zero-fill, so "already zero" is a
   perfectly reasonable outcome here, not a specific gap needing more searching) — not chased further this
   session, low priority.

4. **The first task's actual body — ✅ NEW, disassembled for the first time this session.** The task
   descriptor at `0x2033605c` (`entry_point=0x201871f0|1` (Thumb), `priority=2`, `flags=1`) pointed at an
   address Ghidra had *never analyzed at all* — still raw undefined bytes despite being a live,
   runtime-reached entry point (a genuine analysis gap, same general family as the ARM/Thumb
   disassembly-context bugs elsewhere in this project, though this one is "never disassembled" rather
   than "mis-disassembled"). Created the function (`first_task_entry`) and decompiled it — a clean,
   generic **ITRON/FreeRTOS message-dispatch loop**, not itself the hardware-init code:
   ```
   loop:
     msg = queue_receive(*puRam20187224, INFINITE)     ; FUN_20186de4 (itron trampoline) ->
                                                         ; FUN_20186c74, a real ring-buffer-backed
                                                         ; queue-receive implementation
     if status != RECEIVED: retry
     handler = msg_dispatch_lookup_trampoline(msg.param) ; 0x2018711c -> resolve_and_invoke_msg_callback,
                                                          ; which validates the message record
                                                          ; (validate_msg_record), checks a
                                                          ; "has-registered-callback" flag, and
                                                          ; resolves the callback pointer
     if handler != NULL: handler(msg.param)
   ```
   This is a real, previously-undocumented piece of the boot picture: **the first task isn't the hardware-
   init code itself — it's a generic dispatcher that waits for messages and routes them to registered
   handlers.** `*puRam20187224` (the queue handle) is itself runtime-populated (holds `0x203907d8` at the
   point checked), consistent with everything else in this codebase that needs a fresh per-boot system
   table.

5. **The still-open gap**: `FUN_2002b29c` (the cold-boot-vs-power-state dispatcher that leads to
   `FUN_2002afc0`'s hardware init — `port_bulk_gpio_init_pass2`, the `DRESD`/DSP-reset write, etc. — see
   [[ic7300-signal-chain]]) has **zero direct callers anywhere in `body.bin`**. Given the dispatch
   mechanism just found in step 4, the obvious reading is that it's registered as a callback and reached
   through exactly this message-dispatch loop, not called directly — but **which message ID reaches it,
   and who posts the very first message that kicks off the rest of the boot sequence, hasn't been found
   yet.** This is now the concrete, well-scoped next step for "how does execution get from the scheduler
   starting to real hardware init running" — and, per the reason this audit started, it's also the most
   likely place to eventually find whatever (if anything, in software) releases the DSP from reset.

**Honest completeness verdict**: the chain from power-on through to the scheduler running its first task
is now solid and gap-free (steps 1-4). The one real, unresolved link is step 5 — a classic "callback
registered somewhere, dispatched by ID, static analysis can't easily enumerate the ID→handler table
without either finding the registration call sites for every handler or getting live/JTAG visibility into
the queue's actual traffic." Two small, low-priority loose ends noted along the way (`FUN_2002b878`'s
purpose, and confirming what `DAT_200050c4`/`DAT_200050c8` point at) — neither blocks the picture above,
both cheap to close out if picked up.
