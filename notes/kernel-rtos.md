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

Full derivation, search order, and retracted intermediate guesses in the history file
("Archived from kernel-rtos.md on 2026-09-24"). Current facts:

- **Dispatch chain**: `civ_rx_frame_stage_and_dispatch` (`0x2000b258`) → `civ_dispatch_lookup_validate`
  (`0x2000b03c`, indexes `g_civ_cmd_table`, base `0x2018aa2c`, 43 entries `0x00`-`0x2A`, each
  `{handler_base_idx; subcmd_list ptr}`) → `civ_dispatch_invoke_handler` (`0x2000acd8`,
  permission-gates against `g_civ_handler_table`, base `0x2018ab84`, 16 bytes/entry, function pointer
  at `+4`) → the real per-command handler. Cross-checked entry-by-entry against the manual (pages
  19-2–19-13): every unimplemented table slot matches a real gap in the documented command list.
- **Command `0x2A`/subcommand `0x01` is real, fully implemented, and completely undocumented** —
  absent from the manual and from `wfview`'s ~40-model rig database. Handler:
  `civ_cmd_2a_handler_UNDOCUMENTED` (`0x20010710`). Frequency ceiling 60 MHz. Data byte 2 ("engage")
  reaches `tuner_engage_gpio_toggle(1)` (`FUN_2001e720`) — the same call the documented `1C 01` tuner
  "start tuning" path makes; data byte 1 ("arm") shares `tuner_freq_and_txstate_precheck` with that
  path. Reads as an alternate, more lightly gated trigger into the same tuner-engage hardware
  (independent state machine `g_civ_2a_state`), plausibly a factory/production-test shortcut — not
  confirmed as literally "the tuner command."
- **Tuner relay network, bit-level, confirmed against the schematic (2026-09-09)**: `TSTB1`=`P7_1`,
  `TSTB2`=`P7_2`, `TSTB3`=`P7_5`, `TSTB4`=`P7_6`, `TCLK`=`P7_3`, `TDAT`=`P7_4`, `TOE`=`P7_7`;
  `PHASEI`/`IMPI` read as `P7_8` (unconfirmed whether genuinely shared/muxed or a transcription slip).
  `TCLK`/`TDAT` are **not** bit-banged — `tuner_relay_serial_bus_init` (`FUN_200b3a30`) routes
  `P7_3`/`P7_4` to a real on-chip serial-shift peripheral at `0xE800A800`-`0xE800A814` (register shape
  matches RSPI2's poll-then-write pattern, not yet identified against the RZ/A1H manual). `TSTB1`-`4`
  genuinely **are** GPIO, driven by `tuner_relay_tstb_strobe_dispatch` (`FUN_200b391c`, event `0xa0`
  off the generic per-tick dispatcher) via masked writes to `PSR7` (`0xFCFE311C`, not the plain `P7`
  data register) from `g_tuner_tstb_pulse_table` (`0x20335F98`) — bit 1/2/5/6 for index 0/1/2/3, an
  exact independent match to the schematic. Both functions renamed + PLATE-commented in Ghidra.
- Surrounding cluster (not renamed, roles read clearly but not independently hardware-cross-checked):
  `FUN_200b3c5c` (cold-boot driver init) calls `tuner_relay_serial_bus_init`, sets all 4 chips to
  `0x555`, asserts `TOE` low, waits 10ms, calls `FUN_200b3bf4` (default/all-off pattern).
  `FUN_200b3d34` (periodic per-tick state machine) builds a 2-bit-per-output pattern from two 24-byte
  target arrays (sizing matches `notes/ic7300-hardware.md`'s `RL20xx`/`RL21xx` relay table) and hands
  it to `FUN_200b3718`/`FUN_200b37dc`.
- **`tuner_jack_poll_and_autotrigger`** (`FUN_2006672c`, runs every idle-loop tick) and
  **`tuner_jack_signal_precheck`** (`FUN_20066154`) read the `[TUNER]` jack's `TCON`/`EKEY` pins
  (`P0_4`/`P6_2`) via `g_ppr_register_base` (`0xFCFE3200` family) — a third independent trigger path
  into the tuner-engage primitive (`tuner_engage_from_jack_trigger` → `tuner_engage_gpio_toggle`),
  alongside documented CI-V (`1C 01`) and undocumented CI-V (`0x2A`). `TCON`/`EKEY`/`ESTA` are the
  *external* tuner-accessory jack's own signals — not confirmed to be the same bus as the internal
  relay network's `TSTB`/`TCLK`/`TDAT`. `PPR1` = "Port Pin Read register, Port 1" (`0xFCFE3204`),
  this project's own naming shorthand, not a distinct schematic signal; `SWRL`=`P1_15`/`TPWRL`=`P1_14`
  are bits of that same register (open: whether `0x2001f168` reads it and uses bits 14/15).
- **Retracted along the way**: `DAT_2001f50c`/`DAT_2001f510` are **not** a GPIO shadow — they're
  pointers to a generic shared status/interlock flags byte pair (`0x203902d4`/`0x203902d6`) with 60+
  unrelated call sites, not tuner-specific. See history for the retraction detail.


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
| `thunk_FUN_2007ea68` (`0x2007ea84`) | *dynamic, caller-supplied* | `openvg_gpu_isr`'s own peripheral (see below) | — | — | ✅ **CONFIRMED, 2026-09-21** (`icom-main-idle-loop-not-reached` thread) — the "2D/vector-graphics rendering resource" hypothesis below is now confirmed at manual + interrupt-ID level, not just circumstantial: `0xe8100000`/`UNIDENTIFIED_SLV5_PERIPH_BASE` (renamed `OPENVG_GPU_BASE` in Ghidra) is the RZ/A1H manual's own **"Renesas Graphics Processor for OpenVG™"**, and its completion interrupt is exactly GIC IDs 130-133 (manual Table 7.3, "OpenVG graphics processor" INT0-INT3) — matching the `0x82`-`0x85` `slv5_periph_configure` (renamed from `FUN_2014ee46`) registers via `FUN_2007ec74`. This was, in the end, **the actual root cause of the whole multi-session "`main_idle_loop` never reached" thread**: with no interrupt source ever modeled at this address in `qemu-machine`, `ui_graphics_lifecycle_task` (this table's own row above) blocked forever in `rtos_wait_flag` (renamed from `FUN_20153ec2`) waiting for `openvg_gpu_isr` (renamed from `FUN_2014e9f8`) to post back — which deadlocked the entire cold-boot dispatch chain one call short of `main_idle_loop`, with the CPU parked in the RTOS idle task the whole time. Fixed with a new minimal device model, `qemu-machine/src/openvg.c` — see that file's own header comment for the full derivation and `qemu-machine/README.md`'s matching Status entry. Old text below kept for the historical trail (2026-08-29/08-30 investigation), superseded by the above: 🟡 **mechanism traced, peripheral base confirmed as a direct unwritten literal, identity hypothesis upgraded** — activated on a connect/disconnect-shaped event touching an unidentified peripheral at `0xe8100000` (RZ/A1H bus-matrix slave SLV5). 2026-08-29: verified `UNIDENTIFIED_SLV5_PERIPH_BASE` has no hidden indirection (single unwritten literal-pool constant, confirmed via a write-reference check — not a mutable pointer cell); separately found `slv5_periph_connect_disconnect_handler` is called as the `"vgStartUp"` step of a real, string-confirmed EGL+OpenVG graphics-stack bring-up (`graphics_stack_startup_egl_openvg`, `0x20079240`) — new working hypothesis: a 2D/vector-graphics rendering resource, not the earlier retracted "USB subsystem" guess. 2026-08-30: identified `graphics_stack_startup_egl_openvg`'s own caller as `ui_graphics_lifecycle_task` (the table row above) — this whole subsystem starts as that task's very first action. **Same session, separately**: traced the native platform's real window/display-attach code (`native_show_window_multi_display`) and confirmed it genuinely supports attaching a window to multiple simultaneous displays via a runtime bitmask — real architectural evidence for planned multi-display support, independent of and stronger than the earlier `VDC50`/`VDC51` argument — but the mask itself is a runtime-only value (blank in the static image, same signature as `kernel_start`'s own mystery task), so whether more than one display is ever actually active needs live hardware, not more static reading. Also confirmed the 960×552 pixmap surface specifically is plain memory with no path to this display-attach mechanism at all. See history's final session for the full trace. |
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

## `sd_menu_dispatch_task`'s command dispatch, byte-verified — a clean custom-app injection point (2026-09-25)

Picked up while starting to scope [[icom-custom-code-goal]]'s Phase 2 (injection-point design). Fully
decompiled and, unusually for this table, **cross-checked against the raw ARM listing instruction by
instruction**, not just the decompiler's view — worth doing whenever a jump table is involved, since the
decompiler's `switch` rendering can silently paper over exactly the detail (which case IDs are really
distinct vs. aliased to the same target) that matters for repurposing one.

- **Real inline ARM computed-branch jump table, not a separate data table.** The dispatch is
  `cmp r0,#0x29; addcc pc,pc,r0,lsl#2` at `0x20027550`/`0x20027554` — ARM's PC-relative-branch idiom, so
  the "table" is just 41 consecutive 4-byte `b <target>` instructions starting at `0x2002755c` (case 0),
  one per case ID `0x00`-`0x28`; `r0 >= 0x29` falls through to the same shared default. Verified by reading
  the raw listing and hand-checking every entry against the decompiler's `switch` — all 41 match exactly.
- **12 case IDs are genuinely dead — real, currently-unreachable no-ops, not just "unhandled":**
  `0x00, 0x02-0x06, 0x0a, 0x0e-0x10, 0x12-0x14` all branch to the same shared default stub at `0x20027710`
  (`iVar2 = 0`, i.e. "done, nothing happened"). Confirmed by reading each slot's actual branch target in the
  raw listing (`0x2002755c`+`4*id`), not inferred from the decompiler alone. **Any one of these 12 IDs can
  be repurposed by overwriting exactly one 4-byte `b` instruction** (its own slot in the table) to point at
  new, appended code instead of the shared default — no table growth, no `cmp` bound change, and (since the
  ID already reliably no-ops today) repurposing it can't regress any real existing behavior even if some
  static or dynamic path currently *can* produce that ID without anyone having found it.
- **Dispatch state lives in one fixed-address struct, not per-call arguments** — `DAT_20027840` holds the
  literal `0x2039011c` (the struct's real base, confirmed via direct memory read, not just symbol name);
  the command-ID field the switch reads is `+0x44` (`0x20390160`), and the task's own wait call is
  `FUN_20186f60(*(struct+0x40), 0xffffffff)` — an infinite-timeout wait on a queue/event handle stored at
  `+0x40` (also part of the same struct, populated at boot by `FUN_20027740`'s `FUN_20186e98(...)` queue-
  create call). Because the struct sits at a **fixed, static address**, any other code anywhere in the
  image — not just code that's part of this dispatcher — can post a command to this task by writing the
  desired case ID to `0x20390160` and then signalling the queue handle read live from `0x2039011c+0x40`
  (see `rtos_post_event`, `0x20186fb4`, already named from an earlier session) — this doesn't require
  knowing the handle's value statically, since the trigger code can just read it at runtime.
- **`firmware_update_main` (case `0xb`) is the existing proof this dispatch has no special-casing per
  command** — per [[firmware-update]]'s own entry-point trace, "nothing special gates entry to it; it's
  reached the same way any other SD-menu action is." Same holds for any newly-added case.
- **The ~28 direct writes to the `+0x44` field are the real public "post a command" API, not internal
  chaining** — corrected 2026-09-25 (Opus fresh-eyes pass, spot-checked and confirmed against real
  listings): each of the 28 is a small wrapper at `0x20022afc`-`0x200233f0` that stashes its own arguments
  (e.g. a path via `strcpy` into a fixed scratch buffer), writes its fixed case constant to
  `*(0x2039011c+0x44)`, and tail-calls the real signal primitive — **`sd_menu_task_signal`**
  (renamed from `FUN_200223ec`, confirmed by listing: `ldr r0,[0x2039011c]; ldr r0,[r0,#0x40]; b
  0x20186f00`, the ARM→Thumb veneer for `FUN_20186f08`, a plain single-argument RTOS signal — **not**
  `rtos_post_event`, an earlier guess in this file corrected). E.g. `sd_menu_post_firmware_update`
  (renamed from `0x20022fc0`) posts case `0xb` this exact way — confirmed by decompile. Companion
  helpers: `sd_menu_task_poll_result` (`0x20023414`, returns `+0x44`: 0=done, -1=error via `+0x48`,
  anything else=still busy), `sd_menu_task_try_lock`/`_unlock` (`0x20021500`/`0x20021520`, a simple
  byte-flag mutex at `+4`). **A new custom case should copy this exact convention**: try_lock → write the
  case constant → `sd_menu_task_signal` → poll → unlock.
- **The full tap-to-post chain, traced end to end (2026-09-25)**: the SD-card-menu screen's item records
  live in a **different, previously undocumented table** from the `0x2018f0ec` generic list widget —
  20-byte records `{en_label, jp_label, cb_action +8, cb_query +0xc, flags +0x10}` starting around
  `0x2018eebc` (Firmware Update's own record), with siblings for Save/Load Setting, Format, Unmount, REC
  Start/Stop, Play Files, CI-V Address, etc., stride `0x14` through roughly `0x2018ed80`-`0x2018f000`.
  Tapping "Firmware Update" calls its record's `cb_action` (`0x2005dd1c`), which calls `FUN_2005dc4c(path,
  1)` — this checks the SD card is ready and the **SD-UI state byte `*(u8*)0x20390368` is 0**, then sets
  it to `0x37`. A **separate 104-entry per-state handler table, `0x2019b70c`**, is ticked every idle-loop
  pass (from both `main_idle_loop` and `main_operating_loop`) by a function currently misnamed
  `digital_mode_log_writer_tick` (`0x2005d234` — real name is something like `sd_ui_state_tick`, not yet
  renamed) for any state `< 0x68`; states `0x33`-`0x3d` dispatch to `firmware_update_progress_dialog_sequencer`
  (`0x2005b2a0`), whose state `0x37` handler does `try_lock` then posts case `0x26` (`factory_file_load`,
  possibly under too narrow a name — it's used here as a generic "load the selected file" step, not
  something factory-specific), state `0x38` polls, a confirm-dialog round trip runs through
  `operating_mode_change_dispatch` (`0x2005807c`, a strong candidate for `notes/ui-menu.md`'s own
  long-unresolved "final hand-off" mystery — not chased further here), then state `0x39` posts case `0x27`
  (MD5 verify), state `0x3a` polls, and **state `0x3b` finally calls `sd_menu_post_firmware_update`
  (case `0xb`)**. So the real chain is: **tap → set SD-UI state byte → per-state tick-table handler(s) →
  post a case to `sd_menu_dispatch_task`** — two dispatch layers, not one.
- **This changes the injection-point picture**: the `0x2019b70c` 104-entry state table is itself very
  likely to have dead/unused state IDs the same way `sd_menu_dispatch_task`'s own case table did (not yet
  swept for one) — repurposing a dead *state* there, rather than a dead *case* here, would let a custom
  app's whole load-and-run sequence execute in **`main_idle_loop`'s own context**, which matters for CI-V
  emission (see the CI-V section above/below): the confirmed TX pump (`civ_tx_pump`, see below) also runs
  from `main_idle_loop`, so same-context execution avoids a real cross-task race the case-based approach
  would otherwise need a lock-and-retry loop to guard against. See `sdk/app-loader-design.md` for the
  updated concrete plan.

Ghidra renames made during this pass (all spot-checked against real listings/decompiles before being
recorded here): `sd_menu_task_signal` (`0x200223ec`), `sd_menu_task_poll_result` (`0x20023414`),
`sd_menu_task_try_lock` (`0x20021500`), `sd_menu_task_unlock` (`0x20021520`),
`sd_menu_post_firmware_update` (`0x20022fc0`), `civ_tx_pump` (`0x20011384`, see the CI-V section below),
`civ_reply_mark_ready` (`0x2000aa88`), `civ_read_handler_build_reply` (`0x2000ade0`).

## CI-V reply staging, byte-verified — corrects `sdk/api/serial-civ.md`'s TX section (2026-09-25)

The confirmed TX path (`serial-civ.md`) turns out to be a stateful pump reading a *staged reply*, not a
callable "send this buffer" API — tracing exactly how a real handler stages that reply (needed so a custom
app can emit one CI-V frame without corrupting live radio state) surfaced real corrections to what was on
record. Spot-checked against `civ_rx_frame_stage_and_dispatch`'s own decompile (confirmed
`pcVar3[0xca] = *(char *)(iVar2 + 3)` — a flag copy, not a length) and `civ_tx_pump`'s listing.

- **`g_civ_handler_table` entries hold *three* function pointers, not one**: `+0x0` flags (permission
  bits + packed exact/min/max data-length fields, as `serial-civ.md` already had), `+0x4` **set_handler**
  (`civ_dispatch_invoke_handler`'s target — a command that *changes* something), `+0x8` **read_handler**
  (used when the incoming data length equals the table's "exact length" field — a command that only
  *reports* something, called with `r0` = write cursor into the reply buffer, returns the advanced
  cursor), `+0xc` **read_precheck** (must return 0 or the reply is NG). Resolves the earlier "cmd `0x03`'s
  handler table entry reads as null" confusion from this session's own first pass: `0x03` (idx 4) is a
  pure read command (`+0x4` genuinely null, `+0x8`/`+0xc` populated) — not a missing/unimplemented slot.
- **Reply framing has no length field — it's `0xFD`-terminated.** `serial-civ.md`'s "length at `0xca`"
  was wrong; `rxbuf[0xca]` (`rxbuf` = `0x20396ad4`, confirmed fixed address) is a **flag** copied from a
  small fixed context block at `0x20390031` (`ctx`; `+2` rx length, `+3` reply-ready flag, `+5`
  channel-select 0/2 for the two independent CI-V channels this dispatch multiplexes, `+6` channel-2 busy).
- **Real build order** (`civ_read_handler_build_reply`/`0x2000ade0` and siblings, called from
  `civ_dispatch_invoke_handler`): write 3-byte header `[to][from=own CI-V addr, `*(u8*)0x203de525`][cmd]`
  (`FUN_2000aab0`) → optionally append the matched subcommand byte → call the read/set handler, which
  appends payload and returns the new cursor → write `0xFD` (`FUN_2000aacc`) → `civ_reply_mark_ready`
  (`0x2000aa88`) sets `ctx+3 = 1`. Short fixed replies (FA/NG, FB/OK) go through `FUN_2000aaf4`/`FUN_2000ab9c`,
  same shape. Back in `civ_rx_frame_stage_and_dispatch`, the completed reply is copied from a scratch build
  buffer (`0x20396d20`) into `rxbuf+0x66` (100 bytes max) and `rxbuf[0xca]` is set from `ctx+3` — this is
  the exact mechanism a hand-written emitter needs to imitate.
- **`civ_tx_pump`** (renamed from `FUN_20011384`) masks the SCIF0 GIC IDs (`0xdf`/`0xdd`/`0xde`) for its
  own body (CPU IRQs stay enabled otherwise) and, when the driver-state byte (`drv`, fixed address
  `0x20390039`) has bit `0x40` set and `rxbuf[0]==0` and `rxbuf[0xca]!=0`: copies `rxbuf+0x66..FD` into the
  real TX bu​ffer (`rxbuf+0x313`, prefixed with the two `0xFE` preamble bytes), sets `drv |= 0x0c`, clears
  `rxbuf[0xca]`. The actual byte-by-byte UART send (`FUN_20011598`/`FUN_200108b0`) is echo-driven with
  real CI-V bus-collision handling (jam byte, up to 5 retries) — **writing raw bytes to the UART directly
  would bypass this and break collision handling**, another reason to stage through `rxbuf`/`drv` exactly
  like a real handler rather than poking the driver lower down.
- **`drv` bit `0x40`** = "reply window open, pump should pick this up" — set by the RX state machine on a
  real inbound frame, and exactly the bit a custom emitter needs to set itself (last, after staging the
  reply bytes) to get its own frame picked up on the next `civ_tx_pump` pass.
- **Cookbook for an app to emit one arbitrary frame**, matching a real handler's own convention exactly
  (full bit tables, the cross-task race this implies, and the recommended same-context mitigation are in
  `sdk/app-loader-design.md`):
  ```
  require rxbuf[0]==0 && rxbuf[0xca]==0 && (drv & 0x78)==0 && drv[2]==0   (else back off, retry later)
  write rxbuf+0x66 .. : [to][from = *(u8*)0x203de525][cmd][payload...][0xFD]   (<=100 bytes, no FE FE — the pump adds it)
  rxbuf[0xca] = 1
  drv |= 0x40   (last)
  ```

## `kernel_start`'s bring-up initializes a runtime memory pool right after the static image (2026-09-25)

Found while getting [[icom-custom-code-goal]]'s first real proof-of-concept custom code
(`sdk/examples/civ-hello-world/`) to actually run — code appended to `body.bin` right after its own static
image end (`0x20395b18`) decompressed into RAM correctly but was silently zeroed before `main_idle_loop`
ever ran, breaking the whole approach. Traced far enough to explain it, not exhaustively:

- `kernel_start` (`0x20005290`) → `run_ctors_and_start_kernel` (walks the C++ static-ctor table, already
  known) → `FUN_20186d2c` (a `get_cpsr_mode`-gated trampoline, same "SWI(0) if User, call direct if
  privileged" shape as `itron_act_tsk` and siblings) → **`FUN_20188574`**, whose own existing plate comment
  already correctly identified it as matching `R_OS_InitMemManager`'s "lazy init, if not yet initialised"
  shape — confirmed here, not just re-cited.
- `FUN_20188574` calls **`FUN_201876f0(heap_base, heap_size)`** — a genuine heap/pool initializer:
  places a single free-block header at `heap_base + heap_size - 4` and zeros two words there. Live literal
  values: `heap_base = 0x20416198` (`DAT_201885e0`), `heap_size = 0x9f88` (~40 KB, read from
  `*(u32*)0x20336054`). This specific pool is too small on its own to explain code being wiped as far out
  as `0x20500000`+ (see below), so it's evidence the region is genuine heap/kernel-object territory, not
  the complete mechanism — the rest of the picture needs either deeper tracing or live JTAG, not attempted
  further this session.
- **Empirically confirmed, live in `qemu-machine` (more conclusive than the static trace above)**: a
  marker byte pattern written at `0x20500000` survived a ~5 second window post-boot but was gone —
  overwritten with plausible code-shaped bytes, not just zeroed — by ~20 seconds into boot. A wider sweep
  (markers at `0x20500000`/`0x20600000`/`0x20700000`/`0x20800000`/`0x20900000`/`0x209d0000`/`0x209f0000`,
  checked only after confirming full boot via live CI-V replies) found `0x20600000` and every point checked
  above it undisturbed. **This region keeps being written into as boot progresses, not just once at a
  fixed early point** — a genuine runtime allocator's behavior, not a one-shot BSS-style clear.
- **Reusable methodological point**: this project has, more than once, treated "zero `references_to` hits
  across a broad static sweep" as evidence a RAM region is safe/unused (see `notes/band-scope-state-history.md`'s
  own sweep, and `sdk/app-loader-design.md`'s first three passes, which cited exactly that sweep and got
  the placement wrong as a direct result). That inference has a real hole its own author already flagged
  and this session failed to apply: literal-pool-based `references_to` can only ever find *compile-time*
  addresses; genuine heap/allocator content is invisible to it by construction, regardless of how much of
  it is actually live. **Treat this whole class of "unreferenced RAM" claim as unverified until confirmed
  by a live marker-write-then-reboot-and-check test, not by static sweep alone.**

Open: the exact allocator responsible for the `0x20500000`-ish writes (this ~40 KB pool alone doesn't
explain it), and the true upper bound of the affected region (confirmed unsafe up to somewhere between
`0x20500000` and `0x20600000`, confirmed safe at `0x20600000`-`0x209f0000` in spot checks after one
specific boot sequence — not proven safe under heavier/longer-running activity).

## SD-card file I/O: 4 real open/read/seek/close wrapper functions, byte-verified (2026-09-25)

Found and confirmed live (`sdk/examples/sd-card-app/`) by reading `firmware_update_main`'s own real,
working file-read code — it opens the SD-card update container this exact way — rather than guessing at
`file_rpc_post_command`'s raw ring-buffer message layout cold. These are 4 of the 26 tiny per-command
wrapper functions `sdcard_file_rpc_dispatch_task`'s own retraction comment already flagged as existing
(`0x200bc0fc`-`0x200bca08`) but hadn't individually documented:

| Function | Address | Command ID | Signature | Role |
|---|---|---|---|---|
| — | `0x200bc5f4` | `0xf` | `(path, flags, &handle_out, scratch36)` | open |
| — | `0x200bc6a4` | `0x11` | `(handle, dest_buf, len, &actual_out)` | read |
| — | `0x200bc754` | `0x13` | `(handle, offset, 0, &actual_out)` | seek |
| — | `0x200bc64c` | `0x10` | `(handle, 0)` | close |

Each is a plain C function (`0x24`-byte block assembled on the caller's stack, `memmove`d, then a tail call
into `file_rpc_post_command` — confirmed by listing, not just decompile: the wrapper's own return value is
exactly `file_rpc_post_command`'s r0, still live in r0 at the wrapper's own `ldmia sp!,{...,pc}` return).
**Call it, then wait on the result**: `FUN_200214b0(return_value, 0x46)` — a generic "wait for this RPC to
complete" helper, `0x46` a fixed tick-count timeout used identically by all four calls in
`firmware_update_main` — returns `0` on success. `path` is a plain null-terminated ASCII string
(`"C:\IC-7300\..."`, confirmed against a real literal at `0x20016fec`). Live-verified end to end: opening a
real file, reading its bytes into RAM, and closing it, all via these four calls from freshly-written code
(not from existing firmware call sites), works correctly — and a missing file fails the open call cleanly
(nonzero return, no hang) rather than crashing.

**Corrects `sdk/api/filesystem.md`'s earlier guess** (`6`/`9`/`0x13`/`0x17` for open/write/read/list) — only
`0x13` checks out, as seek rather than read. The other IDs in that older guess aren't re-verified; the
dispatch table has 26 entries total (`0`-`0x1a`, per `sdcard_file_rpc_dispatch_task`'s own decompile), so
they may still be valid for other operations (write, directory listing) just not confirmed here.

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
| `frontpanel_mcu_release_reset` (renamed 2026-09-21 from `FUN_200b47f0`) | ✅ **resolved 2026-09-21, manual + schematic confirmed** | Writes `PSR1` (Port Set/Reset register 1, `0xFCFE3104` — matches the manual exactly) to set `P1_0` high. Real IC-7300 schematic (user-supplied): `P1_0` is net **"FRES"**, wired through the front-panel board's own JTAG connector `RESET_IN` → 10k resistor → `RESET_OUT` → the front-panel CPU's (`IC501`, RL78) reset pin. **This function releases the front-panel MCU from reset** — cold_boot_hw_init's 2nd action, right before any SCIF3 front-panel traffic. |
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
| `nvram_multirecord_load_and_verify`, `nvram_writeback_pump_tick`, `nvram_writeback_flush_sync` (renamed from `FUN_2001a104`/`FUN_2001a1ec`/`FUN_2001a2f4`) | ✅ resolved this sweep | A third NVRAM cluster: loads 4 records via chunked `nvram_read_at_offset` calls, computes a total 32-byte-page count across them, and divides `0x28a` by that count via `udiv32_generic` (renamed 2026-09-21 — **not a checksum/CRC as first guessed**, a plain unsigned-division routine; see the follow-up section below). `nvram_writeback_pump_tick` (the diff-and-send pump, found earlier this session in the front-panel-buffer investigation and initially mis-scoped as narrowly SCIF3-related) and `nvram_writeback_flush_sync` (a synchronous drain loop, structurally identical to `scif3_dynqueue_flush_sync`) are this cluster's own send/flush pair — confirming this whole family is a **generic settings-writeback mechanism reused across multiple subsystems**, not front-panel-specific. |
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

## `cold_boot_hw_init` sweep — Opus-review corrections and manual/schematic confirmations (2026-09-20/21)

Full derivation, the reviewing agent's own flagged caveats, and Ghidra-state bookkeeping moved to
the history file. Two follow-up passes cross-checked the sweep table's own shakiest entries against
the RZ/A1H manual and the real schematic (both already local in `/data/misc/icom/7300/doc/`).
Corrected/confirmed findings — these supersede the matching sweep-table rows above, which still show
the old `FUN_*` names/guesses:

- `FUN_200293c8`/`FUN_200506d0` are **not** a 6-channel peripheral — they're external-IRQ pin config,
  renamed `ext_irq6_config_init` (`P1_6`) / `ext_irq1_config_init` (`P1_1`). Confirmed
  `IRQ_n = GIC ID 32+n` directly against the manual's interrupt-source table. `P1_1` is the RTC
  (`IC351`) interrupt line (`notes/memory-map.md`). `ext_irq1_config_init` never registers/unmasks a
  handler — where that happens is still open.
- `FUN_200605fc` is **not** an RTC bit-bang — it's SSIF (I2S audio) bring-up: `0xE820B000`=`SSICR_0`,
  `0xFCFE3200`=`PPR0` (pin-read, so the ~20 wait loops are I2S clock/word-select settling, not
  bit-banged data), DMAC channels 3/4/5. `0xCC`→`SSIFCR_0` (TIE=1,RIE=1, ch0), `0xC4`→`SSIFCR_1`
  (TIE=0,RIE=1, ch1), decoded against the manual's own bit layout. `FUN_200b7020`'s earlier guessed
  connection to this cluster is unsupported (retracted, no evidence either way). The real RTC is I2C
  on `RIIC1`, unrelated.
- `FUN_2007ed9c`/`FUN_2007ede0` confirmed (by `references_to`, not just address proximity) to belong
  to `ui_graphics_lifecycle_task` — both store message-buffer handles into `0x20390634`, read from
  inside that task's own address range. Buffer contents still unknown.
- `FUN_2017c618` → `udiv32_generic` (plain unsigned division, not a checksum — corrects the
  NVRAM-cluster writeup above). `FUN_2017c766` → `memset_zero_generic` (tail-calls the real
  `memset_generic` at `FUN_2017c758`). `FUN_20024738` → a signature-keyed NVRAM boot-mode dispatcher
  (reads a 16-byte tag at NVRAM offset `16000`, walks a 4-entry signature+function-pointer table;
  enumerating the 4 entries is a good next step, not done).
- `FUN_2002aeac` is not a trivial always-0 stub — its return value is genuinely stored and branched
  on inside `cold_boot_hw_init`, and its sibling branch writes the same NVRAM offset `16000` slot
  `FUN_20024738` reads and dispatches on — a plausible (not proven) deliberately-disabled feature
  gate.
- `FUN_20005dd8`'s delay unit confirmed **microseconds, on `OSTM1`** (not `OSTM0`, not `dly_tsk`
  ticks): comparison is `us*32 <= counter` at the confirmed 32MHz `P0φ`. Renamed
  `ostm1_busywait_delay_us`/`ostm1_counter_start`/`ostm1_delay_target_reached`/`ostm1_counter_stop`;
  `cold_boot_hw_init`'s own three calls are 10ms/30ms/100µs, not ticks.
- `FUN_2000a0a8`'s (`0x20396AC8`), `FUN_2000a264`'s (`0x20390028`), and `FUN_20035af4`'s
  (`0x203901FD`/`0x203FC621`) target addresses each appear in exactly one literal pool image-wide — a
  real, checked negative result. Genuinely bounded: needs a QEMU RAM watchpoint (live RAM access) to
  go further, not more static reading.
- Schematic ground truth (user-supplied) resolves two physical-pin questions: `P1_6` (external IRQ6)
  is net **"PDV"**, `VOUT` of `IC361` (NJU770F43 voltage-detector/supervisor IC) — a real
  power/voltage-detect interrupt, not yet cross-linked to this project's own PWRK/power-state work.
  `P1_0` (written by `frontpanel_mcu_release_reset`) is net **"FRES"**, wired through the front-panel
  board's JTAG connector `RESET_IN` → a 10k resistor → `RESET_OUT` → the front-panel CPU's (`IC501`,
  RL78) own reset pin — confirming `cold_boot_hw_init`'s 2nd action releases the front-panel MCU from
  reset, right before any SCIF3 front-panel traffic.

Ghidra state: 13 further renames/plate comments across both passes, all saved.
