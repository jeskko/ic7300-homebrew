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

**2026-09-25, eighth pass — a real SDK: loader v1 + C runtime + the first GUI app, live-tested.**
`sdk/loader/` supersedes the three proof-of-concept hooks with one loader firmware (C, not
assembly). It keeps the Homebrew Apps row and adds a per-`main_idle_loop` tick (`civ_tx_pump`'s
call site again) and a header-checked APP.BIN ABI (`sdk/include/hb/abi.h`). Apps are C
(`sdk/tools/build_app.py`). The runtime runs `main()` as a coroutine on the UI thread, so
blocking calls yield to the firmware instead of freezing it. `sdk/examples/hello-gui/` shows a
firmware-drawn "Hello, world!" [OK] dialog and returns after OK; its emulator test covers
relaunch and fail-closed. Design, ABI and new open items: `sdk/loader/README.md`.

**Same day, ninth pass — multiple apps.** Apps now live in `C:\homebrew\*.BIN`, and Homebrew Apps
opens a picker. It's a real firmware list screen, one row per app, listed with the firmware's own
directory RPCs (`sdk/api/filesystem.md`). There's no free list screen to use, so the loader
borrows PLAYER SET (screen `0x63`) while the picker is shown and restores it afterwards
(`notes/ui-menu.md`, "Screens"). Live-tested, including paging and the 14-app cap.

**2026-09-25, fifth pass — SD-card app loading also WORKING, LIVE-TESTED.** `sdk/examples/sd-card-app/`
builds directly on `civ-hello-world`: the firmware hook now opens `C:\IC-7300\APP.BIN`, reads it into RAM,
and calls it, using 4 real wrapper functions found by reading `firmware_update_main`'s own working file-read
code (`0xf`=open/`0x11`=read/`0x13`=seek/`0x10`=close, full detail in `sdk/api/filesystem.md`). Live-verified:
a real, separate `APP.BIN` file loaded from the SD card at runtime emits its own distinct CI-V frame (proving
it genuinely ran, not the firmware hook itself), the radio resumes normally afterward, and — tested
separately — a missing `APP.BIN` fails closed with no frame, no hang, no crash. This is the actual "install
an app = drop a file on the SD card, no reflash" mechanism this whole design has been aiming at.

**2026-09-25, sixth pass — the real "Homebrew Apps" menu button also WORKING, LIVE-TESTED.**
`sdk/examples/homebrew-apps-menu/` replaces the hidden key combo with a genuine, visible row in the real
SD CARD menu, driven through the actual touchscreen UI (screenshots in that example's README) rather than
CI-V alone: MENU → SET → SD Card now shows 3 pages instead of 2, page 3 has one correctly-labeled
"Homebrew Apps" row, and tapping it loads and runs `APP.BIN` from the SD card exactly like `sd-card-app`
does — same CI-V frame, same clean resume, same fail-closed behavior with no `APP.BIN` present. Found by
tracing the real settings-list engine (`notes/ui-menu.md`'s "SET-style settings-list engine" section) —
two *data* patches (an item count and a list pointer, plus one new catalog record in a confirmed-unused
padding gap), no instruction touched at all, unlike the other two examples' `main_idle_loop` retarget. This
closes out the user's original two-part ask (a place to run apps from SD card, and a way to put a homebrew
apps button in the menu) — both are now real, live-tested mechanisms, not just designs.

What's left: everything a real app SDK needs beyond "load and call one fixed file" (multiple apps, a real
memory/size budget, versioning, an app-picker UI if more than one app is ever installed at once).

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

## A real "Homebrew Apps" menu button — DONE, live-tested

> **2026-09-25**: fully traced and built — see `notes/ui-menu.md` ("SET-style settings-list engine") for the
> real mechanism and `sdk/examples/homebrew-apps-menu/` for the working, live-tested result. The record
> layout this section originally guessed was off by one field group; the real layout is `{action, query,
> flags, en, jp}` at base `0x2018ed48` (`g_settings_item_catalog`).

The real mechanism: every SET-tree list screen (SD CARD among them) is driven by a per-category item list
(`g_settings_category_registry`, `0x201993e0`) indexing a shared 20-byte-record catalog
(`g_settings_item_catalog`, `0x2018ed48`, `{action, query, flags, en, jp}`) — a different table from the
already-known 72-byte generic list widget (`0x2018f0ec`) `notes/ui-menu.md` documents for QUICK MENU/MEMORY
MENU/etc. Tapping a catalog-backed row tail-calls `catalog[val].action` with no arguments. The SD CARD
menu's own registry entry has exactly 8 real items and no dead slot — but the item count and the list
pointer are both plain data, and there's a confirmed-unused padding gap right after the registry table to
put a new catalog record in. `sdk/examples/homebrew-apps-menu/` patches both (no instruction touched at
all) and is live-verified through the real touchscreen UI: MENU → SET → SD Card genuinely shows 3 pages
instead of 2, with a correctly-labeled "Homebrew Apps" row on the new page that loads and runs `APP.BIN`
from the SD card exactly like `sd-card-app` does.

## Trigger for the first proof of concept

Historical: this section covers the trigger choice for `civ-hello-world`/`sd-card-app` specifically, made
before the real menu button (above) existed. Kept for context on why a hidden combo was the right call for
*those* two examples' own scope — `homebrew-apps-menu` is the real trigger now, for anything that wants one.

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

## Adversarial review pass, 2026-09-25 (seventh pass) — real bugs found and fixed

Dispatched a dedicated Opus review of all three `sdk/examples/` after the sixth pass shipped —
explicitly told to assume more mistakes were plausible (this session had already found and fixed
one real wrong assumption, the RAM-placement bug above) and to independently re-verify load-bearing
claims against real decompiles rather than trust the existing notes. It found several confirmed,
real issues; all code fixes below were regression-tested afterward against the exact same live
tests each example's own README already documents (same frames, same clean resume, same
fail-closed behavior) before being considered done.

**Fixed:**
- **A real, serious bug: a failed SD-card read executed whatever was already at the app's load
  address (`0x20610000`).** The read wrapper writes a *negative error code* into the caller's
  "bytes actually read" output on failure, not just a real byte count on success — both loaders'
  original check (`actual != 0`) treated a negative errno as "got data" and jumped into the load
  address regardless. Fixed: check the read call's own RPC-wait result *and* require
  `0 < actual <= max size` with a signed comparison. This directly contradicted this session's own
  "fails closed" documentation for both `sd-card-app` and `homebrew-apps-menu` — the missing-file
  case (the one actually live-tested before) happens to short-circuit earlier and was never
  affected, which is why it wasn't caught until this review.
- **A wrong diagnosis, corrected: `cpsid`/`cpsie` do not fault in this hook's execution context.**
  An earlier pass's own live crash led to the conclusion "CPS faults as undefined here" — wrong.
  `civ_tx_pump`, the real firmware function every one of these hooks calls first, executes `cpsid
  i`/`cpsie i` on every single tick from the identical calling context, confirmed both by a fresh
  Ghidra listing and by re-examining this session's own earlier live single-step trace (which had
  already, if unnoticed at the time, walked straight through that exact instruction without
  fault). The real cause of the original crash was the RAM-placement bug two sections up, fixed
  separately by relocating to `0x20600000` — removing `cpsid`/`cpsie` was never the fix, just a
  coincidentally-timed second change. Re-added, matching `civ_tx_pump`'s own critical-section
  scope, closing a real (narrow) race the unmasked version had.
- **Missing cache maintenance before executing freshly-loaded code.** QEMU's TCG execution doesn't
  model I-cache/D-cache incoherency, so this was invisible to every emulator test so far. Added a
  standard ARMv7-A clean-D-to-PoU / invalidate-I-to-PoU / BPIALL / DSB / ISB sequence over the
  loaded bytes before jumping in. **Still unverified live** — flagged below.
- **Stack alignment, fixed twice.** `try_open`/`try_read`/`try_close` used a single-register
  `push {lr}`, breaking 8-byte AAPCS alignment at a call into real firmware code (harmless for this
  specific callee chain, a real trap for reuse). The *first* fix attempt (`push/pop {r0, lr}`) was
  itself a new bug, caught live by this session's own regression test: `pop {r0, lr}` restores the
  *original* r0 over the RPC result the caller needs, so open/read appeared to succeed (handle set,
  buffer touched) but the caller's success check read noise instead of the real result, and no
  frame went out. Fixed properly with `r1` (never a return-value register for these calls) instead
  of `r0`. Kept as a documented lesson in both `.s` files' own header comments — a "trivial"
  alignment fix touched a return-value register without that being obvious from the diff alone.
- **`homebrew-apps-menu`'s new list/label relocated from appended RAM into the firmware's own
  read-only image.** Not a bug fix — a risk-reduction change the review prompted: the SD CARD menu
  is also the Firmware Update recovery path, and leaving its item list/label in the same
  less-certain RAM region an earlier bug already bit once would mean the *whole menu* renders
  garbage if that RAM were ever found unsafe, not just the one new row. `build.py` now copies the
  list/label bytes into the confirmed-unused padding gap next to the new catalog record; only the
  new row's own `action` pointer still points into appended code.

**Confirmed correct, no fix needed** (the review checked these specifically and found them safe):
`scratch36`'s uninitialized-buffer content (genuinely dead, never read by the real open handler);
no firmware-relied-upon register is clobbered by any of these hooks; the 32 KB read is genuinely
capped by the underlying device read loop, can't overrun past the buffer; the registry
count/pointer patch's consumers all read the count correctly (as a byte) and clamp the cursor, so
no out-of-bounds access; the visibility/diode-region filters don't special-case the new row's index
or category; the CI-V staging buffer can never be mistaken for a real reply mid-write (checked
against both the RX ISR and `civ_rx_frame_stage_and_dispatch`'s own field layout).

**Not fixed — real, documented, open risks** (see the open items list below for the short form):
`RPC_WAIT`'s unbounded wait if the RPC ring is ever full; no SD-ready/recorder-busy gate before
running (every sibling SD-menu row checks both); no `APP.BIN` content validation; the cache-
maintenance fix is unverified on real hardware; `homebrew-apps-menu`'s catalog index is read as a
truncated byte by one unrelated consumer (confirmed harmless in the 1.42 image checked, but
version-dependent); the registry/catalog patch's cursor state persists to EEPROM (confirmed benign
against stock firmware, but a real persistent side effect).

Each example's own README has the fuller per-file writeup and its own regression-test confirmation.

## Open items (as of 2026-09-25, eighth pass)

`sdk/loader/README.md` has the loader-v1-specific ones (coroutine stack vs. the RTOS, the
borrowed dialog); the list below still applies to it unless marked.


- **The `-icount` hang.** All three examples reliably hang under this machine's usual `-icount`
  timing (`--fast` included); plain unthrottled execution works correctly and repeatably. Not
  root-caused — worth a real look before trusting any future hook-based test under `-icount`, and
  before assuming this class of hook is safe on real hardware (which has no `-icount` equivalent,
  so may just be unaffected, but that's an assumption, not a confirmed fact). May be related to the
  RAM-placement question below, surfacing under different boot timing — not established either way.
- **The full extent of the "unsafe past static image end" region** — confirmed unsafe at `0x20395b18` and
  `0x20500000` (progressively, not instantly), confirmed safe at `0x20600000` and several points above it
  after a full boot. The exact boundary between unsafe and safe, and whether "safe so far in these tests"
  could still be consumed by heavier runtime activity (e.g. BMP capture, voice recording, other large
  buffer allocations this project already knows exist) over a longer running session, is not established.
  **Update 2026-09-25:** a whole-range marker sweep in the emulator found `0x20601000`–`0x2080afff`
  (2088 KB) never written, across boot, the scope, TX, the QSO recorder recording and playing back,
  the RTTY decoder, and SD save/load (`notes/memory-map.md`, "RAM above the homebrew loader"). Screen
  capture and voice-TX recording weren't covered live, but were traced statically: all of their
  buffers are fixed or bounded and below `0x205dcf60` (same file, "Screen capture and voice
  recording"). It hasn't run on hardware. **ABI v3 (same day) uses it:** a 1 MB app region,
  the framebuffers and a 448 KB heap, all in `0x20610000`–`0x207fffff` (`sdk/loader/README.md`).
- **`RPC_WAIT`'s unbounded wait.** Not a timeout despite the `0x46` argument's name in earlier notes
  (corrected in `notes/kernel-rtos.md`) — a full RPC ring makes it spin forever. Because every
  example's hook runs from `main_idle_loop` (or, for the menu button, whatever UI-tap context calls
  it), a hang here freezes more than just the custom feature. The firmware's own equivalent callers
  avoid this blast radius by running in `sd_menu_dispatch_task`'s own context instead — moving the
  load there is the real fix, a genuine redesign not attempted yet.
- **No SD-ready / recorder-busy gate.** Every sibling SD-menu row checks both before running; none
  of these examples do. Real risk if the trigger fires during recording (`voice_audio_tick` also
  runs from `main_idle_loop`) or with no card / after Unmount.
- **Cache maintenance is unverified on real hardware** — QEMU can't show whether it was needed or
  whether the sequence added is correct. **Cacheability settled 2026-09-25:** the live translation
  table maps `0x20000000`–`0x207fffff` as normal write-back write-allocate and executable
  (`notes/memory-map.md`, "RAM layout from static analysis"), so the loader's D-clean + I-invalidate
  and the SDK's D-clean before scan-out are both required, not optional. The sequence itself is
  still untested on silicon.
- **`APP.BIN` content validation** — partly closed by loader v1: magic, ABI version, entry and RAM
  bounds are checked (a headerless file is refused, tested). No checksum yet, so a truncated or
  corrupted file with a valid header would still run.
- **App SDK** — started (loader v1: C apps, versioned ABI, blocking UI calls; ABI v3 2026-09-25: 1 MB region + 448 KB heap).
  Multiple apps are done too (`\homebrew\*.BIN` + a picker, at most 14). Not yet done:
  per-app metadata (display name, icon, version) beyond the file name.
- Minor: `operating_mode_change_dispatch` (`0x2005807c`) was flagged mid-trace as a strong candidate for
  `notes/ui-menu.md`'s own long-standing "final hand-off" mystery — not chased here, noted for whoever
  picks that specific thread back up.
- Closed, checked negative: the `0x2019b70c` SD-UI state table has no dead/unused entry — don't re-sweep it.
- Closed, superseded: the `ram_placeholder` execute-permission question — moot, see above.
- Closed, live-tested: the real "Homebrew Apps" menu button — see `sdk/examples/homebrew-apps-menu/`.
- Closed, confirmed: `file_rpc_post_command`'s open/read/seek/close command IDs (`0xf`/`0x11`/`0x13`/`0x10`)
  — see `sdk/api/filesystem.md` and `sdk/examples/sd-card-app/`.
- Closed, fixed and regression-tested: the read-failure bug, the cpsid misdiagnosis, missing cache
  maintenance, and the stack-alignment bug (and its own second-order bug) — see above.
