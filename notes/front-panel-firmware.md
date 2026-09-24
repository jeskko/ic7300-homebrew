# Front-panel MCU (`IC501`) firmware — location unknown, open again

See [notes/front-panel-firmware-history.md](front-panel-firmware-history.md) for the full narrative,
including a full RL78-disassembly investigation into a file that turned out to be the wrong one — the
tooling built along the way remains valid and reusable, just not yet pointed at a confirmed target.

**Picking this thread back up? Read
[notes/front-panel-protocol-handout.md](front-panel-protocol-handout.md) first** — a dedicated onboarding
doc (2026-09-07) covering where to start and what to watch for that would point at a real (still unfound)
front-panel firmware image. **Its top-priority question (do physical buttons reach the main CPU over
`SCIF3`?) is now resolved — yes — see the Open questions section below**; the handout also contains a
now-corrected address typo (`0x200301f2` should read `0x203901f2`) flagged there too.

## Current state

- **`IC501` = `R5F104LCAFB`**, Renesas **RL78/G14** (64-pin LQFP), the Display/Front Unit's MCU — see
  `notes/ic7300-hardware.md`. Renesas's own datasheet confirms 32 KB code flash / 4 KB data flash / 4 KB
  RAM.
- **The main-CPU side of the link to this MCU is fully reverse-engineered**: `SCIF3` (base `0xE8008800`,
  pins `P6_0`/`P6_1`), a 33-byte packet-framed UART, `0xFE`/`0xFD` byte framing (same template as
  CI-V/`SCIF0` and the service-mode link/`SCIF1`). `scif3_frame_rx_statemachine`/
  `scif3_frame_dispatch_by_type` parse frames into a shared front-panel status buffer;
  `boot_check_mode1_combo`/`boot_check_mode5_combo` (in `cold_boot_hw_init`) read buffer offset `0xd` bits
  3/4 to detect the MENU+FUNCTION service-mode combo. See `notes/kernel-rtos.md`'s "Factory/service mode"
  section.
- **Confirmed, 2026-09-07: physical button presses do reach the main CPU over this `SCIF3` link.**
  `scif3_key_bitfield_scan_and_resolve` (`0x2002fbc8`, called every tick from `ui_input_poll_tick`) diffs
  live bytes at the `SCIF3` buffer's offset `0xd`-`0x11` against a shadow copy, resolves the changed bit
  through `g_scif3_bit_to_keycode_table` (`0x2018d980`), and feeds the result to
  `key_event_resolve_and_route`'s own key-code struct — verified against the already-known `MENU`/`QUICK`
  key codes. See the Open questions section and `notes/front-panel-firmware-history.md` for the full trace.
- **`front_cpu.bin` is *not* front-panel firmware — retracted.** It was extracted from the v1.42 update
  container's "component0" under a working hypothesis it was `IC501`'s image; two independent checks ruled
  that out: its content is 134,272 bytes, over 4x the RL78/G14's entire 32 KB flash capacity, and the
  transport code that sends this exact chunk during an update is hard-wired to the `SCIF5`/DSP path with no
  `SCIF3` (front-panel) branch anywhere. A follow-up disassembly cross-check (mainline `tic6x-objdump` and
  Capstone `CS_ARCH_TMS320C64X`, agreeing byte-for-byte) positively confirms it's genuine **TMS320C674x
  (DSP)** object code instead. See [[multi-cpu-images]].
- **RL78 tooling stood up and confirmed working, ready for whenever real front-panel firmware turns up**:
  [xyzz/ghidra-rl78](https://github.com/xyzz/ghidra-rl78) (Ghidra processor-module extension, installs
  cleanly into this Ghidra 12.1.2, no compiled Java) and `rl78-objdump`/`rl78-readelf` (mainline GNU
  binutils, built from plain upstream source — recipe in the history file). Neither has been pointed at a
  confirmed real target yet.

- **Resolved, 2026-09-07 — how the main CPU gets the front-panel version shown on the version-info
  screen.** `scif3_frontpanel_identify_handshake` (`0x20037424`) runs once at cold boot: sends an
  outbound `0xF0`-type frame over `SCIF3` (`scif3_send_frame`/`0x20037214`) and blocks with a timeout
  for the front panel's reply, which lands in `g_scif3_rx_status_buffer` (`0x203dcab6`); bytes 1-12 get
  copied into `g_frontpanel_latched_status` (`0x203dca96`), and bytes 1-3 of *that* are what
  `ui_version_screen_populate_fields` (`0x2004366c`) formats and displays as `"Front CPU:"` on the
  version-info screen (`ui_version_screen_draw_and_compare`, `0x200a94c8`) — a real live query of
  `IC501`, not a cosmetic/stored value, but only once per boot, not per screen visit. The value **is
  actively compared** too: `ui_version_screen_draw_and_compare` diffs it against
  `g_update_candidate_version_struct+8`, and a mismatch gates a detail panel plus 3 status-row widgets
  (real triangle/return-arrow icons — an ahead/behind/restart-needed three-state indicator, not text).
- **What feeds the "update candidate" side of that comparison is genuinely still unknown, despite an
  extensive search.** `g_update_candidate_version_struct`'s base offset and the much-more-widely-shared
  `g_screen_display_scratch_buf` (52 reference sites) both turned out to be reused verbatim by several
  completely unrelated screens (a QSO-recorder storage-capacity display, a memory-channel editor) — real
  evidence this is a generic, heavily-multiplexed scratch buffer, not a dedicated "update file info"
  struct. Leading (unconfirmed) hypothesis: a generic previous-frame-vs-current-frame snapshot pair used
  for redraw-skipping across many screens — though RAM-sharing with unrelated screens doesn't actually
  disprove it still carries real update-file data specifically for *this* screen when it's active
  (mutually-exclusive screens legitimately share scratch RAM). The version-info screen's own field
  *interpretation* (which offset means which component) is solid, independently verified via the label
  strings drawn alongside each value. `notes/multi-cpu-images-history.md`/`notes/band-scope-state.md`
  independently reached the same "generic multiplexed buffer" conclusion earlier, with a sharper framing
  worth trying: populated via `FUN_2008cff8`'s own generic dispatch logic, not a dedicated writer — which
  internal case of `FUN_2008cff8` handles this screen is the concrete next step, not yet tried. DSP
  Program/Data/FPGA fields on the same screen, by contrast, **are** confirmed live-queried at boot
  (`ui_version_screen_populate_fields`'s DSP-field source and `dsp_identity_query_record0`'s destination
  are the same address, `0x203def00`). All 3 ARM address-formation idioms (literal-pool, `MOVW`/`MOVT`,
  `ADR`/PC-relative) were swept against the full decoded instruction stream for a writer — zero hits.
  Full trace, every ruled-out candidate, and the narrative behind this in the history file.
- **Checked, 2026-09-07 — does a detected mismatch actually trigger a firmware push? No, from 3 angles.**
  The mismatch branch's own calls are 100% UI-drawing, nothing touches flash/transport code. The real
  updater (`firmware_update_main`) shares zero code or data with the comparison structs — reached only via
  a manual SD-card-menu confirmation. Even `firmware_update_main`'s own per-chunk trigger
  (`chunk_needs_update`/`DAT_2002217c`) has no writer found anywhere (checked exhaustively, a real open
  mystery of its own). And the transport those 3 chunks would use (`chunk_transport_send_data`) is
  reconfirmed non-`SCIF3` regardless.
- **Checked, 2026-09-07 — no firmware-image-write mechanism found for the front panel (not exhaustive).**
  `scif3_send_frame` (the only outbound-frame-construction primitive found) is called exclusively from
  `scif3_driver_pump_tick` (`0x200373ac`), which is in turn its *only* caller — a fully self-contained,
  narrow outbound driver. Every send is a single bounded frame (≤33 bytes) triggered by a simple
  status-bit flag or the boot-time handshake above — no loop, no offset/size-header parsing, nothing
  shaped like an erase/program sequence. A real negative result for the handout's goal 2, but not a
  whole-image sweep — it only rules out this specific call graph, not every possible path.

## Open questions

**Correction, 2026-09-24** — `g_scif3_bit_to_keycode_table` bit-index 16 (bitfield byte `0x0f` bit 0)
is actually **A/B** (code `0x09`), and bit-index 23 (byte `0x0f` bit 7) is actually **SPLIT** (code
`0x0c`) — an earlier pass checked these two entries against FRONT CHECK MODE's *list positions* (9 and
12) instead of real `SCIF3` key codes. The real `MENU`/`QUICK` bits are byte `0x0d` bit 3 (code `0x0f`,
`MENU`) and byte `0x0d` bit 6 (code `0x11`, `QUICK`) — this is the mapping the "physical button presses
reach the main CPU" finding in Current state above relies on; the mechanism itself
(`scif3_key_bitfield_scan_and_resolve` diffing the buffer and resolving through this table) is correct,
only the two example bit→key identifications were wrong. Full `SCIF3` field layout and key-code table:
[notes/front-panel-report.md](front-panel-report.md).

- **Where real `IC501` firmware actually lives, if the update container carries it at all.** Checked one
  specific angle 2026-09-07 (see above): the update file's "Front CPU" version field
  (`ui_version_screen_draw_and_compare`'s `+0xa4`, in the update-candidate struct
  `g_update_candidate_version_struct`) is compared against the *live-queried* struct above, confirming the
  comparison is real and meaningful — but this doesn't itself locate an image; still open.
- Whether `IC501` is field-updated at all, or factory-programmed once and never touched by this mechanism —
  leaning further towards "never updated over `SCIF3`" after the negative result above, but not settled.
- **The exact `SCIF3` type→offset→field mapping (which bit is which physical key) is still not fully
  decoded** beyond the corrected `MENU`/`QUICK` entries above — buffer offset `0xd`-`0x11` (bits 0-39) is
  a physical-key bitfield, and the rest of `g_scif3_bit_to_keycode_table` (`0x2018d980`, 40 bytes) is
  straightforward to read off for the remaining bits.
