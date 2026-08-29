# Main-CPU kernel/RTOS

See [notes/kernel-rtos-history.md](kernel-rtos-history.md) for the full session-by-session narrative and evidence trail.

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

## Retracted: RTOS not found by name via string search

An early whole-`body.bin` string search for `TOPPERS`/`ITRON`/`T-Kernel`/`TRON`/`eSOL` found zero hits (all
`Copyright`/`kernel` string hits were unrelated font-library false positives) — superseded once the direct
FreeRTOS source comparison above confirmed the RTOS identity. The one reusable finding from that search:
`"RENESAS RZ/A1 SD Driver Ver4.01"` at `0x201812e0`, the versioned SD-card driver behind the SD-card update
flow ([[firmware-update]]). Full search log and detail in the history file.

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
   "Living reference: full boot-time task catalog" section below) — this is a different, more useful list
   than the originally-envisioned TCB search, though the *TCBs themselves* remain runtime-populated as
   originally found (`0x203907c4` still blank/`0xff` in the static image).
4. New: confirm `PTR_DAT_20005954`'s exact role/layout against
   `pxCurrentTCB`'s real usage in `tasks.c` before renaming further
   kernel globals — see "RTOS identity" section above.
5. New: `itron_act_tsk` and siblings are misleadingly named now that
   ITRON isn't the right frame — worth renaming once each one's wrapped
   FreeRTOS API is identified by comparing call-site argument shapes
   against `task.h`/`queue.h`/`semphr.h` signatures.

*(Later sessions accumulated more open items than are listed here — the real CI-V command dispatcher
consumer, `kernel_start`'s own mystery task, `thunk_FUN_2007ea68`'s peripheral identity, the SCIF1
turnaround/service-mode triggers, etc. See the "Session handoff" sections in the history file for the
fuller, more current running list.)*

## Living reference: full boot-time task catalog

Every `itron_act_tsk` call site (11 total, exhaustively cross-checked 3 independent ways in the 21st
session) plus the 2 known non-trampoline/dynamic activations, each with its task descriptor
(`{entry_point, priority, flags, stack_size}`, 16 bytes) read directly from RAM. This table was first
assembled in the 23rd session and is refreshed here from the corrections made through the 29th/30th and
later sessions (the `sdcard_file_rpc_dispatch_task` CI-V retraction, the `thunk_FUN_2007ea68` USB-guess
retraction, etc.) — see the history file for the full derivation and narrative behind every row.

| Caller | Descriptor | Entry point (current name) | Priority | Stack | Status |
|---|---|---|---|---|---|
| `FUN_20188574` (kernel bootstrap, direct inner-function call, **not** a trampoline call — see history's 24th-session correction) | `0x2033605c` | `first_task_entry` (`0x201871f0`) | 2 | 0x320 | ✅ generic ITRON/RTOS message-dispatch loop |
| `kernel_start` (`0x200052a4`) | `0x203907c4` — **still genuinely unidentified**, still blank/`0xFF` in the static image | ? | ? | ? | ❌ open — real task, unknown identity, needs live JTAG |
| `FUN_200096c8` (`0x200096f0`) | `0x201988ec` | `status_poll_task_200095d8` | **-2** | 0x400 | ✅ real body confirmed (26th session, GUI ARM-disasm fix) — polls a status byte, dispatches to small helpers; purpose not identified |
| `FUN_2001439c` (`0x200143b8`) | `0x201988fc` | `periodic_poll_task_20014384` | 0 | 0x800 | ✅ confirmed — trivial `itron-trampoline-delay(5) → sample → store` loop |
| `FUN_2001627c` (`0x2001631c`) | `0x20016800` | `voice_recording_file_task` (renamed from `queue_driven_task_2001745c`) | 1 | 0x2000 (largest stack in the catalog, now explained) | ✅ **fully resolved** — manages recording audio to the SD card's `C:\IC-7300\Voice` folder: builds an RTC-date-stamped filename, runs a 4-state open/process/close file-I/O state machine over a 4-slot ring buffer, and is a confirmed real client of the already-known SD-card file-RPC service (`file_rpc_post_command`, commands `6`/`9`) also used by `sdcard_file_rpc_dispatch_task`. Closes the loop with the `audio_buffer_task` pair's "plausibly WAV record/playback" hypothesis below — very likely the file-I/O half of that same feature |
| `FUN_20027740` (`0x200277f0`) | `0x2002784c` | `sd_menu_dispatch_task` | 0 | 0x1800 | ✅ **fully resolved** — the SD-card operations menu's central 42-case dispatcher; case `0xb` calls `firmware_update_main` directly, confirming the update flow starts from routine SD-menu interaction, nothing more exotic |
| `cold_boot_hw_init` (`0x2002b02c`, renamed from `FUN_2002afc0`) | `0x2019889c` | `FUN_2007ef5c` (the UI/display task — entry point itself never renamed) | — | — | ✅ examined — allocates screen objects, runs a message loop |
| `FUN_2006c4a8` (`0x2006c584`) | `0x201988cc` | `audio_buffer_task_2006bb58` | 0 | 0x800 | ✅ confirmed real state machine — ring-buffer wraparound arithmetic found; leaning circular audio buffer manager (plausibly SD-card WAV record/playback), not fully confirmed |
| `FUN_2006c4a8` (`0x2006c594`, **same caller as above — spawns 2 tasks together**) | `0x201988dc` | `audio_buffer_task_2006c2c4` | 0 | 0x800 | ✅ confirmed real state machine — same subsystem as above, shares state at `0x2006c3d4` |
| `thunk_FUN_2007ea68` (`0x2007ea84`) | *dynamic, caller-supplied* | *unresolved* | — | — | 🟡 **mechanism traced, peripheral base confirmed as a direct unwritten literal, identity hypothesis upgraded** — activated on a connect/disconnect-shaped event touching an unidentified peripheral at `0xe8100000` (RZ/A1H bus-matrix slave SLV5). 2026-08-29: verified `UNIDENTIFIED_SLV5_PERIPH_BASE` has no hidden indirection (single unwritten literal-pool constant, confirmed via a write-reference check — not a mutable pointer cell); separately found `slv5_periph_connect_disconnect_handler` is called as the `"vgStartUp"` step of a real, string-confirmed EGL+OpenVG graphics-stack bring-up (`graphics_stack_startup_egl_openvg`, `0x20079240`) — new working hypothesis: a 2D/vector-graphics rendering resource, not the earlier retracted "USB subsystem" guess. See history's final session for the full trace. |
| `FUN_200aa5d4` (`0x200aa5c8`) | `0x2019890c` | `bmp_capture_task` | **-1** | 0x1000 | ✅ **fully resolved** — the BMP screen-capture-to-SD-card feature (real `BITMAPFILEHEADER`/`BITMAPINFOHEADER` construction) |
| `FUN_200b995c` (`0x200b999c`) | `0x201988ac` | `sdcard_file_rpc_dispatch_task` (renamed **twice**: `task_probe_200b9c00` → `civ_command_dispatch_task` (wrong guess) → `sdcard_file_rpc_dispatch_task`, see history's "civ_command_dispatch_task retraction" section) | 1 (highest priority in the catalog) | 0x1800 | ✅ **fully resolved, then corrected** — **not** CI-V; a generic internal SD-card file-access RPC service (open/read/write/close/rename/list), dispatched through a function-pointer table by command ID |
| *(no direct call site found — genuine static-analysis dead end, not yet fixed)* | `0x20361318` | `sys_monitor_task_entry` (`0x200b94e8`) | 3 | 0x800 | ✅ examined — runs the `cold_boot_hw_init` chain (`DRESD`/GPIO init); its own **caller**, `sys_monitor_task_loop` (`0x2003bb70`), *is* known — called unconditionally every iteration by `FUN_2002b29c` — but what activates `sys_monitor_task_entry` itself remains unresolved |

**Bottom line, as of the latest session that touched this catalog**: every task in the table has a real,
disassembly-confirmed body (no task still shows garbage/unexamined code). Exactly two identities remain
genuinely open — `kernel_start`'s own mystery task (`0x203907c4`, a thoroughly reconfirmed static-analysis
dead end: no writer anywhere in the compiled image, needs live JTAG) and `thunk_FUN_2007ea68`'s dynamically
activated task (mechanism fully traced; the peripheral it serves is not yet identified — confirmed **not**
USB, and now has a well-evidenced but not register-level-confirmed new hypothesis: a 2D/vector-graphics
rendering resource, see the table row above; a follow-up "external monitor" theory, prompted by a sibling
model — IC-7610 — reportedly having a DVI output via a separate sub-CPU, was tested and only partially
supported: the actual "connect" gate traced to a plain software resource-allocation check, not a hardware/
GPIO detect, so this is more likely the shared graphics middleware's generic resource-acquire terminology
running identically on every product that uses it, not evidence of an IC-7300-specific external connector;
see history for the full trace). A handful of resolved tasks still have their real-world *purpose* only partially pinned down
(the `audio_buffer_task` pair — now better-supported as the WAV record/playback ring-buffer half of the same
feature `voice_recording_file_task` handles the file-I/O half of, see that row above —
and `status_poll_task_200095d8`) — see history for what's been tried on each.
