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
3. ~~No static task list recoverable~~ — **partially resolved, 23rd session**: a static list of task
   *activation call sites* (not TCBs) IS recoverable, via `itron_act_tsk`'s 11 direct call sites (see the
   "Full boot-time task catalog" section below) — this is a different, more useful list than the
   originally-envisioned TCB search, though the *TCBs themselves* remain runtime-populated as originally
   found (`0x203907c4` still blank/`0xff` in the static image).
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
   r0 = *0x200052b0                     ; = 0x203907C4 -- a runtime-populated descriptor slot
                                        ; (confirmed blank/0xff in the static image, per this
                                        ; file's "Open questions" #3)
   itron_act_tsk(r0, 0)                  ; activate a task -- **correction, 24th session: NOT
                                          ; first_task_entry, see below** -- this slot's real
                                          ; task is still unidentified
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

   **Correction (24th session)**: the text above previously implied `first_task_entry` is activated by
   `kernel_start`'s own `itron_act_tsk(r0, 0)` call (step 3). **That's wrong.** Prompted by a direct question
   ("did we miss any tasks?"), checked `references_to` on `FUN_201888f4` (`itron_act_tsk`'s real inner
   function) directly, not just on the `itron_act_tsk` trampoline — found exactly 2 callers: the trampoline
   itself, and `FUN_20188574` (the kernel bootstrap's own lazy-init routine, called from `FUN_20186d2c`,
   i.e. **earlier** in `kernel_start`'s sequence than its own `itron_act_tsk` call). `FUN_20188574` calls
   `FUN_201888f4` **directly, bypassing the trampoline entirely** (it already runs in a privileged context,
   so it skips the trampoline's mode-check/SWI-trap logic) — with argument `0x2033605c`, confirmed by
   reading the literal directly: **this is `first_task_entry`'s own descriptor.** Same function also
   creates, in the line right before, the message queue `first_task_entry` waits on (`FUN_20188650`,
   writing the new queue handle to `0x203907d8` — the exact address `*puRam20187224` resolves to at
   runtime, confirmed by reading both literals) — a clean, closed loop: **`FUN_20188574` both creates
   `first_task_entry`'s queue and activates `first_task_entry` itself, in that order, before `kernel_start`
   reaches its own `itron_act_tsk` call.** `kernel_start`'s own call therefore activates some *other*,
   still-unidentified task via the runtime-populated slot `0x203907C4` — genuinely open, not
   `first_task_entry` as previously written. This also means the "11 call sites" count from the 21st
   session (verified exhaustively for calls to the *trampoline*) was never claimed to cover direct
   inner-function calls — checked that gap now too: `FUN_201888f4` has only the 2 callers just described,
   so no further direct-call activations are being missed.

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

## `FUN_2002b29c`'s real caller found and confirmed (20th session)

Followed up on step 5 above. **`references_to` was blind here for the same reason it's been blind
elsewhere in this project**: the caller was never disassembled by Ghidra at all, so no xref existed to
find. Fell back to the project's established `objdump`-ground-truth technique (`arm-none-eabi-objdump -D
-b binary -m arm --adjust-vma=0x20005000`, plain ARM mode, over the full `body.bin`) and grepped for
`2002b29c` as a branch target across the whole image — found a real `bl 0x2002b29c` at `0x2003bb70`,
inside a small, complete, sensible-looking ARM function:

```
2003bb48: blx 0x201870ac      ; call, store result into *0x2039020c
2003bb54: mov r4, #0
2003bb58: ldr r5, [0x2039030e]; ...
2003bb5c: strb r4, [r5]        ; *0x2039030e = 0
2003bb60: mov r7, #1
2003bb64: ldr r6, [0x20390311]
2003bb68: strb r4, [r6]        ; *0x20390311 = 0        <- loop target
2003bb6c: bl 0x20062c64         ; (probably a wait/yield primitive, not yet examined)
2003bb70: bl 0x2002b29c          ; *** the call we were chasing ***
2003bb74: strb r7, [r5]          ; *0x2039030e = 1
2003bb78: b 0x2003bb68            ; loop forever
```

This loop is entered via an unconditional jump (`b 0x2003bb48`) from `0x200b950c`, which is itself the
tail of a function starting around `0x200b94e8` that — per the same raw-disassembly reading — writes two
entries into what looks like a small ID-indexed handler table (via a bounds-checked "`table[id] = ptr`"
helper at `0x200b9490`: entry `id=0 -> 0x20005960`, entry `id=0x86 -> 0x200059b4`), then executes `svc
0x1` (a different SVC immediate than the usual `SWI(0)` reschedule trap — matches this file's earlier
note about a separate, narrow SWI-immediate-indexed table with "only one real entry"), then falls straight
into the loop above.

**Confirmed by the user forcing ARM disassembly via the GUI over the two affected ranges** (same fix as
`reset_handler`/`swi_handler`/`set_sys_mode_stack_pointer` before) — both now decompile cleanly and match
the hand-derived reading exactly, no surprises:

- **`sys_monitor_task_entry`** (`0x200b94e8`, renamed from the placeholder `sys_monitor_task_setup_probe`)
  — a real task, statically activated via the descriptor at `0x20361318`
  (`{entry_point=0x200b94e8, priority=3, flags=1, stack_size=0x800}`):
  ```c
  FUN_200b9490(0, DAT_200b968c);      // handler-table[0]    = 0x20005960
  FUN_200b9490(0x86, DAT_200b9690);   // handler-table[0x86] = 0x200059b4
  software_interrupt(1);               // SVC #1 -- the "one real entry" in the separate
                                        // SWI-immediate-indexed table this file's "Confirmed:
                                        // a real, ITRON-shaped preemptive kernel" section
                                        // already documents (base+bound via 0x20184850)
  sys_monitor_task_loop();              // tail-call, never returns
  ```
- **`sys_monitor_task_loop`** (`0x2003bb48`, renamed from `sys_monitor_task_loop_probe`) — the task's main
  body, confirmed as a genuine unconditional loop:
  ```c
  do {
      select_active_slot_resources();  // FUN_20062c64 -- re-checks the "SX3765 V1.00-003"
                                        // literal (the SAME string base.dat's own boot-time
                                        // A/B slot picker checks, see [[base-loader]]) and
                                        // refreshes a couple of resource-pointer pairs
                                        // accordingly -- an application-level echo of the
                                        // boot loader's own slot-selection logic, not
                                        // previously connected to anything at this level
      FUN_2002b29c();                   // the cold-boot/power-state dispatcher itself
  } while (true);
  ```
  (`FUN_201870ac()` at the loop's top, which the earlier hand-reading flagged as an unknown "wait/yield"
  call, turned out to be trivial: just the usual kernel-context check + `SWI(0)` trap if not privileged,
  no actual wait — so `FUN_2002b29c` really is called **unconditionally, every single loop iteration**,
  not gated on any queue/message content.)

**Net result**: `FUN_2002b29c` — and everything downstream of it (`FUN_2002b1c8`, `FUN_2002afc0`,
`port_bulk_gpio_init_pass2`, the `DRESD`/DSP-reset write — see [[ic7300-signal-chain]]) — runs inside its
own small, dedicated, statically-activated **system monitor task**, not via the message-dispatch loop
`first_task_entry` implements.

## Chasing the two remaining loose ends (21st session)

**Loose end 2 resolved at the mechanism level: `FUN_200b9490` is a generic, ~50-call-site-wide event/ISR
handler registration primitive, not anything specific to the system monitor task.** Renamed
`register_event_handler(event_id, handler_ptr)` — bounds-checked write into a global `id`-indexed table
(bound `DAT_200b9684`, table base `DAT_200b9688`). `references_to` turned up **49 call sites** (with more
beyond the first page) spread across the entire firmware — including, tellingly, the already-documented
front-panel UART init (`FUN_20036ee8`, registers IDs `0xe9`-`0xec` alongside `FUN_200b83d0`/
`FUN_200b8244`/`FUN_200b8308`/`FUN_200b8328` — the *same* generic RTOS event-flag utilities this project
already found gating the front-panel driver and the `chunk4`/`chunk5` ring-buffer consumer, see
[[multi-cpu-images]] and [[ic7300-signal-chain]]). This is clearly the shared event/ISR-dispatch
infrastructure underlying a large fraction of this firmware's driver architecture — `sys_monitor_task_entry`
registering IDs `0`/`0x86` is just two more ordinary entries in the same table, nothing exotic. Sanity-
checked one of the UART's own registered handlers (`0x20036c68`, target of `DAT_200375ac`) — it's real,
sensible ARM code (`stmdb sp!,{r3,r4,r5,r6,r7,lr}`, a normal handler prologue) that **also isn't yet
defined as a full Ghidra `Function`**, for the same structural reason as our two targets: reachable only
through this table, invisible to call-graph-based function discovery. This substantially de-risks the
earlier worry about `0x20005960` specifically — it's very likely just an ordinary small handler in this
same widespread pattern, not something uniquely dangerous to disassemble; still not force-fixed this
session (no new information changed the specific "sits right next to `swi_handler`'s literal pool" caution
from before), but the risk read is now better-calibrated: low, not "genuinely uncertain."

**Loose end 1, exhaustively checked, ends in a real (not lazy) static-analysis dead end.** Cross-validated
three independent ways that `itron_act_tsk` (`0x20187044`) has **exactly 11 real call sites in all of
`body.bin`** — Ghidra's own `references_to`, an `objdump` ARM-mode grep, and an `objdump` Thumb-mode grep
all agree on the same 11 addresses, no more. Checked every single one's first argument: none resolves,
statically, to the system monitor task's descriptor (`0x20361318`) — most are direct `DAT_` literals
pointing at a *different* cluster of descriptors (`0x2019 88xx`-`0x2019 89xx` range, several distinct
application tasks including the UI/display task `FUN_2007ef5c` and one more at `0x200b9c00`), one is
`kernel_start`'s own single-purpose runtime-populated slot (`0x203907C4`, already known to be blank in the
static image), and one (`FUN_2007ea68`/`thunk_FUN_2007ea68`) takes a caller-supplied pointer whose own
caller **also has zero references anywhere, in either ARM or Thumb disassembly** — the same invisible-
caller situation `FUN_2002b29c` was in before the GUI fix, except here there's no `bl`/`blx` instruction
anywhere in the image to even point at (checked via the same `objdump` grep technique, clean negative).
**Honest conclusion**: this task's activation is reached through either a computed/register-indirect branch
(not a plain immediate `bl`, so invisible to text-based disassembly search entirely) or a genuinely
different, not-yet-identified creation primitive — not a disassembly-context bug like the one that blocked
`FUN_2002b29c`'s caller, and not fixable by the same "force ARM disassembly at address X" technique, since
there's no single address to fix. Matches this file's own long-standing, already-accepted position that
task/TCB activation is a real static-analysis dead end elsewhere too (see "Open questions" #3 above) —
correctly a low-priority loose end, not worth further static effort; would resolve immediately with live
JTAG visibility into the one dynamic call site.

This closes the "boot-chain completeness audit" section's one open gap for practical purposes: the full
path from power-on through the scheduler starting, into the system monitor task, into `DRESD` being
driven low, is now traced end-to-end.

## Full boot-time task catalog (23rd session) — answering "have we looked at all tasks?"

Answer going in: **no** — only 3 of at least 11 statically-locatable tasks had been examined
(`first_task_entry`, the UI/display task, `sys_monitor_task_entry`). Enumerated the rest properly.

**Every `itron_act_tsk` call site found (11 total, exhaustively confirmed in the 21st session via 3
independent methods — see above)**, each with its task descriptor read directly from RAM
(`{entry_point, priority, flags, stack_size}`, same 16-byte shape throughout):

| Caller | Descriptor | Entry point | Priority | Stack | Status this session |
|---|---|---|---|---|---|
`FUN_20188574` (kernel bootstrap, direct inner-function call — see the 24th-session correction below the
task list; **not** a trampoline call, found by checking `FUN_201888f4`'s callers directly) | `0x2033605c` | `0x201871f0` | 2 | 0x320 | ✅ examined (14th/19th sessions) — `first_task_entry`, generic message-dispatch loop |
| `kernel_start` (`0x200052a4`) | `0x203907c4` (runtime-populated, genuinely unidentified — **corrected 24th session, was previously miscredited as `first_task_entry`'s activator**) | ? | ? | ? | ❌ open — real task, unknown identity |
| `FUN_200096c8` (`0x200096f0`) | `0x201988ec` | `0x200095d8` | **-2** | 0x400 | 🟡 new — Ghidra decompile silently **wrong** (no error flagged, but `objdump` shows a completely different real body: loop calling `blx 0x20186d0c`-style trampolines) |
| `FUN_2001439c` (`0x200143b8`) | `0x201988fc` | `0x20014384` | 0 | 0x800 | 🟡 new — **same silent-wrong-decode problem**: `objdump` shows a real periodic loop (`blx 0x20186d0c` with `r0=5`, `bl 0x20015628`, store, loop) that Ghidra's decompile completely misses, showing an unrelated one-shot body instead |
| `FUN_2001627c` (`0x2001631c`) | `0x20016800` | `0x2001745c` | 1 | 0x2000 (largest stack seen) | 🟡 new — explicit "Bad Instruction"-style garbage (`in_ZR`/`halt_baddata`) |
| `FUN_20027740` (`0x200277f0`) | `0x2002784c` | `0x20027528` | 0 | 0x1800 | 🟡 new — decompiles without an explicit error, but shows uninitialized-register use (`unaff_r5`) — almost certainly also wrong, not yet cross-checked against `objdump` |
| `FUN_2002afc0` (`0x2002b02c`) | `0x2019889c` | `0x2007ef5c` | — | — | ✅ examined (20th session) — the UI/display task (allocates screen objects, message loop) |
| `FUN_2006c4a8` (`0x2006c584`) | `0x201988cc` | `0x2006bb58` | 0 | 0x800 | 🟡 new — explicit garbage decompile (many `unaff_rX`) |
| `FUN_2006c4a8` (`0x2006c594`, **same caller as above — spawns 2 tasks together**) | `0x201988dc` | `0x2006c2c4` | 0 | 0x800 | 🟡 new — explicit garbage decompile |
| `thunk_FUN_2007ea68` (`0x2007ea84`) | *dynamic, caller-supplied* | *unresolved* | — | — | ❌ still unresolved — no static caller of the thunk found either (see 21st session) |
| `FUN_200aa5d4` (`0x200aa5c8`) | `0x2019890c` | `0x200aa580` (`bmp_capture_task`) | **-1** | 0x1000 | ✅ **resolved, 25th session — the BMP screen-capture-to-SD-card task**, see below (no disassembly issue at all here, decompiles cleanly) |
| `FUN_200b995c` (`0x200b999c`) | `0x201988ac` | `0x200b9c00` | 1 (highest seen) | 0x1800 | 🟡 new — **mixed**: garbage at entry, but real-looking code visible further in (calls `FUN_200cb5dc`/`FUN_200cb72c`/`FUN_200cb278`/`FUN_200cbcf0`/`FUN_200b9fc8` — a plausible read/parse/retry protocol handler) |
| *(no direct call site found)* | `0x20361318` | `0x200b94e8` | 3 | 0x800 | ✅ examined (20th/21st sessions) — `sys_monitor_task_entry` |

**Bottom line: 8 previously-unexamined tasks found, and every single one hits some form of disassembly
trouble** — either Ghidra's own explicit "Bad Instruction"/garbage-decompile pattern (6 of 8), or (more
concerning) a **silent wrong decode with no error at all**, caught only by cross-checking against
`objdump` (confirmed for 2 of 8 — `0x200095d8` and `0x20014384` — not yet checked for the rest). This is a
substantially bigger batch than the earlier "1 genuinely broken spot out of 139 bookmarks" sweep found —
because, like `first_task_entry` and `sys_monitor_task_entry` before them, **these addresses were never
touched by Ghidra's auto-analysis at all** (no direct caller in the static call graph → no bookmark ever
created → invisible to the bookmark-based sweep method used in the 17th session). Left all 8 as
placeholder-named (`task_probe_<address>`), un-renamed `Function` objects with this status — **don't trust
any of their current decompiled bodies**.

## Going through the new tasks via `objdump` ground truth, no GUI fix yet available (25th session)

Rather than wait for each address to get the GUI fix, read every remaining task's real ARM disassembly
directly via `objdump` (the established, trustworthy fallback used throughout this project) to classify
them without depending on Ghidra's decompiler at all.

**`bmp_capture_task` (`0x200aa580`) — fully resolved, no disassembly issue here at all.** Turned out
`0x200aa580` itself decompiles perfectly cleanly (Ghidra was never confused about *this specific address*
— the garbage-decompile symptom reported earlier came from creating the `Function` boundary right at the
task entry before checking whether the entry instruction itself was even part of the problem; here it
wasn't). Body is a trivial infinite loop calling two already-clean functions:
```c
loop:
    bmp_capture_signal_done();   // FUN_200aa548 -- signals a queue/semaphore
    bmp_capture_write_file();    // FUN_200aa3c0 -- see below
```
`bmp_capture_write_file` (`FUN_200aa3c0`) is unambiguous: it builds a real **Windows BMP file** —
constructs a 14-byte `BITMAPFILEHEADER` (magic `"BM"` = `0x424d`, file-header size field = `54` =
`0x36`) immediately followed by a 40-byte `BITMAPINFOHEADER` (`biSize` = `40` = `0x28`, standard fields
including a 24-bit-depth-shaped value), then calls two more functions to actually write pixel data and
polls for completion before signalling done. **This is the IC-7300's screen-capture-to-SD-card feature**
— renamed and PLATE-commented in Ghidra (`bmp_capture_task`/`bmp_capture_write_file`/
`bmp_capture_signal_done`), no GUI fix needed.

**The `FUN_2006c4a8` pair (`0x2006bb58`/`0x2006c2c4`) — real shape confirmed, purpose only partially
pinned down.** Both are genuine ARM state-machine tasks (not garbage) reading a state byte from a **shared**
global struct at `0x2006c3d4` (`+4` for one task, `+5` for the other) and jumping through a
`addcc pc,pc,r0,lsl#2` table — 9 states for the first, 11 for the second. Their shared setup functions
(`FUN_2006c45c`/`FUN_2006c410`, already cleanly analyzed, called once before either task starts) initialize
near-identical 0x1a-byte descriptor structs — `{ready=0, ready=0, buf_size=0x40, count=0, mode=3,
sentinel=-1, 0, tag_byte, tag_byte}` — differing only in the last two tag bytes (`0x20`/`0x40` for one,
`0x60`/`0x60` for the other). One of the first task's states calls `FUN_2006bc84` → `FUN_200bc6a4`, **the
same generic "read N bytes from an open file" primitive `firmware_update_main` uses for SD-card reads** —
a real, if partial, clue. **Two plausible readings, not settled either way**: the `0x40`-byte chunk size
matches both a classic SD/FAT sector-adjacent buffering size *and* a USB full-speed endpoint max-packet
size — don't over-read the tag bytes as confirming either. Given the confirmed file-read call, buffered
SD-card I/O (plausibly the WAV recorder/player, which the IC-7300 has) is at least as well-supported as a
USB-endpoint-pair guess — recorded as open rather than asserting the more "interesting"-sounding option.

**`0x20027528` — real code, the largest state machine found this session (42 states, `cmp r0,#0x29`),
purpose not yet classified.** Same general shape as the pair above (calls `FUN_20186f60`, zeroes fields in
a per-task struct first) but substantially bigger — worth a dedicated look on its own before guessing what
it is.

**`0x2001745c` — real code, not yet classified.** Calls `FUN_2017d454` first, conditionally
`FUN_20186e68` (a different itron-cluster trampoline than the `FUN_20186f60`/`FUN_20186e98` pair seen
elsewhere), and reads/writes small structs before its own dispatch. Has the largest stack of any task
found (`0x2000` = 8192 bytes) — worth noting as a hint toward something with deep call chains or large
local buffers.

**`0x200095d8`/`0x20014384` — real bodies now confirmed via `objdump` (already noted last session), still
not classified beyond "periodic/loop-driven, calls into the itron trampoline cluster".** `0x20014384`'s
loop (`blx 0x20186d0c` with `r0=5`, `bl 0x20015628`, store result, loop) reads as a genuine
"do-something-every-5-ticks" polling task; `0x200095d8`'s body is longer and not fully read yet.

**Status after this session**: 1 of 8 fully resolved (`bmp_capture_task`), 2 of 8 have their real shape
confirmed but purpose only partially pinned down (the `FUN_2006c4a8` pair), 1 of 8 confirmed real but
uncharacterized (`0x20027528`, the biggest one), 3 of 8 confirmed real but not yet deeply read
(`0x2001745c`, `0x200095d8`, `0x20014384`), and `0x200b9c00` unchanged from last session (mixed
garbage/real, not re-checked this pass). The GUI ARM-disassembly fix would still help all of these decompile
properly in Ghidra, but per `bmp_capture_task`'s example, `objdump` alone is enough to make real progress
without waiting for it.

**Two priorities for next time**:
1. **Force ARM disassembly via the GUI** over all 8 new addresses (same fix pattern used successfully for
   `sys_monitor_task_entry`/`sys_monitor_task_loop` in the 20th session) before re-examining any of them —
   given the silent-wrong-decode risk demonstrated here, don't trust a "clean" decompile without an
   `objdump` cross-check either.
2. **`FUN_2006c4a8` is the most interesting lead**: it's the only caller spawning *two* tasks together
   (`0x2006bb58`/`0x2006c2c4`), and its own setup work (clearing a 220-byte buffer, a distinct 8-entry ×
   24-byte table, calls to `FUN_2006c45c`/`FUN_2006c410`) has the shape of a real paired
   producer/consumer or client/server subsystem — worth checking first once the disassembly is fixed.

Also worth remembering: the descriptor's `priority` field takes small **negative** values in three cases
(`-2`, `-1`, and (indirectly, `first_task_entry`'s own activation path uses a runtime-populated slot, not
checked for sign) — `FUN_201888f4` (`itron_act_tsk`'s real inner function) explicitly allows priority in
`-3..3`, confirmed already in this file's very first section — so negative priorities are a real, valid,
used range in this firmware, not a sign of a misread field.

**Follow-up (24th session) — user asked "did we miss any tasks?", and yes, in a specific way**: not a
missing *task* (all 12 rows above were already in the catalog), but a wrong *attribution* — `kernel_start`'s
own `itron_act_tsk` call had been miscredited as activating `first_task_entry`, when it actually activates
a genuinely separate, still-unidentified task (see the corrected boot-chain sequence and the table above).
Caught by checking `references_to` on `itron_act_tsk`'s *inner* function directly (`FUN_201888f4`) rather
than only on the public trampoline — this also confirmed there's exactly one such "direct, trampoline-
bypassing" activation in the whole image (inside the kernel bootstrap, for `first_task_entry`), so no
*additional* hidden activations of that kind are being missed either. **Net effect: the task count actually
went up by one** — `kernel_start`'s own slot is a real 12th/13th task (on top of the dynamic
`thunk_FUN_2007ea68` case), just with its identity still unknown, exactly like that other one.

## All 8 remaining tasks resolved cleanly — the user had already done the GUI fixes (26th session)

Went the `objdump`-ground-truth route in the 25th session without re-checking whether Ghidra's decompiles
had changed — they had. **The user had already forced ARM disassembly over all 8 remaining addresses**;
every one now decompiles cleanly, no garbage, no silent-wrong-decode. Re-examined all of them properly.
Two genuinely significant finds:

**`sd_menu_dispatch_task`** (`0x20027528`, renamed from `task_probe_20027528`) — the big 42-case dispatcher
turns out to be the **SD-card operations menu's central task**, driven by a command-ID field read from a
shared struct after each queue-wait:
```c
switch (cmd_id) {
    case 0xb: iVar2 = firmware_update_main();   // *** confirmed direct call ***
    case 0xc/0xd/0x11/0x15/0x17-0x28: ~20 distinct FUN_2002xxxx handlers
      (format, save/load settings, memory-keyer file ops, etc. — not
      individually identified)
    ...
}
```
This is a genuinely important, concrete confirmation: `firmware_update_main` is invoked as one command
among many from this ordinary SD-card-menu task, not from any special/separate path — useful context for
anyone picking the `chunk4`/`chunk5` mystery back up, since it confirms the whole update flow starts from
routine user-menu interaction on this one task, nothing more exotic.

**`sdcard_file_rpc_dispatch_task`** (`0x200b9c00`, renamed from `task_probe_200b9c00`, then from
`civ_command_dispatch_task` — **see the 2026-08-29 "civ_command_dispatch_task retraction" section near the
end of this file for the full story**) — a structured command-protocol handler: waits on a queue for a
command ID (`FUN_20186de4`, the same queue-receive primitive `first_task_entry` uses), reads a framed byte
sequence one byte at a time via a retry loop until the accumulated length matches the expected ID, then
**invokes the command through a function-pointer table indexed by that ID**
(`(**(code**)(g_file_rpc_handler_table_ptr + id*4))(...)`), and waits for completion before looping. This
byte-framed, opcode-indexed shape originally looked like a plausible CI-V command processor (hence the old
name), but turned out on full inspection to be a generic internal SD-card file-access RPC service
(open/read/write/close/rename/list), unrelated to CI-V — not confirmed by any CI-V-specific evidence, and
now positively disconfirmed by cross-checking its full handler table against the real thing.

**The `FUN_2006c4a8` pair** (`audio_buffer_task_2006bb58`/`2006c2c4`) — confirmed real state machines
sharing state at `0x2006c3d4`. Went one level deeper into their sub-handlers this session:
`FUN_2006b99c` does position/seek-style arithmetic and calls `FUN_2006ad30`; other sub-calls show buffer-
position math wrapping against a size field at `DAT_2006c404+0x30` — a classic ring-buffer wraparound
shape. **Leaning towards a circular audio buffer manager (plausibly the SD-card WAV record/playback
feature)** rather than the USB-endpoint-pair guess floated last session, though still not fully confirmed
either way — recorded as the better-supported of the two readings, not as settled fact.

**The smaller three** (`status_poll_task_200095d8`, `periodic_poll_task_20014384`,
`queue_driven_task_2001745c`) — all confirmed real, none deeply chased for *purpose* this session:
- `periodic_poll_task_20014384` matches last session's `objdump` prediction exactly: a trivial
  `itron-trampoline-delay(5) → sample → store` loop.
- `status_poll_task_200095d8` polls a status byte and dispatches to a handful of small helper calls —
  purpose not identified.
- `queue_driven_task_2001745c` (largest stack in the catalog, `0x2000`/8 KB) is genuinely queue-driven
  (same `FUN_20186de4` primitive again) with a two-state message-type dispatch and a flag-toggle side
  effect (`FUN_200c6374`) — purpose not identified, but the large stack is a hint worth remembering if this
  gets picked up again.

**Every task in the catalog is now either fully examined or has a real, disassembly-confirmed body** —
this closes out the "have we looked at all tasks" thread from the 23rd/24th sessions structurally (all
bodies are real code now, nothing left showing garbage), even though a few purposes remain open questions
rather than disassembly problems. The two still-genuinely-unresolved *identities* are `kernel_start`'s own
mystery task (`0x203907C4`) and the fully-dynamic `thunk_FUN_2007ea68` case — both need live/JTAG
visibility, not more static reading, per the reasoning already laid out earlier in this file.

**Follow-up: does any catalogued task touch the DSP or `DRESD` directly? No.** Checked systematically:
- `DRESD` (`P2_6`): the exhaustive literal-pool search in [[ic7300-signal-chain]] already established only
  `port_bulk_gpio_init_pass1`/`pass2` ever reference the whole P2 register block — and those are only
  reached via `sys_monitor_task_loop → FUN_2002b29c → FUN_2002b1c8/FUN_2002afc0`. **`sys_monitor_task_entry`
  remains the only task that touches `DRESD`**, exactly as already documented — none of the newly-examined
  8 tasks add to this.
- **DSP-adjacent hardware** (SSIF0/1 audio-DMA registers, RSPI2): checked `references_to` on the SSIF/DMAC
  transfer function (`FUN_200b5dc0`/`FUN_200b5cdc`) — all 6 callers sit in the `0x200b1xxx`-`0x200b7xxx`
  driver cluster (reached via `FUN_200b0f68`'s case `2`, i.e. through the same ISR/event-handler mechanism
  as `rspi2_transmit`, not from inside any application task's own code). **None of the 12 catalogued tasks
  call into SSIF or RSPI2 driver code directly** — that hardware is touched only from the ISR/event-handler
  layer sitting below the task layer.
- `audio_buffer_task_2006bb58`/`2006c2c4`: their own globals (`DAT_2006c3d4`, `DAT_2006c404`, etc., all read
  and confirmed this session) resolve to **plain RAM addresses**, not hardware registers — these two tasks
  manage buffers only, with no direct register-level touch found. If they really are the WAV
  record/playback subsystem (still unconfirmed), any actual DSP/SSIF interaction would happen one layer
  further down, through the same shared event/queue mechanism `FUN_200b0f68` drains — not visible in these
  two tasks' own code.
- **The one real DSP connection at the *task* level**: `sd_menu_dispatch_task`'s case `0xb` calls
  `firmware_update_main`, whose "3 extra chunks" mechanism (`FUN_20025044` → the `0xb0`/`0xe2`-tagged ring
  buffer) is [[multi-cpu-images]]'s standing hypothesis for how **DSP Program/DSP Data firmware** actually
  reaches its destination during an update — but that ring buffer's real consumer is still not found (RSPI2/
  SSIF-audio/front-panel-UART were ruled out last session). So the honest answer is: **one task
  (`sd_menu_dispatch_task`) is the trigger point for the one mechanism suspected of reaching the DSP, but
  no task's own code — including that one — has been shown to touch DSP hardware directly.**

## `civ_command_dispatch_task` retraction, full CI-V transport confirmation (2026-08-29, 29th session)

User's explicit ask this session: dig into `civ_command_dispatch_task` (`0x200b9c00`) and see if it exposes
any publicly undocumented CI-V commands, per the 4-step plan recorded at the end of the previous session's
entry. Worked all 4 steps. **Bottom line: this task is not the CI-V command processor at all** — renamed to
`sdcard_file_rpc_dispatch_task`. The real CI-V transport, separately, is now proven at the code level (not
just pins). Both results below; the task's own Ghidra plate comment carries the same summary.

### Step 1 — transport confirmed at the code level, not just pins

Traced `sdcard_file_rpc_dispatch_task`'s own byte source: it drains a ring buffer (`DAT_200ba16c`) via
`file_rpc_queue_pop_byte` (renamed from `FUN_200b972c`). The only function that ever *pushes* into that
ring buffer is `file_rpc_queue_push_byte` (renamed from `FUN_200b96c4`), and — see Step 2 — that push path
turned out to be purely internal/software, not hardware-driven.

Separately, and this part is a real, solid confirmation: found SCIF0's real interrupt-driven receive path
and it **does** implement genuine CI-V byte framing.
- RZ/A1H manual (`REN_r01uh0403ej0600_rz_a1h_MAT_20210129-2931443.pdf`, port function tables) confirms
  `P6_9`/`P6_10`'s alt-functions are `TxD0`/`RxD0` — **SCIF channel 0**, register base `0xE8007000`
  (`SCSMR_0`..`SCEMR_0`, `SCFRDR_0` receive-data register at `+0x14`). This is the exact pin pair the user
  transistor-level-traced to CI-V (`CTXD`/`CRXD`, see [[ic7300-signal-chain]]).
- `scif0_civ_driver_init` (renamed from `FUN_20010c68`) configures SCIF0's registers and calls
  `register_event_handler(0xdf, ...)` / `(0xdd, ...)` / `(0xde, ...)` — the ISR-registration primitive
  documented earlier in this file (`FUN_200b9490`, 49+ call sites). The registered handler pointer
  (`DAT_200115ec`) resolves through a 4-byte jump stub to `scif0_civ_rx_isr` (renamed from `FUN_20010b6c`),
  confirmed by direct disassembly at `0x20010c64` (`b 0x20010b6c`).
- `scif0_civ_rx_isr` reads `SCFRDR_0` (channel-base `+0x14`, i.e. literally `0xE8007014` for this
  statically-bound channel-0 instance — confirmed the channel context global `DAT_200115dc` holds the raw
  image value `0xE8007000`) and calls `civ_frame_rx_statemachine` (renamed from `FUN_2001099c`).
- `civ_frame_rx_statemachine` is unambiguously a CI-V frame parser: `param_1==0xFE` sets frame-sync state;
  at frame position 1 it compares the byte against `*(byte*)(DAT_200115d4+0x59)` (the radio's own CI-V
  address, with a broadcast-mode flag at `+0x5a`) and aborts the frame on mismatch — genuine CI-V
  destination-address filtering; `param_1==0xFD` (with length ≥4) copies the whole frame body into a
  driver-owned buffer (`DAT_200115c8`) and sets a "frame ready" flag. The TX-side sibling (`FUN_200110a8`,
  not renamed) explicitly writes two `0xFE` preamble bytes when building outgoing frames, matching CI-V's
  two-byte preamble exactly.
- **Exhaustively searched for who reads `DAT_200115c8`'s "frame ready" flag and forwards the parsed command
  byte onward** (the natural next link, which would either confirm or refute that
  `sdcard_file_rpc_dispatch_task` is fed by real wire traffic). All 4 direct readers of that address stay
  inside the same low-level SCIF0 driver file (`0x20010xxx`-`0x20012xxx`); none reach
  `sdcard_file_rpc_dispatch_task`'s queue or ring buffer, nor any other task-level dispatch found this
  session. **Genuine open item, not resolved**: the real consumer of completed CI-V frames — i.e. the actual
  frequency/mode/etc. command processor implementing the documented CI-V feature set — was not located.
  Static analysis dead-ended the same way task-activation searches have before (see this file's earlier
  sections); a live JTAG trace of `DAT_200115c8`'s flag byte would settle it quickly.

**Verdict**: SCIF0 = CI-V is now proven by matching protocol behavior (address filtering, FE/FD framing),
not just by pin tracing. What consumes its parsed frames is still unknown.

### Step 2 — the "civ_command_dispatch_task" handler table dumped, and it isn't CI-V

`g_file_rpc_handler_table_ptr` (renamed from `DAT_200ba194`) is itself a pointer variable; its held value
(read directly from the image) is `0x20336090` — that address is the real table base, 27 populated 4-byte
entries (command IDs `0x00`-`0x1a`), terminated by a null entry at `0x1b`:

| ID | Address | What it calls | Read as |
|----|---------|----------------|---------|
| 0x00 | `0x200b9e4c` | — | `mov r0,#0; bx lr` — no-op/ping |
| 0x01 | `0x200b9e54` | `vfs_open_ex` (mode 2/0x100 or 0x180 flags) | directory-entry read (find-first style) |
| 0x02 | `0x200b9e90` | `vfs_open`, then loops `vfs_read_dir_entry` | list directory |
| 0x03 | `0x200ba244` | shared helper `FUN_200b9fc8` (itself: `vfs_open_ex` + `vfs_read_record`/`vfs_write_record`) | open + read/write |
| 0x04 | `0x200ba298` | tail-call `vfs_rename` (2 paths) | rename/move |
| 0x05 | `0x200ba2ac` | `vfs_read_record` then `vfs_write_record` | read-modify-write a 0x34-byte record |
| 0x06 | `0x200ba340` | `vfs_open_ex` (mode 0) | open/stat |
| 0x07 | `0x200ba378` | tail-call `vfs_close` | close |
| 0x08 | `0x200bb1cc` | large, not fully chased | — |
| 0x09 | `0x200bb794` | `vfs_read_dir_entry` loop | find-next entry |
| 0x0a | `0x200bb7c8` | tail-call `FUN_200cb04c` | — |
| 0x0b | `0x200bb7dc` | same body as 0x01 | directory-entry read variant |
| 0x0c | `0x200bb818` | tail-call `vfs_open` | open |
| 0x0d | `0x200bb82c` | tail-call `vfs_rename` | **same target as 0x04** — duplicate alias |
| 0x0e | `0x200bb840` | same body as 0x05 | **same as 0x05** — duplicate alias |
| 0x0f | `0x200bb8d4` | `vfs_open_ex` (mode 0x180 hardcoded) | open in write/create mode |
| 0x10 | `0x200bb90c` | tail-call `vfs_close` | **same target as 0x07** — duplicate alias |
| 0x11 | `0x200bb958` | `FUN_200c9bec` | — |
| 0x12 | `0x200bb98c` | `FUN_200c9d0c` | — |
| 0x13 | `0x200bb9c0` | `FUN_200c9e9c` | — |
| 0x14 | `0x200bb9f4` | large — `FUN_200c9634`/`FUN_200cc1ec`, string/format work | set-parameter-by-name (best guess) |
| 0x15 | `0x200bbc68` | `FUN_200c9634`, `FUN_200ca02c`, sets a flag field | set persistent flag/state |
| 0x16 | `0x200bbcc0` | reads back that same flag field, small state machine | query/clear counterpart of 0x15 |
| 0x17 | `0x200bbd28` | `FUN_200cc148` | — |
| 0x18 | `0x200bbd58` | large, wildcard-style path sanitization (`'*'` insertion) | path/glob helper |
| 0x19 | `0x200bc00c` | tail-call `FUN_200cb8d8` | — |
| 0x1a | `0x200bc020` | `FUN_200c5dd0` | — |

The shared primitives (`vfs_open`/`vfs_open_ex`/`vfs_read_record`/`vfs_write_record`/`vfs_close`/
`vfs_rename`/`vfs_read_dir_entry`, all `0x200caxxx`-`0x200ccxxx`, renamed this session) all resolve a
path string through `FUN_200ca70c` to a device object with its own operations vtable at `obj+0x10`
(slots at `+0x24`/`+0x28`/`+0x2c`/`+0x30`/`+0x34` for rename/read/write/lock/readdir-style operations) —
a textbook small VFS layer, not a ham-radio command set.

**Cross-checked against the real, published CI-V command table** (`IC-7300_ENG_FM_12b.pdf`, pages 19-3
through 19-10, commands `0x00` through `0x28`, the user's freshly-supplied manual copy): the documented
set is frequency/mode read-send, VFO/memory operations, scan, split, tuning step, attenuator, levels,
meters, CW message send, power on/off, transceiver ID, memory/band-stacking/keyer contents, a huge `0x1A
0x05` sub-table of settings, tone/RTTY/scope settings, etc. — **nothing resembling open/read/write/close/
rename/list-directory anywhere in the full documented range**. If `sdcard_file_rpc_dispatch_task`'s IDs
were literally the raw CI-V wire command byte, sending real command `0x03` ("read operating frequency")
would instead open-and-read-or-write a file — contradicted by the radio's well-known, working CI-V
behavior. So this table's ID space is not the CI-V wire command byte.

### Step 2b — confirmed as a generic, CI-V-unrelated internal file-RPC service (this settled it)

Found the real callers. Every one of the 27 handlers is *also* directly reachable — no UART involved at
all — through 26 tiny per-ID wrapper functions at `0x200bc0fc`-`0x200bca08` (each is a fixed 0x58-byte
stub hardcoding one ID 1-26 and calling `file_rpc_post_command`, renamed from `FUN_200bc048`, which stages
the packed args into the task's own ring buffer via `file_rpc_queue_push_byte` and posts to its queue —
this is the *entire* path that ever feeds that ring buffer; no hardware ISR reaches it). Traced
`references_to` on the "open" wrapper (ID 6, `FUN_200bc2b4`): **14 call sites scattered across completely
unrelated subsystems** — `0x20017xxx`, the `0x2002xxxx` SD-menu cluster, `0x2006xxxx`. One of them
(`FUN_20017000`) opens the literal path **`"C:\IC-7300\Voice\..."`** (string literal at `0x20016fec`,
read directly from the image) — the SD-card voice-memory-message feature — and drives it through open
(id 6) → read (id 5, via a sibling wrapper `FUN_200bc3e4`) → close (id 7) exactly per this table.

This is conclusive: `sdcard_file_rpc_dispatch_task` and its 27-entry table are the firmware's **generic,
shared, single-threaded SD-card file-access RPC service** — used by ordinary unrelated features (voice
memory playback confirmed; SD-menu and other `0x2006xxxx`/`0x2002xxxx` subsystems likely too, not
individually chased) that need serialized file open/read/write/rename/list access, going through one task
so file-system state stays consistent. It has no established connection to CI-V beyond a superficial
shape resemblance (byte-framed queue receive + ID-indexed function-pointer table) that misled the naming
in an earlier session. Retracted the name; renamed the task and its main supporting functions in Ghidra
(`sdcard_file_rpc_dispatch_task`, `g_file_rpc_handler_table_ptr`, `file_rpc_post_command`,
`file_rpc_queue_push_byte`/`pop_byte`, `vfs_open`/`vfs_open_ex`/`vfs_read_record`/`vfs_write_record`/
`vfs_close`/`vfs_rename`/`vfs_read_dir_entry`, `scif0_civ_driver_init`/`scif0_civ_rx_isr`/
`civ_frame_rx_statemachine`); full plate comment on the old `civ_command_dispatch_task` address records
this retraction for anyone who lands there from an old note or an old Ghidra bookmark.

### Steps 3/4 — moot given the above, but worth stating explicitly

Step 3 (cross-reference against the published CI-V table) is done above, as the evidence that disconfirmed
the hypothesis rather than confirming an undocumented-command list. Step 4 (decompile any undocumented-
looking candidates before calling them "hidden") — there are no candidates to decompile *as CI-V commands*,
since the table isn't CI-V's. Reframed as an ordinary internal-API question instead, the file-RPC service's
own handlers (0x08, 0x11-0x14, 0x17-0x1a in the table above) are still only partially characterized —
listed as open items below, not urgent.

### Open items for next time, no priority order

1. **The real CI-V command dispatcher is still unfound.** `civ_frame_rx_statemachine` proves SCIF0 parses
   genuine CI-V frames and stores them at `DAT_200115c8`, but nothing that reads that flag was found to
   forward the parsed command byte to a frequency/mode/etc. handler. This is now the concrete next step if
   "undocumented CI-V commands" is still the goal — needs either a deeper static sweep of the 4 known
   readers' callers (all inside `0x20010xxx`-`0x20012xxx`, not yet each individually decompiled) or live
   JTAG (watch `DAT_200115c8`'s flag byte, or the SCFRDR_0 register directly, while sending a real CI-V
   command from a PC).
2. A handful of the 27 file-RPC handlers (`0x08`, `0x11`-`0x14`, `0x17`-`0x1a`) weren't individually
   decompiled to full clarity this session (only their immediate call targets were identified from raw
   ARM ground-truth disassembly, all hitting the known ARM/Thumb Ghidra disassembly-context bug — see this
   file's "Known Ghidra project quirk" section — since this whole table region had never been examined
   before). Low priority unless the file-RPC service itself becomes independently interesting.
3. Two exact-duplicate table entries found (`0x0d`≡`0x04` both `vfs_rename`; `0x0e`≡`0x05` the
   read-modify-write pair; `0x10`≡`0x07` both `vfs_close`) — plausibly just compiler/linker artifacts from
   two source call sites sharing a common small wrapper, not investigated further.

## SCIF1 service-mode protocol — a second, parallel CI-V-shaped bus, not CI-V itself (2026-08-29, 30th session)

Follow-up to the retraction above: while chasing which physical pin SCIF1 (`0xE8007800`, found servicing
alongside SCIF0 in `main_idle_loop`) is wired to, the user asked to look at the *behavior* of whatever
services it instead of the pin. That turned into a real, substantial finding — not the pin, but a second
CI-V-shaped protocol running on this second channel, structurally similar to real CI-V but functionally a
calibration/self-test handshake, not a remote-control link. Kept separate from `civ_frame_rx_statemachine`/
SCIF0 throughout — nothing here changes that section's conclusions.

**Structurally, SCIF1's driver is a parallel CI-V implementation.** `scif1_svc_build_frame_header`
(`0x200122c0`) writes the exact same `0xFE 0xFE <dst> <src> <cmd>` preamble as real CI-V — but with its own
"own address" byte read from a *different* config offset than SCIF0's, i.e. a separately-addressed bus, not
a mirror of the same one. `scif1_svc_frame_rx_statemachine` (`0x20012594`, renamed from `FUN_20012594`) is a
near line-for-line structural duplicate of `civ_frame_rx_statemachine`. `scif1_svc_driver_init`
(`0x20011df4`) registers **4** interrupt IDs (`0xe1`-`0xe4`) versus SCIF0's 3 — one extra, not yet
individually chased. Servicing runs from `main_idle_loop` exactly like SCIF0 (`scif1_svc_rx_service`,
`0x20012854`, called right after `civ_frame_rx_statemachine`'s own servicer in the same loop body) — so at
the *driver* level this looked, at first, like it might just be a second physical CI-V-capable port. It
isn't, per the command-level behavior below.

**The command set is calibration-shaped, not remote-control-shaped.** `scif1_svc_command_dispatch`
(`0x20012f5c`, renamed from `FUN_20012f5c`) range-switches a command byte `0x00`-`0x32` into 6 sub-handlers.
Decompiled all of them (`scif1_svc_status_field_switch`/`0x20012ddc` plus `FUN_20012ba4`/`c00`/`c5c`/`ce8`/
`d34`/`d84`) — every single one is a **threshold or bit-flag comparator**: read a "measured" byte or 16-bit
field from the received frame's own payload (`DAT_20013088`, offsets `0x18`-`0x1f`) and compare it against
a target value carried in the outgoing command (`DAT_20013080+2`/`+4`), returning pass/fail or a small
enum, sometimes testing specific status bits instead of a numeric threshold
(`FUN_20012d34` tests two individual bits of a 16-bit status word). Nothing here resembles frequency/mode/
level get-or-set — this is the shape of a calibration or self-test handshake with whatever's connected,
not a documented-or-undocumented CI-V *command*. `DAT_20013088`'s "measured" fields are read-only
everywhere in `body.bin` outside this cluster — never written by any other traced function — consistent
with them being populated by real received frames from the wire, not synthesized internally.

**When it runs is gated behind a system-wide "service mode."** Traced the trigger back to
`system_mode_request_dispatch` (`0x2002a6b8`, renamed from `FUN_2002a6b8`), which runs every iteration of
`sys_monitor_task`'s own polling loop (via `FUN_2002b1c8` — the same function chain that holds the DSP in
reset at boot, see the "DRESD" sections above). It reads a "mode request" byte (`DAT_2002a158`, values seen:
`1`-`5`, `6`-`9` as a group, `0xb`) and switches `DAT_2002a4a4`, which selects which of several idle-loop
variants runs next (`0`=`main_idle_loop`, `1`=`FUN_20053154`, `4`=`svc_mode_idle_loop`/`0x20053270`,
`5`=`FUN_20053418`). **Request values `6`/`7`/`8` all select mode `4`** (`svc_mode_idle_loop`, which
services *only* SCIF1, not SCIF0/CI-V) **and inject a synthetic starting command** (`0x00`/`0x1a`/`0x28`
respectively) into SCIF1's dispatcher via `scif1_svc_post_synthetic_command` (`0x2001305c`) — so those three
request values look like three distinct sub-operations of the same service mode (a `2001302c`/`13048`/
`1306c` triple of call sites, one per starting command, already found earlier). Entering this state is
heavyweight: it also re-activates essentially the entire task set (`sdcard_file_rpc_dispatch_task`,
`sd_menu_dispatch_task`, the `audio_buffer_task` pair, `periodic_poll_task`, `queue_driven_task`, etc.) and
reopens *both* SCIF0 and SCIF1 — a full subsystem restart, not a lightweight feature toggle.

**Genuine dead end, not pushed further this session**: who writes `DAT_2002a158` to actually request modes
6/7/8 (i.e. what triggers this service mode) was not found. Checked two ways: `references_to` on the
address returns only 5 hits, all reads, all inside `system_mode_request_dispatch`'s own small cluster; and a
full-image `objdump` ARM disassembly grepped for any `movw`/`movt` pair or literal-pool word building
`0x2002a158` anywhere in `body.bin` — zero hits. Same class of static-analysis wall this project has hit
before on "what activates task X" questions (e.g. `sys_monitor_task_entry`'s own activator, still unresolved
per the "Chasing the two remaining loose ends" section above) — most likely populated through a message/
event mechanism rather than a direct memory write, not traced this session.

**Working hypothesis, not confirmed**: given the CI-V-shaped framing + own address + calibration/threshold
comparator command set + full-system reinit + reached only through what looks like a deliberate, rarely-used
mode, the most likely explanation is a **factory/service test or calibration link** — either to an internal
test point or an external factory jig — rather than anything end-user-facing (no evidence tying it to the
documented `1A 05 00 73` "CI-V Output for ANT" tuner feature, or to anything else in the public manual).
Not settled; flagged as a hypothesis, not fact, per this project's usual standard.

**Also still unresolved from the prior session's thread**: which physical pin(s) SCIF1's `RxD1`/`TxD1`
alt-function actually uses. All RZ/A1H datasheet candidate pin pairs (`P9_3`/`P9_4`, `P2_5`/`P2_6`,
`P6_12`/`P6_13`, `P4_12`/`P4_13`, `P7_3`/`P7_4`) are already claimed by other confirmed nets in the existing
CPU pinout table (boot flash QSPI, `DRESD`/`PSTB`, USB, SD-card, tuner respectively) — a real, unreconciled
conflict. The firmware's own driver init doesn't set the pin-mux itself (checked); it would live in the
generic `port_bulk_gpio_init_pass1`/`pass2` bulk-config functions, which use pointer-index arithmetic off a
single base rather than per-register literals, so a literal-pool search (which worked for the dead end above)
doesn't apply here either — resolving this needs the same kind of manual per-port pointer-offset decode that
the `DRESD`/`P2` trace took real, dedicated effort to do, or the schematic.

**Resume point if this thread continues**: (a) decompile `scif1_svc_command_dispatch`'s remaining 4-interrupt
detail (the extra `0xe4` ID vs. SCIF0's 3) for a possible clue about what's on the other end; (b) try tracing
`DAT_2002a158` via the RTOS event/message primitives instead of direct memory writes (check which
`register_event_handler` IDs feed into structures near this cluster); (c) do the SCIF1 pin-mux decode or ask
the user to check the schematic for any unlabeled/secondary alt-function silkscreen on the 5 candidate pins;
(d) live JTAG, once available, would likely resolve both the trigger and the pin question quickly by simply
watching `DAT_2002a158` and the SCIF1 register block during radio operation.

## Factory/service mode — a real, external fact confirms the shape, third independent piece of evidence found (2026-08-29, 30th session, continued)

**User-supplied ground truth**: the IC-7300 has a real, documented-by-experience service mode — enter it by
shorting the contacts in the REMOTE (CI-V) plug, then powering on while holding MENU and FUNCTION. This
directly explains why the SCIF1 investigation above found what it found: the REMOTE jack is exactly
`CTXD`/`CRXD`/`CBSY` (SCIF0's own pins), so "short the REMOTE contacts" is a physical condition readable as
raw GPIO state on the same pins SCIF0 uses — and MENU/FUNCTION are front-panel keys reported over SCIF3
(the separate front-panel-MCU link). Went looking for the exact boot-time detection code for this condition
and for who requests `DAT_2002a158`'s "mode 6/7/8" — did not find either (see below), but found a **third,
independent, very concrete piece of evidence** for a real factory/service subsystem while looking.

**Found: a real "factory data" file and a full MD5-verified load/save mechanism.** A string search for
plausible service-mode text turned up `"C:\IC-7300\IC-7300_factory"` (literal path at `0x20025620`, no
extension visible in the image) — a real SD-card file, not a menu label. It's used by three consecutive,
previously-unidentified cases in `sd_menu_dispatch_task`'s existing 42-case switch:
- **case `0x26`** → `factory_file_load` (renamed from `FUN_200253f4`): opens the file via the same
  open/read/seek/close file-RPC primitives `sdcard_file_rpc_dispatch_task` exposes, reads a 3-field header
  (4 bytes each, `+0`/`+0xd`/`+0x1a` stride) and compares each field against a fixed reference table,
  recording a pass/fail byte per field.
- **case `0x27`** → `factory_file_verify_md5` (renamed from `FUN_20025650`): a full, real **MD5 checksum
  validator** over the file's 3 segments — reads each segment in up to `0x8000`-byte chunks through
  `md5_init`/`md5_update`/`md5_final`, compares the computed digest against a stored 16-byte MD5 per
  segment, and maintains a live 0-255 progress value (`*(DAT_20024a04+0x50)`) while doing it — i.e. this
  is built to run under a visible progress bar, not silently in the background.
- **case `0x28`** → `factory_file_case28_report` (renamed from `FUN_20025300`, purpose least certain of the
  three): references the same path string and the per-segment pass/fail bytes case `0x26` records; reads
  as some kind of report/log/summary step, not fully traced.

This is a genuine, deliberately-engineered **factory calibration/settings backup-and-restore mechanism with
real integrity checking** — not a stray leftover. Exactly the shape you'd expect behind a documented
short-and-hold service-mode entry: load calibration constants back onto a radio after a board/EEPROM
replacement, with MD5 verification so a corrupted or wrong-model file gets rejected before it's trusted.

**Three independent findings now point at the same real subsystem, not yet proven wired together in the
traced call graph**: (1) SCIF1's parallel CI-V-shaped-but-calibration-behaved protocol (previous section);
(2) this MD5-verified `IC-7300_factory` file mechanism; (3) the `DAT_2002a158` "mode request" byte whose
values `6`-`9` trigger a full-system reinit through `system_mode_request_dispatch`. All three are exactly
the pieces a real factory/service mode would need (a calibration data channel, a backup/restore file with
integrity checking, and a distinct system-wide operating state) — treated as a strong, well-supported
working picture, not asserted as a proven single mechanism.

**Still not found, despite specifically looking with the user's new fact in hand**:
- The boot-time code that reads the REMOTE jack pins (`P6_9`/`P6_10`/`P7_11`) as raw GPIO to detect a short,
  and/or the MENU+FUNCTION-held report from the front panel over SCIF3, and combines them into a "enter
  service mode" decision. Checked the cold-boot dispatcher (`FUN_2002b29c`, decides between
  `FUN_2002b1c8`/cold-boot-init and `FUN_20029ca4`/power-state-loop) and its own condition functions
  (`FUN_20029224`/`FUN_20029270`/`FUN_200291d8`) — these turned out to be ordinary EEPROM regional-setting
  checks (parameter IDs `16000`/`16000`/`0x3fc0`), not GPIO/key reads; not the right place.
  `references_to`/string search for "SERVICE"/"TEST MODE"/"FACTORY"/"ADJUST" found only the factory-file
  path (useful) and ordinary menu-string-table hits (not useful) — no smoking-gun boot-time check located.
- Who sets `DAT_20027840+0x44` (`sd_menu_dispatch_task`'s own command-ID field) to `0x26`/`0x27`/`0x28` —
  same "genuine dead end" class as `DAT_2002a158`'s writer: `references_to` on the containing struct's base
  address returns only reads clustered inside `sd_menu_dispatch_task` itself, no external poster found. This
  would be the natural place to find whether these 3 menu cases are UI-gated behind a "service mode active"
  check, or freely reachable — not resolved.
- Whether the front-panel MCU (`IC501`) itself detects the key-hold condition and reports a distinct status
  to the main CPU, versus the main CPU polling ordinary key-state and combining it with GPIO itself — not
  determined; would need the front-panel SCIF3 packet format decoded (33-byte packets, framing already
  confirmed in [[ic7300-signal-chain]], key-code table not yet extracted).

**Resume point if this thread continues**: decode the front-panel SCIF3 33-byte packet's key-code field
(would let all boot-time "held key" checks be searched for directly by value, likely the single highest-
leverage next step); trace `factory_file_case28_report`'s output destination (display buffer vs. log file)
to settle its exact purpose; check whether `sd_menu_dispatch_task`'s case `0x26`-`0x28` are reachable from
the *ordinary* SD-card menu UI or only from a separate, service-mode-only menu screen (would need the
SD-menu's own item-visibility table, not yet located — likely separate from the 216-item general menu table
already documented); live JTAG would very plausibly resolve the actual boot-condition check in minutes by
just watching GPIO/SCIF3 traffic during a real service-mode entry.

**Same session, continued — user's hunch on the REMOTE-jack-short detection mechanism, and a start on the
SCIF3 packet-dispatch structure.** User suggested the short probably isn't read as raw GPIO but detected by
the SCIF0 UART hardware itself (through the level converters) — worth noting `FUN_200108b0` (SCIF0's retry/
echo-verify path, reached when a specific status bit is set during RX) does exactly this shape of check:
compares a just-received byte against `DAT_200115c0[7]` ("last transmitted byte") and counts mismatches —
but this reads as CI-V's ordinary bus-collision/echo-verify logic (a real, standard part of single-wire
CI-V arbitration), not an obviously special boot-time short-detector; not confirmed either way.

Went to look at the front-panel (SCIF3) side as suggested and found its own frame-receive machinery for the
first time: `scif3_frame_rx_statemachine` (`0x20036c68`, found via its `register_event_handler` IDs
`0xe9`-`0xec`) is a **third independent implementation of the exact same `0xFE`/`0xFD` framing** used by
CI-V (SCIF0) and the service-mode link (SCIF1) — strong confirmation this whole firmware has one shared
low-level driver template reused across all three internal serial links. Once a full frame arrives, control
passes to `scif3_frame_dispatch_by_type` (`0x20036bb8`): the packet's first content byte is a "type" (up to
32 values), and the rest of the frame gets copied into a shared status buffer at an offset determined by
that type (`DAT_20037590 + type` — different types can write overlapping regions of one larger front-panel
status struct, not fixed independent slots). This is almost certainly where keypad/encoder state lands, but
**the exact type→offset→field mapping, and which bit is MENU vs. FUNCTION, was not decoded this session** —
a real, scoped-out next step (comparable in size to the diode-matrix or `DRESD` traces), not a quick lookup.
Both functions renamed and commented in Ghidra.

**Same session, continued — found the real boot-time button-combo mechanism.** Pushed into
`cold_boot_hw_init` (renamed from `FUN_2002afc0`, already known for `port_bulk_gpio_init_pass2`/`DRESD`)
and found, right near its end, exactly the kind of check being looked for: three functions, each testing
the front-panel status buffer (`DAT_2002b4d8`, populated from real received SCIF3 packets via
`scif3_frame_dispatch_by_type`) for a specific held-button/reported condition, each setting a different
outcome:
- **`boot_check_mode1_combo`** (renamed from `FUN_2002ae18`): bits 3+4 (`0x18`) of buffer offset `0xd` both
  set, plus a separate status word's bit clear → sets `DAT_2002a4a4 = 1`.
- **`boot_check_mode5_combo`** (renamed from `FUN_2002add0`): bit 3 of offset `0xd` AND bit 4 of offset
  `0xe` (a *different* byte) both set → sets `DAT_2002a4a4 = 5`.
- **`boot_check_challenge_response`** (renamed from `FUN_2002ad18`): an exact 3-byte match (`0x04`, `0x01`,
  `0x21`) at offsets `0xd`/`0xe`/`0xf`, **then a 10-byte checksum verification** against a stored reference
  (`*(byte*)(DAT_2002a0ec+i+0x70) + (i+1)*3`, compared byte-for-byte against `DAT_2002b4dc+i`) — this reads
  as a real challenge/password check, not a simple button-hold, and sets a different flag entirely
  (`DAT_2002a130` bit `0x40`, not `DAT_2002a4a4`).

All three share one extra gating condition, `*DAT_2002b4d4 == 1` — traced this and it looks like an
ordinary "hardware already brought up once" flag (read-only everywhere found, consistent with a natural
`.bss` zero → set-once pattern across the boot sequence), not a REMOTE-jack-short indicator specifically;
no explicit SCIF0/CI-V/REMOTE state check was found feeding into any of the three conditions above. Whether
the physical REMOTE-jack short is required by hardware design (e.g. it changes what the front panel itself
reports) or is checked completely separately wasn't resolved.

`cold_boot_mode_dispatch` (renamed from `FUN_2002b1c8`) is the caller: runs `cold_boot_hw_init` once, then
reads `DAT_2002a4a4` to pick which idle-loop variant to enter (`0`=`main_idle_loop`, `1`=`svc_mode1_idle_loop`,
`5`=`svc_mode5_idle_loop`) — **this is a separate mechanism from `DAT_2002a158`'s mode 4** (previous
section's `system_mode_request_dispatch`/`svc_mode_idle_loop`), reached only at cold boot via these
button-combo checks, not via the runtime mode-request byte.

**Strong supporting evidence `svc_mode1_idle_loop` (mode 1) is the real service mode**: unlike
`main_idle_loop`'s ~90 peripheral-service calls, it services almost nothing *except* SCIF0
(`civ_frame_rx_statemachine`'s driver) **and** SCIF1 (`scif1_svc_rx_service`) — a genuine reduced-
functionality state keeping both CI-V and the SCIF1 calibration-shaped link alive while dropping nearly
everything else. This is a coherent, well-supported picture, not proven: a real factory/service mode would
plausibly look exactly like this (minimal peripheral set + both special-purpose serial links live).

**MENU+FUNCTION identification — CONFIRMED, same session.** Front-panel CPU (`IC501`, `R5F104LCAFB` RL78 —
this project doesn't have its own firmware dumped/analyzed, main-CPU `body.bin` only) pin data the user
supplied shows 8 buttons wired as **direct, individual GPIO inputs** (not matrixed) on consecutive pins
`P70`-`P77`: `P70`=`TRSK` (transmit), `P71`=`TUNK` (tuner), `P72`=`AMPK` (p.amp/att), `P73`=`MENUK`
(**MENU**), `P74`=`SCPEK` (labeled "scope" on the schematic — see correction below), `P75`=`MPADK`
(labeled "mpad" on the schematic — same correction), `P76`=`QMENUK` (quick menu), `P77`=`XFCK` (xfc) —
plus a 4×4 resistor-multiplexed matrix on `KI10`-`KI13` (`P20`-`P23`, 16 more buttons: clear/notch/nr/nb,
set/speech/auto-tune/ts, m-ch-dn/m-ch-up/a-b/v-m, split/rit/dTX/clear) and two "twin" dial-encoder pairs.

Under the natural "bit index = pin number − `P70`" convention, buffer offset `0xd` bit 3 (`0x8`) = `P73` =
MENU, and bit 4 (`0x10`) = `P74`. No button in the schematic-derived pin list above is literally named
FUNCTION, so this looked unresolved — **until the user found the schematic itself is wrong**: physical PCB
silkscreen has switch `S10` = FUNCTION and `S11` = M.SCOPE, but the schematic mislabels them as `S10` =
SCOPE and `S11` = MPAD (a real schematic authoring error — "made by a summer trainee" per the user, not a
firmware or reasoning error on this project's part). `P74`'s `SCPEK` net goes to `S10`, which is really
**FUNCTION**, not scope — so buffer bit 4 = FUNCTION. **`boot_check_mode1_combo`'s condition (bits 3+4 of
offset `0xd` both set) is therefore literally "MENU + FUNCTION held together"** — an exact match to the
user's real, external service-mode entry procedure. (Corollary, not independently confirmed: `P75`/`MPADK`,
wired to `S11`, would by the same correction be the real M.SCOPE key rather than MPAD — the schematic's
`S10`/`S11` labels read as swapped as a pair, not each independently wrong.)

**REMOTE-jack-short condition — also CONFIRMED, same session.** `boot_check_mode1_combo`'s *second*
condition (`*(ushort*)(DAT_2002a0d0+0x18) & 0x400) == 0`) resolves cleanly: `DAT_2002a0d0` holds the literal
value `0xFCFE3200` — the RZ/A1H's `PPRn` (port pin-**read**) register family base (already established in
[[ic7300-signal-chain]]'s `HSK1` finding). `+0x18` = `+6×4` = `PPR6`, Port 6's raw pin-read register; bit
`0x400` = bit 10 = **`P6_10`, the main CPU's own `CRXD`** (CI-V receive) pin. The condition requires this
bit **clear**, i.e. `CRXD` reads **low**. CI-V idles high on an unshorted bus, so `CRXD` reading low means
the REMOTE jack's contacts are shorted — exactly the user's described hardware condition. Renamed
`DAT_2002a0d0` → `g_ppr_register_base` in Ghidra. Confirms (closer to) the user's own hunch from earlier in
this thread: the short *is* detected at the hardware level through the pins SCIF0 already uses for
CI-V — just via the GPIO block's own pin-read capability (which reflects the real electrical level
regardless of the pin's peripheral-mode configuration) rather than through SCIF0's UART status registers
directly. This is also why an exact-address literal search for this earlier came up empty: the code
computes the offset from one stored base pointer at runtime rather than using a separate hardcoded literal
per register — the same pattern seen elsewhere in this firmware's driver code.

**This closes the main thread of the investigation that started with the `civ_command_dispatch_task`
retraction two sessions ago, completely**: `boot_check_mode1_combo` requires **MENU + FUNCTION held on the
front panel, AND the REMOTE/CI-V jack's contacts shorted (`CRXD` pulled low)** — both halves of the user's
real, external service-mode entry procedure, now both identified and matching exactly. Reached from
`cold_boot_hw_init` at cold boot, selecting `svc_mode1_idle_loop` (services almost nothing except CI-V/SCIF0
and the calibration-shaped SCIF1 link — independently consistent with a real factory/service mode). Also
independently reachable at *runtime* (not just cold boot) via `DAT_2002a158` == `1` or `10`, handled the
same way inside `system_mode_request_dispatch` — a cross-link between the two mode-selection mechanisms
documented separately in the previous two sections, not previously connected.

**Genuinely open, lower priority, not investigated further this session**: `boot_check_mode5_combo` and
`boot_check_challenge_response`'s own exact roles beyond their trigger conditions; `boot_check_challenge_response`'s
result flag (`DAT_2002a130` bit `0x40`) has **no reader found anywhere** (checked its containing byte's
full `references_to` — 4 total hits, all either the write itself or unrelated bits of the same byte) —
possibly vestigial/leftover from development, possibly missed; what runtime-requesting mode 1/10 via
`DAT_2002a158` is actually for (vs. the boot-time path); and `boot_check_challenge_response`'s 10-byte
reference data (`DAT_2002a0ec+0x70`) isn't readable statically — it's plain uninitialized RAM in the image
(confirmed: a direct read attempt at that computed address failed as inaccessible), consistent with it
being populated at runtime (EEPROM or similar) rather than a firmware-embedded constant, but not confirmed
further.

## Session handoff (2026-08-29, end of 30th session) — candidates for next session, static-analysis-first

This 3-part session (`civ_command_dispatch_task` retraction → SCIF1 → factory/service mode) is now closed
out end to end. JTAG hardware is still not in hand, so the list below is deliberately ordered
**static-analysis-tractable first**, saving the JTAG-only items for last.

**Good static-analysis candidates, roughly this session's own leftover threads**:
1. **`boot_check_mode5_combo`'s own role** — its trigger condition is known (bit 3 of front-panel offset
   `0xd` = MENU, AND bit 4 of offset `0xe` — a *different* status byte, not yet identified against the
   front-panel pinout) but `svc_mode5_idle_loop`'s own behavior beyond "services SCIF0+SCIF1, posts a
   `FUN_2002b818(0xb)` request under one more condition" wasn't characterized as deeply as mode 1's.
2. **A handful of the 27 file-RPC handlers** (table entries `0x08`, `0x11`-`0x14`, `0x17`-`0x1a` in
   `sdcard_file_rpc_dispatch_task`'s table) — only their immediate call targets were identified from raw
   ARM ground-truth, not fully decompiled (this whole table region had never been examined before this
   session, all hit the known ARM/Thumb Ghidra bug — see "Known Ghidra project quirk" above — worth a GUI
   force-ARM pass over `0x200bb1cc`-`0x200bc044` and `0x200bbc68`-`0x200bbd58` if picked up).
3. **`factory_file_case28_report`'s exact purpose** — least-characterized of the three `IC-7300_factory`
   file cases; traces the same path string and per-segment pass/fail bytes but its output destination
   (display buffer? log file? something else?) wasn't traced.
4. **Whether `sd_menu_dispatch_task`'s cases `0x26`-`0x28`** (the factory-file load/verify/report trio) are
   reachable from the *ordinary* SD-card menu UI, or gated behind service mode — would need the SD-menu's
   own item-visibility/label table (separate from the already-documented 216-item general menu table),
   not yet located.
5. **The front-panel SCIF3 packet's full type→field mapping** — `scif3_frame_dispatch_by_type`'s dispatch
   mechanism is understood, but only 2 of up to 32 message types (`0xd`/MENU+FUNCTION-adjacent, used this
   session) have any field meaning attached. Decoding more of this table would likely resolve
   `boot_check_mode5_combo`'s second bit and other loose ends fast.
6. **The real CI-V command dispatcher** (frequency/mode/etc. — the actual documented feature) is still
   unfound. `civ_frame_rx_statemachine` proves SCIF0 parses real CI-V frames and stores them at
   `DAT_200115c8`, but nothing that reads that flag was found to forward the parsed command byte onward.
   Only the 4 direct readers (all inside `0x20010xxx`-`0x20012xxx`) were checked at the top level — their
   own callers weren't individually walked. Plausibly the single highest-value item on this list if picked
   up, since it's the one piece of the original "undocumented CI-V commands" question genuinely still open.

**Older standing candidates, not touched this session, still open** (see [[ic7300-signal-chain]]/
[[multi-cpu-images]]/[[diode-matrix]] for detail): the firmware-update restart trigger (check the SCIF3
front-panel driver for a reload/reset command); settling the `audio_buffer_task` pair's exact purpose
(leaning WAV record/playback, not confirmed); re-checking `HSK0`/`FRWT`/`RTD` against the `PPRn` family now
that `HSK1`/the REMOTE-short bit both turned up there — worth a systematic PPRn sweep of the remaining
signal-chain unknowns while this register family is fresh; identifying `kernel_start`'s own mystery task
(descriptor `0x203907C4`, activated but never matched to a real function); `chunk4`/`chunk5`'s real
consumer in the multi-CPU-image update mechanism; D408/D411/D414/D417 (4 of 19 diode-matrix positions,
exhaustively searched across all 10 firmware versions already, genuinely no consumer found — would need a
fresh angle, not more of the same search).

**JTAG-dependent, deliberately last** — static analysis has hit genuine, well-documented walls on these,
confirmed dead ends via `references_to` and whole-image literal/`objdump` searches, not just "not found
yet": exact bit-to-button mapping beyond MENU/FUNCTION (would need the front-panel MCU's own firmware, not
dumped, or watching `DAT_2002b4d8` live while pressing buttons); SCIF1's physical pin (every RZ/A1H
candidate already claimed by another confirmed net — a real conflict, resolvable by watching the SCIF1
register block live, or by the user checking the schematic for an unlabeled secondary alt-function);
`DAT_2002a158`'s writer and other task-activation questions (`sys_monitor_task_entry`'s own activator,
`kernel_start`'s mystery task); confirming `boot_check_challenge_response`'s 10-byte reference data's
runtime source.
