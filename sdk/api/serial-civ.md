# Serial I/O: CI-V (SCIF0)

**The most SDK-ready subsystem in the whole project** — both RX and TX are traced end-to-end, and the
physical link is reachable two ways without any new hardware research (see `app-requirements.md`'s App 1).

All addresses below verified against the live Ghidra project (`body.bin`) 2026-08-30.

## Physical layer

SCIF channel 0, register base `0xE8007000`, pins `P6_9`/`P6_10` (`CTXD`/`CRXD`). Confirmed at the hardware
level to fan out to **both** the physical `[REMOTE]` jack and a USB-side path through a `CP2102` USB-to-
UART bridge (`IC641`) — CI-V is available over either physical interface, same protocol
(`notes/ic7300-signal-chain.md`'s port-pinout table).

## ✅ RX path

- **`civ_frame_rx_statemachine`** (`0x2001099c`, verified) — the real CI-V frame parser. `0xFE` byte sets
  frame-sync state; at frame position 1, compares the byte against the radio's own configured CI-V address
  (with a broadcast-mode flag) and aborts on mismatch — genuine destination-address filtering; `0xFD` (with
  length ≥4) copies the completed frame body into a driver-owned buffer and sets a "frame ready" flag.
- Reached from `scif0_civ_rx_isr`, itself driven by the SCIF0 RX interrupt via the shared
  `register_event_handler` ISR-registration primitive.

## ✅ Application-level dispatch (the actual command processor)

- **`civ_rx_frame_stage_and_dispatch`** (`0x2000b258`) — polled from 3 sites piggybacked on unrelated
  periodic ticks (not a dedicated CI-V task). Copies the completed frame into a scratch buffer, calls the
  real dispatcher below, and — if a reply was produced — writes it back into the RX buffer (`rxbuf`,
  fixed address `0x20396ad4`) at offset `0x66`. **Correction, 2026-09-25**: `0xca` is **not** a length
  field — replies are `0xFD`-terminated, and `rxbuf[0xca]` is a plain "reply ready" flag (copied from a
  small fixed context block, `ctx` = `0x20390031`, `+3`). See `notes/kernel-rtos.md`'s "CI-V reply
  staging" section (2026-09-25) for the full byte-level build order, the `g_civ_handler_table` entry
  layout (three function pointers per entry, not one — set/read/read-precheck), and a verified cookbook
  for emitting one arbitrary frame the way a real handler does.
- **`civ_dispatch_lookup_validate`** (`0x2000b03c`, verified) — rejects any command byte `>= 0x2b`, then
  indexes **`g_civ_cmd_table`**: a pointer cell at `0x2000b250` (verified 2026-08-30 to hold the literal
  value `0x2018aa2c`) pointing at 43 entries × 8 bytes, one per command byte `0x00`-`0x2A`, each
  `{u8 handler_base_idx; u8 pad[3]; char *subcmd_list}`.
- **`civ_dispatch_invoke_handler`** (`0x2000acd8`) — permission-gates against a current-mode byte and the
  matched entry's flags, then calls the real handler through **`g_civ_handler_table`**: a pointer cell at
  `0x2000b234` (verified 2026-08-30 to hold `0x2018ab84`), 16 bytes/entry, function pointer at `+4`.
- Cross-checked entry-by-entry against the real manual (`docs/IC-7300_ENG_FM_12b.pdf`,
  pp. 19-2–19-13) — every unimplemented table slot matches a real manual gap.

## ✅ TX path

- **`civ_tx_pump`** (renamed 2026-09-25 from `FUN_20011384`, `0x20011384`) reads the reply Icom's own
  handlers stage at `rxbuf+0x66` (`0xFD`-terminated) gated on the `0xca` ready flag, copies it into the
  real TX buffer with the two `0xFE` preamble bytes prepended, and hands off to the echo-driven,
  collision-handling byte-send loop (`0x200110a8`/`FUN_20011598`/`FUN_200108b0`). Full detail and a
  verified cookbook for staging a reply exactly like a real handler: `notes/kernel-rtos.md`'s "CI-V reply
  staging" section (2026-09-25).

A "hello world" that emits text as a CI-V-framed message over the existing link needs **no new hardware
research** — every piece (SCIF0 driver init, TX framing, the physical REMOTE-jack/USB path) is already
confirmed. This is the standout first-app candidate.

## 🔎 Open: a standalone UART primitive, independent of CI-V's own framing

**`FUN_20011598`** (`0x20011598`, verified to exist, not yet renamed/analyzed) is seen called from multiple
SCIF0-adjacent sites — plausibly a generic "queue this byte for TX on whichever channel" helper. Its
exact register-level behavior (SCFTDR polling shape, whether it correctly serializes against concurrent
CI-V traffic) has **not been individually traced** — this function is located but not yet documented as
its own primitive. Needed before a new task could emit raw UART bytes without going through the CI-V
frame-state machine's own semantics.

## ✅ Resolved: coexistence with real CI-V traffic (2026-09-25)

Answered by the cookbook above rather than `FUN_20011598` (a raw single-byte UART primitive that turns
out to bypass real CI-V bus-collision handling — not the right layer to hook at all). Staging through
`rxbuf`/`drv` exactly like a real handler means a custom emitter politely defers to any real in-flight
traffic (its own pre-check refuses to stage a reply if the bus/reply-buffer is busy) rather than garbling
it. The remaining real risk is a **cross-task race** if the emitting code runs in a different task context
than `civ_tx_pump` (which runs from `main_idle_loop`) — see `sdk/app-loader-design.md` for the current
plan (running the app in that same context to avoid it entirely, rather than only relying on lock/retry).

## Also present, less explored for SDK purposes

- **SCIF1** — service/calibration link, a near-line-for-line structural duplicate of the SCIF0 driver
  (own `0xFE`/`0xFD` framing), reachable over the same USB `CP2102` bridge on a second channel. See
  `notes/kernel-rtos.md`.
- **SCIF3** — front-panel packet protocol (33-byte frames). See [`input.md`](input.md).
- **SCIF5** — DSP command/data link (also `0xFE`/`0xFD`-framed UART, not McASP as an early pin-mapping
  guess suggested). See `notes/multi-cpu-images.md`.
