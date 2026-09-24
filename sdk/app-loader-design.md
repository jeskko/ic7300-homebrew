# The app loader: concrete design (started 2026-09-25)

`roadmap.md`'s Phase 2 asked three open questions — where the loader hooks in, where app code lives in
memory, and what an app can safely do. This file is where those get concrete, real-address answers as
they're settled. See `README.md` for how `sdk/` relates to `notes/`: this file mixes cited facts with
genuine design decisions, and is expected to keep changing.

**Goal for the first cut, per the user's own scoping (2026-09-25)**: the simplest possible real
proof of concept — the radio boots a one-time-modified `body.bin`, something triggers the hook, the hook
loads a tiny app from the SD card, the app emits something over the CI-V bus, and the radio resumes
completely normal operation with no crash, no hang, no visible side effect other than the one CI-V
message. Nothing about a real app SDK, a real "Homebrew Apps" UI, or safety hardening needs solving yet —
those come after this one round-trip is proven, the same way the firmware-update reframing was proven in
the emulator before anything else was built on top of it.

**2026-09-25, second pass**: an Opus fresh-eyes investigation (dispatched from this design's first draft)
resolved both blocking open questions from the first pass — the CI-V staging cookbook, and the real
SD-card-menu tap-to-post chain (see `notes/kernel-rtos.md`'s "CI-V reply staging" and "`sd_menu_dispatch_task`'s
command dispatch" sections for the full verified detail; spot-checked against real listings/decompiles
before being recorded, per this project's own verification discipline). That work also surfaced a cleaner
hook point than the one this file originally proposed — see below.

## Hook point: revised twice — landed on a `main_idle_loop` call-site retarget

The first draft proposed a dead `sd_menu_dispatch_task` case ID (still fully valid, see
`notes/kernel-rtos.md`). The second pass proposed a dead SD-UI *state* instead, to get `main_idle_loop`'s
own execution context (matching the CI-V TX pump, avoiding a cross-task race). **That lead was checked and
came back negative**: all 104 entries of the `0x2019b70c` state-tick table were read directly (raw memory
read, not inferred) — every single one is a real, non-null function pointer in the `0x2005xxxx`-`0x2020ccxx`
range. No dead slot exists in this table; it's fully populated. Recorded here so a future pass doesn't
re-attempt the same sweep.

**Final choice: retarget one existing `main_idle_loop` call-site instruction**, the same "change one 4-byte
instruction, touch nothing else" philosophy as the dead-case approach, but applied to a *call site* instead
of a *dead table slot* — which gets the context-match benefit without needing a table to have spare room at
all:

- `main_idle_loop` calls `civ_tx_pump` directly and unconditionally every pass, at `0x20052f64`
  (`bl 0x20011384` — verified by listing; the neighboring instruction at `0x20052f5c`,
  `bl 0x2000b258`/`civ_rx_frame_stage_and_dispatch`, matches this task's own earlier finding exactly,
  cross-confirming the address range really is inside `main_idle_loop`).
- Retarget that one instruction to `bl <homebrew_tick>` (an appended trampoline), where `homebrew_tick`
  is: `push {lr}; bl civ_tx_pump; bl homebrew_check_and_run; pop {lr}; bx lr` — calls the original target
  first (100% preserves existing behavior, including its own return-address handling — this is why the
  trampoline needs its own `push`/`pop {lr}`, since it now nests two calls where the original site made
  one), then our own addition, then returns exactly where `main_idle_loop` expects.
- **This one retarget gives every future app the right context for free** — no per-app race analysis
  needed, since `homebrew_check_and_run` (and anything it calls, including a loaded app's `app_main`) now
  always executes from inside `main_idle_loop`'s own tick, same as `civ_tx_pump` itself.
- **Trigger and action collapse into one place.** `homebrew_check_and_run` can itself test a cheap,
  no-I/O condition every tick (see "Trigger" below) and, on a fresh (debounced) match, do the SD load +
  call + CI-V emit inline — no separate "arm a flag, a later hook consumes it" indirection needed, and no
  dependency on `sd_menu_dispatch_task` or any menu/case machinery at all for this first proof of concept.
  Fail-closed is automatic: any failure (file missing, read error) just returns without emitting anything;
  `main_idle_loop` never sees a difference from the case where the trigger condition wasn't met at all.
- The two earlier hook candidates (dead `sd_menu_dispatch_task` case, SD-UI state) remain valid ideas for
  a *later* app that specifically wants SD-menu-task's own execution context (e.g. heavier file I/O
  alongside real SD-menu traffic) — not wrong, just not needed for this first, CI-V-focused proof of
  concept, and the SD-UI-state option specifically is now a closed, checked-negative lead.

## Trigger: a debounced front-panel key combo, checked inline in `homebrew_check_and_run`

No SD-card polling needed for the trigger itself (only for the actual app-load, once triggered) — cheapest
and lowest-ambiguity is a bitfield test against the already-resident front-panel status buffer
(`notes/front-panel-report.md`'s fully live-verified key-code table, offsets `0x0d`-`0x11` of
`g_scif3_rx_status_buffer`/`g_frontpanel_latched_status`), debounced with a one-byte "already handled this
press" latch so it fires once per press/hold rather than once per tick. Concrete combo choice deferred to
implementation (any two-key combo not already tested together elsewhere is fine — the existing service-mode
combos, e.g. MENU+FUNCTION, are all single-purpose and already gated well before this new code would run,
so no collision risk either way).

## Loader code lives appended to `body.bin`; the app blob loads fresh from SD into free RAM

Unchanged from the first pass:

- The **loader** (the small fixed function that runs at the repurposed hook, reads the app file, copies it
  into RAM, and calls it) has to be baked into the flashed image — `tools/icom_fw`'s packer already
  round-trip-verifies a length-changing append at the end of a real decompressed body
  (`tools/verify_pack.py`, `roadmap.md` Phase 1), so the loader's machine code is simply appended after the
  static image's current end (`0x20395b18`) and the one hook slot (whichever table is chosen above) is
  repointed at it. No new packer work needed.
- The **app blob itself is not part of the flashed image** — the loader reads it fresh from the SD card
  every time the hook fires (first cut: a fixed path, e.g. `C:\IC-7300\APP.BIN`, fixed max size well under
  budget — 64 KB placeholder) into a confirmed-empty RAM region, **`0x20500000`+** (inside Ghidra's
  `ram_placeholder` block, `0x20395b18`-`0x209ffffe`; zero `references_to` hits at `0x20500000` itself,
  consistent with the broader confirmed-empty `0x20408000`-`0x209c8000` sweep in
  `notes/band-scope-state-history.md`; ~6.7 MB of headroom above it). This is the actual "install an app =
  drop a file on the SD card, no reflash" ergonomics `roadmap.md`'s reframing promised.
- Loaded via `file_rpc_post_command` (`0x200bc048`) commands `6` (open) / `0x13` (read-at-offset) — this
  primitive is a cross-task RPC (consumed by the separate `sdcard_file_rpc_dispatch_task`), so it's safe
  to call from either candidate hook context, not just from inside `sd_menu_dispatch_task`.
- **Still open**: `file_rpc_post_command`'s command catalogue (`sdk/api/filesystem.md`) doesn't yet
  document a "close" ID (`6`/`9`/`0x13`/`0x17` known) — needed before this leaks a handle on every run; a
  single manually-triggered test can tolerate the leak, a real feature can't.
- **Still an assumption, not yet checked**: Ghidra's `ram_placeholder` block is marked `rw-` (no execute)
  in the tool's own bookkeeping — very likely just conservative labeling from whenever the block was added
  (a single 10 MB MMU section shouldn't plausibly carry a sub-region execute-never bit), not a real
  hardware restriction, but worth a real check (QEMU single-step in `qemu-machine/`, or live JTAG) before
  trusting it on real hardware.

## CI-V emission: resolved — see `notes/kernel-rtos.md`

Full verified byte-level cookbook (buffer/flag addresses, the exact staging order a real handler follows,
the `drv` bit table, and the cross-task race this design's hook-point choice is built to avoid) now lives
in `notes/kernel-rtos.md`'s "CI-V reply staging" section — that's `notes/` territory (confirmed facts
about the real firmware), not restated here to avoid two copies drifting apart. Short version for this
design: the app stages `[to][from][cmd][payload...][0xFD]` at `rxbuf+0x66` (`rxbuf = 0x20396ad4`), sets the
ready flag `rxbuf[0xca] = 1`, then sets `drv |= 0x40` last (`drv = 0x20390039`) — the exact sequence a real
command handler follows, just with an arbitrary payload instead of a real command's own reply data.

## A real "Homebrew Apps" menu button — a concrete, previously-unknown lead

The tap-to-post trace surfaced the actual mechanism a menu item like "Firmware Update" uses: a
**previously undocumented 20-byte item-record table** around `0x2018eebc` (`{en_label, jp_label,
cb_action, cb_query, flags}`, Save/Load Setting, Format, Unmount, REC Start/Stop, Play Files, CI-V Address
and more as siblings, stride `0x14`) — a real, different table from the already-known 72-byte generic list
widget (`0x2018f0ec`) `notes/ui-menu.md` documents for QUICK MENU/MEMORY MENU/etc. **Not yet traced**: what
widget code actually reads this table (how many records it renders, whether the count is a fixed constant
or scans for a terminator) — the load-bearing question for whether a genuinely new, visible row can be
added the same low-risk way the dead case IDs/states can (repurpose an existing, already-counted-but-dead
slot, if one exists) versus needing to understand and extend the render/count logic itself. Real next step
for the *menu button* half of the user's ask, separate from — and not blocking — the CI-V hello-world
proof of concept, which doesn't need a visible menu entry at all for its first trigger (see below).

## Trigger for the first proof of concept

Doesn't need to be the real menu button (that's the separable lead above). Cheapest, lowest-ambiguity
options, either wired into the same hook location chosen above:
- A dedicated, currently-unused front-panel key combo (`notes/front-panel-report.md` has the complete,
  live-verified key-code table) checked once per tick — held long enough to be clearly deliberate, never
  producible by accident.
- A marker-file check (e.g. "does `C:\IC-7300\APP.BIN` exist") gated to run only occasionally (not every
  tick) to bound the SD-card access cost — arguably the more natural first trigger for "run an app from the
  SD card" specifically, since it needs no front-panel interaction at all.

Pick one when writing the actual hook code; both are equally valid for a first test and neither blocks the
other design pieces above.

## Open items (as of 2026-09-25, third pass)

- **The `0x2018eebc` item-record table's own render/count logic** — the real path to a genuine menu button
  (separate from, and not blocking, this design's CI-V proof of concept).
- **`file_rpc_post_command`'s close command ID.**
- **The `ram_placeholder` execute-permission assumption.**
- **Not yet written**: the actual `homebrew_tick`/`homebrew_check_and_run`/loader/app machine code, the
  one-instruction `main_idle_loop` retarget, and an `icom_fw`-packed test image — this design is now
  concrete enough to build against; implementation is the next step.
- Minor: `operating_mode_change_dispatch` (`0x2005807c`) was flagged mid-trace as a strong candidate for
  `notes/ui-menu.md`'s own long-standing "final hand-off" mystery — not chased here, noted for whoever
  picks that specific thread back up.
- Closed, checked negative: the `0x2019b70c` SD-UI state table has no dead/unused entry (see above) — don't
  re-sweep it.
