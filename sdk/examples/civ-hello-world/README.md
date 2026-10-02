# `civ-hello-world` — the first custom code actually running on (emulated) IC-7300 firmware

> **Historical proof of concept, superseded by [`sdk/loader/`](../../loader/).** Kept as the record of how the injection mechanism was first proven. New apps should use the loader and the C SDK (see [`sdk/README.md`](../../README.md)); this example's baked-in payload (no `APP.BIN`) and key-combo trigger are not how the current loader works.

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

## Corrections from an adversarial review pass (2026-09-25)

An adversarial review of all three `sdk/examples/` (see `sdk/app-loader-design.md`'s own writeup for the
full report) found and this session fixed one real bug in this specific example:

- **`app.s`'s CI-V staging used to skip IRQ masking based on a wrong diagnosis.** The original
  version's comment claimed `cpsid`/`cpsie` "fault as undefined in this hook's execution context" —
  that was wrong. `civ_tx_pump` itself (the real firmware function this hook calls first, every
  tick) executes `cpsid i`/`cpsie i` from the exact same calling context, confirmed both by a real
  Ghidra listing and by this session's own earlier live single-step trace. The crash that prompted
  the original (wrong) diagnosis had a different, already-fixed cause — see `app.s`'s own updated
  header comment. `cpsid`/`cpsie` are back now, matching `civ_tx_pump`'s own critical-section scope,
  closing a real (if narrow) race the unmasked version had: an RX-ISR update landing between the
  final `drv` read-modify-write could drop a bit `civ_tx_pump` needs on its next pass. Regression-
  tested after the fix: combo still produces the exact same frame, still resumes cleanly.

Two more findings from the same review apply to this example but weren't separately re-tested here
(each is exercised more directly by `sd-card-app`/`homebrew-apps-menu`, which share the same file-
I/O and RAM-placement design):

- The `0x20600000` placement is confirmed safe empirically (marker-write-then-reboot testing,
  `sdk/app-loader-design.md`), but static evidence for *why* it's safe (this region sits past
  where the linker's own zero-init/heap/stack area ends, roughly `0x205dcf60`-`0x2080c400`) only
  narrows the picture — it doesn't rule out DMA or other runtime-allocated buffers this session
  never specifically exercised (BMP capture, voice recording). Longer/heavier real-world sessions
  are the real test of that, not yet done.
- The `-icount` hang (below) may be the same underlying issue as the RAM-placement question,
  showing up under different boot timing — not established either way.

## Known limitations (this is a proof of concept, not a real SDK)

- The app logic (the "HOMEBREW" emit) is baked directly into the hook, not loaded from an SD card
  at runtime — see `sdk/examples/sd-card-app/` for that next increment (built the same day).
- The `-icount` hang is real and unexplained. Don't test or demo this under `-icount` timing.
- The trigger is a hidden key combo, not a real "Homebrew Apps" menu button — see
  `sdk/examples/homebrew-apps-menu/` for that next increment (also built the same day).
