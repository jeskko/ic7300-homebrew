# Main-CPU kernel/RTOS

See [notes/kernel-rtos-history.md](kernel-rtos-history.md) for the full session-by-session narrative and evidence trail.

Traced from `body.bin` (rebased to `0x20005000`, see [[base-loader]]),
starting from `kernel_start` (`0x20005290`, called from
`reset_handler`) through `swi_handler`, the SWI/SVC exception vector.

## Confirmed: a real, ITRON-shaped preemptive kernel

- **Call convention**: kernel calls check the caller's current ARM mode
  (`get_cpsr_mode`, `0x20184bb8` — `mrs r0,cpsr; and r0,r0,#0x1f`). If in
  User mode (`0x10`), trap via `software_interrupt(0)`; if already in a
  privileged mode, call the kernel function directly — skips trap overhead
  for kernel-to-kernel calls. Every traced wrapper (`itron_act_tsk` and
  its siblings) follows this exact pattern.
- **SWI(0) is the general kernel-call trap.** A separate SWI-immediate
  indexed table exists (base+bound both resolve through
  `0x20184850`, `*(uint*)0x20184850 == 1`) but has only **one** real
  entry (`0x200b940c`) — a narrow special case, not the main mechanism.
  Almost everything goes through SWI #0, which internally decides the
  actual operation some other way (not yet pinned down — likely a
  function-code value passed in a register/stack slot before the trap).
- **`swi_handler` (`0x20005050`+8*4... entry at `0x200056dc`,
  ARM-mode — see below) implements a full context switch**, not just a
  syscall dispatch:
  - Compares a pointer pair at `0x20005954` (current vs. next task
    context — a scheduler ready-queue pointer pair)
  - `coproc_moveto_Context_ID(...)` — writes ARM CP15's Context ID
    (ASID) register — **confirms per-task memory-context tagging**, i.e.
    genuine preemptive multitasking with MMU-level task isolation, not a
    cooperative/single-context loop
  - Reads/masks/writes `Coprocessor_Access_Control` around the VFP/NEON
    enable bits (`0xf00000` = CP10/CP11) — **lazy FPU context save**:
    only saves VFP state if the outgoing task actually used it
  - Ends with an indirect call through the new task's saved context —
    the actual switch-to-next-task jump
- Confirms exception vectors execute in ARM state even where Thumb code
  exists elsewhere in the binary (architectural guarantee, not
  implementation choice) — relevant if more vector-adjacent code needs
  the same ARM-vs-Thumb disassembly-context fix `reset_handler` and
  `swi_handler` both needed (see [[base-loader]]).

## RTOS identity: CONFIRMED — FreeRTOS, Renesas RZ/A1H port, with real customizations

Resolved by direct comparison against
`scratch/r01an5093ej0170-rza1-swpkg/` (Renesas application note
R01AN5093, "RZ/A1H Software Package"), which bundles a full FreeRTOS
port at `gcc/RZA1H_SoftwarePackage/RZA1H_Sample/src/freertos/portable/gcc/arm_ca9_rza1h/`
(`portasm.S`, `port.c`, `portmacro.h`). Multiple independent, exact
matches against our decompiled `swi_handler`:

- **GIC register addresses**: the port's `INTC_ICCIAR_ADDR = 0xE820200C`
  / `INTC_ICCEOIR_ADDR = 0xE8202010` fall exactly inside the
  `0xE8200000`–`0xE822FFFF` I/O region already in [[memory-map]].
- **Lazy FPU/VFP save**: the port's `ulPortTaskHasFPUContext`-gated
  `VPUSH{D0-D15}`/`VPUSH{D16-D31}`/`FMRXNE FPSCR` sequence matches our
  handler's `Coprocessor_Access_Control`-gated VFP save exactly.
- **Overall shape**: `portSAVE_CONTEXT` → `vTaskSwitchContext()` →
  `portRESTORE_CONTEXT` matches our handler's structure (compare
  current-vs-next TCB pointer, then jump into the new task).
- **`portAPSR_USER_MODE == 0x10`** in `port.c` is the exact constant
  `get_cpsr_mode`'s callers compare against.

**Icom customization identified, not stock behavior:** in the reference
port, the `0x10`/User-mode check happens exactly once, as a startup
assertion in `xPortStartScheduler()` (`configASSERT(ulAPSR !=
portAPSR_USER_MODE)`) — ordinary FreeRTOS API calls (`xQueueSend`,
`vTaskDelay`, etc.) are plain C function calls, no trap involved. Our
firmware instead repeats the "check mode, `SWI(0)` if User /
call-direct if privileged" pattern across multiple distinct wrapper
functions (`itron_act_tsk` and its siblings — misleading names now,
kept for continuity, should probably be renamed once the real FreeRTOS
task-API correspondence is confirmed). Combined with the
`coproc_moveto_Context_ID` (ASID) tagging found in `swi_handler`, which
the stock port also doesn't do: **Icom appears to have extended this
base port with real unprivileged user-mode task support and per-task
MMU memory-context isolation** — a genuine, non-trivial modification
beyond the Renesas reference sample, not something to assume away.

**Version caveat — the bundled package is newer than what Icom likely
used.** `scratch/r01an5093ej0170-rza1-swpkg/` (from
https://www.renesas.com/en/software-tool/rza1h-freertos-software-package)
bundles **FreeRTOS Kernel v10.6.1** (`manifest.yml`/`History.txt`,
released August 2023). The IC-7300 first shipped ~2016, roughly 7 years
earlier — likely built against FreeRTOS ~8.x/9.x for this port, not
v10.6.x. Concretely: `History.txt` shows v10.6.0 (July 2023) introduced a
"new MPU wrapper" with opaque kernel-object handles, **task context saved
in the TCB instead of on the task's own stack**, and syscalls running on
a **separate privileged-only stack**. Our `swi_handler` does none of
that — it pushes context onto the *current* stack, the older/classic
`portSAVE_CONTEXT` style — which is actually corroborating evidence
Icom's build predates that overhaul, not a reason to doubt the match.
**Trust the stable, hardware-fixed and long-unchanged parts of this
comparison (GIC addresses, overall save/switch/restore shape, VFP lazy
save) — don't assume exact struct layouts/handle representations match
without finding an older, contemporaneous package.**

Now that the base is identified, `scratch/r01an5093ej0170-rza1-swpkg/`'s
`tasks.c`/`queue.c`/`semphr.h`/etc. give real FreeRTOS symbol names and
struct layouts to match against — much faster than guessing from
scratch. Worth re-deriving `PTR_DAT_20005954`'s exact role (currently
read as a pointer *pair*, whereas stock `pxCurrentTCB` is a single
pointer — possibly Icom added a small before/after snapshot struct
around the `vTaskSwitchContext()` call) before renaming further kernel
globals, rather than assuming a 1:1 stock layout.

## Retracted: RTOS not found by name via string search

An early whole-`body.bin` string search for `TOPPERS`/`ITRON`/`T-Kernel`/`TRON`/`eSOL` found zero hits (all
`Copyright`/`kernel` string hits were unrelated font-library false positives) — superseded once the direct
FreeRTOS source comparison above confirmed the RTOS identity. The one reusable finding from that search:
`"RENESAS RZ/A1 SD Driver Ver4.01"` at `0x201812e0`, the versioned SD-card driver behind the SD-card update
flow ([[firmware-update]]). Full search log and detail in the history file.

## Open questions
1. ~~How SWI(0) actually selects which kernel operation to run~~ —
   resolved: it isn't a syscall-number dispatch at all, `SWI(0)` is
   FreeRTOS's yield/reschedule trap. The `itron_act_tsk`-named wrappers
   are Icom's own "elevate to privileged mode, call this FreeRTOS API
   function directly" trampolines, not per-syscall dispatch — rename
   candidates once individual wrapped APIs are identified.
2. ~~Whether the Renesas RZ/A1 BSP/FIT package reveals the bundled
   RTOS~~ — resolved, see above: FreeRTOS, confirmed by direct source
   comparison.
3. ~~No static task list recoverable~~ — **partially resolved, 23rd session**: a static list of task
   *activation call sites* (not TCBs) IS recoverable, via `itron_act_tsk`'s 11 direct call sites (see the
   "Living reference: full boot-time task catalog" section below) — this is a different, more useful list
   than the originally-envisioned TCB search, though the *TCBs themselves* remain runtime-populated as
   originally found (`0x203907c4` still blank/`0xff` in the static image).
4. New: confirm `PTR_DAT_20005954`'s exact role/layout against
   `pxCurrentTCB`'s real usage in `tasks.c` before renaming further
   kernel globals — see "RTOS identity" section above.
5. New: `itron_act_tsk` and siblings are misleadingly named now that
   ITRON isn't the right frame — worth renaming once each one's wrapped
   FreeRTOS API is identified by comparing call-site argument shapes
   against `task.h`/`queue.h`/`semphr.h` signatures.

*(Later sessions accumulated more open items than are listed here — `kernel_start`'s own mystery task,
`thunk_FUN_2007ea68`'s peripheral identity, the SCIF1 turnaround/service-mode triggers, etc. See the
"Session handoff" sections in the history file for the fuller, more current running list.)*

## CI-V command dispatcher — found, and one genuine undocumented command (2026-08-30)

The long-open "real CI-V command dispatcher consumer" item above is resolved. Full derivation in the
history file's "The real CI-V command dispatcher found, and a genuine undocumented command (0x2A)
confirmed" section — summary:

- **`civ_rx_frame_stage_and_dispatch`** (`0x2000b258`) → **`civ_dispatch_lookup_validate`** (`0x2000b03c`,
  indexes **`g_civ_cmd_table`**, base `0x2018aa2c`, 43 entries covering wire command bytes `0x00`-`0x2A`,
  each `{handler_base_idx; subcmd_list ptr}`) → **`civ_dispatch_invoke_handler`** (`0x2000acd8`, permission-
  gates against **`g_civ_handler_table`**, base `0x2018ab84`, 16 bytes/entry, function pointer at `+4`) →
  the real per-command handler.
- Cross-checked entry-by-entry against the real manual (`/data/misc/icom/7300/doc/IC-7300_ENG_FM_12b.pdf`,
  pages 19-2 to 19-13): every unimplemented table slot (`0x0C`/`0x0D`/`0x12`/`0x1D`/`0x1F`/`0x20`/`0x22`/
  `0x23`/`0x24`/`0x29`) matches a real gap in the manual's own command list — strong confirmation this
  table really is CI-V's (unlike the retracted `sdcard_file_rpc_dispatch_task` false lead below).
- **Command `0x2A`, subcommand `0x01` is real, fully implemented, and completely absent from the manual**
  (whose table ends at `28 00`) — **the undocumented CI-V command**. Cross-checked against `wfview`
  (open-source multi-model CI-V control suite, github.com/wf-group/wfview): **zero knowledge of `0x2A`
  across all ~40 of its supported Icom models**, not just the IC-7300 — no third-party implementation
  anywhere in that project's rig database has ever encountered it either. Handler:
  **`civ_cmd_2a_handler_UNDOCUMENTED`** (`0x20010710`).
- **Continued tracing (same day) — converges on the antenna tuner engage hardware.** The frequency
  ceiling is `60,000,000` (60 MHz) exactly. Far more significantly: the handler's data byte `2`
  ("engage") reaches **`tuner_engage_gpio_toggle(1)`** (renamed `FUN_2001e720`, bit-twiddles
  `DAT_2001f50c`/`DAT_2001f510` in tandem) — the *identical* call the real, documented `1C 01` tuner
  command's own "start tuning" path (`civ_cmd_1c01_tuner_handler` → `civ_cmd_1c01_tuner_dispatch` →
  `tuner_start_tuning_sequence`) makes, and data byte `1` ("arm") shares `tuner_freq_and_txstate_precheck`
  with that same documented path. Reads as an **alternate/bypass trigger for the tuner engage hardware**,
  independently coded with lighter gating (skips the documented path's TX-state/split/mode checks) —
  plausibly a factory/production-test shortcut, not confirmed as literally "the tuner command" since its
  own state machine (`g_civ_2a_state`) is fully independent.
- **Correction, same day**: `DAT_2001f50c`/`DAT_2001f510` are **not** a GPIO shadow as first guessed —
  they're pointers to `0x203902d4`/`0x203902d6`, a generic shared status/interlock flags byte pair with
  60+ unrelated call sites across the firmware (checked via `references_to`), not tuner-specific state.
  The user supplied a real schematic/parts-list map of the tuner's relay network (4× `BU2092FV-E2`
  serial-in/parallel-out relay drivers, shared `TDAT`/`TCLK`/`TOE`, individually latched by `TSTB1`-`4`
  — full relay map now in `notes/ic7300-hardware.md`), but the raw P7 port data register
  (`0xFCFE301C`) that would carry those signals has exactly one reference in the whole image — the
  generic one-time boot GPIO init, same signature as the already-solved `DRESD`/`P2_6` case — so the
  real runtime relay-shift-out code goes through some other, not-yet-found indirection.
  **Exact bit assignments confirmed off the schematic, 2026-09-09** (previously only known as
  "shared `TDAT`/`TCLK`/`TOE`, individually latched by `TSTB1`-`4`" with no bit numbers): `TSTB1`=`P7_1`,
  `TSTB2`=`P7_2`, `TSTB3`=`P7_5`, `TSTB4`=`P7_6`, `TCLK`=`P7_3`, `TDAT`=`P7_4`; also on the same port,
  `TOE`=`P7_7`, and both `PHASEI`/`IMPI` read as `P7_8` (unconfirmed whether that's a genuine shared/
  muxed pin or a transcription slip — don't treat as settled either way).

  **Found, same day, using those exact bit numbers as a targeted search** — the standing "some other,
  not-yet-found indirection" is resolved. `TCLK`/`TDAT` are **not** bit-banged GPIO at all —
  `tuner_relay_serial_bus_init` (renamed from `FUN_200b3a30`) configures `PFC7`/`PFCE7`/`PFCAE7`/`PMC7`
  to put `P7_3`/`P7_4` into real peripheral (alt-function) mode, routing them to a small on-chip
  serial-shift peripheral at `0xE800A800`-`0xE800A814` (register shape matches the confirmed RSPI2
  poll-status-then-write-data pattern, but at different addresses — **not yet identified against the
  RZ/A1H manual**, a real open item if the exact shift-clock timing ever matters). This fully explains
  why no `TCLK`/`TDAT` bit-bang loop on raw `P7` was ever found — it was never going to be GPIO.

  `TSTB1`-`4`, however, genuinely **are** GPIO, driven by `tuner_relay_tstb_strobe_dispatch` (renamed
  from `FUN_200b391c`, registered as event `0xa0` off the same generic per-tick dispatcher already
  confirmed servicing RSPI2/SSIF/front-panel) via masked writes to `PSR7` (`0xFCFE311C`, not the plain
  `P7` data register — this is why the earlier `0xFCFE301C`-only search came back empty, the exact same
  "check the *actual* register used, not just the data register" lesson this project has hit before).
  Picks one of 4 pending "dirty" bits (one per relay-driver chip) and pulses it via a 2-phase write pair
  (the "hi" half then, after a ~35-iteration software delay, the "lo" half) from a 32-byte table labeled
  `g_tuner_tstb_pulse_table` (`0x20335F98`, lo half at the base, hi half at `+0x10`) — **the touched bit
  for index 0/1/2/3 is bit 1/2/5/6, an exact, independent match to the schematic's `TSTB1`/`TSTB2`/
  `TSTB3`/`TSTB4` = `P7_1`/`P7_2`/`P7_5`/`P7_6`**, real code confirming the real pin reading with zero
  ambiguity in the bit selection itself (the two 16-bit halves' precise set-vs-clear polarity within
  `PSR7` carries the same "structurally motivated, not independently confirmed" caveat `gpio.c`'s own
  docstring already flags for this register family generally). **Renamed and PLATE-commented in Ghidra,
  2026-09-09** (both functions plus the table) — high-confidence findings only; the still-open items
  below were deliberately left as their auto-generated `FUN_*` names.

  Surrounding cluster, also found (not yet renamed — their own roles read clearly from direct decompile,
  but weren't independently cross-checked against real hardware the way the two renamed functions were):
  `FUN_200b3c5c` (cold-boot driver init, called from the same init-sequence block as the front-panel/
  SD-card driver inits) calls `tuner_relay_serial_bus_init`, sets all 4 chips' target state to `0x555`,
  asserts `TOE` active-low via `PSR7`, waits 10ms, then calls `FUN_200b3bf4` (default/all-off relay
  pattern). `FUN_200b3d34` (the periodic per-tick state machine, serviced by the same generic dispatcher
  as `tuner_relay_tstb_strobe_dispatch`) builds a 2-bit-per-output pattern from two 24-byte target
  arrays — sizes matching `notes/ic7300-hardware.md`'s own `RL20xx`/`RL21xx` relay table almost exactly
  (4 chips × 6 outputs × 2 bits) — and hands it to `FUN_200b3718`/`FUN_200b37dc`, which marks the
  per-chip dirty bits `tuner_relay_tstb_strobe_dispatch` consumes.

  **Not yet proven, medium confidence**: the concrete end-to-end link from `tuner_engage_gpio_toggle`/
  `civ_cmd_1c01_tuner_handler`/CI-V `0x2A` down to this specific cluster — plausible (same general RAM
  region, same idle-tick-dispatcher servicing pattern already confirmed for other tuner-adjacent code)
  but no direct pointer/call chain traced yet connecting them. `PHASEI`/`IMPI` (`P7_8`) ambiguity also
  still open — `port_bulk_gpio_init_pass1` does configure that bit as *input*, at least consistent with
  either/both being a CPU-read sense signal rather than an output, but no specific `PPR7` bit-8 read
  found yet to confirm which.
- **First real hardware-pin-level confirmation, same day**: following up the user's `EKEY`/`PHASEI`/
  `IMPI`/`SWRL`/`TPWRL` hint found **`tuner_jack_poll_and_autotrigger`** (renamed `FUN_2006672c`, runs
  every idle-loop tick) and **`tuner_jack_signal_precheck`** (renamed `FUN_20066154`) — both read the
  `[TUNER]` jack's `TCON`/`EKEY` pins (`P0_4`/`P6_2`) directly via the real port-pin-read register
  (`g_ppr_register_base`, `0xFCFE3200` family), the first genuine GPIO-level tie of this code cluster to
  a real, schematic-named signal (see `notes/ic7300-signal-chain.md`'s `P0_4`/`P6_2` rows). On a
  successful `EKEY` + frequency/TX-state check, `tuner_jack_poll_and_autotrigger` calls
  **`tuner_engage_from_jack_trigger`** → `tuner_engage_gpio_toggle` — a **third independent trigger path**
  into the same primitive, alongside documented CI-V (`1C 01`) and undocumented CI-V (`0x2A`).
  **Open refinement**: `TCON`/`EKEY`/`ESTA` are the *external* tuner-accessory jack's own signals — not
  confirmed to be the *internal* relay network's own `TSTB`/`TCLK`/`TDAT` bus, so "coordinates with the
  tuner subsystem" and "physically drives the internal relay network" remain two separately-unconfirmed
  claims. The `SWRL`/`TPWRL` hint didn't pan out on this function's own threshold locals (traced to
  generic shared state, not power/SWR samples) — best remaining lead is checking `0x2001f168` (the
  paired tuner housekeeping function) for a `PPR1` read, not yet done. **`PPR1` clarified, 2026-09-09**:
  not a distinct schematic-named signal (the user checked and couldn't find one under that name) —
  it's this project's own already-established register-naming shorthand for "the Port Pin Read
  register, Port 1" (`0xFCFE3204`, already used this way in `notes/firmware-update.md`'s own
  `P1_6`/`PDV` trace), which reads all of Port 1's pins at once. `SWRL`=`P1_15`/`TPWRL`=`P1_14` (both
  already in `notes/ic7300-signal-chain.md`'s own P1 pin table) are just two bits *of* that same
  register, not separate registers of their own — so the still-open lead is unchanged in substance
  (does `0x2001f168` read `0xFCFE3204` at all, and if so do bits 14/15 matter to it), just no longer
  confusing to phrase. See the history file for full derivation.

## Living reference: full boot-time task catalog

Every `itron_act_tsk` call site (11 total, exhaustively cross-checked 3 independent ways in the 21st
session) plus the 2 known non-trampoline/dynamic activations, each with its task descriptor
(`{entry_point, priority, flags, stack_size}`, 16 bytes) read directly from RAM. This table was first
assembled in the 23rd session and is refreshed here from the corrections made through the 29th/30th and
later sessions (the `sdcard_file_rpc_dispatch_task` CI-V retraction, the `thunk_FUN_2007ea68` USB-guess
retraction, etc.) — see the history file for the full derivation and narrative behind every row.

| Caller | Descriptor | Entry point (current name) | Priority | Stack | Status |
|---|---|---|---|---|---|
| `FUN_20188574` (kernel bootstrap, direct inner-function call, **not** a trampoline call — see history's 24th-session correction) | `0x2033605c` | `first_task_entry` (`0x201871f0`) | 2 | 0x320 | 🟡 **deepened, 2026-08-30, but genuinely kernel-internal — not a nameable feature the way most other tasks were**: its two callees (`resolve_and_invoke_msg_callback`, `ready_list_requeue_by_priority`) are real FreeRTOS/ITRON-shaped scheduler bookkeeping (priority-indexed ready-list manipulation), not application-level callback dispatch — plausibly analogous to FreeRTOS's own Timer/Daemon service task, not confirmed as literally that. **Corrected** a specific claim from the 24th session along the way: `resolve_and_invoke_msg_callback` only ever returns small status codes, never a function pointer, so it can't be what the loop's `SWI(0)` trampoline resolves to — what that trampoline actually invokes remains genuinely unresolved (same standing "how does `SWI(0)` dispatch" open question below), and the old "`FUN_2002b29c` is reached through this mechanism" hypothesis is weaker now, not confirmed |
| `kernel_start` (`0x200052a4`) | `0x203907c4` — **still genuinely unidentified**, still blank/`0xFF` in the static image | ? | ? | ? | ❌ open — real task, unknown identity, needs live JTAG |
| `FUN_200096c8` (`0x200096f0`) | `0x201988ec` | `spectrum_scope_fft_task` (renamed from `status_poll_task_200095d8`) | **-2** | 0x400 | ✅ **fully resolved** (2026-08-30) — the real-time band-scope/spectrum-scope FFT computation task: double-buffered against a sample producer, runs a genuine 512-point FFT (`spectrum_scope_fft_and_dbscale`) then converts per-bin magnitude to a dB-scaled byte (0-255) against a per-mode threshold pair — the actual "bar height" data behind the scope display, complementing [[band-scope-state]]'s already-documented frequency-axis/mapping logic |
| `FUN_2001439c` (`0x200143b8`) | `0x201988fc` | `rtty_decode_log_poll_task` (renamed from `periodic_poll_task_20014384`) | 0 | 0x800 | ✅ **fully resolved** (2026-08-30) — drives the RTTY digital-mode decode-to-SD-card logging feature every 5 ticks: writes the decoded text to `C:\IC-7300\Decode\Rtty` as either a `.txt` or `.htm` file (format selected by a mode byte). Previously marked "confirmed" on body-shape alone despite its generic name — a real gap in the earlier triage, since the loop's own sample/store call was never actually chased. **Traced one level deeper, same day**: the log writer is a generic file-I/O client (same `0x2003bXXX` cluster as `bmp_capture_write_file`) over a shared struct (`0x2039bfc4`) whose `+0x2d` "pending record" field gets promoted from a `+0x18` "staging" field under an IRQ-disabled critical section — but **who writes the staging field (the actual decoder) wasn't found**; confirmed this cluster never references the `SCIF5` DSP-link register base directly, so the real producer is further upstream, same open shape as `voice_recording_file_task`'s own unresolved audio source. A candidate larger dispatcher function was flagged and then un-flagged as the known ARM/Thumb bug (2026-08-31 correction — the bookmarks cited as evidence were stale, verified against live disassembly). **Same day, follow-up**: decompiling the functions uncovered while fixing 8 genuinely-broken bookmarks (separate from the stale ones) found real, likely-related infrastructure — a filename-collision-avoidance helper (`FUN_200ba490`, real `~N` short-filename numeric-tail scan wrapping a `strtol`-shaped parser) and a pair of directory-entry filters (`FUN_200ba82c` checks `"TXT"`/`"HTM"` — RTTY's own two log formats; `FUN_200ba938` skips `.`/`..` and date-parses `"20"`-prefixed real entries) — plausibly the actual SD-card "list existing decode logs, generate a unique RTC-date-stamped filename" mechanism behind this task's own file writes. **2026-09-07**: the containing dispatcher now has a real boundary (`0x200bb1cc`, gained between sessions, still under its stale `civ_table_id08` name) — decompiled in full, it's a generic SD-card content-browser (directory-scan loop dispatching on a content-format byte 0-9, RTTY = format 4) reached only through an unidentified 16-entry function-pointer table at `0x20336090`; its own copies are all fixed-size and properly bounded against the 8.3-short-filename-only constraint, **no overflow found here**. Real progress on the actual decoder question came from a different angle instead: found `operating_mode_change_dispatch` (`0x2005807c`) and its per-mode "on enter this mode" function-pointer table (`0x2019ac0c`) — initially flagged as the best lead in the project, **retracted same day**: once the user applied the manual ARM/Thumb fix and the RTTY/CW-R-shared entry (`0x20056fd4`) actually decompiled, it turned out to be a single-line flag clear, not demod-arming code. The whole table is now confirmed to be lightweight UI/interlock bookkeeping (mostly no-ops), closing it as a lead. Two CPU-side approaches (file-write path, mode-change path) have now dead-ended on "who produces the decoded characters" — working theory is the DSP demodulates continuously and the boundary is DSP-internal, unreachable from `body.bin`. Full trace in `notes/kernel-rtos-history.md`'s "Tracing the RTTY decoder", "Following up on the bookmark-cleanup fixes", and "Picking the RTTY/SSTV thread back up" (+ same-day correction) sections |
| `FUN_2001627c` (`0x2001631c`) | `0x20016800` | `voice_recording_file_task` (renamed from `queue_driven_task_2001745c`) | 1 | 0x2000 (largest stack in the catalog, now explained) | ✅ **fully resolved** — manages recording audio to the SD card's `C:\IC-7300\Voice` folder: builds an RTC-date-stamped filename, runs a 4-state open/process/close file-I/O state machine over a 4-slot ring buffer, and is a confirmed real client of the already-known SD-card file-RPC service (`file_rpc_post_command`, commands `6`/`9`) also used by `sdcard_file_rpc_dispatch_task`. **Correction**: originally guessed here to be the file-I/O half of the `audio_buffer_task` (now `voice_tx_memory_*`) pair below — that pair turned out to read from a *different* folder (`C:\IC-7300\VoiceTx`, TX voice-message playback, vs. this task's own `C:\IC-7300\Voice`) — sibling features sharing the same file-RPC plumbing, not two halves of one feature |
| `FUN_20027740` (`0x200277f0`) | `0x2002784c` | `sd_menu_dispatch_task` | 0 | 0x1800 | ✅ **fully resolved** — the SD-card operations menu's central 42-case dispatcher; case `0xb` calls `firmware_update_main` directly, confirming the update flow starts from routine SD-menu interaction, nothing more exotic |
| `cold_boot_hw_init` (`0x2002b02c`, renamed from `FUN_2002afc0`) | `0x2019889c` | `ui_graphics_lifecycle_task` (renamed from `FUN_2007ef5c`) | — | — | ✅ **fully resolved** — the master graphics lifecycle task: it's the one that calls `graphics_stack_startup_egl_openvg` (identifying which task actually starts the whole EGL+OpenVG/SLV5 subsystem, see the `thunk_FUN_2007ea68` row below), creates the real 480×272 on-screen EGL window surface (the IC-7300's actual touchscreen resolution) plus a 960×552 off-screen EGL pixmap surface (exactly 2× the window, purpose — supersampled render target vs. tiled canvas — not confirmed), then runs a 2-state init/present-frame loop with a clean EGL surface/context teardown on exit |
| `FUN_2006c4a8` (`0x2006c584`) | `0x201988cc` | `voice_tx_memory_control_task` (renamed from `audio_buffer_task_2006bb58`) | 0 | 0x800 | ✅ **fully resolved** — control/lookup side of the TX Voice Memory (pre-recorded voice message) playback feature: lists `C:\IC-7300\VoiceTx` via the SD-card file-RPC service, resolves a numeric message-slot to its file via a sorted-table binary search |
| `FUN_2006c4a8` (`0x2006c594`, **same caller as above — spawns 2 tasks together**) | `0x201988dc` | `voice_tx_memory_stream_task` (renamed from `audio_buffer_task_2006c2c4`) | 0 | 0x800 | ✅ **fully resolved** — file-read/streaming side of the same feature, shares control struct `0x2006c3d4` with its sibling above; reads file blocks via the same file-RPC service (command `0x13`, read-at-offset) — the "ring-buffer wraparound" shape noted earlier turned out to be buffered file-read position tracking, not raw PCM/hardware ring-buffer access |
| `thunk_FUN_2007ea68` (`0x2007ea84`) | *dynamic, caller-supplied* | *unresolved* | — | — | 🟡 **mechanism traced, peripheral base confirmed as a direct unwritten literal, identity hypothesis upgraded** — activated on a connect/disconnect-shaped event touching an unidentified peripheral at `0xe8100000` (RZ/A1H bus-matrix slave SLV5). 2026-08-29: verified `UNIDENTIFIED_SLV5_PERIPH_BASE` has no hidden indirection (single unwritten literal-pool constant, confirmed via a write-reference check — not a mutable pointer cell); separately found `slv5_periph_connect_disconnect_handler` is called as the `"vgStartUp"` step of a real, string-confirmed EGL+OpenVG graphics-stack bring-up (`graphics_stack_startup_egl_openvg`, `0x20079240`) — new working hypothesis: a 2D/vector-graphics rendering resource, not the earlier retracted "USB subsystem" guess. 2026-08-30: identified `graphics_stack_startup_egl_openvg`'s own caller as `ui_graphics_lifecycle_task` (the table row above) — this whole subsystem starts as that task's very first action. **Same session, separately**: traced the native platform's real window/display-attach code (`native_show_window_multi_display`) and confirmed it genuinely supports attaching a window to multiple simultaneous displays via a runtime bitmask — real architectural evidence for planned multi-display support, independent of and stronger than the earlier `VDC50`/`VDC51` argument — but the mask itself is a runtime-only value (blank in the static image, same signature as `kernel_start`'s own mystery task), so whether more than one display is ever actually active needs live hardware, not more static reading. Also confirmed the 960×552 pixmap surface specifically is plain memory with no path to this display-attach mechanism at all. See history's final session for the full trace. |
| `FUN_200aa5d4` (`0x200aa5c8`) | `0x2019890c` | `bmp_capture_task` | **-1** | 0x1000 | ✅ **fully resolved** — the BMP screen-capture-to-SD-card feature (real `BITMAPFILEHEADER`/`BITMAPINFOHEADER` construction). ~~🔎 New lead, 2026-08-31: `"DAT"`/`"BMP"`/`"PNG"` tag table at `0x200ba820`-`2b`, plausibly a capture-format selector for this feature~~ — **retracted, same day, after actually decompiling the neighboring functions**: they check filenames against `"TXT"`/`"HTM"` and `"."`/`".."` instead, nothing to do with `bmp_capture_task` at all — see `notes/kernel-rtos-history.md`'s "Following up on the bookmark-cleanup fixes" section for what that cluster actually is (a shared SD-card directory-listing/filename-dedup helper, more likely tied to `rtty_decode_log_poll_task`) |
| `FUN_200b995c` (`0x200b999c`) | `0x201988ac` | `sdcard_file_rpc_dispatch_task` (renamed **twice**: `task_probe_200b9c00` → `civ_command_dispatch_task` (wrong guess) → `sdcard_file_rpc_dispatch_task`, see history's "civ_command_dispatch_task retraction" section) | 1 (highest priority in the catalog) | 0x1800 | ✅ **fully resolved, then corrected** — **not** CI-V; a generic internal SD-card file-access RPC service (open/read/write/close/rename/list), dispatched through a function-pointer table by command ID. **Independently corroborated dynamically, 2026-09-08**: forcing a call into `firmware_update_main` over GDB (`qemu-machine/`'s QEMU port) traced real execution into exactly this task's own posting mechanism (`file_rpc_post_command`) — confirms this static identification from the running-code side too. Whether this task itself ever actually gets scheduled to consume that posted command, against a hand-hijacked (not properly kernel-created) calling context, is the open question that dynamic-testing thread left off on — see `qemu-machine/README.md`'s "Forcing `firmware_update_main` directly" section |
| *(genuine static-analysis dead end — see status)* | `0x20361318` | `sys_monitor_task_entry` (`0x200b94e8`) | 3 | 0x800 | ✅ **fully resolved** (2026-08-30) — registers two event handlers now confirmed to be **full FreeRTOS context-switches** (`irq_context_switch_id0`/`id86`, structurally identical to `swi_handler`'s own scheduler logic — a hardware-IRQ-triggered entry point into the same mechanism `SWI(0)` provides for software-triggered reschedules), then executes a one-time `SWI(1)` into `enable_mmu_caches_branch_predictor` (invalidates TLB/I-cache/D-cache/branch-predictor, then enables all three via `SCTLR` — the boot-time-to-running cache/MMU transition), then tail-calls `sys_monitor_task_loop` (`select_active_slot_resources` + `FUN_2002b29c`, forever). Resolving these three callees needed the project's known ARM/Thumb disassembly-context bug fixed by the user at all three addresses. **What remains open**: the `itron_act_tsk` call site activating this task (descriptor `0x20361318`) — re-confirmed via a fresh literal-byte search as a genuine, permanent static-analysis dead end, the same "runtime-populated, needs live JTAG" signature as `kernel_start`'s own mystery task |

**Bottom line, as of the latest session that touched this catalog**: every task in the table has a real,
disassembly-confirmed body (no task still shows garbage/unexamined code). Exactly two identities remain
genuinely open — `kernel_start`'s own mystery task (`0x203907c4`, a thoroughly reconfirmed static-analysis
dead end: no writer anywhere in the compiled image, needs live JTAG) and `thunk_FUN_2007ea68`'s dynamically
activated task (mechanism fully traced; the peripheral it serves is not yet identified — confirmed **not**
USB, and now has a well-evidenced but not register-level-confirmed new hypothesis: a 2D/vector-graphics
rendering resource, see the table row above; a follow-up "external monitor" theory, prompted by a sibling
model — IC-7610 — reportedly having a DVI output via a separate sub-CPU, was tested and only partially
supported: the actual "connect" gate traced to a plain software resource-allocation check, not a hardware/
GPIO detect, so this is more likely the shared graphics middleware's generic resource-acquire terminology
running identically on every product that uses it, not evidence of an IC-7300-specific external connector;
see history for the full trace). **Correction (2026-08-30)**: this bottom line previously claimed the
triage was complete at this point — wrong. `rtty_decode_log_poll_task` (renamed from
`periodic_poll_task_20014384`) had been marked "confirmed" on body-shape alone, but its actual
sample/store call was never chased, and its still-generic name should have flagged it as an
oversight. It's now resolved too (see its table row above) — every task in the catalog genuinely
has a fully-characterized body now. **One more nuance, same day**: `first_task_entry` was looked at
again too, at the user's prompt — its "generic message-dispatch loop" description undersold real
depth underneath (genuine FreeRTOS/ITRON-shaped scheduler-internal bookkeeping, not an application
feature), but that depth doesn't resolve into a nameable *purpose* the way the other tasks did, and
along the way corrected a specific wrong claim from the 24th session (see its table row above). So
the honest final count: every task has a resolved, characterized body; all but three have a clean,
nameable real-world purpose; `kernel_start`'s own descriptor and `thunk_FUN_2007ea68`'s peripheral
identity remain genuine static-analysis dead ends needing live JTAG; `first_task_entry` is
genuinely kernel-internal machinery rather than an unresolved mystery — a different, third category,
not a loose end.

## Living reference: `cold_boot_hw_init`'s own call-by-call sweep (2026-09-20)

`cold_boot_hw_init` (the task-catalog table above already covers the one task it activates,
`ui_graphics_lifecycle_task`) is a long, linear sequence of hardware/subsystem bring-up calls in
its own right — most were still bare `FUN_` names. Swept every direct call in this function's own
body (not deeper call graphs, except where noted) at the user's request, to see how many could be
identified. Full derivation, live-QMP work, and the front-panel cross-link in
`notes/front-panel-protocol-handout.md` and `qemu-machine/README-history.md`'s dated entries where
applicable — this table is the organized result, kept current as understanding improves.

**Headline finding**: a real NVRAM/EEPROM settings-integrity subsystem, previously completely
unmapped, drives this function's own cold-boot reset-mode decision (`all_reset_system_mode_action`
vs. `partial_reset_system_mode_action`) — and one of its checks reads bits from the confirmed
SCIF3 front-panel status struct, a genuine, previously-unknown cross-link to today's separate
front-panel-protocol thread.

| Call (in order) | Status | What it does |
|---|---|---|
| `FUN_200293c8(0)` | 🟡 shape known | Sets/clears bit `0x40` across 6 similarly-offset struct instances, toggles a couple of hardware bits, registers + enables GIC interrupt ID `0x26`. 6-instance shape suggests a multi-channel peripheral (ADC? a UART-channel-context array?); ID `0x26` not yet cross-checked against this project's own IRQ map. Not renamed. |
| `pwrk_power_state_write_to_eeprom_if_changed` | ✅ already named | — |
| `port_bulk_gpio_init_pass2` | ✅ already named | — |
| `FUN_20005dd8(10000)` / `(30000)` / `(100)` | 🟡 shape known | Generic busy-wait/delay primitive (arm a countdown via `FUN_20005d78`, poll `FUN_20005d88` until expired, stop via `FUN_20005dc8`) — shape matches an ITRON `dly_tsk`-style relative-time delay. Not renamed (units not confirmed). |
| `FUN_200b47f0` | 🟡 shape known | One-line: copies a fixed config word into offset `+0x104` of a struct also touched by the next call. |
| `scif3_frontpanel_init_and_latch_version` | ✅ already named | — |
| `scif3_dynqueue_post_and_flush` | ✅ named this session (front-panel thread) | — |
| `FUN_200b4800` | 🟡 shape known | Clears ~30 individually-selected fields (not a bulk memset) of the same struct `FUN_200b47f0` touches, spanning offsets up to `+0x306` — a real, moderately complex status/context struct reset. Subsystem not identified. |
| `FUN_2007ed9c` / `FUN_2007ede0` | 🟡 shape known | ITRON message-buffer-creation shape (`FUN_20186e98`, no following `itron_act_tsk`). Address sits just before `ui_graphics_lifecycle_task`'s own entry point — plausibly that task's own resource setup (see plate comment at `0x2007ed9c`), not confirmed. |
| `itron_act_tsk(DAT_2002b4e8, 0)` | ✅ already known | Activates `ui_graphics_lifecycle_task` — see the task-catalog table above. |
| `bmp_capture_task_bootstrap` (renamed from `FUN_200aa5d4`) | ✅ resolved this sweep | Creates `bmp_capture_task`'s ITRON resources and activates it — cross-confirmed directly against the existing task-catalog row. |
| `spectrum_scope_fft_task_bootstrap` (renamed from `FUN_200096c8`) | ✅ resolved this sweep | Same shape, for `spectrum_scope_fft_task` — cross-confirmed against the task catalog. |
| `FUN_200506d0` | 🟡 shape known | Near-identical shape to `FUN_200293c8` above, targeting GIC interrupt ID `0x21` instead of `0x26` — likely a sibling peripheral or the other half of the same one. Not renamed. |
| *(inline busy-wait, `while (*flag < 0x32)`)* | — | Not a call. |
| `FUN_200b5b64` / `FUN_200b5be0` / `FUN_200b5ea4` / `FUN_200b5f38` | 🟡 extensively pre-documented | DMAC channel 0 (+ MTU2 channel 0) configuration and completion busy-wait — the subject of a huge portion of this project's own `README-history.md` (the ring-overflow investigation). Not renamed here; `FUN_200b5f38`'s exact current behavior may be stale given the later DMAC completion-race fix — worth a fresh look before trusting old characterizations, not attempted this sweep. |
| `scif5_dsp_link_driver_init`, `scif5_wait_hsk1_ready`, `dsp_boot_handshake`, `dsp_cmd_table_init`, `dsp_identity_query_record0/1/2`, `rspi2_driver_init` | ✅ already named | — |
| `FUN_200b7020` | 🟡 shape known | 7-byte rolling-record comparison + checksum-shaped accumulation, calls `FUN_200b6bcc`/`FUN_200b6d60`. RTC (real-time clock chip) suspected given the shape (date/time-record rollover detection) — not confirmed. |
| `FUN_2005f8ac`, `FUN_2005fcb4`, `FUN_2005f9b0`, `FUN_2005fac4`, `FUN_200609c0` | 🟡 shape known | Small per-field clears of what looks like one shared driver's state struct(s), all in the same code-address neighborhood as the next row. RTC suspected, not confirmed. |
| `FUN_200605fc` | 🟡 shape known, plate comment added | Disables IRQs, bit-bangs ~20 alternating clock/status-poll cycles against a small register pair (`FUN_20360b0c`/`FUN_20360b24`, this project's own generic single-bit helpers), re-enables IRQs. Strongest candidate: a software-bit-banged serial link to a simple external chip — the IC-7300's real-time clock is the leading hypothesis. Worth a dedicated follow-up session (resolve the live `DAT_` addresses, correlate with a real RTC read/set). |
| `FUN_200291d8()` → conditionally `nvram_block_3e80_verify_16b`, `cold_boot_reset_mode_frontpanel_flag_check`, `nvram_block_3e80_verify_7b` → `all_reset_system_mode_action` / `partial_reset_system_mode_action` | ✅ **resolved this sweep — the cold-boot reset-mode decision tree** | A cascade of NVRAM settings-block integrity checks (see below) decides between a full and a partial system reset at cold boot. `cold_boot_reset_mode_frontpanel_flag_check` (renamed from `FUN_2002aeb4`) reads bits `0x40`/`0x2` of the confirmed SCIF3 front-panel RX status struct's offset `+0xf` — a real, previously-unknown link between this decision and the front-panel protocol thread. `FUN_2002aeac` (not renamed, trivial) is a stub that unconditionally returns 0. |
| `nvram_block_3fc0_verify`, `nvram_block_3fc0_repair_write`, `nvram_block_3e80_repair_write`, `nvram_block_3df0_verify`, `nvram_block_3df0_repair_write_a`, `nvram_block_3df0_repair_write_b`, `nvram_settings_180b_save`, `nvram_settings_180b_load`, `nvram_settings_180b_restore_rom_defaults` (all renamed this sweep, from `FUN_200291d8`/`FUN_20029178`/`FUN_200291a8`/`FUN_200292bc`/`FUN_20029198`/`FUN_200291c8`/`FUN_2000790c`/`FUN_20006c74`/`FUN_20008328`) | ✅ **resolved this sweep — a real NVRAM/EEPROM settings-integrity subsystem** | Built on two newly-identified generic primitives, `nvram_read_at_offset`/`nvram_write_at_offset` (renamed from `FUN_2001e510`/`FUN_2001e484` — read/write N bytes at a fixed NVRAM offset through this project's own RPC-backed persistence layer) and `memcmp_generic` (renamed from `FUN_2017c81e`, standard 3-way compare). At least 3 independent fixed-offset settings blocks (`0x3df0`, `0x3e80`/16000, `0x3fc0`), each with its own verify(read+compare)/repair(write) pair, plus a dedicated 180-byte block with save/load/restore-factory-defaults functions. `restore_rom_defaults` copies from a separate ROM-resident default source — the same "seed from a fixed constant" pattern found for the front-panel status buffer earlier the same day. |
| `nvram_wearleveled_ring_save` / `nvram_wearleveled_ring_load` (renamed from `FUN_2001f7b4`/`FUN_2001f6bc`) | ✅ resolved this sweep | A genuine **wear-leveled EEPROM ring buffer** — base offset `0x2000`, 64-byte slots, 8-slot rotating index (`if (index > 7) index = 0`). Real evidence this firmware does EEPROM wear-leveling somewhere, not just flat fixed-offset storage. |
| `nvram_multirecord_load_and_verify`, `nvram_writeback_pump_tick`, `nvram_writeback_flush_sync` (renamed from `FUN_2001a104`/`FUN_2001a1ec`/`FUN_2001a2f4`) | ✅ resolved this sweep | A third NVRAM cluster: loads 4 records via chunked `nvram_read_at_offset` calls, computes a total 32-byte-page count across them, and calls an unexplored `FUN_2017c618` (likely a checksum/CRC, given the `0x28a` constant passed) to validate. `nvram_writeback_pump_tick` (the diff-and-send pump, found earlier this session in the front-panel-buffer investigation and initially mis-scoped as narrowly SCIF3-related) and `nvram_writeback_flush_sync` (a synchronous drain loop, structurally identical to `scif3_dynqueue_flush_sync`) are this cluster's own send/flush pair — confirming this whole family is a **generic settings-writeback mechanism reused across multiple subsystems**, not front-panel-specific. |
| `diode_matrix_cold_boot_init` (renamed from `FUN_2003c530`) | ✅ resolved this sweep | Calls the already-known `scan_diode_matrix_p5`/`sync_diode_matrix_to_eeprom`, then classifies a status word into a 0/1/2 result stored into another struct. The diode-matrix cold-boot bootstrap entry point. |
| `FUN_2002c1e0` | 🟡 shape known | Clears an 808-byte (`0x328`) struct via a `memset`-shaped helper (`FUN_2017c766`, not yet renamed — likely `memset_generic`, sibling to `memmove_generic`/`memcmp_generic`), then two more unexplored calls. Runs as part of the `0x3df0` NVRAM-block repair path. |
| `FUN_2000a0a8`, `FUN_2000a264`, `FUN_2006756c`, `FUN_20035af4` | 🟡 shape known | Small per-field struct clears, subsystems not identified. Not renamed. |
| `tuner_jack_signal_precheck`, `emergency_screen_checkbox_state_sync`, `boot_check_mode1_combo`, `boot_check_mode5_combo`, `boot_check_challenge_response` | ✅ already named | — |

**What this sweep did NOT do**: descend a second level into every helper (e.g. `FUN_2017c618`'s
own checksum algorithm, `FUN_2006099c`/`FUN_20024738`/`FUN_200247d4`'s own bodies, the exact
peripheral behind the RTC-suspected bit-bang cluster or the two GIC-ID-0x26/0x21 channel-init
functions) — those are real, concrete next steps for a follow-up session, not attempted here.
Ghidra state: 21 renames, several plate comments (notably at `cold_boot_hw_init` itself,
`0x200605fc`, `0x200293c8`, `0x2007ed9c`), all saved.
