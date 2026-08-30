# Task model: creating a task for an app to run in

Scope: what's confirmed about how a task gets created and runs, once it exists. **Excludes** how a new
app's own descriptor would get placed somewhere the scheduler reads it — that's `roadmap.md`'s Phase 2
(injection point), out of scope here per `sdk/README.md`.

All addresses below verified against the live Ghidra project (`body.bin`) 2026-08-30.

## ✅ Task activation: `itron_act_tsk`

**`itron_act_tsk`** (`0x20187044`, verified) is the trampoline every known boot-time task goes through.
Checks current ARM mode (`get_cpsr_mode`) — if in User mode, traps via `SWI(0)`; if already privileged,
calls the real FreeRTOS task-creation API directly. Takes a 16-byte descriptor:

```
struct task_descriptor {
    void  *entry_point;   // +0x00
    int    priority;      // +0x04
    int    flags;         // +0x08 (role/meaning not decoded)
    size_t stack_size;     // +0x0c
};
```

11 real call sites are catalogued in [`notes/kernel-rtos.md`](../../notes/kernel-rtos.md)'s "Living
reference: full boot-time task catalog" — every one of them is a live, worked example of a real descriptor.
A few, for concrete shape reference:

| Task | Descriptor addr | Priority | Stack |
|---|---|---|---|
| `voice_recording_file_task` | `0x20016800` | 1 | `0x2000` |
| `sd_menu_dispatch_task` | `0x2002784c` | 0 | `0x1800` |
| `spectrum_scope_fft_task` | `0x201988ec` | -2 | `0x400` |
| `sdcard_file_rpc_dispatch_task` | `0x201988ac` | 1 (highest in catalog) | `0x1800` |

Lower numeric priority values run more urgently (`-2` outranks `0`/`1`) — inferred from the catalog, not an
independently confirmed FreeRTOS-priority-scale fact.

## 🔎 Open: does a *new* task need special ASID/MMU handling?

Confirmed real Icom customization on top of stock FreeRTOS: unprivileged user-mode tasks with **per-task
ASID/MMU isolation** — `swi_handler`'s context switch calls `coproc_moveto_Context_ID`, tagging ARM CP15's
Context ID register per task (`notes/kernel-rtos.md`'s RTOS-identity section). Every catalogued task reaches
shared subsystems (SCIF drivers, the graphics stack, the SD filesystem) through established call paths from
inside this isolation model. **Not yet confirmed**: whether a task created through the ordinary
`itron_act_tsk` mechanism just inherits working access to those subsystems, or whether some additional
step (a specific ASID assignment, a syscall-style trampoline) is needed first. This is the single highest-
priority open question for this file — it affects every app equally, regardless of what the app actually
does once running.

## ✅ Task-local timing

FreeRTOS tick/delay primitives are available as-is — matched directly against the real, locally-available
Renesas `R01AN5093` FreeRTOS source (`scratch/r01an5093ej0170-rza1-swpkg/`, see `notes/kernel-rtos.md`).
No additional research needed for basic periodic-task / game-loop timing. **Caveat**: the bundled reference
package is FreeRTOS v10.6.1 (2023); the IC-7300's actual build is almost certainly older (~8.x/9.x, given
a ~2016 ship date) — trust the stable, hardware-fixed parts of the comparison (GIC addresses, save/switch/
restore shape), not exact struct layouts, until an older contemporaneous package is found.

## Naming caveat

`itron_act_tsk` and its sibling wrapper functions are named for continuity with an early (and since-
retracted) ITRON hypothesis — they're actually "elevate to privileged mode, call this FreeRTOS API function
directly" trampolines. The name predates the FreeRTOS identification and is kept only because renaming
every wrapper to its real FreeRTOS API equivalent hasn't been done yet (`notes/kernel-rtos.md`'s open
questions #1/#5).
