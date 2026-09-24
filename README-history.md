# IC-7300 firmware reverse engineering — history

Full narrative archive behind [README.md](README.md), which carries only the current-state
summary. Sections below are moved verbatim from README.md, in their original order, as it grew
past a reasonable size. Read the active README first; come here only for "how did we get here".

## Archived from README.md on 2026-09-24

## Status

- ✅ Container format understood well enough to account for **100%** of
  every firmware file's bytes across all 10 releases (see `tools/`,
  `notes/container-format.md`) — the original extraction script
  (`tunk3.py`) silently dropped ~37% of each file.
- ✅ LZSS decompression algorithm fully documented (`notes/decompression-lzss.md`), and a working
  compressor built to match (`tools/icom_fw/lzss.py`'s `compress()` — a valid encoder for this exact
  format, not a byte-for-byte match to Icom's own, which isn't needed for correctness and, incidentally,
  compresses somewhat better in practice). Paired with a new container packer
  (`tools/icom_fw/container.py`'s `pack()`) that rebuilds a checksum-correct update file from a modified
  body, round-trip-verified against all 10 real releases (`tools/verify_pack.py`) — this was
  `sdk/roadmap.md`'s Phase 0 tooling gap, now filled; only the live-hardware test itself remains.
- ✅ Boot ROM / vector table structure in `base.dat` mapped, and the full
  boot sequence traced in Ghidra through to the main firmware's entry
  point (`notes/base-loader.md`).
- ✅ Main firmware (`body.bin`) correctly based in Ghidra at `0x20005000`
  (RAM, not `0x18000000`/flash — a wrong initial guess corrected this
  session), with real backing memory for the flash-resident boot loader
  and both A/B update slots' font/chunk data (`notes/memory-map.md`).
- ✅ Firmware update mechanism fully traced end-to-end: SD-card source,
  dual A/B flash slots, SPI-NOR erase/program routines, and a preliminary
  (unconfirmed) finding that no cryptographic signature check exists in
  the update path (`notes/firmware-update.md`).
- ✅ Main-CPU kernel identified: **FreeRTOS**, Renesas RZ/A1H port (AN
  R01AN5093, matching source now in `scratch/r01an5093ej0170-rza1-swpkg/`),
  confirmed by direct comparison against the decompiled SWI handler —
  with real Icom customizations on top (unprivileged user-mode tasks,
  per-task ASID/MMU isolation) not present in the stock port
  (`notes/kernel-rtos.md`).
- ✅ **"SX3765" identity resolved**: it's the part marking on `IC501`
  (`R5F104LCAFB`, Renesas RL78), the Display Unit's own MCU — confirmed
  via the service manual's Display Unit parts list and independently
  visible on the Front Unit schematic sheet. Every `"SX3765 Vx.xx-yyy"`
  string in the firmware is a compatibility/version check against this
  chip, not an embedded second-processor firmware image
  (`notes/multi-cpu-images.md`).
- ✅ **Diode-matrix regional gating**: physical layout, scan mechanism,
  bit-to-diode mapping, and individual diode functions confirmed in code
  for 12 of 19 documented positions
  (D401/403/404/405/406/407/409/410/413/416/420/423 — D420 confirmed a
  16th session in as the direct raw-bit gate for the JP-only "4630kHz"
  Emergency-mode checkbox's visibility, found via the settings-list
  builder/filter chain rather than any of the diode-alias xref sweeps
  that had missed it for 15 prior sessions), plus **official per-diode
  version-population data** from the service manual parts list (which
  diode is populated on which of the 7 named export variants). 6
  positions remain unresolved (D408/411/414/417 fully unknown — D417
  specifically ruled out as the 70MHz/4m-band gate a 17th session in,
  real function still unknown — D419/D422 have hypotheses but no code
  consumer found across 4 sessions) — a newly-found likely clone/
  full-settings-export function is the top lead for finishing these
  (`notes/diode-matrix.md`). An 18th session also resolved the
  years-open `region_code`-to-country mapping (a lookup-table step had
  been skipped in the old derivation) and identified region codes 5/6 as
  Taiwan/Korea by diode-presence intersection, consolidating every
  region's full band-plan/channel-list table into a new
  `notes/band-plans.md`.
- ✅ **Full hardware BOM** for both the IC-7300 (all 5 boards: Main,
  Display, RF, PA, Tuner — `notes/ic7300-hardware.md`) and, as a side
  investigation, the IC-9700 (`notes/ic9700-hardware.md`), plus the
  IC-7300's complete SDR signal chain and a 17-page schematic sheet map
  (`notes/ic7300-signal-chain.md`).
- ✅ **Full boot-time RTOS task catalog**: all tasks the scheduler ever activates identified,
  named, and disassembly-confirmed (SD-card menu, screen capture, audio buffering, a generic
  file-access RPC service, system monitor/DRESD-init, several small poll/queue tasks) — see
  `notes/kernel-rtos.md`'s task catalog and the "Full boot-time task catalog" section.
- ✅ **CI-V transport confirmed at the code level** (not just pins): SCIF0's driver implements
  real `FE`/`FE`/dst/src/cmd CI-V framing with destination-address filtering.
- ✅ **The real CI-V command dispatcher found, and a genuine undocumented command identified**
  (2026-08-30): `civ_dispatch_lookup_validate` indexes a 43-entry command table (`g_civ_cmd_table`,
  wire commands `0x00`-`0x2A`) into a 16-bytes/entry handler table (`g_civ_handler_table`, function
  pointer + permission/length-bound fields), cross-checked entry-by-entry against the official manual
  (`/data/misc/icom/7300/doc/IC-7300_ENG_FM_12b.pdf`, pp.19-2–19-13) — every unimplemented slot matches
  a real gap in the manual. **Command `0x2A` sub `0x01` is real and fully implemented but has no manual
  entry at all** (the manual's table ends at `28 00`) — a confirmed undocumented CI-V command, also
  absent from `wfview`'s command set for every one of its ~40 supported Icom models, not just this one
  (github.com/wf-group/wfview). Its
  handler (`civ_cmd_2a_handler_UNDOCUMENTED`) gates a real hardware enable/disable toggle behind a
  60 MHz frequency-ceiling check and a subsystem re-sync. **Traced further the same day: it converges
  on the exact same hardware-engage call (`tuner_engage_gpio_toggle`) as the real, documented `1C 01`
  antenna-tuner command's own "start tuning" path** — reads as an independently-coded, more lightly
  gated alternate/bypass trigger for the tuner engage hardware, plausibly a factory/test shortcut.
  **Correction**: the two registers it touches turned out to be a generic, heavily-shared status-flags
  byte pair (60+ unrelated call sites), not tuner-specific GPIO state as first guessed — the real
  relay-shift-out code driving the tuner's schematic-confirmed relay network (user-supplied parts/net
  map now in `notes/ic7300-hardware.md`) hasn't been located yet (the raw port register it would use
  has only the generic one-time boot-init reference). **First real hardware-pin confirmation**: two
  sibling functions independently read the `[TUNER]` jack's `TCON`/`EKEY` pins via the real GPIO
  pin-read register and, on `EKEY` plus a frequency/TX-state check, trigger the *same* engage call —
  a third independent path in, alongside documented CI-V and undocumented CI-V. See `notes/kernel-rtos.md`'s "CI-V command
  dispatcher" section.
- ✅ **Real service/factory mode fully decoded, end to end**: entry condition (front-panel
  MENU+FUNCTION held **and** the REMOTE/CI-V jack's contacts shorted, detected via a raw GPIO
  pin-read of the CI-V receive pin), the boot-time code that checks it, and the reduced-
  functionality mode it enters (only CI-V and a second, parallel calibration-shaped protocol
  on a separate UART channel stay active) — see `notes/kernel-rtos.md`'s "Factory/service
  mode" section. A structurally similar internal file-RPC service (mistakenly named
  `civ_command_dispatch_task` in an earlier session — retracted) was found along the way.
- ✅ **Hardware JTAG debug access** identified and a connector confirmed populated on the
  board — see `notes/hardware-debug-access.md`. Adapter hardware ordered, not yet arrived;
  once available, several open items above (the real CI-V dispatcher, some task-activation
  and mode-2/pin-mux questions) are flagged as better resolved live than by continued static
  guessing.
- ✅ **UI icon/bitmap resource format fully solved**: a 708-entry pointer table
  (`g_icon_table`, RAM `0x20335234`) indexes 32-byte header structs (offset/
  width/height) each followed by tightly-packed BGRA8888 pixel data, row
  stride padded to a 4-pixel boundary (both quirks found by re-reading the
  real consumer code, `icon_blit_by_id_v1`/`_v2`, after user-caught decode
  bugs); extracted and rendered all 708 icons cleanly (`tools/extract_icons.py`)
  and individually identified/renamed 150 of them in Ghidra — the complete
  touchscreen UI icon set (`TUNE`/`SPLIT`/filter labels/meters/arrows/menu
  icons/etc.), confirmed visually (`notes/bitmaps.md`, `notes/icon_table.csv`).
- ✅ **`SCIF5` identified as the real DSP command/data link** (a 5th serial
  channel, not previously catalogued): full transport chain traced from
  `firmware_update_main`'s "3 extra chunks" mechanism down to the literal
  `SCFTDR_5` register, MTU2-timer-paced; also carries live, ongoing
  parameter-sync traffic during normal operation, not just updates. Confirms
  DSP Program/DSP Data get written **live during the firmware update**, not
  deferred to a post-restart check — almost certainly by the DSP itself
  reprogramming its own boot flash (`IC902`, corrected this session from an
  earlier "FPGA config flash" misattribution — its `CS`/`DO`/`DI`/`CLK` pins
  trace directly to the DSP's own `BOOT[4:0]`/SPI0 strapping pins). **Open**:
  where `DRESD` (DSP reset) actually gets released — still not found in the
  traced main-CPU call graph despite this — see `notes/multi-cpu-images.md`.
- ✅ **DSP/Front CPU firmware images precisely located, unpacked, and verified**:
  the exact per-component file-offset formula for the "3 extra chunks" (Front
  CPU/DSP Program/DSP Data) decoded from `firmware_update_main` and confirmed
  byte-exact (LZSS consumption + MD5) against a real v1.42 container —
  supersedes the old `chunk4`/`chunk5-tail` model. New tool:
  `tools/icom_fw/dsp_chunks.py`.
- ✅ **DSP disassembly retracted from "blocked" to actually working**: the DSP
  is a TMS320C6745 (TI C674x VLIW) — mainline GNU binutils' `tic6x` target and
  Capstone's `TMS320C64X` both disassemble it correctly (cross-validated
  against each other and against TI's own official `dis6x`, all three
  agreeing). `dsp_program.bin` and (surprisingly) `front_cpu.bin` are now both
  confirmed genuine TMS320C674x object code — **`front_cpu.bin` is *not*
  front-panel firmware after all**, retracting the earlier "component0 = Front
  CPU" hypothesis. `dsp_data.bin` shows no such code signature and is a
  better-supported bet for "compressed FPGA bitstream relayed by the DSP" — a
  specific structural match (a 32-byte preamble length) to an independently
  reverse-engineered same-family Cyclone chip adds real support beyond the
  original byte-histogram argument. **All 3 components' real identities fully
  resolved, 2026-08-30**: correlating each one's content-change points across
  every locally-held release against Icom's own officially-published
  `DSP Program`/`DSP Data`/`FPGA` version fields for each release gave a
  perfect, zero-discrepancy match — `front_cpu.bin` is actually `DSP Program`,
  `dsp_program.bin` is actually `DSP Data` (the internal working names for
  these two were swapped), and `dsp_data.bin` is confirmed `FPGA` (elevating
  the bitstream hypothesis from "better-supported bet" to solidly confirmed).
  One genuine surprise stands: the component identity-tracking `DSP Data` is
  confirmed real executable DSP code, not a calibration table — Icom's
  "Program"/"Data" naming apparently isn't a code/non-code split. See
  `notes/multi-cpu-images.md` and `notes/front-panel-firmware.md`.
- 🔎 **Front-panel MCU (`IC501`, RL78/G14) firmware: location still genuinely
  unidentified, but the `SCIF3` link and version/update questions are now
  thoroughly resolved.** `front_cpu.bin` (extracted, expecting this to be it)
  turned out to be DSP code instead (see above) — real front-panel firmware's
  location in the update container, if it's covered by this mechanism at all,
  remains open. RL78 tooling (Ghidra `xyzz/ghidra-rl78`, stock binutils'
  `rl78` target) is installed and confirmed working regardless.
  **2026-09-07/08, a multi-session thread run essentially to ground**:
  - ✅ **Physical button presses do reach the main CPU over `SCIF3`** — a
    real question this project's own UI-menu tracing had sharpened, resolved
    the same week it was raised: `scif3_key_bitfield_scan_and_resolve` diffs
    live `SCIF3`-buffer bytes against a shadow copy and feeds the resolved
    key code into the main input-routing struct, verified against known
    `MENU`/`QUICK` key codes.
  - ✅ **How the main CPU gets the displayed front-panel version**: a
    genuine one-shot `SCIF3` handshake at cold boot
    (`scif3_frontpanel_identify_handshake` — sends an outbound `0xF0`
    "identify" frame, blocks for the reply) latches the result once per
    boot, not per menu-visit. DSP Program/Data/FPGA version fields are
    likewise confirmed live-queried at boot, over `SCIF5`.
  - ✅ **No firmware-write mechanism to the front panel found** — the entire
    outbound `SCIF3` driver is a small, fully self-contained 2-function call
    graph; every send is a single ≤33-byte frame, nothing chunk/erase/
    program-shaped. A real negative result (not a whole-image sweep).
  - ✅ **The version-info screen's comparison logic, and the real
    update-progress dialogs, both fully traced** — including a genuinely
    satisfying complete-chain confirmation: the exact bilingual (English/
    Japanese) message table and activation call chain behind the real
    on-screen sequence during an update (`"Checking the file"` →
    `"Updating MAIN CPU firmware"` → `"Updating DSP/FPGA firmware"` →
    `"completed, will restart"`), independently confirmed against a real
    hardware video recording. The dialog sequencer turned out to be the
    exact same function that sets the `"Fup_AutoEnd_3765"` post-reboot
    marker below — two previously-separate threads, one function. Confirmed
    by direct listing that `firmware_update_main` itself synchronizes with
    this sequencer via a literal shared-flag busy-wait — a real task-to-task
    handshake, not inference. See `notes/firmware-update.md`'s "`FUN_200aa750`
    IS the real update-dialog renderer" section for the full derivation.
  - 🔎 Still open: where real front-panel firmware would live if it exists at
    all; who populates the version-info screen's "candidate" comparison
    struct (checked exhaustively — every known ARM address-formation idiom
    comes back empty; leans toward "generic multiplexed buffer, not update-
    file-sourced" but not proven either way); the "Checking the file." message's own
    activating item.
  See `notes/front-panel-firmware.md`, `notes/front-panel-firmware-history.md`,
  and `notes/firmware-update.md` for the complete trace.
- ✅ **Two substantial finds from extending Ghidra's memory map to the RZ/A1H's
  real, datasheet-confirmed 10 MB on-chip RAM range** (previously only
  `body.bin`'s own ~3.7 MB static image was mapped): (1) a likely answer to
  the long-open "how does the radio restart after a firmware update"
  question — a `"Fup_AutoEnd_3765"` marker written to the very top of RAM
  right before the same watchdog-reset sequence used elsewhere, checked and
  cleared on the next boot. **2026-09-08: the function that sets this
  marker's trigger flag turned out to be `firmware_update_progress_dialog_
  sequencer`** — the same function that drives the real on-screen "Updating
  MAIN CPU firmware"/"Updating DSP/FPGA firmware"/"completed" dialog
  sequence, with the marker set one state past the DSP/FPGA dialog. The
  marker's *consumer* side is still a confirmed dead end — the flags it sets
  on a successful match have zero readers anywhere in the static image
  (`notes/firmware-update.md`); (2) a previously uncharted shared "live
  radio/UI state" structure with a spectrum/band-scope display sub-region
  (mode selector + computed low/high frequency bounds, feeding what looks
  like a frequency→screen-position mapping function) — see
  `notes/band-scope-state.md`.
- 🟡 **RTOS task catalog's two remaining mystery tasks, chased further without JTAG**: found that 9
  of the catalog's tasks share one compiled 16-byte-stride descriptor array (cleanly bounded by an
  adjacent HF band-plan table — no hidden extra task there). `kernel_start`'s own mystery task
  descriptor is re-confirmed, more rigorously than before, as a genuine JTAG-only dead end (zero
  writers anywhere in the image, both via Ghidra and a raw literal scan). The other mystery
  (`thunk_FUN_2007ea68`, a dynamic task activation) got its *mechanism* fully traced — reached
  through a chain of ARM/Thumb interworking veneers down to real callers touching an unidentified
  peripheral at `0xE8100000`, triggered by a connect/disconnect-shaped event. **An initial "USB
  subsystem" guess was checked directly against the real RZ/A1H hardware manual and retracted** —
  the manual's actual USB2.0 controller bases are `0xE8010000`/`0xE8207000`, not `0xE8100000`, and
  no chapter documents anything at that address at all. The peripheral's real identity is open
  again. Also brought `notes/kernel-rtos.md`'s task-catalog table current with several
  session-old renames that had never made it back into the table itself. **Follow-up (2026-08-29,
  later session)**: verified the `0xE8100000` base has no missed indirection (a single, never-written
  literal-pool constant, checked at the raw-bytes level) — and found its handler is called as the
  `"vgStartUp"` step of a real, string-confirmed **EGL + OpenVG graphics-stack bring-up sequence**
  (`graphics_stack_startup_egl_openvg`), replacing the retracted USB guess with a new, well-evidenced
  (but not register-level-confirmed) hypothesis: a 2D/vector-graphics rendering resource. Also
  surfaced a real embedded **zlib** implementation nearby, of unconfirmed relation to this subsystem
  — a new, previously-unknown fact in its own right. **Separately, fully resolved another catalog
  task that had sat at "purpose not identified" since the 26th session**: `voice_recording_file_task`
  (renamed from `queue_driven_task_2001745c`, notable for having the catalog's largest stack) manages
  recording audio to the SD card's `C:\IC-7300\Voice` folder, confirmed as a real client of the
  already-known SD-card file-RPC service. **Follow-up (2026-08-30)**: also resolved the
  `audio_buffer_task` pair, renamed `voice_tx_memory_control_task`/`voice_tx_memory_stream_task` — a
  **sibling** feature (TX Voice Memory pre-recorded message playback, reading a *different* folder,
  `C:\IC-7300\VoiceTx`), correcting last entry's guess that it was the same feature's other half.
  **Also resolved the catalog's last-thinnest entry**, `ui_graphics_lifecycle_task` (renamed from
  `FUN_2007ef5c`) — turns out to be the task that actually **starts the whole EGL+OpenVG/SLV5
  subsystem** documented above, and creates the IC-7300's real 480×272 touchscreen EGL window surface
  plus a 960×552 off-screen pixmap surface of unconfirmed purpose. **Follow-up (2026-08-30, same day)**:
  traced the native platform's real display-attach code and found **genuine multi-display support built
  into the architecture** — a window can be attached to multiple simultaneous outputs via a runtime
  bitmask, stronger evidence for planned multi-display than the earlier `VDC50`/`VDC51` argument — but
  the "which displays are available" mask itself is a runtime-only value, blank in the static image
  (same signature as `kernel_start`'s own unresolved mystery task), so confirming more than one display
  is ever actually active needs live hardware. Also confirmed the 960×552 pixmap specifically is plain
  memory with **no** path to this display-attach mechanism at all — ruling it out as a direct
  external-display feed (also confirmed the BMP screen-capture feature doesn't use it either — it
  captures exactly 480×272, the real window resolution). **Follow-up (2026-08-30, same day)**: moved
  `sys_monitor_task_entry` from "examined" to **fully resolved**, needing the user to fix this
  project's known ARM/Thumb disassembly-context Ghidra bug at 3 addresses — its two registered event
  handlers turned out to be **full FreeRTOS context-switches**, a second, hardware-IRQ-triggered
  entry point into the exact same scheduler logic already documented under `swi_handler`, and its
  one-time `SWI(1)` call turned out to be the boot-to-running cache/MMU transition (invalidate
  TLB/I-cache/D-cache/branch-predictor, then enable all three via `SCTLR`). **Follow-up
  (2026-08-30, same day) — closes out the task-catalog triage entirely**: `status_poll_task_200095d8`
  (renamed `spectrum_scope_fft_task`) turned out to be a genuine **512-point FFT spectrum analyzer**
  — double-buffered against a sample producer, computing dB-scaled per-bin magnitude bytes that are
  very likely the band-scope display's actual "bar height" data, complementing
  `notes/band-scope-state.md`'s already-documented frequency-axis/"bar position" state. **Correction,
  same day**: that "triage entirely closed" claim was premature — `periodic_poll_task_20014384`
  (renamed `rtty_decode_log_poll_task`) had been marked "confirmed" on body-shape alone, purpose
  never actually chased, caught only because its name was still generic. It's the IC-7300's real
  RTTY digital-mode decode-to-SD-card logging feature (writes to `C:\IC-7300\Decode\Rtty` as `.txt`
  or `.htm`). **One more nuance, same day**: looked at `first_task_entry` too (its "generic
  message-dispatch loop" description was the vaguest left standing) — genuinely deepened (its two
  callees are real FreeRTOS/ITRON-shaped scheduler-internal bookkeeping, not an application
  feature), and along the way **corrected a wrong 24th-session claim**: the function it names as the
  loop's callback source only ever returns status codes, never a function pointer, so that specific
  attribution doesn't hold up — what the loop's `SWI(0)` trap actually invokes remains genuinely
  unresolved, the same standing "how does `SWI(0)` dispatch" open question. Final honest count:
  every task has a resolved body; all but three have a clean, nameable purpose; two are confirmed
  JTAG-only dead ends; `first_task_entry` is real kernel-internal machinery, a distinct third
  category rather than a loose end. See `notes/kernel-rtos.md`.
- ✅ **RZ/A1H peripheral SVD imported into Ghidra** (70 peripherals, real register names/structs) via
  a patched community loader script — saves datasheet lookups on any future peripheral-register work.
  A cross-reference sweep against all 70 bases turned up one genuinely new finding (the RIIC1/RIIC2
  I2C driver functions, narrowing the RTC/EEPROM I2C-bus assignment in `notes/memory-map.md`) and
  surfaced an important tooling gotcha (the `ghidra` MCP's `references_to` can misattribute a hit by
  one peripheral bank when code reaches a register through an indirect pointer rather than a direct
  literal — always confirm via a raw memory read before trusting one). See `notes/memory-map.md`'s
  "Peripheral SVD import + xref sweep" section.
- 🔎 **Open, side investigation**: IC-9700 (different radio, separate
  firmware format) — container structure mapped and compared across all
  37 known releases, but the compression/encryption scheme itself is
  **not cracked** after a thorough negative sweep (LZSS-family
  parameters, buffer-seeding, XOR-whitening, and — new, 2026-08-30 —
  zlib/raw-DEFLATE all tried and ruled out, against both real components
  now, see below). **Major validation the same day**: read the user's own
  live IC-9700's firmware-info screen (6 independently-versioned
  components: Main CPU, Sub CPU, Front CPU, FPGA Program, FPGA Data, DV
  DSP) as ground truth, then scraped Icom's own official EN + JP support
  pages for the full per-component version history of **all 20 publicly
  documented releases** (v1.02–v1.50, 2019–2025) — this **perfectly,
  with zero discrepancies, validates** the byte-diffing-derived
  "component 2 changes at only 7 of 18 consecutive-release transitions"
  finding against Icom's own changelog data, and confirms the release
  file-naming convention (`Jnnn`/`Ennn`) directly encodes the Main CPU
  version. A follow-up attempt to isolate FPGA Program's exact byte range
  from DV DSP's inside "component 2" (using releases where the official
  data says only one of them changed) found a real complication: overall
  file size shifts slightly release to release, so a component's size
  change cascades into apparent byte-diffs across everything packed after
  it — a concrete fix (account for the size delta before diffing) is
  identified but not yet implemented. Earlier the same effort found the
  container has **exactly two real, per-release-varying regions** — a
  ~2.7MB one (`0x10038`) and a ~7.2MB one (`0x800038`) — separated by
  large completely-fixed stretches; the second region's size is a
  plausible match for the on-board Cyclone V FPGA's configuration
  bitstream (`notes/ic9700-hardware.md`'s `IC7601`), a real but
  not-yet-confirmed hypothesis. See `notes/ic9700-container-format.md`.
  Also probed the user's live IC-9700 over the network as an intermission
  (2 TCP ports open, 1111 and 60000 — the latter is D-STAR
  data/picture-transfer per a user-supplied source quote; a full port
  scan briefly took the radio off the network, worth remembering to scan
  embedded devices much more conservatively next time) — see chat
  history, not yet written up as a note. Separately, confirmed the
  IC-9700's JTAG connector and full pinout are identical to the
  IC-7300's own (`10FLT-SM2-TB`) — see `notes/hardware-debug-access.md`.
  Genuine cold-start effort, no prior art existed for this radio going in.
- ✅ **UI menu/touchscreen system, first look, 2026-09-07**: a factory "FRONT CHECK MODE" self-test
  screen lists all 13 real physical front-panel buttons by name (`MENU` is button 9, `QUICK` is button
  12), reached via a 7-state factory-screen selector. Separately, found and correctly decoded (after
  catching and fixing a self-made record-alignment error — see `notes/ui-menu.md`'s "Correction") a
  generic, reusable touchscreen **list-menu widget** (72-byte records) that drives QUICK MENU, MEMORY
  MENU, REC/SET, Meter Type, SELECT, and other real screens — traced its full selection/navigation/
  commit flow end-to-end through real, named, decompiled code (visible-item scan → focus resolution →
  activate → **on-commit action** → redraw), confirming **three distinct on-press action patterns**:
  a direct live-config-byte write, a delegated external setter call, and a confirm-dialog-then-cycle-
  state flow (which also turned up a genuine, reusable popup/dialog subsystem entry point,
  `ui_show_message_dialog`). Getting there needed fixing two real never-disassembled code gaps with
  `tools/ghidra_scripts/FixArmThumbMode.java` — which itself needed a bug fixed first (its single
  `disassemble()` call only followed control flow and stopped at the first return, leaving later
  independent functions in the same range undefined; it now sweeps the whole range). **Same day,
  the physical-button chain**: traced a real key press (confirmed for `MENU` and `QUICK`) all the way
  from the main idle loop's input poll through a massive raw-input resolver, a key-code lookup, a
  genuinely new **279-entry system-wide command dispatch table** (`g_system_command_table`), and each
  button's own command handler, to a queued screen-open request — five newly-named functions and the
  command table itself, all confirmed through real decompiled code. **Checked the final hand-off and
  corrected the earlier guess**: the queued request's two values turned out to be callback function
  pointers (to small precondition helpers), not the list-widget's data pointers as first assumed, and
  the one real consumer found doesn't actually call either callback for these two buttons' requests —
  what genuinely switches the visible screen is still unresolved. See `notes/ui-menu.md`.
- ✅ **Minimal firmware emulator MVP reached, 2026-09-08**: a Unicorn Engine-based emulator
  (`emu/`) boots a real firmware release through `base.dat`'s traced boot sequence for real —
  PC lands exactly on `body.bin`'s entry point, RAM there matches `tools/icom_fw`'s independent
  LZSS decompressor byte-for-byte, and execution continues cleanly into `body.bin` until the
  first genuinely unmodeled peripheral access, hit as a clean stop rather than a crash.
  Architected so every future peripheral (SCIF UART, timer/IRQ, SD card, GPIO/EEPROM) is purely
  additive. Building it also surfaced and fixed a real `0x2c`-byte address-offset bug in
  `notes/base-loader.md`'s own offset table. See `emu/README.md`.
- ✅ **Escalated to a real custom QEMU machine, 2026-09-08**: the Unicorn emulator hit a genuine
  engine limitation (chunked `emu_start` calls corrupt ARM/Thumb state, blocking the one thing
  needed to unblock `body.bin`'s real `WFE` wait — a correctly-delivered periodic timer
  interrupt). `qemu-machine/` is a from-scratch QEMU machine (real `arm_gic` + two real `OSTM`
  timers at the real hardware addresses) that boots real, unmodified firmware deep into
  `body.bin`, independently cross-validated against the Unicorn emulator's own trace (both
  reach the exact same real addresses). Whether a real GIC IRQ actually wakes the `WFE`-parked
  CPU is not yet conclusively confirmed (a GDB-scripting reliability gap this session, not a
  demonstrated QEMU problem). See `qemu-machine/README.md`.
