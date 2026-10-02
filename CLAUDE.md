# IC-7300 firmware RE — session conventions

- Orient from `README.md` (usage) and `notes/status.md` (RE scoreboard), then the relevant `notes/<topic>.md`. Read `*-history.md` / `archive/` only for "how did we get here".
- Notes: `X.md` holds current state (facts, living tables, open questions). Session narrative goes in `X-history.md`; fold only the delta into `X.md`. Move text into the history file instead of deleting it.
- Verify claims (your own or a subagent's) against listings or live runs before recording them as confirmed.
- Commit validated progress without asking.

## Firmware and paths
- No Icom firmware/manuals are in this repo — the user supplies their own. Tooling reads the firmware dir from `$ICOM_FW_DIR` (default `firmware/`). Never commit firmware, manuals, or schematics.

## Ghidra
- `body.bin` is based at `0x20005000` (address = file offset + `0x20005000`); raw-file pointer scans must use this base, not `0x20000000`. Full RE setup in `notes/re-workflow.md`.

## qemu-machine
- Before hand-rolling a device model, check `qemu-machine/qemu-src/hw/` for a reusable or cross-checkable existing one.
- Use `--icount off` for SD-card work (heavy mounts stall under `-icount`). `-icount shift=1` is the normal default otherwise.
- For timing-sensitive inspection prefer QMP (`human-monitor-command` → `xp`/`info registers`) over GDB: a breakpoint/watchpoint/single-step perturbs IRQ and timer scheduling (see `icom-gdb-perturbation-resolved` memory / `qemu-machine/README-history.md`). If a GDB breakpoint is needed, keep it bounded/one-shot and pin `-icount shift=N`, then cross-check against a GDB-free re-capture.

## Subagents
Delegate trivial or mechanical work to cheaper models: `haiku` for searches, lookups, renames and bulk edits, `sonnet` for routine multi-step edits. Keep analysis and judgment in the main session. Give each agent its own files so parallel agents don't collide.
