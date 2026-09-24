# IC-7300 firmware RE — session conventions

- Orient from `README.md` → Status and the relevant `notes/<topic>.md`. Read `*-history.md` / `archive/` only for "how did we get here".
- Notes: `X.md` holds current state (facts, living tables, open questions). Session narrative goes in `X-history.md`; fold only the delta into `X.md`. Move text into the history file instead of deleting it.
- Ghidra: `body.bin` is based at `0x20005000`.
- Before hand-rolling a `qemu-machine/` device model, check `qemu-machine/qemu-src/hw/` for an existing one.
- Verify claims (your own or a subagent's) against listings or live runs before recording them as confirmed.
- Commit validated progress without asking.

## Subagents
Delegate trivial or mechanical work to cheaper models: `haiku` for searches, lookups, renames and bulk edits, `sonnet` for routine multi-step edits. Keep analysis and judgment in the main session. Give each agent its own files so parallel agents don't collide.
