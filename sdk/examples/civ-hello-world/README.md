# `civ-hello-world` — the first custom code actually running on (emulated) IC-7300 firmware

Live-tested 2026-09-25 in `qemu-machine`. This is the proof of concept `sdk/roadmap.md` Phase 2/3
asked for, at the simplest possible scope: hold a front-panel key combo, the radio emits one CI-V
frame, and resumes completely normal operation. Everything before this was design and static
analysis (`sdk/app-loader-design.md`); this is where it actually ran.

## What it does

Holding **XFC + SPEECH/LOCK** together (a combo not otherwise used, front-panel key names per
`notes/front-panel-report.md`) makes the radio emit one CI-V frame carrying the ASCII payload
`HOMEBREW`, using the exact staging convention a real CI-V command handler uses (`notes/
kernel-rtos.md`'s "CI-V reply staging" section) — so it goes out through the normal collision-aware
TX path, not a raw UART poke. Release and re-press to fire again (debounced: holding doesn't repeat).

## How it's installed

`app.s` is assembled and linked at a fixed address, then spliced into a real firmware container's
decompressed body alongside a single retargeted instruction inside `main_idle_loop` (see `app.s`'s
own header comment and `sdk/app-loader-design.md` for exactly why, including a real dead end —
code placed right after the static image's own end gets silently overwritten by a runtime memory
pool within seconds of boot; `0x20600000` is confirmed empirically clean).

```
python3 sdk/examples/civ-hello-world/build.py
# -> scratch/civ_hello_world_142.dat (a real, checksum-correct, repacked firmware container)
```

This does **not** touch the SD-card update flow's own MD5/flash-write acceptance path — that's
still real-hardware-only, per `sdk/roadmap.md`'s Phase 0 status. What's tested here is that the
hook mechanism itself — a one-instruction retarget plus appended code, applied through this
project's own `tools/icom_fw` packer — boots and runs correctly.

## Reproducing the test

```
python3 qemu-machine/tools/build_flash.py scratch/civ_hello_world_142.dat qemu-machine/flash.bin
python3 qemu-machine/tools/run_gui.py --no-pwrk --icount off --civ /tmp/civ.sock --display none
```

**`--icount off` is required**, not just faster — under `-icount` (this machine's usual default,
`--fast` included) the patched image reliably hangs. Root cause not fully chased (a real, separate
open item — see `sdk/app-loader-design.md`); plain, unthrottled execution works correctly and
matches this project's own existing `-icount`-related SD-card gotcha in kind, not in mechanism.

Trigger it (the socket path is whatever `run_gui.py` printed, normally `/tmp/qemu_run_gui_fp.sock`):

```
RZA1H_FPCTL=/tmp/qemu_run_gui_fp.sock python3 qemu-machine/tools/fp.py combo XFC "SPEECH/LOCK" --hold 0.5
```

Then read `/tmp/civ.sock` directly (a raw socket read, not `tools/civ.py`'s own `Civ.cmd()`/
`frames()` helpers — those only keep frames addressed *to* their own fixed controller address,
which this unsolicited frame isn't necessarily using).

## What was actually observed (2026-09-25 session)

- Baseline CI-V (`civ.py sock 03`, read frequency) works before ever touching the combo.
- Holding the combo produces exactly one frame: `fe fe e0 94 50 48 4f 4d 45 42 52 45 57 fd` —
  `to=0xE0 from=0x94 cmd=0x50 "HOMEBREW"` (`from` read live from the radio's own configured CI-V
  address, not hardcoded).
- A 2-second hold still produces exactly one frame (debounce confirmed working, not just
  once-per-tick luck).
- Releasing and re-pressing fires again.
- Immediately after, `civ 03` (read frequency), `civ 04` (read mode), and a follow-up `civ 04`
  retry all get normal replies — the radio's own CI-V dispatch is completely unaffected.

## Known limitations (this is a proof of concept, not a real SDK)

- The app logic (the "HOMEBREW" emit) is baked directly into the hook, not loaded from an SD card
  at runtime — `sdk/app-loader-design.md`'s SD-card loading design (`file_rpc_post_command`) is
  still unimplemented; this proves the injection/CI-V/clean-resume mechanism first, deliberately,
  per the user's own scoping for the first cut.
- No IRQ masking around the CI-V staging critical section (see `app.s`'s own comment — the
  dedicated `cpsid`/`cpsie` instructions fault as undefined in this hook's execution context, a
  real and reusable finding for any future hook in the same context). Best-effort only; a real
  interrupt landing mid-write drops this one attempt, never worse than that.
- The `-icount` hang is real and unexplained. Don't test or demo this under `-icount` timing.
- The trigger is a hidden key combo, not a real "Homebrew Apps" menu button — see `sdk/
  app-loader-design.md`'s own open item on the undocumented menu item-record table.
