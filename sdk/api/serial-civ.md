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
  real dispatcher below, and — if a reply was produced — writes it back into the RX buffer at offset
  `0x66` (length at `0xca`), which the TX side reads to build an outgoing frame.
- **`civ_dispatch_lookup_validate`** (`0x2000b03c`, verified) — rejects any command byte `>= 0x2b`, then
  indexes **`g_civ_cmd_table`**: a pointer cell at `0x2000b250` (verified 2026-08-30 to hold the literal
  value `0x2018aa2c`) pointing at 43 entries × 8 bytes, one per command byte `0x00`-`0x2A`, each
  `{u8 handler_base_idx; u8 pad[3]; char *subcmd_list}`.
- **`civ_dispatch_invoke_handler`** (`0x2000acd8`) — permission-gates against a current-mode byte and the
  matched entry's flags, then calls the real handler through **`g_civ_handler_table`**: a pointer cell at
  `0x2000b234` (verified 2026-08-30 to hold `0x2018ab84`), 16 bytes/entry, function pointer at `+4`.
- Cross-checked entry-by-entry against the real manual (`/data/misc/icom/7300/doc/IC-7300_ENG_FM_12b.pdf`,
  pp. 19-2–19-13) — every unimplemented table slot matches a real manual gap.

## ✅ TX path

- **`FUN_20011384`** (`0x20011384`, verified, not yet renamed) reads the reply payload/length Icom's own
  handlers write at RX-buffer offset `0x66`/`0xca` and builds the outgoing frame.
- **`FUN_200110a8`** (`0x200110a8`, verified, not yet renamed) writes the two `0xFE` preamble bytes CI-V
  framing requires, matching the RX side's own two-byte preamble expectation exactly.

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

## 🔎 Open: coexistence with real CI-V traffic

SCIF0 is actively used for real CI-V traffic in normal operation. A custom app sharing it needs to either
queue politely alongside existing traffic (using `FUN_20011598` above, if it does serialize correctly) or
accept that a naive send might garble a legitimate CI-V session in progress. Not yet decided which failure
mode is acceptable for a first test.

## Also present, less explored for SDK purposes

- **SCIF1** — service/calibration link, a near-line-for-line structural duplicate of the SCIF0 driver
  (own `0xFE`/`0xFD` framing), reachable over the same USB `CP2102` bridge on a second channel. See
  `notes/kernel-rtos.md`.
- **SCIF3** — front-panel packet protocol (33-byte frames). See [`input.md`](input.md).
- **SCIF5** — DSP command/data link (also `0xFE`/`0xFD`-framed UART, not McASP as an early pin-mapping
  guess suggested). See `notes/multi-cpu-images.md`.
