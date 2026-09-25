# The app loader: concrete design (started 2026-09-25)

`roadmap.md`'s Phase 2 asked three open questions — where the loader hooks in, where app code lives in
memory, and what an app can safely do. This file is where those get concrete, real-address answers as
they're settled. See `README.md` for how `sdk/` relates to `notes/`: this file mixes cited facts with
genuine design decisions, and is expected to keep changing.

**Goal for the first cut, per the user's own scoping (2026-09-25)**: the simplest possible real
proof of concept — the radio boots a one-time-modified `body.bin`, something triggers the hook, the hook
emits something over the CI-V bus, and the radio resumes completely normal operation with no crash, no
hang, no visible side effect other than the one CI-V message. **Achieved and live-tested, same day** — see
`sdk/examples/civ-hello-world/`. Nothing about a real app SDK, a real "Homebrew Apps" UI, SD-card app
loading, or safety hardening needed solving to get here — those came next, the same way the firmware-update
reframing was proven in the emulator before anything else was built on top of it.

**2026-09-25, fifth pass — SD-card app loading also WORKING, LIVE-TESTED.** `sdk/examples/sd-card-app/`
builds directly on `civ-hello-world`: the firmware hook now opens `C:\IC-7300\APP.BIN`, reads it into RAM,
and calls it, using 4 real wrapper functions found by reading `firmware_update_main`'s own working file-read
code (`0xf`=open/`0x11`=read/`0x13`=seek/`0x10`=close, full detail in `sdk/api/filesystem.md`). Live-verified:
a real, separate `APP.BIN` file loaded from the SD card at runtime emits its own distinct CI-V frame (proving
it genuinely ran, not the firmware hook itself), the radio resumes normally afterward, and — tested
separately — a missing `APP.BIN` fails closed with no frame, no hang, no crash. This is the actual "install
an app = drop a file on the SD card, no reflash" mechanism this whole design has been aiming at. What's left
of the original goal: a real "Homebrew Apps" menu button (still a hidden key combo today) and everything a
real app SDK needs beyond "load and call one file" (multiple apps, a real memory/size budget, versioning).

**2026-09-25, second pass**: an Opus fresh-eyes investigation (dispatched from this design's first draft)
resolved both blocking open questions from the first pass — the CI-V staging cookbook, and the real
SD-card-menu tap-to-post chain (see `notes/kernel-rtos.md`'s "CI-V reply staging" and "`sd_menu_dispatch_task`'s
command dispatch" sections for the full verified detail; spot-checked against real listings/decompiles
before being recorded, per this project's own verification discipline). That work also surfaced a cleaner
hook point than the one this file originally proposed — see below.

**2026-09-25, fourth pass — WORKING, LIVE-TESTED in `qemu-machine`.** `sdk/examples/civ-hello-world/`
holds the actual running code: hold a front-panel key combo, the radio emits one real CI-V frame, then
resumes completely normal CI-V operation (verified by reading frequency/mode back immediately after,
repeatedly, across a fresh boot). This is the injection+CI-V+clean-resume proof of concept the user asked
for, descoped exactly as they scoped it — the app payload is baked into the hook rather than loaded from
SD card at runtime (the SD-loading design below is unchanged and still the intended next increment, just
not needed to prove the hook mechanism itself works). Getting there surfaced one major correction to this
design (the appended-code placement address was wrong — see "Where appended code actually has to live"
below) and one real unexplained gotcha (`-icount` hangs the patched image; plain unthrottled execution
doesn't). See `sdk/examples/civ-hello-world/README.md` for the full test log and exact repro steps.

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

## Where appended code actually has to live — corrected, live-tested (2026-09-25, fourth pass)

**This section's earlier claim was wrong, and the error is worth recording plainly.** The first three
passes assumed the region right after `body.bin`'s own static image (`0x20395b18`+, inside Ghidra's
`ram_placeholder` block) was safe free space to append a loader into, on the strength of a *static*
argument: zero `references_to` hits across a broad sweep (`notes/band-scope-state-history.md`). That
argument has a real hole its own author already flagged and this design failed to apply: a genuine runtime
allocator's contents are invisible to static xref analysis *by construction* — only compile-time
literal-pool addresses show up as hits, never something a heap manager hands out at runtime. That's exactly
what this region turned out to be.

**Live-tested, root cause found**: code appended at `0x20395b18` decompresses into RAM correctly (confirmed
present, byte-for-byte, at `body.bin`'s very entry point and still at `kernel_start`'s entry) but is
**zeroed out by the time `cold_boot_hw_init` starts** — i.e., wiped during kernel/RTOS bring-up, well before
`main_idle_loop` ever runs. Traced (not fully, but far enough to be useful) to `kernel_start` →
`FUN_20186d2c` → `FUN_20188574`, a lazy memory-manager init routine whose own plate comment already
correctly identified it as matching `R_OS_InitMemManager`'s shape; it initializes a heap starting at
`DAT_201885e0` = `0x20416198` with a `size` field read from `0x20336054` = `0x9f88` (~40 KB) — too small on
its own to explain reaching `0x20395b18` or beyond, so this specific pool isn't the full story, but it
confirms the region is heap-adjacent kernel-object territory, not dead space. **A second, empirical finding
matters more than fully chasing the exact allocator**: markers written at `0x20500000` survived a ~5 second
window but were gone (overwritten with plausible code-shaped bytes, not just zero) by ~20 seconds into
boot — i.e., **something keeps writing further into this region as boot progresses, not just once**. A
sweep of markers across the rest of the 10 MB RAM space, checked after confirming full boot (live CI-V
replies), found `0x20600000` and several addresses above it undisturbed. **`0x20600000` is the address
`sdk/examples/civ-hello-world/` actually uses**, confirmed to survive a complete boot to steady state,
repeatedly, across multiple fresh tests.

**Practical rule going forward**: anything appended past `body.bin`'s own static image end needs a padded
gap out to a address empirically confirmed safe (marker-write-then-reboot-and-check, the method used here)
before being trusted — a clean `references_to` sweep alone is not sufficient evidence for this class of
region, and should not be cited as such again in this project. `0x20600000`+ is confirmed only as far up
as spot-checked (`0x20700000`, `0x20800000`, `0x209d0000`, `0x209f0000` all survived too in the same
sweep) — not the same as "confirmed safe all the way to `0x209fffff`."

## Loader code lives appended to `body.bin`; the app blob loads fresh from SD into free RAM

- The **loader** (the small fixed function that runs at the repurposed hook, reads the app file, copies it
  into RAM, and calls it) has to be baked into the flashed image — `tools/icom_fw`'s packer already
  round-trip-verifies a length-changing append at the end of a real decompressed body
  (`tools/verify_pack.py`, `roadmap.md` Phase 1), confirmed live-working too (`sdk/examples/civ-hello-world/`
  pads the body out to `0x20600000` with ~2.5 MB of zeros, which LZSS compresses cheaply enough to still
  fit the fixed compressed-slot budget with room to spare) — the loader's machine code is appended there and
  the one hook slot is repointed at it. No new packer work needed.
- The **app blob itself should still not be part of the flashed image** — loading it fresh from the SD card
  each time the hook fires is still the design (first cut: a fixed path, e.g. `C:\IC-7300\APP.BIN`, fixed
  max size well under budget — 64 KB placeholder) into RAM **at or above the newly-confirmed-safe
  `0x20600000`+ region** (not `0x20500000` — see the correction above). This is the actual "install an app =
  drop a file on the SD card, no reflash" ergonomics `roadmap.md`'s reframing promised. **Not yet built** —
  `civ-hello-world`'s payload is baked in, not SD-loaded; this is the next real increment.
- Loaded via `file_rpc_post_command` (`0x200bc048`) commands `6` (open) / `0x13` (read-at-offset) — this
  primitive is a cross-task RPC (consumed by the separate `sdcard_file_rpc_dispatch_task`), so it's safe
  to call from either candidate hook context, not just from inside `sd_menu_dispatch_task`.
- **Still open**: `file_rpc_post_command`'s command catalogue (`sdk/api/filesystem.md`) doesn't yet
  document a "close" ID (`6`/`9`/`0x13`/`0x17` known) — needed before this leaks a handle on every run; a
  single manually-triggered test can tolerate the leak, a real feature can't.
- **Superseded**: the earlier "`ram_placeholder` execute-permission" open item assumed the wrong region was
  even the right place to check — moot now, since live testing already proves code executes correctly once
  placed somewhere the region isn't being overwritten (RAM there is demonstrably executable; the failure
  mode was always content, never permissions).

## CI-V emission: resolved — see `notes/kernel-rtos.md`

Full verified byte-level cookbook (buffer/flag addresses, the exact staging order a real handler follows,
the `drv` bit table, and the cross-task race this design's hook-point choice is built to avoid) now lives
in `notes/kernel-rtos.md`'s "CI-V reply staging" section — that's `notes/` territory (confirmed facts
about the real firmware), not restated here to avoid two copies drifting apart. Short version for this
design: the app stages `[to][from][cmd][payload...][0xFD]` at `rxbuf+0x66` (`rxbuf = 0x20396ad4`), sets the
ready flag `rxbuf[0xca] = 1`, then sets `drv |= 0x40` last (`drv = 0x20390039`) — the exact sequence a real
command handler follows, just with an arbitrary payload instead of a real command's own reply data.

## A real "Homebrew Apps" menu button — a concrete, previously-unknown lead

> **Update 2026-09-25**: this is now traced; see `notes/ui-menu.md` ("SET-style settings-list engine") and
> the open item below. The record layout quoted in this section is off by one field group. The real
> layout is `{action, query, flags, en, jp}` at base `0x2018ed48`.

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

## Open items (as of 2026-09-25, fifth pass)

- **The `-icount` hang.** Both `civ-hello-world` and `sd-card-app` reliably hang under this machine's usual
  `-icount` timing (`--fast` included); plain unthrottled execution works correctly and repeatably. Not
  root-caused — worth a real look before trusting any future hook-based test under `-icount`, and before
  assuming this class of hook is safe on real hardware (which has no `-icount` equivalent, so may just be
  unaffected, but that's an assumption, not a confirmed fact).
- **The full extent of the "unsafe past static image end" region** — confirmed unsafe at `0x20395b18` and
  `0x20500000` (progressively, not instantly), confirmed safe at `0x20600000` and several points above it
  after a full boot. The exact boundary between unsafe and safe, and whether "safe so far in these tests"
  could still be consumed by heavier runtime activity (e.g. BMP capture, voice recording, other large
  buffer allocations this project already knows exist) over a longer running session, is not established.
- **A real app SDK beyond "load and call one fixed file"** — `sd-card-app`'s loader has a fixed path, a
  fixed 32 KB size cap, and no versioning/multi-app story. Real next-layer design work, not yet started.
- **A real "Homebrew Apps" row: the static analysis is done; it needs an implementation and a live test.**
  The render, count and tap logic is traced in `notes/ui-menu.md` ("SET-style settings-list engine"). There
  is no dead slot in the SD CARD list (category 0x18, 8 real items), but the count is plain registry data.
  The minimal patch is:
  - `registry[0x18]` at `0x20199500`: count 9, list pointer → a new 9-entry list with an extra `(3, N)`
  - one 20-byte `{action, 0, 0x00010700, en, jp}` catalog record at `0x2018ed48 + N*20` in the unused
    padding `0x20199758`–`0x201998cc` (e.g. `0x2019975c`, N = 0x881)

  The action is reached by `bx` with no arguments.
- Minor: `operating_mode_change_dispatch` (`0x2005807c`) was flagged mid-trace as a strong candidate for
  `notes/ui-menu.md`'s own long-standing "final hand-off" mystery — not chased here, noted for whoever
  picks that specific thread back up.
- Closed, checked negative: the `0x2019b70c` SD-UI state table has no dead/unused entry — don't re-sweep it.
- Closed, superseded: the `ram_placeholder` execute-permission question — moot, see above.
- Closed, confirmed: `file_rpc_post_command`'s open/read/seek/close command IDs (`0xf`/`0x11`/`0x13`/`0x10`)
  — see `sdk/api/filesystem.md` and `sdk/examples/sd-card-app/`.
