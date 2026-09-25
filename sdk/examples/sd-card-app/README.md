# `sd-card-app` — apps loaded from the SD card, no reflash to iterate

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

## Known limitations

- Fixed 32 KB max read size, fixed load address, fixed path — no real app-management scheme
  (multiple apps, versioning, a real menu entry) yet. This proves the mechanism; a real SDK
  layer is future work (`sdk/app-loader-design.md`'s open items).
- No IRQ masking around the CI-V staging critical section — same known limitation as
  `civ-hello-world`, inherited unchanged (`app_main.s`'s own CI-V emit code is identical in
  shape).
- The `-icount` hang (see `civ-hello-world/README.md`) applies here too — not re-verified
  separately, but there's no reason to expect it wouldn't.
