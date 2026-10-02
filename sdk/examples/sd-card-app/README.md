# `sd-card-app` — apps loaded from the SD card, no reflash to iterate

> **Historical proof of concept, superseded by [`sdk/loader/`](../../loader/).** Kept as the record of how the injection mechanism was first proven. New apps should use the loader and the C SDK (see [`sdk/README.md`](../../README.md)); this example's fixed `C:\IC-7300\APP.BIN` path and key-combo trigger are not how the current loader works.

Live-tested 2026-09-25 in `qemu-machine`, built directly on
[`sdk/examples/civ-hello-world/`](../civ-hello-world/)'s proven injection mechanism. This is
the actual "install an app = drop a file on the SD card" ergonomics `sdk/roadmap.md`'s
reframing promised, working end to end for the first time: **flash the loader hook once**, then
every "app install" after that is just copying a file onto the SD card.

## What it does

Same trigger as `civ-hello-world` — hold **XFC + SPEECH/LOCK** — but instead of a baked-in
payload, the firmware hook now opens `C:\IC-7300\APP.BIN` from the SD card, reads it into RAM,
and calls it. `app_main.s` (the file that becomes `APP.BIN`) emits a CI-V frame with payload
`SDAPP` (deliberately different from `civ-hello-world`'s `HOMEBREW`, and a string that exists
nowhere in the firmware image itself — only in the separate file), then returns.

## How it's built and installed

```
python3 sdk/examples/sd-card-app/build.py
# -> scratch/sd_card_app_142.dat  (flash this once)
# -> scratch/APP.BIN              (drop this on the SD card, iterate freely)
```

Two genuinely separate artifacts: `sd_card_app_142.dat` is a real, checksum-correct, repacked
firmware container (same one-instruction `main_idle_loop` retarget as `civ-hello-world`, plus
the loader code appended at the confirmed-safe `0x20600000`) — flash this **once**.
`APP.BIN` is a completely standalone flat ARM binary, linked at a fixed load address
(`0x20610000`) the loader always uses — this is the file you'd actually iterate on.

## The file I/O primitive — a real find, not a guess

`loader_hook.s` calls `FUN_200bc5f4`/`FUN_200bc6a4`/`FUN_200bc64c` (open/read/close) directly,
with a plain C calling convention — no hand-crafted RPC message needed. These are 3 of a family
of 26 tiny per-command wrapper functions this project already knew existed
(`sdcard_file_rpc_dispatch_task`'s own retraction comment, `notes/kernel-rtos.md`) but hadn't
individually documented. Found by reading `firmware_update_main`'s own real, working file-read
code (it opens the SD-card update container the exact same way) — a much more reliable anchor
than guessing at `file_rpc_post_command`'s raw message layout from cold. Confirmed command IDs:
**open = `0xf`, read = `0x11`, seek = `0x13`, close = `0x10`** (`sdk/api/filesystem.md`'s
earlier `6`/`9`/`0x13`/`0x17` guess was for a different/incomplete reading — these are the real,
decompiled values). `FUN_200214b0(return_value, 0x46)` waits for the RPC to complete and
returns 0 on success, for all four calls.

## Reproducing the test

```
python3 qemu-machine/tools/build_flash.py scratch/sd_card_app_142.dat qemu-machine/flash.bin
python3 qemu-machine/tools/build_sdcard.py -o scratch/sdcard.img --size-mb 64
mmd -i scratch/sdcard.img@@1M "::IC-7300"
mcopy -i scratch/sdcard.img@@1M scratch/APP.BIN "::IC-7300/APP.BIN"
python3 qemu-machine/tools/run_gui.py --no-pwrk --icount off --civ /tmp/civ.sock \
    --sd scratch/sdcard.img --display none
```

(`--icount off` is required — see `civ-hello-world/README.md`'s own note, same unexplained
open item.) Trigger and read the frame exactly as in `civ-hello-world/README.md`.

## What was actually observed (2026-09-25 session)

- Baseline CI-V works before ever touching the combo.
- Holding the combo with `APP.BIN` present on the card produces exactly:
  `fe fe e0 94 51 53 44 41 50 50 fd` — `to=0xE0 from=0x94 cmd=0x51 "SDAPP"`. Since that string
  and command byte exist only in the separate `APP.BIN` file, this is direct proof the app was
  genuinely loaded from the SD card and executed, not that the firmware hook itself did
  something.
- Immediately after, `civ 03`/`civ 04` (read frequency/mode) both get normal replies.
- **Fail-closed, tested with an empty SD card (no `APP.BIN` at all)**: boots fine, the combo
  produces no frame at all (no partial/garbage output), and the radio remains fully responsive
  to normal CI-V commands afterward — open failing cleanly short-circuits the whole load/run
  path, exactly as designed.

## Corrections from an adversarial review pass, and a second bug found while fixing them (2026-09-25)

An adversarial review of all three `sdk/examples/` found several real issues (`sdk/app-loader-design.md`
has the full report). Fixed here, all regression-tested afterward (same frame, same clean resume,
same fail-closed behavior with no `APP.BIN`):

- **Real bug, confirmed: a failed read used to execute whatever was at `0x20610000`.** The read
  wrapper writes a *negative error code* into `*actual_out` on failure, not just on success — the
  original check here (`actual != 0`) treated a negative errno as "got some bytes" and called into
  the load address anyway, on stale data from a previous run (or power-on garbage on real
  hardware). Fixed: `load_and_run_app` now checks `try_read`'s own RPC-wait result *and* requires
  `0 < actual <= APP_MAX_SIZE` with a signed comparison.
- **Real bug, missing: no cache maintenance before jumping into freshly-loaded code.** QEMU's TCG
  execution doesn't model I-cache/D-cache incoherency, so this was invisible in every emulator test
  — on real Cortex-A9 silicon, code the CPU just wrote via a path that only guarantees data-side
  visibility can be fetched stale. Added `cache_flush_range` (clean-D-to-PoU / invalidate-I-to-PoU /
  BPIALL / DSB / ISB over the bytes actually read) before the `blx`. **Still unverified live** — QEMU
  can't show whether this was needed or whether it now works correctly; flagged for a real-hardware
  or cache-model check before relying on it.
- **Stack alignment**: `try_open`/`try_read`/`try_close` used to `push {lr}` alone (4 bytes, breaking
  8-byte AAPCS alignment at a public call boundary) — harmless for this specific callee chain but a
  trap for reuse. **First fix attempt introduced a second, worse bug**, caught live by this session's
  own regression test after making it: padding with `push {r0, lr}` / `pop {r0, lr}` silently
  destroys the RPC result these functions return in `r0` (the `pop` restores the *original* garbage
  `r0` over top of it) — the app opened the file correctly but the read result was read as noise,
  so no frame went out even though nothing had crashed. Fixed properly with `r1` instead of `r0` for
  the padding register, which isn't used to return anything. Worth remembering as its own lesson:
  a "trivial" alignment fix touched a return-value register without that being obvious from the
  diff alone — reread what pop restores, not just whether push/pop are balanced.

**Not fixed, documented as a real open risk**: `RPC_WAIT` (`0x200214b0`) is not actually a timeout —
the `0x46` argument only selects an error-code mapping inside it; the wait itself blocks forever on a
kernel semaphore/event. If the SD-menu task's own RPC ring is ever full, `file_rpc_post_command`
returns `2`, which no real request handle will ever equal, so `RPC_WAIT` spins forever waiting for a
match that can't happen. Because this hook runs from `main_idle_loop`, that would freeze the whole
UI and the CI-V pump, not just this feature. The deeper fix (running the load from
`sd_menu_dispatch_task`'s own context instead, so a hang stays confined the way the firmware's own
equivalent failures already are) is a real redesign, not attempted in this pass.

## Known limitations

- Fixed 32 KB max read size, fixed load address, fixed path — no real app-management scheme
  (multiple apps, versioning) yet. This proves the mechanism; a real SDK layer is future work.
- No size/magic/checksum validation of `APP.BIN`'s own content before executing it.
- No SD-card-ready or voice-recorder-busy gate before running — every sibling SD-menu row checks
  both; this hook doesn't. Running while the radio is mid-recording could block `main_idle_loop`
  (which `voice_audio_tick` also runs from) for as long as the SD RPC takes.
- The `RPC_WAIT` unbounded-hang risk above.
- The cache-maintenance fix above is unverified on real hardware.
- The `-icount` hang (see `civ-hello-world/README.md`) applies here too — not re-verified
  separately, but there's no reason to expect it wouldn't.
