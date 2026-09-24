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

## Hook point: revised — a dead SD-UI *state* over a dead dispatch *case*

The first draft of this design proposed repurposing one of `sd_menu_dispatch_task`'s 12 dead case IDs
(still a completely valid, verified mechanism — see `notes/kernel-rtos.md`). **Revised recommendation**:
hook a dead entry in the **SD-UI per-state tick table** (`0x2019b70c`, 104 entries, ticked every pass of
`main_idle_loop`/`main_operating_loop` by the function currently misnamed `digital_mode_log_writer_tick`,
`0x2005d234`) instead, for one concrete reason the case-based approach doesn't have:

- **Context match with the CI-V TX pump avoids a real race.** The verified CI-V cookbook
  (`notes/kernel-rtos.md`) requires staging a reply and setting `drv |= 0x40` for `civ_tx_pump` to pick up
  — but `civ_tx_pump` itself runs from `main_idle_loop`'s own polling sites. If the app's CI-V emission
  runs inside `sd_menu_dispatch_task` (a separate RTOS task, scheduled independently), there's a genuine
  window where the pump reads/write-backs `drv` in the middle of the app's own read-modify-write,
  silently dropping the frame — survivable with a lock/poll/retry loop, but avoidable entirely by running
  the whole app (load, call, CI-V emit) from a hook that ticks in `main_idle_loop`'s **own** context, the
  same one the pump uses. A dead state in the `0x2019b70c` table gives exactly that.
- **This table has not yet been swept for a dead/no-op entry** the way `sd_menu_dispatch_task`'s case
  table was — concrete next step, same methodology (read the raw dispatch code/table, find an ID that
  currently resolves to a no-op).
- The `sd_menu_dispatch_task` case-based hook remains a fully valid fallback (e.g. if every SD-UI state
  turns out to be live, or for a later app that specifically wants SD-menu-task context for heavier file
  I/O) — nothing about it was wrong, it just carries the cross-task CI-V race that the state-table hook
  avoids for free. Keep both documented; pick per-app.

Either hook point shares the same fail-closed shape: return/reset to idle (`0` for the dispatch-task case
convention, or the SD-UI state byte `0x20390368` back to `0`) on any missing/unreadable app file, exactly
matching how every existing real case/state already behaves on completion — no new "resume radio
operation" logic needs writing, it's the existing convention for free either way.

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

## Open items (as of 2026-09-25, second pass)

- **Sweep the `0x2019b70c` SD-UI state table for a dead/unused entry** (this design's new preferred hook).
- **The `0x2018eebc` item-record table's own render/count logic** — the real path to a genuine menu button.
- **`file_rpc_post_command`'s close command ID.**
- **The `ram_placeholder` execute-permission assumption.**
- Minor: `operating_mode_change_dispatch` (`0x2005807c`) was flagged mid-trace as a strong candidate for
  `notes/ui-menu.md`'s own long-standing "final hand-off" mystery — not chased here, noted for whoever
  picks that specific thread back up.
