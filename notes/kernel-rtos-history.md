# Main-CPU kernel/RTOS — session history

Full session-by-session narrative and evidence trail behind [notes/kernel-rtos.md](kernel-rtos.md),
which carries only the current-state summary (confirmed facts, the living task-catalog table, and
open questions). Sessions below are in chronological order. Addresses, register values, hex offsets,
function names, and [[wikilink]] cross-references are preserved verbatim from the original notes.

## Prior investigation (pre-numbered-session era): RTOS not found by name, despite a thorough string search

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

**Update, 2026-08-29 (new session)**: `FUN_2002b29c` turns out to be more central than this entry's own
Ghidra plate comment (still saying "which message ID reaches it isn't found yet" — stale, not updated after
this session resolved it) suggests. It's also the direct caller of both halves of a newly-found
firmware-update-restart marker mechanism — `fup_autoend_marker_check_and_clear` near its start,
`fup_autoend_marker_write` (via `FUN_20029ca4`) near its end — found by extending Ghidra's memory map to the
RZ/A1H's real 10 MB on-chip RAM extent. Full writeup, since it's really a firmware-update-thread finding,
in [[firmware-update]]'s new `"Fup_AutoEnd_3765"` section.

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

**Table last brought current 2026-08-29 (much later session)** — the version below reflects every
rename/retraction from the 25th/26th/29th sessions (see the sections following it); if you're
reading an old copy of this file, the version that follows is the one to trust.

| Caller | Descriptor | Entry point (current name) | Priority | Stack | Status |
|---|---|---|---|---|---|
| `FUN_20188574` (kernel bootstrap, direct inner-function call, **not** a trampoline call — see the 24th-session correction below) | `0x2033605c` | `first_task_entry` (`0x201871f0`) | 2 | 0x320 | ✅ generic ITRON/RTOS message-dispatch loop |
| `kernel_start` (`0x200052a4`) | `0x203907c4` — **still genuinely unidentified**, still blank/`0xFF` in the static image | ? | ? | ? | ❌ open — real task, unknown identity, needs live JTAG |
| `FUN_200096c8` (`0x200096f0`) | `0x201988ec` | `status_poll_task_200095d8` | **-2** | 0x400 | ✅ real body confirmed (26th session, GUI ARM-disasm fix) — polls a status byte, dispatches to small helpers; purpose not identified |
| `FUN_2001439c` (`0x200143b8`) | `0x201988fc` | `periodic_poll_task_20014384` | 0 | 0x800 | ✅ confirmed — trivial `itron-trampoline-delay(5) → sample → store` loop |
| `FUN_2001627c` (`0x2001631c`) | `0x20016800` | `queue_driven_task_2001745c` | 1 | 0x2000 (largest stack in the catalog) | ✅ confirmed — queue-driven (`FUN_20186de4`), two-state message dispatch + a flag-toggle side effect (`FUN_200c6374`); purpose not identified |
| `FUN_20027740` (`0x200277f0`) | `0x2002784c` | `sd_menu_dispatch_task` | 0 | 0x1800 | ✅ **fully resolved** — the SD-card operations menu's central 42-case dispatcher; case `0xb` calls `firmware_update_main` directly, confirming the update flow starts from routine SD-menu interaction, nothing more exotic |
| `cold_boot_hw_init` (`0x2002b02c`, renamed from `FUN_2002afc0`) | `0x2019889c` | `FUN_2007ef5c` (the UI/display task — entry point itself never renamed) | — | — | ✅ examined — allocates screen objects, runs a message loop |
| `FUN_2006c4a8` (`0x2006c584`) | `0x201988cc` | `audio_buffer_task_2006bb58` | 0 | 0x800 | ✅ confirmed real state machine — ring-buffer wraparound arithmetic found; leaning circular audio buffer manager (plausibly SD-card WAV record/playback), not fully confirmed |
| `FUN_2006c4a8` (`0x2006c594`, **same caller as above — spawns 2 tasks together**) | `0x201988dc` | `audio_buffer_task_2006c2c4` | 0 | 0x800 | ✅ confirmed real state machine — same subsystem as above, shares state at `0x2006c3d4` |
| `thunk_FUN_2007ea68` (`0x2007ea84`) | *dynamic, caller-supplied* | *unresolved* | — | — | 🟡 **mechanism traced, peripheral identity still open, 2026-08-29 (much later session)** — an initial "USB subsystem" guess was checked against the real RZ/A1H manual and retracted; see the section below |
| `FUN_200aa5d4` (`0x200aa5c8`) | `0x2019890c` | `bmp_capture_task` | **-1** | 0x1000 | ✅ **fully resolved** — the BMP screen-capture-to-SD-card feature (real `BITMAPFILEHEADER`/`BITMAPINFOHEADER` construction) |
| `FUN_200b995c` (`0x200b999c`) | `0x201988ac` | `sdcard_file_rpc_dispatch_task` (renamed **twice**: `task_probe_200b9c00` → `civ_command_dispatch_task` (wrong guess) → `sdcard_file_rpc_dispatch_task`, see the 2026-08-29 "civ_command_dispatch_task retraction" section) | 1 (highest priority in the catalog) | 0x1800 | ✅ **fully resolved, then corrected** — **not** CI-V; a generic internal SD-card file-access RPC service (open/read/write/close/rename/list), dispatched through a function-pointer table by command ID |
| *(no direct call site found — genuine static-analysis dead end, not yet fixed)* | `0x20361318` | `sys_monitor_task_entry` (`0x200b94e8`) | 3 | 0x800 | ✅ examined — runs the `cold_boot_hw_init` chain (`DRESD`/GPIO init); its own **caller**, `sys_monitor_task_loop` (`0x2003bb70`), *is* known — called unconditionally every iteration by `FUN_2002b29c` — but what activates `sys_monitor_task_entry` itself remains unresolved |

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

**Superseded, 26th session (see below): all 8 were subsequently GUI-fixed and now decompile cleanly** —
this "don't trust the decompile" warning and the `task_probe_<address>` placeholder names are historical,
not current. The table above already reflects the final, correct state; read the "All 8 remaining tasks
resolved cleanly" section below for the full resolution before assuming anything in this paragraph still
applies.

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
| 0x08 | `0x200bb1cc` | bulk directory scan, dispatches per-entry by file-extension class (via a family of helpers at `0x200ba580`/`680`/`710`/`82c` etc., each keyed by a string-table offset) | typed bulk directory listing/copy (e.g. building an SD-card file-browser list by file type) |
| 0x09 | `0x200bb794` | `vfs_read_dir_entry` loop | find-next entry |
| 0x0a | `0x200bb7c8` | tail-call `FUN_200cb04c` | — |
| 0x0b | `0x200bb7dc` | same body as 0x01 | directory-entry read variant |
| 0x0c | `0x200bb818` | tail-call `vfs_open` | open |
| 0x0d | `0x200bb82c` | tail-call `vfs_rename` | **same target as 0x04** — duplicate alias |
| 0x0e | `0x200bb840` | same body as 0x05 | **same as 0x05** — duplicate alias |
| 0x0f | `0x200bb8d4` | `vfs_open_ex` (mode 0x180 hardcoded) | open in write/create mode |
| 0x10 | `0x200bb90c` | tail-call `vfs_close` | **same target as 0x07** — duplicate alias |
| 0x11 | `0x200bb958` | thin wrapper, `FUN_200c9bec` | — |
| 0x12 | `0x200bb98c` | thin wrapper, `FUN_200c9d0c` | — |
| 0x13 | `0x200bb9c0` | thin wrapper, `FUN_200c9e9c` | — |
| 0x14 | `0x200bb9f4` | reads file info, on a specific error re-derives a name/extension, classifies it against 2 fixed sets of type codes (`{1,4,6,0xb}`/`{0xe,0x1b,0x1c,0x1e}`), then a create/rename-style call (`FUN_200c3f98`) | file-extension-gated create/rename (best guess) |
| 0x15 | `0x200bbc68` | `FUN_200c9634`, `FUN_200ca02c`, sets a flag field | set persistent flag/state |
| 0x16 | `0x200bbcc0` | reads back that same flag field, small state machine | query/clear counterpart of 0x15 |
| 0x17 | `0x200bbd28` | thin wrapper, `FUN_200cc148` | — |
| 0x18 | `0x200bbd58` | wildcard-style path sanitization (`'*'` insertion), then the SAME 2-set extension classification as 0x14, then 3 separate `FUN_200c5758` category checks setting 3 output flags | filename classifier — is-type-1/2/3 |
| 0x19 | `0x200bc00c` | thin wrapper, tail-call `FUN_200cb8d8` | — |
| 0x1a | `0x200bc020` | thin wrapper, `FUN_200c5dd0` | — |

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

**RETRACTED, 2026-08-29 (next session) — SCIF1's physical pin is now resolved, and the claim below that "the
firmware's own driver init doesn't set the pin-mux itself" was wrong.** The prior sweep only checked for a
literal-pool-built port address referencing a single fixed register; it missed that `scif1_svc_driver_init`
*does* configure port-mux registers, just via the same base-pointer-plus-computed-offset idiom
`port_bulk_gpio_init_pass1`/`pass2` use (not a fresh literal per register). See the new section below,
"SCIF1 and SCIF5 physical pins resolved via each driver's own port-mux code", for the full trace: the real
pins are **`P6_13` + `P7_12`** — not any of the 5 same-port adjacent pairs this session's candidate list
assumed (`P9_3`/`4`, `P2_5`/`6`, `P6_12`/`13`, `P4_12`/`13`, `P7_3`/`4`); the two halves of the UART sit on
*different* ports, which is why the pair-based search never found it.

~~**Also still unresolved from the prior session's thread**: which physical pin(s) SCIF1's `RxD1`/`TxD1`
alt-function actually uses. All RZ/A1H datasheet candidate pin pairs (`P9_3`/`P9_4`, `P2_5`/`P2_6`,
`P6_12`/`P6_13`, `P4_12`/`P4_13`, `P7_3`/`P7_4`) are already claimed by other confirmed nets in the existing
CPU pinout table (boot flash QSPI, `DRESD`/`PSTB`, USB, SD-card, tuner respectively) — a real, unreconciled
conflict. The firmware's own driver init doesn't set the pin-mux itself (checked); it would live in the
generic `port_bulk_gpio_init_pass1`/`pass2` bulk-config functions, which use pointer-index arithmetic off a
single base rather than per-register literals, so a literal-pool search (which worked for the dead end above)
doesn't apply here either — resolving this needs the same kind of manual per-port pointer-offset decode that
the `DRESD`/`P2` trace took real, dedicated effort to do, or the schematic.~~ (superseded above)

**Resume point if this thread continues**: (a) decompile `scif1_svc_command_dispatch`'s remaining 4-interrupt
detail (the extra `0xe4` ID vs. SCIF0's 3) for a possible clue about what's on the other end; (b) try tracing
`DAT_2002a158` via the RTOS event/message primitives instead of direct memory writes (check which
`register_event_handler` IDs feed into structures near this cluster); (c) ~~do the SCIF1 pin-mux decode~~ done,
see below; (d) live JTAG, once available, would likely resolve the mode-6/7/8 trigger question quickly by
watching `DAT_2002a158` during radio operation.

## SCIF1 and SCIF5 physical pins resolved via each driver's own port-mux code (2026-08-29, next session)

Picked up the user's tangent suggestion: rather than keep guessing pins from the schematic/datasheet
candidate-pair approach, go find the actual SCIF *initialization* code itself — it has to set the RZ/A1H
port mux registers somewhere, and that would nail the pin down directly from `body.bin`.

**First, found a register family missing from this project's own port-register-map notes.** The established
table (`Pn`/`PSRn`/`PPRn`/`PMn`/`PMCn`/`PFCn`/`PFCEn`/`PNOTn`/`PMSRn`/`PMCSRn`/`PIBCn`/`PBDCn`/`PIPCn`, see
[[ic7300-signal-chain]]'s `DRESD` section) turned out to be incomplete: there's also a **`PFCAEn`** family at
`PORTn_base + 0xA00` (RZ/A1H's third alt-function-select bit, "Alternate Enable" — `PFCn`+`PFCEn`+`PFCAEn`
together form a 3-bit code selecting 1 of 8 peripheral functions per pin, only enabled when `PMCn`'s
matching bit is also 1). Found it the same way the rest of the table was found originally: cross-referencing
a computed address against the expected offset pattern, this time inside a driver function rather than the
bulk-init sweep — confirmed independently 3 times (once per driver below), so this is solid, not a guess.

**The method: every SCIF driver's own `_driver_init` writes the same 6-register pattern for its own port
bit(s)** — `PBDCn`, `PFCn`, `PFCEn`, `PFCAEn`, `PIPCn`, and finally `PMCn` (the one that actually *enables*
peripheral mode) — each a read-modify-write clearing then re-setting the target bit(s). This is on top of,
and independent from, `port_bulk_gpio_init_pass1`/`pass2`'s own generic bulk pass (which sets baseline
`PMCn`/`PMn`/`Pn`/`PIBCn` values for every port at boot but never touches `PFCn`/`PFCEn`/`PFCAEn` at all —
confirmed by fully hand-decoding both passes' assembly, address-family by address-family). Concretely, for
each driver (`iVarN` below is that driver's own literal-pool copy of the `PBDCn`-family base pointer,
`0xFCFE7100`; offsets are exactly `port_bulk_gpio_init`'s established family deltas):

- **`scif0_civ_driver_init`** (`0x20010c68`, already known-good — `P6_9`/`P6_10` = CI-V `RxD0`/`TxD0`, the
  cross-check for this whole method): sets bits 9+10 of `PBDC6`/`PFCAE6`/`PIPC6`/`PMC6` to 1 and bits 9+10 of
  `PFC6`/`PFCE6` to 0 — function code `(PFCAE,PFCE,PFC)=(1,0,0)=4`. Matches the schematic-confirmed pins
  exactly, validating the method.
- **`scif1_svc_driver_init`** (`0x20011df4`): two *separate* single-port-bit blocks, not one pair on the same
  port. Block 1 (port 6, bit 13 — `iVar & 0x18` offset pattern) sets `PBDC6`/`PFCAE6`/`PIPC6` bit 13 to 1,
  `PFC6`/`PFCE6` bit 13 to 0, and **`PMC6` bit 13 to 1** (function code 4, same code SCIF0 uses) — bit 12 of
  the same registers gets touched too but `PMC6` bit 12 is explicitly left 0 at boot (see below — it's
  enabled dynamically at runtime instead). Block 2 (port 7, bit 12 — offset `+0x1c`) sets **all six** of
  `PBDC7`/`PFC7`/`PFCE7`/`PFCAE7`/`PIPC7`/`PMC7` bit 12 to 1 — function code `(1,1,1)=7`. **Both blocks are
  real** (`PMCn` enabled in both) — so **SCIF1 = `P6_13` + `P7_12` (both, the same signal — RX) + `P6_12`
  (TX, enabled separately at runtime, see below)** — the RX half splits across two different ports, which
  is exactly why the earlier same-port-pair candidate search above never found it. This also resolves
  [[ic7300-signal-chain]]'s "`P7_12` (`UDRXD`/`UDBSY`) — same name as `P6_13`, likely an alias/transcription
  duplicate" note from the port-pinout sweep: **it isn't a duplicate — both are real, distinct pins,
  deliberately activated together by the same driver.** **User-confirmed 2026-08-29 (PCB layout check,
  see "SCIF1 TX/RX pin turnaround" section below)**: `P6_13` and `P7_12` really are the *same net*, tied
  together on the PCB — both are `SCIF1`'s RX line (`UDRXD`/`UDBSY`, into `IC641` `CP2102` pin 26/`TXD`),
  not a TX+RX pair as first guessed here. `P6_12` (`UDTXD`, into `CP2102` pin 25/`RXD`) is the real TX line,
  and is *not* enabled by this boot-time driver at all — see the dedicated section below for why.
- **`scif5_dsp_link_driver_init`** (`0x200b2bb0`): one block, port 8, bits 0+1+2 (offset `+0x20`) *and* bit 11
  in the same instruction sequence. Bits 0/1/2: `PFC8`/`PFCE8`/`PMC8` set to 1, `PFCAE8` set to 0 — function
  code `(0,1,1)=3`, all three with `PMC8` enabled. Bit 11: `PFCAE8` set to 1, `PFC8`/`PFCE8`/`PMC8` left 0 —
  function code 4 prepped but **not enabled** (`PMC8` bit 11 stays 0, so this pin stays plain GPIO/unused by
  this driver). **So SCIF5 = `P8_0`/`P8_1`/`P8_2`** — three pins, not the usual two, consistent with a
  clock/handshake line alongside `TxD5`/`RxD5` (or possibly RTS/CTS-shaped flow control).

**This closes `notes/multi-cpu-images.md`'s handoff item 4 (SCIF5's physical pin) and this file's own
long-open SCIF1 pin question, both from the same method** — go find the driver's own port-mux writes
instead of guessing from datasheet candidate pairs. **Correction to `notes/ic7300-signal-chain.md`**:
`P8_0`/`P8_1`/`P8_2` (schematic names `DSPCK`/`DSPR`/`DSPX`, DSP-side McASP1 pins) were flagged there as
"❌ no CPU-side reference found" — that's now resolved (**found**, this is SCIF5), which also means the
"second independent DSP audio-serial link via McASP1" characterization for those 3 pins was very likely
wrong: the CPU side genuinely drives them as a UART (SCIF5), not an audio serializer. The DSP's own pins
being nominally McASP1-capable doesn't mean the DSP uses them that way — TI DSP pins are commonly
multiplexable between McASP and UART/GPIO, and this design apparently picked UART for boot/control traffic
over `IC901`'s own boot-flash-adjacent link. See `notes/ic7300-signal-chain.md`'s pinout table update and
`notes/multi-cpu-images.md`'s handoff section for the corresponding corrections.

## SCIF1 TX/RX pin turnaround, and a real PCB-level ground truth from the user (2026-08-29, same session, continued)

**User-supplied hardware ground truth** (direct schematic + PCB layout check, not a guess): `P6_13` (physical
pin 12) and `P7_12` (physical pin 34) are schematic-labeled `UDRXD`/`UDBSY`, **and are confirmed tied together
on the PCB** — genuinely the same net, not just a same-named coincidence. `P6_12` (pin 11) is `UDTXD`. Both
nets connect to `IC641` (`CP2102GMR`, the USB-to-UART bridge already confirmed for CI-V) but on **different**
pins from CI-V's own: `UDRXD`/`UDBSY` → `CP2102` pin 26 (`TXD`, bridge transmits → CPU receives — consistent
with `P6_13`/`P7_12` being an *input*), `UDTXD` → `CP2102` pin 25 (`RXD`, CPU transmits → bridge receives —
consistent with `P6_12` being an *output*). This is independent, direct confirmation that `SCIF1`'s
calibration link really is reachable over USB (via this second CP2102 channel), not just the REMOTE jack.

**This raised a real puzzle worth chasing**: `scif1_svc_driver_init` (see above) only enables `PMC6` bit 13
and `PMC7` bit 12 at boot — `PMC6` bit **12** (`P6_12`/`UDTXD`) is left explicitly disabled (GPIO mode) by
that function. If `P6_12` is really the TX line, why does the boot-time driver never turn its alt-function
on?

**Answer, found via a full `references_to` sweep on `PMC6`'s address**: two more functions touch it, both
already reachable from `system_mode_request_dispatch`'s own tail (i.e. they run on **every** iteration of
`sys_monitor_task`'s poll loop, not just at boot or on a mode transition):

- **`scif1_pin_mode_toggle(int)`** (`0x20011bb8`) — gated on a state flag (`*(char*)(DAT_20012514+0x5d)`):
  one branch clears `PMC6` bits 12+13 together (both alt-functions off), the other sets both together (both
  on). Also masks/unmasks SCIF1's own IRQ IDs (`0xe1`/`0xe2`/`0xe3`) in the same branches, and touches `PSR6`
  (port *set* register — a write-1-to-set sibling of the plain data register, not previously seen in this
  project) and `PM6` bit 11. 13 call sites across the firmware — a generic reusable utility, not something
  built only for this one link.
- **`scif1_switch_to_tx_mode(void)`** (`0x200120a8`) — unconditional: sets `PMC6 = (PMC6 & ~0x3000) | 0x1000`
  — **`P6_12` alt-function ON, `P6_13` alt-function OFF** (the exact opposite split from the boot-time
  state) — and flips several bits (5 set, 4/6/3/7 cleared) of SCIF1's own hardware register at `0xE8007808`
  (offset `+8` from `SCSMR_1`), plausibly `TE`/`RE` (transmit/receive enable) in `SCSCR_1`, not confirmed
  against the exact RZ/A1H bit layout yet.

**Both are called from the very end of `system_mode_request_dispatch`** (which runs every poll iteration
regardless of mode, per the existing "Factory/service mode" section below), selected by one status byte:
```c
if (*(char *)(DAT_2002a0ec + 0x60) == 0) scif1_pin_mode_toggle(1);
else                                     scif1_switch_to_tx_mode();
```

**Working hypothesis, not fully proven**: this is a **half-duplex TX/RX turnaround** for `SCIF1` — the
firmware never leaves both `P6_12` (TX) and `P6_13`/`P7_12` (RX) alt-function-enabled at the same time,
switching between "listening" and "transmitting" states on (very likely) every poll iteration. This gives a
clean mechanical explanation for the user's PCB finding: the RX side needs only passive listening, so tying
two candidate input pins to one net is harmless and maybe deliberate (board flexibility or signal integrity);
the TX side needs exactly one active driver, so it's toggled on only when actually transmitting, which is
also consistent with why the *boot-time* driver (`scif1_svc_driver_init`) leaves `P6_12` off — the link
starts in "receive/idle" mode and only switches to TX when something is actually being sent. **Not yet
traced**: who/what sets `DAT_2002a0ec+0x60` (the mode-select byte driving the turnaround itself), and the
exact RZ/A1H bit meaning of the `0xE8007808` register writes in `scif1_switch_to_tx_mode`. Both are natural
next steps if this specific thread continues; full detail in the Ghidra plate comments on
`system_mode_request_dispatch`/`scif1_pin_mode_toggle`/`scif1_switch_to_tx_mode`.

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
  recording a pass/fail byte per field. **Confirmed 2026-08-29 (DSP comms thread)**: that "fixed reference
  table" is actually the DSP's own currently-queried identity/version records (`DAT_200b1ca0`, populated by
  `dsp_identity_query_record0`/`1`/`2` at every boot) — the exact same `+0`/`+0xd`/`+0x1a` offsets are the 3
  identity records' own layout. So this file's header really is checked against the DSP's live identity —
  see `notes/multi-cpu-images.md`'s "DSP command API, continued" section for the full trace.
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
2. ~~A handful of the 27 file-RPC handlers~~ — **done, see new section below.**
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

## File-RPC table fully disassembled and decompiled (2026-08-29, follow-up session)

Closed out handoff item 2 above. User force-ARM-disassembled the 9 confirmed-broken entry points from the
earlier session, then — after a systematic re-check found the fix was incomplete (most of the table's
*internal* branches and several small shared helper functions between entries were still garbage, since
fixing only an entry point doesn't make Ghidra's auto-analysis walk forward through every branch) — force-
ARM-disassembled the whole table range `0x200b9e40`-`0x200bc350` as one contiguous block. That resolved
all but **7 tiny 4-byte islands**, each sitting exactly where two branch paths converge (a known limitation
of bulk-range disassembly: it walks the dominant path through a merge point but doesn't always split the
few bytes the *other* incoming edge lands on): `0x200ba7dc`, `0x200ba8f8`, `0x200ba918`, `0x200bab74`,
`0x200baca8`, `0x200bae10`, `0x200bb2d0`. Left un-fixed (cosmetic only, four-byte gaps don't block reading
the surrounding logic) — worth a quick pass if this area gets revisited.

**Follow-up (same day): whole-program Thumb-conflict hunt.** User's own bulk Ctrl-A "set `TMode`=0 across
the whole program" fix (attempted to finish off any stray Thumb-context regions program-wide in one shot)
failed with "Context register change conflicts with one or more instructions" — Ghidra refusing to
silently overwrite bytes *currently* disassembled as genuine Thumb. Swept all 174 "Bad Instruction"
bookmarks program-wide (Ghidra auto-creates one on every disassembly conflict encountered during analysis —
this project's established proxy for finding these bugs) and checked each address's *current* disassembly
state individually. Result: **only 4 addresses in the whole program are still genuinely live Thumb-context
conflicts**, and all 4 are exactly 4 of the 7 "tiny islands" listed above — the merge-point bytes the bulk
file-RPC-table fix skipped:

- `0x200ba8f8` → real ARM instruction is `ldrh r0,[r5]` (`e1d500b0`)
- `0x200ba918` → real ARM instruction is `ldr r0,[r6]` (`e5960000`)
- `0x200baca8` → real ARM instruction is `ldr r1,[r8]` (`e5981000`)
- `0x200bae10` → real ARM instruction is `str r0,[r1]` (`e5810000`)

(cross-checked against `objdump -m arm --adjust-vma=0x20005000` ground truth). The other 3 islands
(`0x200ba7dc`, `0x200bab74`, `0x200bb2d0`) are **not** conflicts — currently just undefined single bytes,
harmless to a `TMode` bulk set. Every other bookmark checked program-wide (the full remaining ~167) is
stale: either a proper ARM function/instruction already, or genuinely undefined bytes. So the whole-program
`TMode=0` Ctrl-A fix should succeed once these 4 addresses are individually cleared (Clear Code Bytes) and
re-disassembled as ARM — no other blockers exist anywhere else in `body.bin`.

Created `Function` objects at all 27 table entries (named `civ_table_id00`-`civ_table_id1a`) plus the
shared helper functions between them (`civ_table_shared_open_iterate`, `civ_table_str_copy_helper`,
`civ_table_trim_helper`, `civ_table_hexaddr_decode`, `civ_table_helper_580`/`680`/`710`/`82c`/`930`/`938`/
`aa9c`/`ac58`/`ad10`/`ae34`/`af64`/`b148`, `civ_table_notify_helper`) so the decompiler produces real C
instead of raw disassembly. Decompiled the handlers that were previously only characterized from raw
`objdump` reading or not at all:

- **`civ_table_id08`** (`0x08`, previously "large, not fully chased") — a genuine **bulk directory scan**:
  loops `vfs_read_dir_entry`, and for each entry dispatches by a "mode" byte (`uStack_178 & 0xff`, values
  0-9) into a family of helper functions (`civ_table_helper_580`/`680`/`710`/`82c`, each called with a
  distinct large offset literal — `0x4b0`, `0x3520`, `0x44c0`, `0x92e0` — almost certainly indices into a
  string table of known file extensions). Reads as the mechanism behind an SD-card file browser/list — "give
  me every file of type N", not a single-file operation like most of the table.
- **`civ_table_id14`** (`0x14`, previously guessed as "set-parameter-by-name") — reads file info
  (`FUN_200cc1ec`), and on one specific error path re-derives a name/extension and classifies it against two
  fixed sets of type codes (`{1,4,6,0xb}` vs `{0xe,0x1b,0x1c,0x1e}`) before a create/rename-style call
  (`FUN_200c3f98`). Reads as a file-extension-gated create-or-rename, not a settings operation — revises the
  earlier guess.
- **`civ_table_id18`** (`0x18`) — confirms the earlier wildcard-path-sanitization read, and goes further:
  after the `'*'` insertion it runs the **same** two-set extension classification as `0x14`, then does 3
  separate `FUN_200c5758` checks (values 1/2/3) that each set one of 3 output flags — reads as "classify
  this filename into up to 3 category flags" (e.g. is-it-audio/is-it-text/is-it-something-else), most likely
  feeding into the same file-browser mechanism `0x08` drives.
- `civ_table_id11`/`id12`/`id13`/`id17`/`id19`/`id1a` all confirmed as thin one-line wrappers around a
  single deeper function (`FUN_200c9bec`/`FUN_200c9d0c`/`FUN_200c9e9c`/`FUN_200cc148`/`FUN_200cb8d8`/
  `FUN_200c5dd0` respectively) — not pursued further into those deeper functions this pass.

**RETRACTED (2026-08-29): "zero genuine Thumb instructions anywhere" was wrong.** The tip below was written
before checking whether the whole program is really pure ARM — it isn't. There's a real, sizeable Thumb-2
code block, and the user's own attempt to apply the blanket fix below is what surfaced this (see "Whole-
program Thumb-conflict hunt, part 2" further down) — do **not** follow the struck-through advice.

~~this project's Ghidra language module has a `TMode` context register controlling ARM-vs-Thumb disassembly
per address, which auto-analysis is free to flip when it thinks it's found a Thumb-interworking branch.
Since this whole firmware is confirmed pure ARM (zero genuine Thumb instructions found across the entire
project), presetting `TMode = 0` across the whole `body.bin` memory range (Listing → select range →
right-click → **Set Register Values...** → `TMode` → `0`) should stop *fresh* auto-analysis from guessing
Thumb in never-before-touched territory going forward — worth doing once, proactively, rather than
continuing to hit this bug address-by-address as new code gets examined. Doesn't undo already-broken
analysis (still a one-time cleanup where it's already happened), and won't stop a genuine odd-address
`BX`/`BLX` target from switching context at that one spot — but this firmware doesn't appear to do genuine
ARM/Thumb interworking anywhere, so that caveat shouldn't matter in practice.~~

## Whole-program Thumb-conflict hunt, part 2: real Thumb code found, "zero Thumb" retracted (2026-08-29)

Follow-up to the bookmark sweep above. User tried the whole-program `TMode=0` Ctrl-A fix suggested by the
(now-retracted) tip above; Ghidra correctly refused. While investigating why, the user noticed Ghidra
decoding parts of the program around `0x2015xxxx` sensibly in Thumb mode but as garbage in ARM mode —
directly contradicting the "pure ARM" claim. Verified with `arm-none-eabi-objdump` ground truth in both
modes (`-M force-thumb` forces Thumb decode of a raw binary), and confirmed:

- **Real, coherent Thumb-2 code exists**, roughly `0x2014b000`-`0x2017d400` (~200 KB) — clean function
  prologues/epilogues (`push {r4,r5,r6,lr}` / `pop {r4,r5,r6,pc}`), sensible in-range `bl`/`b.w` targets,
  `cbz`/`it`/32-bit Thumb-2 encodings throughout. This is the statically-linked **FreeType** library body —
  matches the `TxtRender_FT2_Init`/`_Create`/`_Destroy`/`_setSize`/`_LoadGlyphData`/`_GetFonInfo` wrapper
  functions and the `truetype`/`postscript-cmaps`/`sfnt-table`/`postscript-font-name` string tables already
  known nearby (e.g. `FUN_20108cc8`'s FreeType internals, `notes/` references to font rendering). One ~14 KB
  **ARM**-mode sub-block sits embedded inside it at `0x20158400`-`0x2015bc00` (confirmed by the same
  ground-truth method — clean ARM function bodies, calls back into the Thumb region and out to known ARM
  helpers around `0x200f3890`/`0x200f4af0`/`0x200f6474`/`0x2017c766`) — plausible per-object-file ARM/Thumb
  mix within one statically-linked library, not unusual for a real-world build.
- **Ghidra's own auto-analysis already has this region correct** — checked live: `0x201544a0` currently
  disassembles in Ghidra's listing as `push {r4,r5,r6,lr}` (Thumb), matching ground truth. That's *why* this
  never showed up in the 174-bookmark "Bad Instruction" sweep above: auto-analysis correctly propagated
  Thumb context here from the start (almost certainly via `BLX`/interworking-branch detection into this
  region), so no conflict was ever recorded. **No fix is needed here** — this was purely a documentation
  error, not a live bug.
- **This is exactly why the blanket Ctrl-A `TMode=0` fix was the wrong tool**: applying it program-wide
  would have silently converted this entire correct, real Thumb-2 library into garbage ARM. The user's
  instinct to test first ("i guess the ctrl-a is not very feasible") caught this before any damage was done.
  The bookmark-based per-address sweep remains the right approach precisely *because* the rest of the
  program (outside this one library) really is ARM-only, so Ghidra's own conflict bookmarks are a reliable
  proxy there — the file-RPC table's 4 genuine islands identified above are still the only real, actionable
  fix outstanding.
- **False-positive trap, worth remembering**: a first-pass coarse scan (`tools/arm_thumb_scan.py`, see
  below) flagged huge additional swaths of the image as "Thumb" — megabytes of it, mostly beyond
  `0x20185000`. Spot-checking several samples (`0x201ad000`, `0x20225000`, `0x202c1000`, `0x202f9000`, …)
  showed these are **font/glyph bitmap data** (highly repetitive byte patterns like `ff ff 00 ff` or
  `85 85 00 85`), not code in either mode — Thumb's denser 16-bit encoding space makes random/structured
  *data* statistically far more likely to "decode clean" than ARM's 32-bit space, so a bare bad-instruction-
  count heuristic produces false positives on data-heavy regions. Only trust a "looks like Thumb" verdict
  after eyeballing actual code-flow coherence (real prologues, in-range branch targets, non-repeating
  bytes) — which is why the FreeType boundaries above were hand-verified, not just heuristic output.

**Tooling built from this**: `tools/arm_thumb_scan.py` — a local, Ghidra-independent ground-truth scanner
(objdump-based, both ARM and forced-Thumb decode, windowed bad-instruction-count classification). Explicitly
a **locator, not an auto-fixer** — it never touches the live Ghidra project, and given both near-misses above
(almost corrupting a correct Thumb region via blanket Ctrl-A; the data-vs-code false-positive trap) it's
staying that way. Use it to narrow down where to look; verify by hand before changing anything in Ghidra,
same as every other fix in this project.

## Session handoff update (2026-08-29, later same day) — read this before the section above

The "Session handoff" section further up this file is from earlier the same day and is now partly stale.
Status update, no need to re-derive:

- **Still genuinely pending, low effort**: the file-RPC table's 4 real Thumb islands identified above
  (`0x200ba8f8`/`0x200ba918`/`0x200baca8`/`0x200bae10`) — exact ARM replacement instructions are documented
  above, just need Clear Code Bytes + re-disassemble in the GUI. Not yet confirmed done.
- **New major thread opened, not on the old handoff list at all — this is the active thread for the next
  session**: DSP interaction. `SCIF5` identified as the DSP's real command/data link (full transport chain
  traced, MTU2-timer-paced, down to `SCFTDR_5`); confirmed DSP Program/DSP Data get written **live** during
  `firmware_update_main`, almost certainly via the DSP reprogramming its own boot flash (`IC902` — corrected
  this session from an earlier, wrong "FPGA config flash" call; its pins trace directly to the DSP's
  `BOOT[4:0]`/SPI0 strapping pins). `DRESD`'s release is still unfound despite the DSP clearly running (live
  `SCIF5` parameter-sync traffic proves it). **Full ranked next-steps list, what's already ruled out, and
  don't-re-derive orientation**: `notes/multi-cpu-images.md`'s "Handoff: DSP comms/firmware thread,
  continuing in a new session" section (end of file) — read that first, not this bullet, before resuming.
- **Closed this session**: the UI icon/bitmap resource format (`notes/bitmaps.md`) — was on the older
  standing-candidates list as "raw/uncompressed bitmap material", now fully solved with 150 icons
  individually identified and renamed.
- The rest of the older "Session handoff" section above (mode5_combo, `factory_file_case28_report`, SCIF3
  packet types, the real CI-V dispatcher, older standing candidates, JTAG-dependent items) is untouched —
  still accurate, still the list to work from once the two items above are resolved.

## Chased the two remaining mystery tasks without JTAG (2026-08-29, much later session)

User's ask: with only 2 genuinely unresolved task identities left in the whole catalog (`kernel_start`'s
own descriptor at `0x203907c4`, and the dynamic `thunk_FUN_2007ea68` case), and the total task count being
small, is there more to find via a wider static sweep before conceding both need live JTAG? Checked both
properly, with a real result on one of them.

**A genuine, previously-undocumented finding along the way: most of the catalog's task descriptors sit in
one compiled array.** Laying out all known descriptor addresses and sorting them revealed 9 of them —
`FUN_2007ef5c` (UI/display), `sdcard_file_rpc_dispatch_task`, `sd_menu_dispatch_task`,
`audio_buffer_task_2006bb58`/`2006c2c4`, `status_poll_task_200095d8`, `periodic_poll_task_20014384`,
`bmp_capture_task`, and `queue_driven_task_2001745c` — packed into a single 16-byte-stride array,
`0x2019889c`-`0x2019891c` (9 × 16 bytes), immediately followed by an unrelated data table that reads as a
literal HF **band-plan/frequency table** (`3500000`/`3580000`, `7000000`/`7200000`, `14000000`/`14350000`,
`21000000`/`21450000`, `28000000`/`29700000` — the 80/40/20/15/10m band edges in Hz), cleanly bounding the
array with no room for a hidden extra descriptor. Read the whole thing directly via `memory.read` and
parsed it in Python rather than trusting `references_to` alone — a useful confirmation that the "descriptor
per task" model this project has used throughout is accurate, but this specific array held no surprises:
every slot matched an already-known task, none pointed at either mystery.

**`kernel_start`'s own mystery task (`0x203907c4`) — re-confirmed as a genuine dead end, more rigorously
than before.** `kernel_start` calls `itron_act_tsk` with this address as a **plain literal**, not a computed
value (`itron_act_tsk(DAT_200052b0, 0)`, where `DAT_200052b0`'s own value is `0x203907c4`) — so a literal
search really should be exhaustive here, unlike some of today's earlier RAM-sweep surprises. Checked two
independent ways: a fresh `references_to` on `0x203907c4` (finds exactly the one reader, `kernel_start`
itself, no writer) and a raw 4-byte literal scan of the whole `body.bin` image (also exactly one match, the
same address). The surrounding 128 bytes are uniformly `0xFF` — not a compact table with one gap the way the
9-task array's "missing" slot first looked, just a genuinely blank reserved region. **This is about as
thoroughly re-confirmed as a static-analysis dead end can be** — there is no writer anywhere in the compiled
image; the real value only exists at runtime. Still needs live JTAG.

**`thunk_FUN_2007ea68` — the activation mechanism itself got fully traced, though the first guess at
*what* it activates didn't survive a check against the real manual (see below).** `FUN_2007ea68` itself is
a generic "activate whatever task descriptor the caller hands me" helper
(`itron_act_tsk(*param_1, param_1)`), unlike every other task's fixed-literal activation — explaining why it
looked like a dead end to begin with. Its caller isn't found by a normal reference lookup because it's
reached through a **chain of ARM/Thumb interworking veneers** (`bx pc` + `b <target>` pairs, the standard
linker-generated pattern for cross-mode calls — several links deep, each auto-named `thunk_FUN_2007ea68` by
Ghidra, which is why they all look identical at a glance). Walked the chain (`0x20184950` →`0x2015824c`
→ two further branches) down to 3 genuine, non-veneer call sites:
- **`FUN_2014ee46`** — touches a peripheral at `0xe8100000` with register offsets
  `0x4004`/`0x4010`/`0x4014`/`0x4018`/`0x401c` and small size constants `0x20`/`0x40`.
- **`FUN_201009d2`** — wraps `FUN_2014ee46` inside a connect/disconnect-shaped state machine, with sibling
  thunk calls (`thunk_FUN_2007ea28`/`thunk_FUN_2007e0f8`) that read as deactivate/queue-teardown
  counterparts — not examined yet.
- **`FUN_2014d146`** — a retry/completion-callback handler that lazily activates a task once a retry
  queue becomes non-empty, the same "activate on demand" shape.

**First guess: "USB device/host controller subsystem" — checked against the real manual and RETRACTED.**
The `0x20`/`0x40` size constants looked like classic USB max-packet sizes, so this session initially
renamed the two functions `usb_controller_configure`/`usb_connect_disconnect_handler` and labelled
`0xe8100000` as `USB_CTRL_BASE`. **User pushed back, correctly**: the only USB connectivity this project has
ever established is via dedicated external chips with their own USB silicon (`IC621` `TUSB2046BIVFRG4` hub,
`IC661` `PCM2901E` audio codec, `IC641` `CP2102GMR` serial bridge — see [[ic7300-hardware]]), none of which
need the main CPU to run a USB protocol stack at all. Checked directly against the real RZ/A1H hardware
manual (`R01UH0403EJ0600`, extracted and searched in full, ~213,000 lines): the actual, explicitly
documented USB2.0 host/function module register bases are `SYSCFG0_0 = 0xE801_0000` (channel 0) and
`SYSCFG0_1 = 0xE820_7000` (channel 1) — **neither matches `0xe8100000`** (easy to misread as the same address
at a glance; they aren't). Searched the whole manual for any register table documenting
`0xE810_0000`-`0xE813_FFFF`: **none exists** — the top-level bus address map (§5.4, Table 5.5) lists this
range only as a generic "I/O area, SLV5" bus-matrix slave, with no peripheral chapter naming what's actually
there. Also checked and ruled out as alternatives: Ethernet (`0xE820_3xxx`/`0xE820_4xxx`, a different bus
slave, SLV4), SD Host Interface (`0xE804_Exxx`, SLV2). **Reverted the Ghidra renames**: `FUN_2014ee46`/
`FUN_201009d2` are back to generic names (`slv5_periph_configure`/`slv5_periph_connect_disconnect_handler`
— kept the "SLV5 peripheral" framing since that bus-matrix fact is solid, dropped "USB" entirely since it
isn't), and `USB_CTRL_BASE` is now `UNIDENTIFIED_SLV5_PERIPH_BASE`.

**Net effect on the "2 mystery tasks" framing, corrected**: down to 1 genuine, thoroughly-reconfirmed
JTAG-only dead end (`kernel_start`'s own descriptor), and 1 where the *mechanism* is now understood (a
dynamic task activated on some connect/disconnect-shaped event touching an unidentified peripheral at
`0xe8100000`/SLV5) but the peripheral's actual identity is genuinely open again — not USB, not yet
replaced with a better-supported guess. **Concrete next steps if this is picked up again**: check whether
any already-traced CPU pin/net connects to a chip-select or control line that would explain what's wired to
this specific bus-matrix slave (SLV5), since the general RZ/A1H I/O bus-matrix slaves elsewhere in this
project have mapped cleanly onto specific on-board peripherals once the right net was checked; or read
`thunk_FUN_2007ea28`/`thunk_FUN_2007e0f8` and the earlier stages of `FUN_201009d2` for more contextual
clues before guessing a specific peripheral again.

## `thunk_FUN_2007ea68`/SLV5: indirection ruled out, and a much better identity lead found (2026-08-29, later session)

User's ask, prompted by the earlier RIIC1/RIIC2 xref-tool mislabeling caught during the SVD peripheral
sweep (see [[memory-map]]): before trusting `UNIDENTIFIED_SLV5_PERIPH_BASE` (`0xe8100000`) at all, rule out
any indirection — is this address reached via a mutable pointer cell that something else could have altered,
the same way the RIIC investigation nearly went wrong?

**Checked directly at the machine-code level, not just the decompiled pseudocode.** `slv5_periph_configure`
(`0x2014ee46`) loads the base through a standard ARM Thumb-2 PC-relative literal-pool read
(`ldr r6,[0x2014f25c]`), and the raw bytes at `0x2014f25c` are `00 00 10 e8` = `0xE8100000` exactly — a plain
compile-time constant sitting in the code section, not a RAM-resident global. **A `references_to` write-check
on `0x2014f25c` finds exactly one reference in the whole image: the single `READ` at `0x2014f174` (the load
itself) — zero writers anywhere.** This is about as unambiguous as a hardware address reference gets in this
codebase: nothing computes it, nothing patches it, and it can't have been altered by anything else at
runtime. The two low-level accessors this base flows into, `FUN_2007e1e0`/`FUN_2007e1d4`, are also confirmed
to be trivial raw pointer dereferences (`*(param_1 + (param_2 & ~3)) = param_3` / `return *(...)`) with no
further table lookup or handle-translation layer — so once the base is confirmed, so is every access through
it. **A hex search for the raw literal `00 00 10 e8` across the whole image found 18 independent copies**
(`0x2014ee00`, `0x2014f25c`, `0x2014f69c`, `0x2014f814`, `0x2014fc1c`, `0x20150074`, `0x201504d4`,
`0x20150934`, `0x20150d90`, `0x201511e8`, `0x201515f8`, `0x20151ac0`, `0x20151f90`, `0x20152418`,
`0x20152670`, `0x2015fc1c`, `0x20160068`, `0x20161394`) — standard ARM compiler codegen (each function keeps
its own literal-pool copy of constants it needs), not a single point of failure even in principle. **Verdict:
no missed indirection, no altered reference — the address is exactly what it appears to be.**

**Unplanned but far more useful discovery, made while characterizing the scope of this module for the check
above.** Listing every function between the first and last of those 18 literal-pool addresses (roughly
`0x2014ee00`–`0x20161394`) turned up **hundreds of functions**, not the 2-3 examined in the original trace —
this "peripheral driver" is a substantial subsystem, not a small isolated block. A `references_to` sweep on
the two raw MMIO accessors (`FUN_2007e1e0`/`FUN_2007e1d4`) found 100+ call sites just in the first page,
confirming real breadth. Pulling `ghidra://program/body.bin/strings` bounded to this address range turned up
**zlib's own verbatim internal error messages** (`"oversubscribed dynamic bit lengths tree"`, `"invalid
distance code"`, `"invalid block type"`, `"unknown compression method"`, `"incorrect header check"`, `"need
dictionary"`, a `"1.1.4"` version-shaped string, etc., clustered `0x2015970c`–`0x2015c124`) — genuine zlib
deflate/inflate algorithm code embedded in this firmware, entirely separate from the project's
already-established custom Okumura LZSS firmware-update compression ([[decompression-lzss]]). A second,
higher-level wrapper's error strings (`"unexpected zlib return code"`, `"zlib IO error"`, `"bad parameters to
zlib"`, `"unsupported zlib version"`, `"deflateEnd failed (ignored)"`) sit earlier and separately, at
`0x200cf038`–`0x200e7234`. **Not yet established whether this zlib/wrapper code is actually used by the SLV5
driver module or is simply linker-adjacent** (a statically-linked library commonly ends up placed near
whatever pulled it in, without that implying a direct logical relationship) — flagged as a real open question,
not asserted as connected.

**The actual identity breakthrough: traced the one external caller of the whole module.**
`slv5_periph_connect_disconnect_handler` (`0x201009d2`) has exactly one caller in the entire image:
`0x200792ac`, inside a function now renamed **`graphics_stack_startup_egl_openvg`** (`0x20079240`). That
function is a **real, unambiguous EGL + OpenVG (Khronos vector-graphics API) bring-up sequence** — not
inferred from shape, but named explicitly by its own embedded debug/log strings, each guarded by an
`"ERROR!! <name>"` printf on failure:
```
ERROR!! NCGSYS_FrameMemCreate (%d)
ERROR!! eglStartUp
ERROR!! initNativeResource
ERROR!! vgStartUp          <- this is the guard around slv5_periph_connect_disconnect_handler()
ERROR!! eglGetDisplay (0x%04X)
ERROR!! eglInitialize (0x%04X)
ERROR!! eglBindAPI (0x%04X)
```
The call sequence in `graphics_stack_startup_egl_openvg` is, in order: `NCGSYS_FrameMemCreate`-equivalent →
`initNativeResource`-equivalent → `eglStartUp`-equivalent → **`slv5_periph_connect_disconnect_handler`,
guarded by the `"vgStartUp"` error string** → `eglGetDisplay` → `eglInitialize` → `eglBindAPI(0x30A1)`.
`0x30A1` is the real Khronos EGL enum value for `EGL_OPENVG_API` (as opposed to `0x30A0`
`EGL_OPENGL_ES_API`) — confirms this specifically targets OpenVG, not OpenGL ES. `"NCGSYS"` is an
unidentified vendor/SDK codename, not yet matched to a specific commercial embedded graphics middleware
package (worth a web search if resumed) — but the EGL/OpenVG identification itself is not a guess, it's a
direct read of the code's own strings.

**Updated working hypothesis for `UNIDENTIFIED_SLV5_PERIPH_BASE`, replacing "not yet replaced with a
better-supported guess" above**: since `slv5_periph_connect_disconnect_handler` *is* (or is a required part
of) this graphics stack's `"vgStartUp"` hardware bring-up step, whatever lives at `0xe8100000` is very
plausibly a **dedicated 2D/vector-graphics rendering resource** — a hardware accelerator/compositor/blitter
distinct from the already-known `VDC5` display-timing-and-output controller ([[memory-map]]), or some
DMA/memory resource this graphics stack specifically needs at startup. **Not yet confirmed at the
register-bit level** — the offsets already known (`0x4004`/`0x4010`/`0x4014`/`0x4018`/`0x401c`) haven't been
individually matched against any known 2D-GPU-IP register layout. Renamed in Ghidra:
`graphics_stack_startup_egl_openvg` (`0x20079240`); `slv5_periph_configure`/
`slv5_periph_connect_disconnect_handler` kept their existing names (still accurate at the bus-matrix-slave
level) with PLATE comments added pointing to this finding. **Concrete next steps if this is picked up
again**: identify the `"NCGSYS"` SDK via web search (may directly document what hardware `vgStartUp` expects);
individually decode the `0x4004`/`0x4010`/etc. register offsets against common 2D-accelerator register
conventions (framebuffer address, stride, format, control/status); check whether the zlib code found nearby
is actually reachable from this graphics stack (e.g. for compressed vector-asset decoding) or is genuinely
unrelated linker-adjacent code.

## Theory tested, same day: is the "connect/disconnect" event an external monitor (DVI on sibling models)?

User's theory: other radio models sharing parts of this codebase reportedly have DVI display connectors —
could the connect/disconnect event this whole subsystem responds to be an external monitor being plugged in,
rather than something internal (which wouldn't need connect/disconnect semantics at all)?

**Supporting circumstantial evidence found**:
- The RZ/A1H genuinely has two independent display-timing channels, `VDC50`/`VDC51` (confirmed via the SVD
  import), and this firmware's own code treats them as a real, generic multi-channel capability, not
  single-channel with dead placeholder fields: found a literal-pool table at `0x2019eeb0`+ holding parallel
  per-channel register-offset arrays for both `VDC50` (base `0xFCFF7400`) and `VDC51` (base `0xFCFF9400`),
  and a channel-configure function (`FUN_20074a4c`) explicitly parameterized by channel index
  (`DAT_200753b8 + channel*0x20`) that indexes into that shared table — written to support N channels
  generically, not hardcoded to one. This is exactly the shape you'd expect if a shared codebase serves both
  single-screen and dual-output sibling models.
- A "connect/disconnect"-triggered bring-up genuinely fits an external, hot-pluggable device better than
  fixed internal hardware (the touchscreen is always physically present — it wouldn't need this framing).

**Not confirmed, real gaps**:
- Could not pin the actual runtime channel-index value(s) passed into `FUN_20074a4c` — its caller
  (`FUN_200713fc`) itself has **zero static references anywhere in the image**, the same "reached only through
  an indirect/dispatch-table call" dead end this project has hit repeatedly elsewhere (task activation,
  `sys_monitor_task_entry`, etc.). Can't yet say from static analysis alone whether `VDC51` is ever actually
  activated on real shipped IC-7300 units, or is dead capability on this specific model.
- **`VDC50`/`VDC51` are a completely different piece of hardware from the `0xE8100000`/SLV5 peripheral** this
  whole thread is actually about (different bus-matrix slave entirely) — even if `VDC51` turns out to be
  live, that wouldn't by itself prove the SLV5/EGL-OpenVG subsystem is what drives it. The two threads are
  thematically connected (both are display-adjacent) but not yet shown to be the same mechanism.
- **A negative result worth recording**: swept `body.bin`'s full string table for any leftover external-
  display/monitor-mode UI text (`DVI`, `HDMI`, `VGA`, `EDID`, `External Display`, `Second`/`Dual` display,
  common external resolutions) — zero hits. Settings menus in this firmware almost always leave a visible
  string even when gated off by a model check (see the diode-matrix regional-variant precedent), so this is
  a real (if not conclusive) point against a user-facing "external monitor" *feature* specifically — doesn't
  rule out a silent/fixed-purpose secondary output with no menu at all.
- No schematic/BOM evidence found in `notes/ic7300-hardware.md`/`notes/ic7300-signal-chain.md` of an actual
  DVI/HDMI/VGA connector or an LVDS-to-DVI/HDMI bridge chip on the IC-7300's own board (only the already-noted
  `IC1212`/`IC1315` `SN65LVDS1DBVR` LVDS transceivers, role "not yet traced").

**Verdict: plausible, genuinely worth keeping as the leading hypothesis, not confirmed.** Best concrete next
step if pursued: get a schematic/BOM for one of the specific sibling models the user has in mind and check
for an LVDS-to-DVI bridge IC — that would settle it far faster than continuing to chase indirect calls
through static analysis alone.

## Follow-up, same day: real hardware context from the user, and the "connect" check traced — it's not a hardware detect

**User-supplied hardware fact, IC-7610 (same era, different radio in this family)**: has a **TFP410PAP** (TI's
well-known parallel-RGB-to-DVI/TMDS transmitter) between its DVI connector and the rest of the system, and
uses a **separate sub-CPU dedicated to driving the display**, distinct from the main radio-control CPU.
**Confirmed, not a transcription slip**: both the main and display sub-CPU are genuinely the same part,
**`R7S721001VCBG`** — initially flagged here as a likely copy/paste error since it matched the main CPU's own
part number exactly, but the user's source PDF (the IC-7610 service manual) turns out to reverse character
order on copy-paste (a real, reproducible PDF text-extraction quirk in that specific document — worth
remembering if this project ever works with that PDF directly, e.g. via `pdftotext` or a similar
extraction tool, since a naive extraction would silently produce backwards part numbers/text throughout).
**`R7S721001VCBG` is the same specific RZ/A1 part already on record as the IC-9700's own main CPU**
([[ic9700-hardware]]) — notably **not** the same part as the IC-7300's own main CPU (`R7S721000`, a
different specific part within the same RZ/A1H family, see [[memory-map]]). So IC-7610 apparently uses two
full RZ/A1 SoCs of the IC-9700's exact main-CPU part, one purely for display/DVI duty — a genuinely
substantial dedicated graphics computer, which fits a real EGL+OpenVG stack being worth having far better
than a minor touchscreen driver would. **Checked, and it's a dead end for new SLV5 documentation, but turned up a directly relevant official fact
instead**: `R7S721001VCBG` and `R7S721000` are **both plain RZ/A1H parts** (confirmed via Renesas' own
product page and distributor listings) — different specific SKUs/package options within the *same* family
and the *same* hardware manual (`R01UH0403EJ0600`) already exhausted for the SLV5 search, not a different
chip family with its own separate datasheet to check. So there's no second manual to search here. **What the
search did surface, from Renesas' own RZ/A1H marketing copy**: *"With 10MB on-chip SRAM, the RZ/A1H supports
up to 2 Displays with WXGA (1280x800) resolution without the need for external memory."* — an official,
direct confirmation that dual-display output is a real, first-class, marketed capability of this exact chip
family, not a speculative reading of `VDC50`/`VDC51` existing in the SVD. Doesn't confirm the specific
`0xE8100000`/SLV5 register-level identity, but it's solid, independent support for the general
"this chip family is built for exactly this kind of dual-output graphics use case" framing this whole thread
has been assembling piece by piece.

**Is EGL/OpenVG itself open source, could that help trace this?** Nuance worth being precise about:
- EGL and OpenVG are **Khronos Group specifications** (free, public documents), not code. The specific
  function names this firmware calls that match the real spec (`eglGetDisplay`, `eglInitialize`,
  `eglBindAPI`) are standardized — any conformant implementation from any vendor uses those exact names, so
  matching them doesn't identify *which* implementation this is.
- Real open-source OpenVG *implementations* do exist (e.g. Ivan Leben's "Vincent", "ShivaVG") but these are
  software/OpenGL-backed rasterizers aimed at desktop Linux — nothing about their internals would explain a
  vendor-specific hardware bring-up sequence on an embedded ARM SoC.
- **The actual distinguishing names here — `NCGSYS_FrameMemCreate`, `initNativeResource`, `eglStartUp`,
  `vgStartUp`** — are *not* part of the Khronos EGL/OpenVG spec at all. They read as a vendor SDK's own
  **porting-layer hooks**: the small amount of platform-specific glue code an integrator (Icom, or whoever
  wrote this firmware) must implement to plug their own hardware into a portable, otherwise-closed-source
  commercial OpenVG driver. That glue code is normally NOT open source even when the SDK is well-documented.
- **Web search for the exact strings came up empty**: `"NCGSYS_FrameMemCreate"`, `"vgStartUp"` +
  `"initNativeResource"` together, and `"NCGSYS"` alone, returned nothing relevant anywhere indexed online —
  not matched to any known commercial embedded graphics SDK (AmanithVG, Vivante, Imagination, etc. context
  didn't turn up either). Genuinely unidentified; plausibly an obscure/Japanese-market SDK with no
  English-language footprint, or a fully in-house Icom codename. **Tracing this from "the EGL/OpenVG side"
  isn't productive right now** — there's no public source to diff against, and the standard API calls
  themselves don't reveal the vendor.

**More useful: actually traced the "connect" gate condition, and it isn't a hardware/GPIO check.**
`slv5_periph_connect_disconnect_handler`'s branch between "do the full bring-up" and "skip it" is decided by
`FUN_20156174()` — read in full, this function **does not touch any hardware register at all**. It checks a
plain RAM flag and, on first call, lazily allocates a small memory pool via the same generic allocator
(`FUN_20153e60`) used throughout this codebase for ordinary buffer/resource management (seen identically in
`riic1_driver_init` and the SCIF driver-init functions). The gate is "did my memory allocation succeed", not
"is an external cable/device physically present". **This weakens the literal "hot-plug" reading of
connect/disconnect** — it looks much more like this SDK's own generic terminology for "acquire" (connect)
and "release" (disconnect) of a rendering context's native resources, which would run identically whether or
not any physical external display exists. (Not exhaustively proven — `slv5_periph_configure`'s own internal
field checks against caller-supplied config values, and the deeper `FUN_200fe14e(0)` condition also gating
this handler, haven't been individually traced to rule out a hardware check further downstream.)

**Updated overall read, incorporating the IC-7610 fact**: if IC-7610 really does use a separate,
same-family sub-CPU to own its display + the TFP410/DVI path, the most likely explanation is that Icom
licensed or wrote this "NCGSYS" graphics middleware **once** and reused it across whichever CPU in a given
product actually owns the display — the main CPU on IC-7300 (no sub-CPU, drives its own touchscreen
directly), a dedicated sub-CPU on IC-7610 (drives its own panel and, via the TFP410, an external DVI output).
Under this reading, the EGL/OpenVG bring-up code found in **this image (`body.bin`, IC-7300's single main
CPU)** is likely genuine, functioning code for the IC-7300's *own* touchscreen — not dead/vestigial DVI
support — while the actual DVI-specific logic on IC-7610, if it exists as a distinct code path at all, would
live in **IC-7610's own firmware** (very possibly its separate sub-CPU's image, which this project doesn't
have). **This reframes the productive next step**: continuing to dig in the IC-7300 image for DVI-specific
evidence is now a weaker bet than it looked before this check — the natural place to look for real DVI
bring-up logic is a firmware dump for IC-7610 (or whichever sibling model), if one becomes available, as a
genuinely new side thread rather than a continuation of this one.

**What `0xE8100000`/SLV5 most plausibly is now, net of all sessions on this thread**: an internal RZ/A1H 2D
rendering/graphics-acceleration resource that this shared graphics middleware brings up as part of its own
generic startup sequence on *any* product using it — not confirmed to be tied to any specific external
connector on the IC-7300 specifically. Still not confirmed at the register-bit level; still open.

## `queue_driven_task_2001745c` fully resolved: the Voice-recording file-I/O task (2026-08-29, later session)

User's ask, after a triage of which catalogued tasks were only lightly analyzed: pick the best remaining
candidate and dig deeper. Picked this one specifically for its outlier stack size (`0x2000`/8 KB, the
largest in the whole catalog, never explained).

**Renamed `voice_recording_file_task`.** Decompiled the task entry point itself first (already had a plate
comment from the 26th session recording it as a real queue-driven state task, purpose unknown) — its own
body reads a queue for a status tag and toggles a flag via `FUN_200c6374`, unremarkable on its own. The real
identity came from following the data, not the control flow:

- **A literal path string, `C:\IC-7300\Voice`, sits inside this task's own small code cluster**
  (`0x20016fec`, well within the `0x20015e00`-`0x2001746c` span this task's code occupies). Checked its two
  references directly:
  - **`voice_file_check_todays_filename`** (renamed from `FUN_20016dd4`, `0x20016dd4`): builds a date-stamped
    filename via the already-named `rtc_shadow_read_atomic()`/`build_date_filename()` helpers, concatenated
    onto the `C:\IC-7300\Voice` path, then compares it against a candidate. Deriving a filename from "the
    current date/time" is a strong, specific signal for *starting a new recording* (playback would need to
    open a user-selected *existing* file, not one just computed from "now").
  - **`voice_file_io_state_machine`** (renamed from `FUN_20017000`, `0x20017000`): a real 4-state (open →
    process → close → idle) file-I/O state machine. Builds the full path (`C:\IC-7300\Voice` + an optional
    subdir/filename param), manages a 4-slot ring buffer of pending blocks (`param_1 + index*8`, index mod
    4), and — the key confirmation — **posts commands through `file_rpc_post_command`** (command IDs `6` and
    `9`, individual semantics not decoded) — **the exact same SD-card file-RPC service already established
    as `sdcard_file_rpc_dispatch_task`'s own dispatch mechanism** (see this file's `civ_command_dispatch_task`
    retraction section). This is a genuine, confirmed real *client* of that service — the RPC dispatcher's
    own investigation never identified a concrete caller, so this closes a real gap on both sides at once.
- **The flag-toggle helper `FUN_200c6374` (not renamed — still a shared/generic-looking primitive, not
  task-specific) sits inside a substantial, separate filesystem-driver module** (`0x200c6300`-`0x200cc300`ish)
  with its own internal debug-log strings: `"FS_TK"`, `"FS_CTL"`, and several `"GRP_FS: ..."` formatted trace
  messages (file/buffer reference counting, "file still busy", "file not blocked", block I/O with
  `blk_shift`). This is a real embedded filesystem driver layer sitting underneath the file-RPC service —
  not chased further this session (out of scope for identifying the task itself), but worth remembering as
  a distinct, substantial subsystem (à la the `SLV5` graphics module) if a future session wants to fully map
  the storage stack.

**Bottom line**: `voice_recording_file_task` is the file-I/O half of the SD-card voice-recording feature —
opens/creates a date-named file under `C:\IC-7300\Voice`, streams data to it in blocks through the
already-known file-RPC service. This directly strengthens (doesn't yet fully confirm) the `audio_buffer_task`
pair's long-standing "plausibly circular audio buffer manager for SD-card WAV record/playback" hypothesis —
the natural reading is now that `audio_buffer_task_2006bb58`/`2006c2c4` manage the live PCM ring buffer while
this task manages writing that buffer's content out to the SD card as a file, i.e. the two are very likely
two halves of one feature, not independently-uncertain guesses anymore. Not yet independently proven (no
direct call/queue link between the two subsystems has been traced) — a good next step if this thread is
picked up again: check whether `audio_buffer_task`'s ring buffer and this task's queue messages share a
common producer, which would nail the connection directly.

**Correction (2026-08-30, next session — see the `voice_tx_memory_*` section below)**: this "two halves of
one feature" reading turned out to be wrong. `audio_buffer_task_2006bb58`/`2006c2c4` read from a *different*
SD-card folder (`C:\IC-7300\VoiceTx`) than this task's own `C:\IC-7300\Voice` — they're sibling features
(TX voice-message playback vs. this task's own recording) sharing the same file-RPC plumbing, not two
halves of one feature. Recorded here rather than silently edited away, per how this project tracks
corrections.

## `audio_buffer_task` pair fully resolved: TX Voice Memory playback, not the record-side counterpart (2026-08-30)

User's ask, continuing the task-catalog triage: dig into the `audio_buffer_task` pair next.

**Renamed `voice_tx_memory_control_task`/`voice_tx_memory_stream_task`.** Decompiled both task entry points
fresh — each is a state machine reading its own state byte from a shared control struct (`DAT_2006c3d4`,
offsets `+4`/`+5`), dispatching to a distinct set of sub-handlers, confirming the existing "sibling state
machines sharing state" read from the 26th session. The real identity, again, came from a literal path
string rather than the control flow:

- **`"C:\IC-7300\VoiceTx"`** (`0x2006b39a`) sits inside `voice_tx_list_messages` (renamed from
  `FUN_2006b2c4`, `voice_tx_memory_control_task`'s state-5 handler) — **note: `VoiceTx`, not `Voice`** — a
  different SD-card folder from `voice_recording_file_task`'s own `C:\IC-7300\Voice`. This one function does
  a real directory listing: builds the path, calls `FUN_200bc8b4` (which posts file-RPC command `0x17`,
  "list"), then for each returned entry builds a full path and calls `FUN_2002232c` with two fields from a
  per-entry struct — populating some kind of message index.
- **`voice_tx_resolve_message_slot`** (renamed from `FUN_2006ad30`, reached from `voice_tx_memory_control_task`'s
  state 1 via `FUN_2006b99c`): converts an ASCII numeric string to packed nibbles, then **binary-searches a
  sorted lookup table** (`DAT_2006a560`-based) to resolve that number to a specific file — exactly the shape
  you'd expect for "resolve TX Voice Memory slot N (1-8, the IC-7300's real documented feature) to its
  backing file."
- **`voice_tx_read_block`** (renamed from `FUN_2006af28`, called from **`voice_tx_memory_stream_task`**'s
  states 1 and 5 — confirming task B, not task A, is the actual data-reader): wraps `FUN_200bc754`, which
  posts file-RPC command `0x13` ("read at offset"). The "ring-buffer wraparound" shape the 26th session
  originally flagged (position math against a size field at `DAT_2006c404+0x30`) is fully explained by this
  — it's buffered file-read position tracking (how far into the file the next read should start), not a raw
  PCM/hardware ring buffer.

**This also rounds out the file-RPC service's command map**, previously only partially known: `6` = open
(seen from `voice_recording_file_task`), `9` = a second write/append-shaped op, `0x13` = read-at-offset,
`0x17` = list directory. Individual field semantics within each command's payload still not decoded.

**Correction to last session's own speculation** (see the correction note added just above, in the
`voice_recording_file_task` section): this pair is **not** the ring-buffer/audio-hardware half of that
task's recording feature — it reads a *different* folder entirely. The real relationship: two sibling
features (TX voice-message **playback** vs. voice **recording**) that happen to share the same underlying
SD-card file-RPC infrastructure, not two halves of one feature. Good general lesson for this thread: a
shared mechanism (same RPC service, same "audio + SD card" flavor) doesn't imply the same feature — the
actual folder path settled it immediately once found, where the control-flow shape alone was ambiguous.

**Still open, not chased this session**: neither task's own code touches SSIF/DAC hardware directly
(consistent with the already-established pattern that hardware access happens only in the ISR/event-handler
layer below the task layer — see this file's "does any catalogued task touch the DSP or `DRESD` directly"
section) — so the actual "decoded WAV samples out to the transmit audio chain" step happens somewhere below
these two tasks, not found here. Also not decoded: the individual field layout of file-RPC commands `6`/`9`/
`0x13`/`0x17`'s payload structs, or exactly what `FUN_2002232c` (called per listed message) does with each
entry.

## `FUN_2007ef5c` fully resolved: the master graphics lifecycle task, and it's what starts the whole EGL/OpenVG thread (2026-08-30)

User's ask, continuing the task-catalog triage: look at the UI/display task next — the catalog's thinnest
entry ("allocates screen objects, runs a message loop", entry point never even renamed).

**Renamed `ui_graphics_lifecycle_task`.** Decompiled it fresh and immediately got the biggest single payoff
of this triage: **its very first action is calling `graphics_stack_startup_egl_openvg()`** — meaning this
task is the actual, concrete starting point of the whole EGL+OpenVG/SLV5 subsystem this file's earlier
sessions traced structurally but never tied to a specific caller. That gap is now closed.

**Two real EGL surfaces created, both confirmed via their own error strings (genuine standard EGL calls,
not guessed from shape):**
- **`egl_create_window_surface`** (renamed from `FUN_20079524`): creates the real, on-screen window —
  **480×272**, the IC-7300's actual documented touchscreen resolution. Full standard sequence:
  `eglChooseConfig` → `createNativeWindow` → `showNativeWindowEx` → `eglCreateWindowSurface` →
  `eglSurfaceAttrib` → `eglCreateContext` → `eglMakeCurrent`.
- **`egl_create_pixmap_surface`** (renamed from `FUN_2007a180`): creates an **off-screen** pixmap surface —
  **960×552**, exactly 2× the window's linear dimensions in both axes. Same EGL config/context sequence,
  `eglCreatePixmapSurface` in place of the window call.

**The main loop is a clean 2-state lifecycle, not really "a message loop" in the traditional sense** (that
framing from the original 26th-session note undersold it): blocks on an event flag each iteration, then:
- State 1 → **`ui_graphics_buffers_init`** (renamed from `FUN_2007ef08`): clears several fixed-size scratch
  buffers, sets 3 ready flags.
- State 2 → **`ui_graphics_present_frame`** (renamed from `FUN_2007ee68`): the actual frame present. Blits a
  region from the pixmap surface into the window surface (the blit call's width parameter, `0x3c0`/960,
  matches the pixmap's own width — confirms the pixmap really is the blit source), then calls what's very
  likely `eglSwapBuffers` (`FUN_20079a88`) on both surfaces. **If this fails while the task was in the ready
  state, the task deliberately exits its main loop** — a real, intentional error-exit path (surface-lost
  handling), not a bug or an unreached branch.

Loop exits on a byte flag reading `-1`, then both EGL surfaces/contexts are torn down cleanly
(`eglDestroySurface`/`eglDestroyContext`-shaped calls, not individually named).

**Open, not resolved this session**: the exact purpose of the pixmap being precisely 2× the window's
dimensions — genuinely ambiguous between "supersampled/anti-aliased offscreen render target, downscaled on
blit" and "a larger fixed canvas the window only ever shows one 480×272 corner of" (the observed blit call
passes the window's own dimensions as the copy size, not a scaled-down size, which is *consistent* with
either reading — a true downscale blit would typically take separate source/dest rectangles, which this
call's argument shape doesn't obviously show, but the relevant blit primitives weren't decompiled deeply
enough to be certain). Worth settling if this thread is picked up again by reading `FUN_200fffea`/
`FUN_200fff8e` (the two blit functions used, currently unnamed) in full.

**Bottom line**: this closes out the task-catalog triage's original four candidates
(`voice_recording_file_task`, the `voice_tx_memory_*` pair, and now this one) — the only task with a
real-world purpose still not pinned down is `status_poll_task_200095d8`.

## Traced the real display-hardware-adjacent init path: genuine multi-display architecture found, but not tied to the 960x552 pixmap (2026-08-30)

User's question, following the `ui_graphics_lifecycle_task` resolution: could the 960×552 pixmap surface be
intended for a planned external display, and did anything trace how the code initializes the actual
display/graphics hardware interface, checking for real dual-display signs there (not just the
`VDC50`/`VDC51` SVD-table-level evidence from the earlier "external monitor" theory session)?

**Traced several real layers below the EGL abstraction, all the way to a genuine architectural finding.**
Both `egl_create_window_surface`'s `createNativeWindow` and `showNativeWindowEx` calls, and
`egl_create_pixmap_surface`'s `createNativePixmap` call, route through a small (`3`-entry) category
dispatcher — renamed **`native_resource_dispatch`** — which looks up category `8` ("native platform
resource manager", registered during `initNativeResource`/`FUN_200fe2ec` via a call literally named
`FUN_2007d8e8(8, ...)`) and hands off to a **runtime-populated 16-entry jump table** at `0x20357414`,
indexed by sub-operation (`0`=create pixmap, `6`=create window, `10`=show window ex — this table itself
*is* present in the static image, unlike some of this codebase's other runtime-only tables).

- **`createNativePixmap` (sub-op 0) → `native_pixmap_alloc`** (renamed from `FUN_200fe5e4`): confirmed to be
  a **plain memory allocation** — computes bits-per-pixel from a format code, aligns width, and allocates
  `width_aligned × height × bpp/8` bytes via the codebase's generic heap allocator (`FUN_201581c4`). No
  hardware register touched anywhere in this function. **This settles the specific "does the 960×552 pixmap
  feed a display" question: it can't, structurally** — pixmaps in this native platform are pure off-screen
  RAM, with no attachment mechanism to any physical output at all.
- **`createNativeWindow` (sub-op 6) → `native_window_link_context`** (renamed from `FUN_200fe8d6`): not a
  buffer allocator — validates format/dimension compatibility between two resource handles and registers the
  pairing into one of up to 8 general-purpose slots. Not display-count-specific.
- **`showNativeWindowEx` (sub-op 10) → `native_show_window_multi_display`** (renamed from `FUN_200feaae`):
  **this is the real find.** It takes a display-selection bitmask from the caller, ANDs it against
  `*(DAT_200fedb8+4)` — a global "which displays are currently available" mask — and then **iterates
  bit-by-bit over the result, calling a per-display attach function
  (`native_display_attach_window`, renamed from `FUN_200fe15a`) once for every set bit**, passing the
  display index each time. This is genuinely, structurally written to support attaching one window to more
  than one simultaneous display output — not hardcoded to a single display the way a single-screen-only
  design would be.

**Where this hits a real static-analysis wall, not a stopping point I chose**: `*(DAT_200fedb8+4)`
(`0x20390a34`) reads as blank `0xFFFFFFFF` in the static image — the exact same "runtime-populated, needs
live hardware" signature already established for `kernel_start`'s own mystery task descriptor
(`0x203907c4`) elsewhere in this file. There is no way to determine from the static image alone whether more
than bit 0 of that mask is ever actually set on real IC-7300 hardware — that's a live-JTAG question, not
something more careful static reading would resolve (consistent with this project's other confirmed dead
ends of this exact shape).

**Answer to the user's question, precisely**: yes, the native graphics platform genuinely supports multiple
simultaneous displays as a real architectural feature at this layer (a bitmask-driven attach loop, not a
single hardcoded display target) — this is stronger, more direct evidence for "they built this with
multi-display in mind" than the earlier `VDC50`/`VDC51` SVD-table argument, which only showed the *raw
timing peripheral* has two channels. But the specific **960×552 pixmap is not itself evidence of this** —
it's confirmed to be plain memory with zero path to any display-attach code. If IC-7300 (or a sibling model)
ever does drive two outputs, the mechanism is this bitmask/attach system operating on the *window* surface,
not the pixmap — and whether it ever actually does is now pinned on the same "needs live hardware" wall as
`kernel_start`'s task, not on anything left unexamined in the static image.

Renamed in Ghidra: `native_resource_dispatch`, `native_pixmap_alloc`, `native_window_link_context`,
`native_show_window_multi_display`, `native_display_attach_window`. Not chased further this session:
`FUN_2007d050` (what `native_display_attach_window` forwards each per-display attach/detach to — the next
candidate if this thread is picked up again, on the chance it reaches down toward real `VDC5`/hardware
register territory).

## Quick follow-up: does the BMP screen-capture feature use the 960x552 pixmap? No. (2026-08-30)

User's question, following the multi-display/pixmap trace above: does `bmp_capture_task` (the
already-resolved screen-capture-to-SD-card feature) draw from the 960×552 off-screen pixmap surface?

**Checked directly and conclusively — it doesn't.** `bmp_capture_write_file`'s `BITMAPINFOHEADER`
construction uses a width constant that decodes to exactly `480` (`0x1e0`, `egl_create_window_surface`'s own
width). More directly: `bmp_capture_convert_pixels_to_bgr24` (renamed from `FUN_200aa2e4`, the actual
32bpp→24-bit-BGR pixel converter) iterates a fixed pixel count, `DAT_200aa62c`, which reads as `130,560`
decimal — **exactly `480 × 272`**, not `960 × 552` (`529,920`). The screen-capture feature captures the real
window/touchscreen resolution, full stop; it has no connection to the pixmap surface at all.

This is one more data point against the pixmap being anything other than what it already looked like: an
internal render-side detail of `ui_graphics_lifecycle_task`'s own presentation pipeline, not a resource any
other examined subsystem (display-attach code, screen capture) treats as a second, higher-resolution "real"
canvas.

## `sys_monitor_task_entry` moved from "examined" to "fully resolved" (2026-08-30)

User's ask, finishing the task-catalog triage: get `sys_monitor_task_entry` (marked "examined" — its own
loop body was already well documented, but its own startup code sat behind the project's known ARM/Thumb
disassembly bug) to "fully resolved."

**Re-verified the activation dead end properly before doing anything else**, given the RIIC xref-tool
lesson from an earlier session: a fresh raw literal-byte search for `0x20361318` (this task's descriptor
address) across the whole image found **zero references anywhere** — confirms the existing plate comment's
claim rather than trusting it blindly. This really is a permanent static-analysis wall, the same class as
`kernel_start`'s own mystery task descriptor.

**The task's own startup code was the real remaining gap.** `sys_monitor_task_entry` registers two event
handlers and executes a one-time `SWI(1)`, but all three targets (`0x20005960`, `0x200059b4`, `0x200b940c`)
were sitting behind Ghidra's known ARM/Thumb disassembly-context bug — decompiles were either garbage or
(for `0x200b940c`) not even a defined instruction. Read all three manually via `arm-none-eabi-objdump`
against `scratch/unpacked/142/body.bin` first, which already gave a solid read:
- `0x200b940c` (`SWI(1)`'s target): clean ARM — `TLBIALL` → `ICIALLU` → a call to `0x200053ac` (a
  `CLIDR`/`CSSELR`/`CCSIDR`-walking full data-cache clean+invalidate, the textbook ARMv7 idiom) → `BPIALL`
  → read `SCTLR`, clear/set a couple of bits, then explicitly set `SCTLR.C`/`SCTLR.I`/`SCTLR.Z` (D-cache,
  I-cache, branch prediction enable). Read as the one-time boot-to-running cache/MMU state transition.
- `0x20005960`/`0x200059b4`: near-identical ARM shape each — a kernel-primitive call, a shared-struct
  store, a counter decrement, a second kernel-primitive call, then an `SPSR` restore. Read (correctly, as
  it turned out) as some kind of interrupt-handler epilogue, but under-read its actual significance.

**User then fixed the ARM/Thumb disassembly context at all three addresses via the GUI.** Re-decompiling
confirmed the manual reads exactly and revealed one of them to be far more significant than the objdump
shape alone suggested:
- **`irq_context_switch_id0`/`irq_context_switch_id86`** (renamed from `handler_id0_probe`/
  `handler_id86_probe`, registered for event ids `0`/`0x86`): **not simple IRQ epilogues — full FreeRTOS
  task context-switches, structurally identical to `swi_handler`'s own already-documented scheduler logic.**
  Same current-vs-next-task pointer pair at `0x20005954`, the same `coproc_moveto_Context_ID` (ASID) write,
  the same lazy-FPU `Coprocessor_Access_Control` handling, the same indirect jump into the new task's saved
  context. This means these two are a **second, hardware-IRQ-triggered entry point into the identical
  scheduler mechanism `SWI(0)` provides for the software-triggered case** — a completely sensible RTOS
  architecture (SVC-triggered reschedule + IRQ-triggered reschedule, both converging on the same core
  logic), and a genuinely new structural fact about how this firmware's scheduler actually gets invoked, not
  something this project had previously connected. Not confirmed: which physical peripheral/GIC line feeds
  event id 0 specifically (event id 86 similarly unconfirmed) — the natural guess is one or both are the
  OS-tick-timer interrupt, but that's inference from shape, not yet independently checked against a real
  timer peripheral's IRQ line.
- **`enable_mmu_caches_branch_predictor`** (created as a proper Function and named at `0x200b940c` — it had
  no Function/instruction defined in Ghidra at all before the fix): decompile matches the manual objdump
  read exactly, function-name for function-name (`coproc_moveto_Invalidate_unified_TLB_unlocked`,
  `coproc_moveto_Invalidate_Entire_Instruction`, `coproc_movefrom_Control`/`coproc_moveto_Control` for
  `SCTLR`). One genuinely new piece past what objdump alone could show: after enabling the caches, it also
  calls `FUN_200b9270`/`FUN_200b9288` and zeroes/sets several fields of a struct at `DAT_200b9384` —
  GIC-adjacent-looking bookkeeping, not chased further this session.

**Status moved to fully resolved.** The task's own behavior — event-handler registration, the `SWI(1)`
cache/MMU transition, and the already-documented forever-loop — is now completely characterized. Only its
activation trigger remains open, and that's accepted as a permanent static-analysis limit of the same kind
already standing for `kernel_start`'s task, not an unfinished thread. This closes out the task-catalog
triage entirely except for `status_poll_task_200095d8`'s real-world purpose.

## `status_poll_task_200095d8` fully resolved: the band-scope's real-time FFT engine (2026-08-30)

User's ask, closing out the task-catalog triage: look at the last remaining "purpose not identified" task.

**Renamed `spectrum_scope_fft_task`.** Decompiled the task entry and its two "small helper" dispatches from
the 26th session's plate comment. The identity came from actually reading `FUN_20008358` (one of the two
"status==0" branch calls, renamed `spectrum_scope_fft_and_dbscale`) in full — unambiguous, not inferred from
shape alone:

1. A bit-reversal permutation over 512 elements — the standard FFT input-reordering step.
2. A textbook radix-2 Cooley-Tukey butterfly loop (stage size doubling each pass, twiddle factors read from
   a nearby cos/sin table) over a 512-element interleaved real/imaginary float array.
3. Post-FFT: per-bin magnitude-squared (`real²+imag²`, using the real-FFT conjugate-symmetry pairing
   `bin[i]`/`bin[512-i]`), a sqrt/log-shaped scale (`FUN_20185810`) times 10 (classic `x·log10(power)`-style
   dB conversion), then clamped/mapped into a byte `0`-`255` against a **per-mode min/max threshold pair**
   (indexed by a mode byte) — 256 output bytes total.

**This is a real-time spectrum analyzer computation, full stop** — the actual FFT magnitude data behind the
IC-7300's band-scope display. The task's own loop (renamed function `spectrum_scope_fft_and_dbscale`'s
caller) turned out to be a clean **double-buffered producer/consumer pipeline**: waits on an event flag,
checks two status bytes gating "not ready" vs. "ready" paths, and when ready, picks one of two alternating
512-float (`0x800`-byte) sample buffers based on a flag the producer sets (avoiding a read/write race with
whatever fills the buffers — plausibly an ADC/DMA sample-capture ISR, not traced this session), runs the FFT
on it, and releases what looks like a lock afterward. A companion function
(renamed **`spectrum_scope_buffers_reset`**) clears all the related buffers — the 256-byte dB output array,
both `0x800`-byte sample buffers, and a couple of smaller ones — consistent with a scope on/off or
mode-change reset.

**Relationship to [[band-scope-state]], not yet nailed down but a natural fit**: that file documents the
scope's *frequency-axis* state (`g_scope_state_mode`/`g_scope_freq_low`/`g_scope_freq_high`, live in
`g_radio_ui_state_base` at `0x2040376c`) and the frequency→screen-position mapping
(`scope_freq_to_position`). This task's per-mode dB threshold table lives at a different base
(`0x203de174`, checked directly — not the same address as `g_radio_ui_state_base`), so the two structures
are confirmed *not* identical, but thematically these read as the two halves of the same on-screen feature:
this task computes each bar's *height* (dB-scaled magnitude), `band-scope-state.md`'s functions compute each
bar's *x-position* (frequency mapping) and the mode/span selection both evidently key off. Worth a future
session tracing whether they share a producer/trigger, but not asserted as the same struct.

**Bottom line**: this closes out the task-catalog triage in its entirety. Every task in the 12-entry catalog
now has both a fully-characterized body and a resolved real-world purpose, except the two genuine,
independently-reconfirmed static-analysis dead ends (`kernel_start`'s own descriptor,
`thunk_FUN_2007ea68`'s peripheral identity) that need live JTAG, not more static reading.

## `periodic_poll_task_20014384` fully resolved: it's the RTTY decode-log task, a real gap in the "triage complete" claim (2026-08-30)

User's prompt: `periodic_poll_task`'s name was still generic despite being marked "confirmed" — is there
something unverified there? Checked, and yes: the previous session's plate comment on this task literally
said "`FUN_20015628`'s exact purpose not chased" — "confirmed" only ever meant "the body is real code, not
garbage," never "the purpose is known." This slipped through the task-catalog triage's earlier "every task
now fully resolved" claim, which was premature.

**Renamed `rtty_decode_log_poll_task`.** The loop itself is trivial (`itron_trampoline_delay(5)` → call →
store one byte), so the real content is in what it calls, `rtty_decode_log_service` (renamed from
`FUN_20015628`) — a small state machine gating on a "done" flag, dispatching on a mode byte to one of two
handlers. Followed the mode-1 handler, **`rtty_decode_log_write`** (renamed from `FUN_20015358`), which
settled it immediately: builds a path under a literal string, `"C:\IC-7300\Decode\Rtty"`, appends a
per-format filename suffix from a small table, and does FatFS-shaped open/write/close calls (the same
`0x2003bXXX` helper cluster and `0x44`/`0x46` command-ID family already seen in `bmp_capture_write_file`) to
write a decoded-text log file.

**Checked the format-suffix table directly rather than guessing**: its two populated entries resolve to
literal `".txt"` and `".htm"` strings — confirms the RTTY decoder can log to either a plain-text or an HTML
file (the mode byte selects output *format*, not a different digital mode as first guessed from the shape
alone).

**This is the IC-7300's real, documented RTTY-decode-to-SD-card logging feature.** Every 5 ticks, this task
services the decode log state machine and records whether work happened; the actual file write only fires
when the log service's internal conditions are met (new decoded content ready, presumably signaled by
whatever runs the actual RTTY demodulation — not traced this session, a natural next thread if this general
area is revisited).

**Correction to last session's own "triage complete" claim**, recorded rather than silently fixed: the
bottom-line summary in `notes/kernel-rtos.md` previously stated every task had a resolved purpose once
`spectrum_scope_fft_task` was done — this task was the miss, caught only because its name was still
generic and the user asked about it specifically. Good general lesson for this thread: a task marked
"confirmed" purely because its *body* decompiles cleanly is not the same claim as its *purpose* being
known — worth checking generic-looking names even after a triage is declared complete.

## `first_task_entry` looked at again: deepened, a wrong 24th-session claim corrected, but genuinely kernel-internal (2026-08-30)

User's prompt: `first_task_entry`'s status ("generic ITRON/RTOS message-dispatch loop") is the vaguest
description left in the catalog — worth a closer look too?

**Yes, and it turned up a real correction, not just more depth.** The 24th session's plate comment already
had a fairly deep read of this task (queue-receive loop, looks up a handler via
`msg_dispatch_lookup_trampoline` → `SWI(0)`, calls it) and named the trap's target
`resolve_and_invoke_msg_callback`, plus flagged a good, concrete open question: `FUN_2002b29c` (the
cold-boot/power-state dispatcher with zero direct callers anywhere) might be reached as a registered
callback through this exact mechanism.

**Followed that thread and found the specific attribution doesn't hold up.** `resolve_and_invoke_msg_callback`
only ever returns small integer status codes (`0`/`0x80`/`0x81`) — never anything resembling a function
pointer. `first_task_entry`'s own loop treats the trampoline's return value as a callable code pointer
(`(*pcVar2)(...)`) — calling through a raw `0x80` or `0x81` as a code address would crash immediately at
boot, which this task evidently doesn't do. So the specific claim "`msg_dispatch_lookup_trampoline` resolves
to `resolve_and_invoke_msg_callback`" is wrong, or at minimum unproven — the real answer needs the same
"how does `SWI(0)` select which kernel operation to run" question this file's own "Open questions" section
already lists as project-wide unresolved, not something newly broken here.

**What the re-examination did establish solidly**: `resolve_and_invoke_msg_callback` and
**`ready_list_requeue_by_priority`** (renamed from `FUN_201881d0`, which it calls) are genuine
FreeRTOS/ITRON-shaped **scheduler-internal bookkeeping** — validates a "message record" struct, then walks
and requeues linked-list nodes across a priority-indexed table (`DAT_20188344`, bounded by a max-priority
constant), using the same `0x20187xxx` kernel-internals helper cluster `itron_act_tsk`'s own real
implementation (`FUN_201888f4`) lives in. This is real, low-level kernel service-task machinery — plausibly
analogous to FreeRTOS's own Timer/Daemon service task (a dedicated task that waits on a queue and executes
deferred kernel-level work per message, exactly matching this task's own shape), though not confirmed to
literally be that same mechanism.

**Net effect on the open `FUN_2002b29c` caller question**: weaker, not stronger. The dispatch mechanism
doesn't look like a generic "register any function pointer as a callback here" table on closer inspection —
it looks like fixed, specific scheduler bookkeeping (priority/ready-list management), which makes "an
arbitrary hardware-init dispatcher gets invoked through this same path" a less natural fit than the 24th
session's comment assumed. Still a live open question; just without the concrete path to an answer that
comment implied.

**Where this leaves `first_task_entry`, honestly**: genuinely deepened — this project now understands its
two real callees far better than "a generic message-dispatch loop" suggested — but what's underneath is
real kernel-internal machinery, not a nameable application feature the way `voice_recording_file_task` or
`spectrum_scope_fft_task` turned out to be. Recording this as a distinct, legitimate category (kernel-internal,
not mystery, not a feature) rather than forcing it into "fully resolved" or leaving the old "generic loop"
description standing uncorrected.

## The real CI-V command dispatcher found, and a genuine undocumented command (0x2A) confirmed (2026-08-30)

User's ask: find the CI-V command table/handler and check for undocumented commands — picking up exactly
where the 29th session's "civ_command_dispatch_task retraction" section left off (see above): "Only the 4
direct readers [of the RX-ready flag global] were checked at the top level — their own callers weren't
individually walked." This session walked that chain to the end.

**The chain**: `civ_frame_rx_statemachine` stores a completed RX frame into a shared buffer pointed to by
`DAT_200115c8` (RAM `0x20396ad4`). That pointer is *also* held by a second, independent global,
`DAT_2000b210` (found by reading `DAT_200115c8`'s actual pointer *value* and searching for references to
that literal address rather than to the pointer-variable's own address — the earlier session's search only
covered the latter, which is why it stalled). `DAT_2000b210` sits in a cluster of ~20 sibling globals
(`0x2000b20c`-`0x2000b254`) belonging to a completely different code region (`0x2000axxx`-`0x2000bxxx`,
far from the SCIF0 driver's own `0x20010xxx`-`0x20012xxx` cluster) — this is the actual application-level
CI-V consumer, not anything previously named.

- **`civ_rx_frame_stage_and_dispatch`** (renamed from `FUN_2000b258`, called from 3 sites in unrelated
  polling loops — serviced piggybacked on a periodic tick, not a dedicated CI-V task): on seeing the RX-ready
  flag set, copies the received frame's payload (`RX_buf+1` = src/cmd/subcmd/data) into a scratch buffer
  (`DAT_2000b228`) and the frame length into a small work struct, then calls the real dispatcher, then — if
  the dispatcher produced a reply — copies the reply back into the RX buffer at offset `0x66` with its
  length at offset `0xca`. This is exactly the `pcVar4+0x66`/`pcVar4[0xca]` pair the *TX* side
  (`FUN_20011384`, `0x20010xxx` cluster) was already known to read to build an outgoing frame — closes the
  loop between RX and TX that was previously only traced halfway from each end.
- **`civ_dispatch_lookup_validate`** (renamed from `FUN_2000b03c`): reads the frame's cmd byte
  (`g_civ_rx_copy_buf+1`), rejects anything `>= 0x2b`, then indexes **`g_civ_cmd_table`** (renamed from
  `DAT_2000b250`'s pointed-to table, base `0x2018aa2c`, 43 entries × 8 bytes, one per possible cmd byte
  `0x00`-`0x2A`): `{u8 handler_base_idx; u8 pad[3]; char *subcmd_list}`. `subcmd_list` is a byte string of
  accepted subcommand values terminated by `0xFD`; a leading `0xFE` sentinel means "this command doesn't
  validate a subcommand byte at all, always dispatch to `handler_base_idx`" (used for VFO-select/scan/
  split/send-CW-message — commands whose data isn't a simple enum). A real match adds the subcommand's
  position in the list to `handler_base_idx`, giving the final index into **`g_civ_handler_table`** (renamed
  from `DAT_2000b234`, same base region immediately following the first table — `0x2018ab84` — confirmed
  contiguous: `0x2018aa2c + 43*8 == 0x2018ab84` exactly): 16 bytes/entry, `{u8 permission_flags; 3 packed
  length-bound bytes; void *handler_fn}`.
- **`civ_dispatch_invoke_handler`** (renamed from `FUN_2000acd8`): permission-gates the call against a
  current-mode byte (`*DAT_2000b230`, 3 possible modes) and the entry's flag byte, then calls
  `handler_fn(remaining_length)` through the function pointer at entry+4, and on success/failure paths
  clears the two "reply pending" flags (`RX_buf[0xcb]`/`[0xcc]`) the earlier session had found written by
  `civ_frame_rx_statemachine` but never traced to a reader — that reader is this function, closing that
  loose end too.

**Cross-checked the whole table against the real manual** (user supplied the exact location this session:
`/data/misc/icom/7300/doc/IC-7300_ENG_FM_12b.pdf`, pages 19-2 through 19-13, full "Command table" and "Data
content description" sections). Every `handler_base_idx == 0` slot (meaning: `civ_dispatch_lookup_validate`
outright rejects the command, no handler at all) matches a **real gap in the manual's own command list**:
`0x0C`/`0x0D` (manual jumps `0B`→`0E`), `0x12` (jumps `11`→`13`), `0x1D` (jumps `1C`→`1E`), `0x1F`/`0x20`
(jumps `1E`→`21`), `0x22`/`0x23`/`0x24` (jumps `21`→`25`), `0x29` (manual's table ends at `28 00`, nothing
after). This is strong, independent confirmation that `g_civ_cmd_table`'s ID space really is the literal
CI-V wire command byte (unlike the earlier, retracted `sdcard_file_rpc_dispatch_task` false lead, whose
table shape looked superficially similar but whose IDs disagreed with the manual at command `0x03`). Every
implemented command's decoded subcommand list also matches the manual closely where checked in detail (e.g.
`0x28`'s single subcommand `0x00` — manual: "28 00, data 00 to 08" — matches exactly; `0x07`'s
`00/01/A0/B0` — manual: VFO A/VFO B/equalize/exchange — matches; `0x0E`'s 14-entry scan list matches the
manual's scan-mode table one-for-one).

**The one exception: `0x2A` is real and implemented, with no manual entry anywhere.** `g_civ_cmd_table[0x2a]`
has `handler_base_idx = 0x8f` and a genuine (non-`0xFE`) subcommand list accepting exactly one value,
`0x01` — not a stub, not a duplicate alias, not zeroed. The manual's command table (`IC-7300_ENG_FM_12b.pdf`
p.19-8) ends at `28 00` with no `0x29` or `0x2A` entry at all, in any sub-table, anywhere in the 12-page
command reference. **This is the undocumented CI-V command** the original investigation (28th/29th sessions)
set out to find.

**`civ_cmd_2a_handler_UNDOCUMENTED`** (renamed from `FUN_20010710`, the real `0x2A 01` handler,
`g_civ_handler_table[0x8f]`'s function pointer): takes one further data byte (min=max length 1, matching
the manual-style `00 to 0x03`-shaped commands), read from a small shared state struct
(`*(DAT_20010868+3)`, a struct also touched by 8 sibling handler functions in the same `0x20010xxx`
cluster — an ordinary "current handler state" scratch area, not `0x2A`-specific) — not yet traced back to
confirm it's literally the wire data byte verbatim, but the shape (0/1/2/3 four-way branch) matches every
other simple enum-style CI-V command in this firmware:
- `0`: calls `FUN_20066c14(0)` — "disable"
- `1`: runs a validation chain (`FUN_20066720`/`FUN_2005361c`/`FUN_200623bc` checked against a frequency
  ceiling at `DAT_200108ac+0x300`/`FUN_20062b3c`/`FUN_2001344c`) and only on success calls a 9-function
  subsystem re-sync (`FUN_200663d8`, itself calling `FUN_2002fd94(1)`/3 more `FUN_20033xxx` calls/
  `FUN_2005361c` again/`FUN_20065db8`/`FUN_20058fec`/`FUN_2004b74c`/`FUN_20061020`) then `FUN_20066c14(1)`
  — "enable, gated on a frequency check and several subsystem preconditions"
- `2`/`3`: call `FUN_20066c38(1)`/`FUN_20066c38(2)`, a sibling function with its own internal state check
  (`*(DAT_20066eac+3)`) and flag writes (`DAT_20066eac+1`/`+2`)

`FUN_20066c14` is a real hardware access, not bookkeeping: it bit-ORs two registers
(`DAT_2001f50c`/`DAT_2001f510`) in tandem — both get the same enable bit written into two different bit
positions each, the classic shape of driving two GPIO/peripheral pins together — and zeroes a 3-field
status struct (`DAT_2001f514`/`518`/`51c`). **Genuinely a guarded hardware enable/disable toggle, not a
no-op or an alias of a documented command.**

**What it does isn't identified yet.** `DAT_2001f50c`/`DAT_2001f510` are RAM addresses (`0x2001f5xx`, on-chip
RAM range), not raw MMIO — they're a shadow/staging copy of *something*, not directly a GPIO peripheral
register, so identifying the real pin needs tracing where this shadow state eventually gets flushed to real
hardware (a register write elsewhere, not found this session) or cross-referencing against the schematic
if a plausible candidate net turns up. The frequency-ceiling gate on the "enable" path (`DAT_200108ac+0x300`)
is the most promising lead for guessing *domain* (something that's only valid below some frequency —
consistent with, but not proof of, an antenna-tuner-adjacent or band-limited RF-chain control, distinct
from the already-documented `1C 01` tuner command since that one's fully accounted for separately) — not
confirmed.

**Open items for a future session**:
1. Trace `DAT_2001f50c`/`DAT_2001f510` to their real hardware sink to identify the actual pin/peripheral
   `0x2A 01` controls — the single most valuable next step, likely resolves what this command is *for*.
2. Confirm `*(DAT_20010868+3)` really is the verbatim CI-V data byte (walk the shared `0x20010xxx`-cluster
   struct-fill code that presumably runs before every sibling handler in that cluster, not just this one).
3. Decode the frequency-ceiling table at `DAT_200108ac+0x300` (what unit, what values) — would help confirm
   or refute the "band-limited" domain guess above.
4. Live JTAG (once hardware arrives, see [[icom-ic7300-re-project]]) could settle this fast: send
   `FE FE <addr> E0 2A 01 01 FD` and watch for any visible radio behavior change, or breakpoint
   `civ_cmd_2a_handler_UNDOCUMENTED` and watch the two GPIO-shaped registers.

Also worth noting for anyone revisiting this: the manual PDF's real path is
`/data/misc/icom/7300/doc/IC-7300_ENG_FM_12b.pdf` (user supplied this exact path this session after the
28th/29th sessions apparently had a temporary copy that wasn't saved anywhere locatable — future sessions
needing the CI-V command table, or any other section of the full manual, should read directly from here
rather than re-deriving from notes).

### Cross-check against wfview (open-source multi-model CI-V control suite) — command 0x2A confirmed unknown there too (2026-08-30, same day)

User's ask: check whether `wfview` (github.com/wf-group/wfview — an actively-maintained, community-driven
CI-V rig-control application supporting ~40 Icom models, whose per-model command sets are hand-authored
against official manuals and community reverse-engineering, not auto-generated) knows about command
`0x2A`, and if not, see what its own command handling suggests.

Cloned the repo and found its real command definitions live in `rigs/*.rig` (INI-format files parsed by
`icomCommander::parseCommand`/rig-loading code in `src/radio/icomcommander.cpp`), not hardcoded in the C++
source — each model's supported CI-V commands are individually listed with hex command strings.
`rigs/IC-7300.rig`'s command set: `0x00`-`0x08`, `0x0B`, `0x0E`-`0x11`, `0x13`-`0x1C`, `0x21`, `0x25`-`0x27`
(plus the `FA`/`FB` NG/OK reply codes) — **no `0x28`, `0x29`, or `0x2A` at all** (`0x28`, real per the
manual, is apparently just a feature wfview hasn't implemented client-side; `0x29`/`0x2A` match this
firmware's own "not implemented for this model" findings, `0x2A` included). `IC-7300MK2.rig` (the newer
hardware revision) has the identical command set, same gap.

**Widened the check across all ~40 of wfview's supported Icom models' `.rig` files** (every model from the
1984 IC-R71 to the 2023 IC-905): grepped every file for a command string starting with byte `0x2A` —
**zero matches, on any model, anywhere in the whole rig database.** (The one incidental `0x2A` hit in the
repo, `CI-V.md`'s per-model bus-address reference table, is the IC-R9000's default *CI-V address*, an
unrelated 1989-vintage coincidence — not a command byte.)

**Conclusion**: this isn't a case of "third-party software just hasn't caught up yet" for one model — the
most complete open-source Icom CI-V implementation that exists, covering the entire modern CI-V-capable
lineup and built from official manuals plus years of community reverse-engineering across many radios,
has no record of command `0x2A` on *any* Icom radio it supports. Reinforces (doesn't newly prove, since the
official manual gap was already conclusive) that `0x2A` is genuinely obscure — consistent with, though not
proof of, a factory/test-only command never meant for end-user or third-party control-software consumption.
Doesn't add new information about what `0x2A` *does* (wfview's own command *names/domains* near it —
`0x21` RIT/`Δ`TX, `0x25`/`0x26` selected-VFO freq/mode, `0x27` scope — don't suggest a family that `0x2A`
would naturally extend), so the open item from the previous section (trace `DAT_2001f50c`/`DAT_2001f510`
to their real hardware sink, or test live via JTAG) is still the only way forward on "what does it do".

### Continued tracing of undocumented command 0x2A — converges on the antenna tuner engage hardware (2026-08-30, same day)

User's ask: continue tracing what `0x2A 01` actually does. Picked up from the previous section's open
item #1 (trace `DAT_2001f50c`/`DAT_2001f510` to their real sink).

**Found the frequency ceiling's real value**: `DAT_200108ac+0x300` (the u32 `civ_cmd_2a_handler_UNDOCUMENTED`
compares the current operating frequency against, via `FUN_200623bc` — confirmed to be a genuine
"read current VFO/operating frequency" helper, not something unrelated) is `0x03938700` =
**60,000,000 — 60 MHz exactly.** Notable: this sits *above* the IC-7300's normal 54 MHz TX ceiling but
*below* its 74.8 MHz RX ceiling — not a match for either documented band edge, consistent with a
generic "somewhere in the HF+6m coverage range" sanity check rather than a TX-specific limit.

**The real find**: traced `civ_cmd_2a_handler_UNDOCUMENTED`'s callees one level further and found they
converge on the exact same low-level primitives used by the **real, documented `1C 01`** command
(antenna tuner OFF/ON/tuning — manual p.19-7). Renamed the documented side's chain to make the parallel
explicit: **`civ_cmd_1c01_tuner_handler`** (`0x2000e318`, `g_civ_handler_table[113]`'s function pointer,
confirmed by computing the real table index for cmd `0x1C` subcommand `0x01`) → **`civ_cmd_1c01_tuner_dispatch`**
(`0x2000e0f4`) → on its own param==2 ("start tuning") path: **`tuner_freq_and_txstate_precheck`**
(renamed from `FUN_2001344c` — checks two status bits plus a band-edge-lookup pair,
`FUN_20013348`/`FUN_200133d8`, against the current frequency) then **`tuner_start_tuning_sequence`**
(renamed from `FUN_20066454`), which calls `FUN_200663d8()` (the same 9-function RF-subsystem re-sync
`civ_cmd_2a_handler_UNDOCUMENTED` also calls) and **`tuner_engage_gpio_toggle(1)`** (renamed from
`FUN_2001e720` — the function that bit-twiddles `DAT_2001f50c`/`DAT_2001f510` in tandem).

`civ_cmd_2a_handler_UNDOCUMENTED` reaches **the identical `tuner_engage_gpio_toggle(1)` call**, just
through its own shorter chain: data byte `2` → `civ_2a_state_engage_or_abort(1)` (renamed from
`FUN_20066c38`) → `tuner_engage_gpio_toggle(1)` directly — skipping `civ_cmd_1c01_tuner_dispatch`'s own
TX-state/split/mode gating (`DAT_2000e260`/`DAT_2000d240`/`DAT_2000f070` checks) entirely. Its data byte
`1` ("arm") independently calls `tuner_freq_and_txstate_precheck()` plus the frequency-ceiling and
`FUN_20062b3c()` checks — a *lighter* version of the same precondition `civ_cmd_1c01_tuner_dispatch`
applies before it will start tuning.

**Full 4-value semantics of `0x2A 01`'s data byte** (own private 4-byte state, `g_civ_2a_state`, renamed
from `DAT_20066eac` — NOT the same state var the documented `1C 01` path uses, this is a fully parallel,
independently-tracked state machine):
- `0` → `civ_2a_state_set(0)` (renamed from `FUN_20066c14`) — unconditional "off"
- `1` → if not already on: validates (freq < 60 MHz, `tuner_freq_and_txstate_precheck()==0`,
  `FUN_20062b3c()==0`) then `FUN_200663d8()` (RF resync) + `civ_2a_state_set(1)` — "arm". If *already*
  on: instead calls `civ_2a_state_engage_or_abort(0)` → `tuner_engage_gpio_toggle(0)` — a disable, not a
  re-arm (asymmetric, worth remembering if this is ever tested live)
- `2` → only accepted once armed: `civ_2a_state_engage_or_abort(1)` → **`tuner_engage_gpio_toggle(1)`** —
  the actual hardware assert, bit-identical to the real tuner's own engage call
- `3` → only accepted once armed: `civ_2a_state_engage_or_abort(2)` — sets an abort/cancel flag
  (`g_civ_2a_state+2`) only, no direct hardware touch — "abort"

**Conclusion, not yet 100% confirmed but well-evidenced**: `0x2A 01` is very likely an **alternate/bypass
trigger for the IC-7300's internal antenna tuner engage hardware**, reusing the tuner's own low-level
primitives (`tuner_engage_gpio_toggle`, the RF-resync call, `tuner_freq_and_txstate_precheck`) through an
independently-coded, more lightly-gated path that skips the documented command's TX-state/mode checks.
Plausibly a factory/production-test shortcut for exercising the tuner motor/relay hardware without
going through the front-panel- or `1C 01`-driven state machine and its safety interlocks — consistent
with (but not proof of) the "factory/test-only, never meant for end users" reading from the wfview
cross-check. Not the *same* command as `1C 01` (independent state, independent — lighter — gating), so
calling it simply "an alias for the tuner command" would overstate the finding; the accurate claim is
"reaches the identical tuner-engage hardware primitive by a different, undocumented, less-guarded path."

**Still open**: `DAT_2001f50c`/`DAT_2001f510`'s real hardware sink (RAM-shadowed, matches the
schematic-confirmed `TCLK`/`TDAT`/`TSTB1`-`4`/`TCON`/`IMPI`/`PHASEI` tuner-interface signal group from
`notes/ic7300-signal-chain.md`'s P7_1-6/P7_8/P7_9/P0_4/P6_2/P6_3 pins, but the write-back path from RAM
shadow to real MMIO/serial-shift-out hasn't been traced — would need either that trace or live JTAG to
turn "shares the tuner's engage primitive" into "confirmed to physically move the tuner network").
Renamed in Ghidra this session: `tuner_engage_gpio_toggle`, `tuner_start_tuning_sequence`,
`civ_cmd_1c01_tuner_dispatch`, `civ_cmd_1c01_tuner_handler`, `tuner_freq_and_txstate_precheck`,
`civ_2a_state_set`, `civ_2a_state_engage_or_abort`, `g_civ_2a_state`.

### Real tuner relay hardware map from the user + correction to the "GPIO shadow" guess (2026-08-30, same day)

User supplied a direct schematic/parts-list reading of the tuner unit: the 4 `BU2092FV-E2` ICs
(`IC2811`/`IC2821`/`IC2831`/`IC2841`) are serial-in/parallel-out relay drivers sharing `TDAT`/`TCLK`/`TOE`,
each individually latched by its own `TSTB1`-`TSTB4`, and each driving a fixed group of relays
(`NL0`-`NL9`/`NC1`-`NC9`/`NCIN`/`NCOUT`/`NCRED`/`NATT1`/`NATT2`, full mapping now in
`notes/ic7300-hardware.md`'s new "Tuner relay network" table) — confirms the tuner is a **relay-switched
stepped L-network**, correcting the service-manual-derived guess that these were "motor driver ICs" for a
motorized roller inductor.

Used this to push the code trace one step further: tried to find the runtime code that serializes a relay
pattern out over `TDAT`/`TCLK`/`TSTBn` (the P7 pins), to settle whether `tuner_engage_gpio_toggle`
(called identically by both the documented `1C 01` tuning-start path and undocumented `0x2A`) actually
drives this network. **Result: a real correction to the previous session's framing, not a confirmation.**
`DAT_2001f50c`/`DAT_2001f510` (the two addresses `tuner_engage_gpio_toggle` bit-twiddles) are themselves
pointer variables holding `0x203902d4`/`0x203902d6` — checking `references_to` on those two literal RAM
addresses turned up **60+ distinct call sites scattered across dozens of unrelated subsystems** (SD-card
menu, cold-boot init, mode dispatch, and many more, spanning `0x2000axxx` through `0x20034xxx`). That's
far too broad a fan-out for tuner-specific hardware state — these are a **generic, shared global
status/interlock flags byte pair**, not a GPIO register shadow as the previous session guessed.
`tuner_engage_gpio_toggle` most likely just asserts/clears a couple of shared "RF-chain busy"-style bits
alongside everything else that touches this byte pair, rather than driving real relay hardware directly
itself.

**Checked the actual P7 port data register** (`0xFCFE301C`, per the RZ/A1H port-register map
`notes/ic7300-signal-chain.md` already established, `PORTn_base=0xFCFE3000`, `Pn` stride 4 bytes/port) via
direct `references_to`: **exactly 2 hits, both inside `port_bulk_gpio_init_pass1`/`port_bulk_gpio_init_pass2`**
— the same generic one-time boot-time port-register initializer already fully documented from the
DRESD/`P2_6` chase (an earlier session found the identical "exactly one reference, the boot bulk-init"
signature for that pin too). `P6`'s data register (`0xFCFE3018`, carries `EKEY`/`ESTA`) shows the same 2
hits and nothing else. `PNOT7`/`P0`'s data register: zero hits at all. **Conclusion**: nothing in the
traced firmware pokes the raw P7/P6/P0 port data registers at runtime — the real relay-shift-out routine,
if it exists in the statically-traced image at all, must go through some other indirection (a computed/
table-driven register address, not a literal one) — possibly related to the already-named but
unexplored `g_ppr_register_base` (`0x2002a0d0`) from earlier signal-chain work, not yet checked against
this specific question.

**Net effect on the `0x2A` finding**: the core conclusion from the previous section stands — `0x2A`'s
"engage" data byte calls the exact same `tuner_engage_gpio_toggle(1)` the documented `1C 01` tuning-start
path calls, which remains real, meaningful evidence that `0x2A` is an alternate/bypass trigger for
whatever `tuner_engage_gpio_toggle` represents. But the previous session's implicit "and this
directly moves tuner hardware" extension is **not confirmed** — corrected the Ghidra plate comments on
both `civ_cmd_2a_handler_UNDOCUMENTED` and `tuner_engage_gpio_toggle` to state this precisely (shared
status-flags byte pair, not a GPIO shadow; real relay-drive code still unlocated). Open item updated
accordingly: find the actual `TDAT`/`TCLK`/`TSTBn` shift-out code (check `g_ppr_register_base`'s other
uses first) to close this out, or fall back to live JTAG once hardware arrives.
