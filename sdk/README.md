# `sdk/` — the forward-looking half of this project

`notes/` is the reverse-engineering record: what's actually true about the real, existing IC-7300
firmware, written as it's confirmed. It doesn't change once something is settled, only gets corrected
when a finding turns out to be wrong.

`sdk/` is different in kind, not just topic: it's the emerging **design and plan for building on top of**
what `notes/` has established — running custom code on the radio, and eventually a real SDK for writing
apps. It's forward-looking and expected to change as work progresses, will eventually hold real code
(headers, a toolchain config, example app sources) alongside markdown, and mixes confirmed facts (cited
back to `notes/`) with genuine design decisions and open questions that don't have a "true answer" to
discover the way an RE finding does.

Started 2026-08-30 as this distinction became worth keeping clean — see [[icom-custom-code-goal]] (memory)
for why this whole effort exists.

## Current contents

- **`roadmap.md`** — the overall goal, the key reframing insight (the firmware-update mechanism has no
  signature check, so a "flash once" custom-app loader is already feasible without needing a
  Bug), and the phased plan.
- **`app-requirements.md`** — for four representative example apps (serial hello-world, display
  hello-world, a simple game, an SSTV receiver), what's already known vs. what still needs researching,
  deliberately excluding the app-launching/memory-placement question (expected to get much easier once a
  live-device memory dump exists via JTAG).

## Expected growth

No fixed structure imposed up front — following this project's own established pattern in `notes/`
(files start flat, split into a lean "current state" file plus a `-history.md` narrative companion only
once they actually get long). As this grows, plausible additions:
- Per-subsystem API reference docs, once an interface is confirmed stable enough to call it part of the
  SDK surface (e.g. `serial.md`, `display.md`, `input.md`, `audio.md`) — promoted out of `notes/` findings
  once they're being relied on rather than just documented.
- `examples/` — eventually real source for the four apps in `app-requirements.md`, not just design notes.
- A toolchain/build doc once the cross-compiler and linker setup is worked out.

Keep new SDK-design material here, not in `notes/` — if something is a confirmed fact about the real
firmware, it belongs in `notes/` (and `sdk/` docs should cite it from there); if it's a plan, a design
choice, or an open question about what to build, it belongs here.

## Keep this in sync with ordinary RE work

`app-requirements.md` and `roadmap.md` each list open questions that ordinary `notes/`-side investigation
can answer incidentally, without anyone specifically going looking for them — e.g. confirming what buffer
`icon_blit_by_id_v1`/`_v2` writes into, decoding more `scif3_frame_dispatch_by_type` message types,
tracing `voice_recording_file_task`'s audio source back to its producer. **When a session's RE work
happens to resolve (or bear on) one of these, update the relevant `sdk/` doc's open-question status at
the same time as recording the finding in `notes/`** — don't leave it to a dedicated "check the SDK docs"
pass, since one may not happen for a long time otherwise.
