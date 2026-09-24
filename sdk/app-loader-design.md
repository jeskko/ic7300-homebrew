# The app loader: concrete design (started 2026-09-25)

`roadmap.md`'s Phase 2 asked three open questions — where the loader hooks in, where app code lives in
memory, and what an app can safely do. This file is where those get concrete, real-address answers as
they're settled. See `README.md` for how `sdk/` relates to `notes/`: this file mixes cited facts with
genuine design decisions, and is expected to keep changing.

**Goal for the first cut, per the user's own scoping (2026-09-25)**: the simplest possible real
proof of concept — the radio boots a one-time-modified `body.bin`, something (TBD, see "Open" below)
triggers the hook, the hook loads a tiny app from the SD card, the app emits something over the CI-V bus,
and the radio resumes completely normal operation with no crash, no hang, no visible side effect other
than the one CI-V message. Nothing about a real app SDK, a real "Homebrew Apps" UI, or safety hardening
needs solving yet — those come after this one round-trip is proven, the same way the firmware-update
reframing was proven in the emulator before anything else was built on top of it.

## Hook point: `sd_menu_dispatch_task`, a repurposed dead case ID

Chosen over the other Phase-2 candidates (`kernel_start`'s mystery task slot, a wholly new task) because
it sidesteps two open questions at once, not just one:

- **No new task, no ASID/privilege question.** `sdk/api/task-model.md` flags "does a new task need
  special ASID/MMU handling" as the single highest-priority open question for *any* app — real Icom
  customization (per-task ASID tagging in `swi_handler`, see `notes/kernel-rtos.md`'s RTOS-identity
  section) that hasn't been tested. Running the loader as a **plain function call inside an already-live,
  already-fully-privileged task's own context** (not a new task, not `itron_act_tsk`) makes that question
  moot for this first cut — whatever access `sd_menu_dispatch_task` already has (SCIF0/CI-V, the SD
  filesystem, everything else), our code inherits for free, no syscall trampoline or privilege step needed.
- **No jump-table growth, no new task descriptor to place.** `notes/kernel-rtos.md`'s new
  "`sd_menu_dispatch_task`'s command dispatch" section (2026-09-25) confirms this task's 42-case dispatch
  is a literal inline ARM computed-branch table, byte-listing-verified, with 12 case IDs (`0x00, 0x02-0x06,
  0x0a, 0x0e-0x10, 0x12-0x14`) that are real, currently-unreachable no-ops today. Repurposing one — current
  pick: **`0x14`** (arbitrary choice among the 12, picked only because it's the last slot before real case
  `0x15`; no other reason) — means the entire patch is **one 4-byte ARM instruction** (`b 0x20027710` →
  `b <new function's address>`, at listing address `0x200275ac`), changing nothing else about the existing
  image. Every other case, every other function, is untouched.
- **The dispatcher's own success/failure convention already matches "exit cleanly."** Every case returns
  0/1 for "done" or anything else for "still busy, re-enter" (`notes/kernel-rtos.md`'s decompile). Our new
  case just needs to return `0` and the task falls straight back into its normal `FUN_20186f60` wait —
  identical to what happens after Format, Save Setting, or any other real menu action completes. No custom
  "resume radio operation" logic needs writing at all; it's the existing convention, for free.

## Where the loader code itself lives: appended to `body.bin`, not free RAM

The loader function (the code that runs at the repurposed case ID) has to be **baked into the flashed
image**, since nothing else would ever put it in RAM. `tools/icom_fw`'s packer already round-trip-verifies
**"a length-changing append"** at the end of a real decompressed body (`tools/verify_pack.py`, cited in
`roadmap.md`'s Phase 1) — so the loader's machine code gets appended as new bytes at the end of the
decompressed `body.bin` (current static image ends at `0x20395b18`, confirmed in
`notes/band-scope-state-history.md`'s broad sweep), and the one jump-table slot above points at it. No new
packer work needed; this is exactly the capability that tooling gap already closed.

## Where the app blob lives at runtime: free RAM, loaded fresh from SD each run

Confirmed-empty candidate: **`0x20500000`+** — inside the Ghidra project's own `ram_placeholder` memory
block (`0x20395b18`-`0x209ffffe`, real RZ/A1H on-chip RAM per `notes/memory-map.md`'s MMU section, which
identity-maps the whole `0x20000000`-`0x209fffff` 10 MB as one region), comfortably clear of both the
static image's own end and the "hot" application BSS window `notes/band-scope-state.md` already mapped
(`0x203f0000`-`0x20404770` — genuinely live data, not free) — checked directly, zero `references_to` hits
at `0x20500000` itself, consistent with the broader confirmed-empty `0x20408000`-`0x209c8000` sweep in
`notes/band-scope-state-history.md`. ~6.7 MB of headroom above it.

**Not yet independently verified**: Ghidra's own `ram_placeholder` block is marked `rw-` (no execute) —
this is very likely just this project's own conservative bookkeeping when the block was added (a single
10 MB MMU section entry shouldn't plausibly have a sub-region execute-never bit baked in, and nothing in
`notes/memory-map.md` suggests one), not a real hardware/MMU restriction, but this is an assumption, not a
confirmed fact — worth a real check (live JTAG, or a QEMU single-step test in `qemu-machine/`) before
trusting that code placed here actually executes, rather than finding out the hard way on real hardware.

The app blob is **not** part of the flashed image at all — the loader reads it fresh from the SD card into
this RAM region every time the hook runs, which is the actual "install an app = drop a file on the SD
card, no reflash" ergonomics `roadmap.md`'s reframing promised. First cut: a fixed path (e.g.
`C:\IC-7300\APP.BIN`), fixed max size (comfortably under the ~6.7 MB headroom — 64 KB is a first-pass
placeholder, no real app needs more yet), loaded via `file_rpc_post_command` (`0x200bc048`,
`sdk/api/filesystem.md`) commands `6` (open) and `0x13` (read-at-offset) — already-used-internally,
lower-risk than the raw VFS layer's own known refcount fragility (`sdk/api/filesystem.md`'s
`fs_object_release_ref_UNSAFE_NEGATIVE` note). **Open**: `file_rpc_post_command`'s command catalogue
(`sdk/api/filesystem.md`) doesn't yet include a documented "close" ID (`6`/`9`/`0x13`/`0x17` known) — needs
finding, or the first cut accepts a leaked file handle (acceptable for a single manually-triggered test,
not for anything real).

## Fail-closed behavior (Phase 2's own stated safety requirement)

Missing/unreadable app file → the loader returns `0` immediately, same as every other no-op case. No
partial state, no hang, no crash — the dispatcher can't tell the difference between "ran an app" and "this
case did nothing," by construction, as long as the loader itself never blocks unboundedly (bound every SD
read with the same timeout convention `file_rpc_post_command`'s existing callers already use) and never
writes outside its own fixed RAM window.

## What the app itself can do: still open, only what's needed for CI-V hello-world

For the very first proof of concept, the app only needs to emit one CI-V frame and return. `emitting a
CI-V frame` turns out to be less simple than "call the TX helper" — `sdk/api/serial-civ.md`'s confirmed TX
path (`FUN_20011384`/`FUN_200110a8`) is a **stateful periodic pump**, not a clean one-shot send API; it
reads a cluster of flag bits and a staged reply buffer that real CI-V command handlers populate as a side
effect of processing an actual inbound command, not something meant to be called out of context. **Two
options, still open, dispatched to a fresh-eyes pass 2026-09-25**:
1. Find a real, simple, already-implemented CI-V command handler, and stage a reply using **exactly the
   same byte sequence/flag bits it does** (mimicry, not calling the pump directly) — the safer option,
   since it reuses code the firmware already trusts, just with our own payload.
2. Failing that, trace the pump functions' own flag/offset semantics precisely enough to drive them
   correctly from cold — the fallback if no simple handler example turns up.

Results land here once in.

## Open items (as of 2026-09-25, this design's first pass)

- **The trigger.** Everything above assumes *something* writes `0x14` to `0x20390160` and calls
  `rtos_post_event` on the handle at `0x2039011c+0x40` — that "something" doesn't exist yet, and how a real
  SD-card-menu screen tap would do this (needed for an eventual real "Homebrew App" menu button, not just
  this first proof of concept) is unresolved — see `notes/kernel-rtos.md`'s matching open item. For the
  very first proof of concept, the trigger doesn't need to be a real menu item at all — the simplest
  option is a small, separate, deliberately-added hook elsewhere (a distinctive front-panel key combo
  already fully decoded in `notes/front-panel-report.md`, or a periodic check for a marker file) that does
  the write-and-post directly; a real visible menu entry is a later, separable milestone once the UI
  hand-off mechanism is understood.
- **The CI-V staging cookbook** (above).
- **`file_rpc_post_command`'s close command ID.**
- **The `ram_placeholder` execute-permission assumption.**
