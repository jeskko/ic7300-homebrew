# Goal: running custom "apps" on the IC-7300 — survey and roadmap

**Stated 2026-08-30.** The end goal of this whole project, beyond documentation for its own sake: the
ability to write and run our own code ("apps") on the radio. Two directions were proposed:

- **(a)** Find a way to get the radio to read and run custom code via something that already interfaces
  with the SD card or a serial port (CI-V/REMOTE, USB) — ideally without a full firmware re-flash, e.g. a
  buffer overrun or similar a robustness bug bug in a file/protocol parser.
- **(b)** If (a) doesn't pan out, build a roadmap toward a genuine custom-firmware capability.

## The key reframing: (a) and (b) aren't actually separate paths

Re-reading what this project has *already* found (not new research, just connecting dots that were
recorded separately):

- **`notes/firmware-update.md`**: the SD-card firmware-update mechanism's only integrity check is a
  **plain, unkeyed MD5** (`FUN_2003c860`/`FUN_2003d38c`/`FUN_2003d458`, confirmed textbook MD5, no keying)
  compared against a 16-byte value read **from the update file itself**. No RSA/ECDSA/HMAC-shaped call
  sequence anywhere in the orchestrator (`FUN_20025ae4`/`firmware_update_main`). An The owner (or in this
  case, us) doesn't need to break MD5 — we just compute it over our own payload and write it into the
  container ourselves, exactly like the legitimate build process presumably does.
- **`notes/base-loader.md`**: the boot sequence (silicon reset → `base.dat` → decompress/relocate →
  jump into `body.bin`) has been fully traced with **zero mention of a checksum, hash, signature, or
  verify step anywhere in it**. The MD5 check above only runs *during the SD-card update write*, as a
  "did this chunk change" / "did it copy correctly" check — not as a boot-time gate. Once bytes are in
  flash, they run, full stop.
- The one remaining theoretical gate — whether the RZ/A1H's own **immutable silicon boot ROM** performs
  a secure-boot signature check on `base.dat` before executing it (a hardware feature, not anything in
  Icom's own code) — **doesn't need to be resolved at all for this goal**, because we don't need to touch
  `base.dat`. Leave Icom's genuine, presumably-boot-ROM-approved-if-required `base.dat` exactly as-is, and
  only modify `body.bin` (the main firmware `base.dat` loads and jumps to with, per the traced sequence,
  no verification of its own). Whatever the silicon does or doesn't check about `base.dat`'s authenticity
  is irrelevant once `base.dat` itself unconditionally trusts what it decompresses next.

**Conclusion: there is no Bug to find, because there is no access-control boundary to cross.**
The firmware-update mechanism — SD card → checksum-only validation → flash → run on next boot — already
*is* an unauthenticated custom-code loading primitive by design (or rather, by the complete absence of a
design decision to prevent it). Goal (a)'s "read and run custom code via the SD-card interface" and goal
(b)'s "custom firmware" converge on the same mechanism, at two different granularities:

- Use the confirmed-unauthenticated update path **once** to flash a `body.bin` containing one small,
  deliberate addition: a loader that watches the SD card for a specific file/folder and, when present,
  loads and runs it as a raw code blob at runtime.
- After that one flash, "installing an app" becomes exactly goal (a)'s ergonomics — drop a file on the SD
  card, no reflashing, no further firmware modification — without ever needing to find or Use an
  actual bug, because the loader we added *is* the intentional trigger mechanism.

This is a better target architecture than either option as originally posed: goal (b) alone (a fully
custom firmware image per app) would mean rebuilding and reflashing the whole image for every app, which
is heavy and slow to iterate on; goal (a) alone (find a a robustness bug bug) is a genuine unknown that
may or may not exist and could take arbitrarily long to find. "Flash the loader once, then use it like
goal (a) forever after" gets the goal (a) experience on a foundation that's already proven to work.

## What's still genuinely unconfirmed (don't overclaim past this)

- **Not yet tested live.** Everything above is a static-analysis conclusion — a real end-to-end test
  (build a minimally-modified `body.bin`, repackage it with a self-computed MD5, and get a real IC-7300 to
  accept and boot it) has not been attempted. This is the single highest-value next milestone: it's cheap
  to attempt and would convert "the security model appears absent" into "we did it."
- **The DSP/FPGA container components must stay untouched.** Their compression schemes aren't fully
  reverse-engineered (FPGA bitstream compression specifically is still undecoded — see
  `notes/multi-cpu-images.md`), so any custom container must carry the *original, unmodified* bytes for
  those components and only touch `body.bin`. Not expected to be a problem (there's no need to touch them
  for this goal), just worth stating as a hard constraint on how a custom container gets built.
- **No packer/compressor exists yet in `tools/icom_fw`** — only `lzss.decompress`/`container.parse`. A
  real custom-firmware build needs the *inverse* operations (LZSS compress, container pack) plus a
  self-computed MD5 — a well-scoped, concrete piece of engineering, not a research question.
- **The"app" injection point/API design is still an open design question**, not just an engineering one —
  see the roadmap below.

## Roadmap

Ordered by leverage/effort, not strict dependency (some phases can run in parallel):

### Phase 0 — prove the update mechanism really is unauthenticated (highest leverage, do first)
Build the minimum viable test: unpack a real release with the existing `tools/icom_fw` code, flip a few
harmless bytes in `body.bin` (e.g. a string constant, not executable code — lowest risk of bricking
anything if some other check *is* found), recompute the MD5, repack into a valid container (needs the
packer/compressor gap above filled first — small task), and attempt a real SD-card update on real
hardware. Success = the radio accepts and boots the modified image. This single test either confirms the
whole reframing above or surfaces a check none of the static analysis found — genuinely worth knowing
before investing further. **Needs**: real hardware access (already available — this isn't gated on JTAG).

### Phase 1 — build the missing tooling
- `tools/icom_fw`: add `lzss.compress()` (inverse of the already-solved decompressor) and
  `container.pack()` (inverse of `parse()`), plus a small "rebuild a container from an unpacked directory,
  recomputing all checksums" CLI command. This is what Phase 0 actually needs, and every later phase
  depends on it too.
- A minimal ARM/Thumb assembler or cross-compiler setup targeting this exact environment (no OS, no libc,
  raw physical/RAM addressing, ARMv7-A userspace-under-FreeRTOS per `notes/kernel-rtos.md`) — GCC's
  `arm-none-eabi` toolchain (already used for `objdump` in this project, see
  `notes/icom-ic7300-re-project.md`'s tooling notes) can very likely serve as the actual compiler; the
  real work is producing a correct linker script/memory layout matching this firmware's real RAM map
  (`notes/memory-map.md`) and calling convention/register-save expectations at whatever hook point is
  chosen.

### Phase 2 — design the injection point and app-loading mechanism
Concrete open design questions, needing a firm answer before writing the loader itself:
- **Where does the loader hook in?** Candidates already surfaced by this project's own RTOS work: a
  currently-unused task slot (`kernel_start`'s own still-unidentified mystery task, descriptor
  `0x203907c4`, has no static writer — plausibly safe to repurpose, though its real intended
  purpose should be understood first so repurposing it doesn't break something); a new task added
  alongside the existing catalog (see `notes/kernel-rtos.md`'s task catalog — the mechanism for adding a
  task, `itron_act_tsk`, is fully understood); or piggybacking on an existing SD-menu item
  (`sd_menu_dispatch_task`, already identified as a 42-case dispatcher) so "run app" appears as a real menu
  choice rather than something automatic/hidden.
- **Where does app code live in memory, and how is it invoked?** A raw position-independent (or
  fixed-address-relocated, since we control exactly what firmware version this targets) machine-code blob,
  loaded from the SD card into a known-safe RAM region (the RZ/A1H's real, datasheet-confirmed 10 MB
  on-chip RAM — this project already found and mapped substantial *unused* headroom beyond `body.bin`'s
  own ~3.7 MB static image when chasing `kernel_start`'s mystery task and the band-scope "hot window" —
  see `notes/band-scope-state.md`/`notes/kernel-rtos-history.md`), then jumped into with a minimal, known
  calling convention (e.g. always entered as `void app_main(void)` with no arguments, matching how
  `itron_act_tsk`'s own task-entry trampolines work).
- **What can an app safely do?** Needs a small, deliberate "SDK" — even just a header file of confirmed
  register/RAM addresses and calling conventions for the things this project has already fully reverse
  engineered (CI-V TX via the confirmed `civ_frame_rx_statemachine`/TX-buffer mechanism, display/EGL
  surface per the graphics-stack findings, SD-card file I/O via `vfs_open`/`vfs_read_record`/etc. already
  named from the file-RPC investigation, frequency/mode read via `FUN_200623bc`-style helpers) — so writing
  an app doesn't require re-deriving the whole firmware's internals from scratch each time.
- **Safety valve**: given this permanently modifies the running firmware, the loader itself should
  fail closed (missing/corrupt app file → do nothing, don't hang or crash normal radio operation) — this
  is a real design requirement, not an afterthought, given the radio is presumably still used as a radio.

### Phase 3 — build and ship the first real app
Once Phase 2's hook exists and boots correctly with a trivial "do nothing" or "blink something obvious"
app loaded from SD card, iterate toward something genuinely useful, informed by what this project already
understands well (CI-V, the UI icon/graphics system, SD-card file I/O, the band-scope/spectrum data).

### JTAG as an accelerant across every phase, not a separate path
Hardware ordered, pins confirmed on both the IC-7300 and IC-9700 (see
`notes/icom-ic7300-re-project.md`/`notes/hardware-debug-access.md`) — not yet arrived as of this writing.
Once available, it doesn't replace any of the above, but sharply de-risks it: Phase 0's live test becomes
reversible (halt-and-inspect instead of "flash and hope," and a hard brick can potentially be recovered by
reprogramming flash directly over JTAG rather than needing the update mechanism to still work); Phase 2's
injection-point questions (what's really safe to repurpose, what a "hot" unused RAM region's actual
runtime contents look like) become directly observable instead of inferred from static analysis; and
early app iterations can be tested by directly writing code into RAM and redirecting execution, without
needing a full rebuild-repackage-reflash cycle each time.

## Secondary track: genuine a robustness bug surfaces (goal (a) in the strict sense)

Not needed to achieve the core goal (see reframing above), but worth keeping as a lower-priority,
opportunistic track — useful if a lighter-weight, no-firmware-modification-at-all trigger is ever wanted
(e.g. proving a concept on a radio you don't want to touch the firmware of at all), or just as an
interesting standalone question. Candidate surfaces, roughly ordered by how much The owner-reachable,
complex parsing they involve — none of these have been examined with an eye toward robustness yet,
this is a survey of *where to look*, not a finding:

- **The SD-card filesystem driver** — `"RENESAS RZ/A1 SD Driver Ver4.01"` (`notes/kernel-rtos.md`),
  almost certainly built on a FAT filesystem middleware (ChaN's FatFs is the near-universal choice for
  this class of Renesas BSP). Parsing untrusted FAT structures (long-filename entries, directory
  chains) from a crafted SD card is a classic embedded Bug class; whether this specific
  driver/version has known issues, or whether Icom's integration constrains the attack surface, is
  unexamined.
- **CI-V/REMOTE and USB (SCIF0)** — now that the real command dispatcher is fully mapped
  (`notes/kernel-rtos.md`'s CI-V section), per-command handlers that accept string/text data (memory
  names, opening message text, CW message send, RTTY memory content — see the CI-V manual's command
  table) are the more promising sub-targets than the framing/dispatch layer itself, which already showed
  disciplined min/max length bounds-checking before handlers are even called.
- **The factory-file loader** (`factory_file_load`/`factory_file_verify_md5`/`factory_file_case28_report`,
  `notes/kernel-rtos-history.md`) — parses SD-card files during service mode. Real attack surface, but
  gated behind already-understood service-mode entry (front-panel MENU+FUNCTION + REMOTE-jack short at
  boot) — a real precondition, not a blocker, since that mode's entry is fully reverse-engineered.
- **Voice-memory / RTTY-log file I/O** (`voice_file_io_state_machine`,
  `notes/kernel-rtos.md`'s task catalog) — mostly *writes* The owner-uncontrolled data (recordings, decode
  logs), so lower priority as an input-parsing surface, but the read side (playback) wasn't specifically
  audited.

None of these need to be resolved for the roadmap above to proceed — they're recorded here so a future
session doesn't need to reconstruct "where would we even look" from scratch if this track is ever picked
up.
