# Open question: how many processor images does the container hold?

The IC-7300 has more than one processor: the main Renesas RZ/A1H application CPU, a DSP (`IC901`, TI
**TMS320C6745**, a TMS320C674x-class VLIW core, self-booting from its own SPI flash `IC902`), an FPGA
(`IC1351`, Altera/Intel Cyclone IV E `EP4CE55F23I7N`), and the front panel's own MCU (`IC501`, Renesas
RL78 `R5F104LCAFB`, marked `SX-3765C-1` — see [[front-panel-firmware]] for that thread). This note tracks
what's actually packaged inside the single `7300_1XX.dat` update container for the non-main-CPU chips, and
how each one actually gets its firmware.

See [notes/multi-cpu-images-history.md](multi-cpu-images-history.md) for the full session-by-session
narrative and evidence trail.

## Current understanding (as of the 2026-08-29 sessions)

**Container structure, solved.** Beyond the main body (`chunk1`/`chunk2` fonts + `chunk3`, all flashed
together in one bulk write bounded by header field `size1`), `firmware_update_main` reads header fields
`size2`..`size7` to build **3 separate LZSS-compressed, MD5-verified sub-components**, computed as:
```
component0_offset = size1 + 0x3c
component1_offset = component0_offset + size2 + 0x10
component2_offset = component1_offset + size4 + 0x10
```
Verified byte-for-byte (exact LZSS-stream consumption + exact MD5 match) against a real v1.42 container.
This supersedes `tools/icom_fw/container.py`'s old `chunk4`/`chunk5-tail` model for this region — a
reasonable early guess from raw byte-accounting, but the real firmware recognizes no chunk4/chunk5
boundary; these 3 components just happen to span across where that boundary falls. New tool:
`tools/icom_fw/dsp_chunks.py`. Extracted files live in `scratch/unpacked/142/`: `front_cpu.bin`,
`dsp_program.bin`, `dsp_data.bin` (component ordering is a working hypothesis from label-order, never
independently confirmed component-by-component).

| Component | File offset (v1.42) | Compressed | Decompressed | Best-current identity |
|---|---|---|---|---|
| 0 (`front_cpu.bin`, internal name — **confirmed wrong**, see below) | `0x252c2c` | 97,862 B | 163,592 B | **`DSP Program`** — confirmed genuine TMS320C674x object code; identity confirmed 2026-08-30 by correlating its own decompressed-content-change points against Icom's officially-published per-release `DSP Program` version field (see below) — every single transition matches |
| 1 (`dsp_program.bin`, internal name — **confirmed wrong**, see below) | `0x26aa82` | 721,836 B | 720,648 B | **`DSP Data`** — confirmed genuine TMS320C674x (DSP) object code (real floating-point C67x+/C674x-specific instruction chains, cross-validated by 3 independent disassemblers), yet its identity tracks Icom's `DSP Data` version field, not `DSP Program` (see below) — Icom's "Program"/"Data" naming apparently isn't a code-vs-non-code distinction, more likely main-application-vs-secondary/rarely-updated-image |
| 2 (`dsp_data.bin`, internal name — **confirmed wrong**, see below) | `0x31ae3e` | 698,201 B | 859,412 B | **`FPGA`** — not code (no coherent instruction chains found); already the best-supported guess from whole-file-histogram/size/preamble evidence (compressed Altera bitstream for `IC1351`), now **strongly confirmed** by version-field correlation (see below) on top of the earlier circumstantial evidence |

**Component identity fully resolved, 2026-08-30, by correlating against Icom's own published sub-component
version history** (the same official EN+JP scrape done for the IC-9700 thread, see
[[ic9700-container-format]] — the IC-7300's own firmware-detail pages turned out to carry the identical
kind of per-release 4-way breakdown: `Main CPU`/`DSP Program`/`DSP Data`/`FPGA`). Extracted and hashed each
of the 3 components' actual decompressed content (not just compressed size, which can drift slightly
without ruling identity) across every locally-held release (`v1.11`–`v1.20`, spanning the only window where
any of these 3 components still changed at all — see [[firmware-versions]]) and compared the resulting
change-points against Icom's own version numbers for each release:

| Transition | `DSP Program` (official) | `DSP Data` (official) | `FPGA` (official) | component0 content | component1 content | component2 content |
|---|---|---|---|---|---|---|
| 1.11→1.12 | 1.05→1.06 (changed) | 1.00→1.00 (same) | 1.10→1.11 (changed) | **changed** | same | **changed** |
| 1.12→1.13 | 1.06→1.07 (changed) | 1.00→1.00 (same) | 1.11→1.12 (changed) | **changed** | same | **changed** |
| 1.13→1.14 | 1.07→1.07 (same) | 1.00→1.00 (same) | 1.12→1.13 (changed) | same | same | **changed** |
| 1.14→1.20 | 1.07→1.07 (same) | 1.00→1.00 (same) | 1.13→1.13 (same) | same | same | same |
| 1.20→1.42 | 1.07→1.07 (same) | 1.00→1.00 (same) | 1.13→1.13 (same) | same | same | same |

A perfect, zero-discrepancy match on all 5 checked transitions for all 3 components simultaneously —
component0's content-change points line up exactly with `DSP Program`'s, component1 is byte-identical
across the *entire* known dataset (matching `DSP Data`'s permanently-pinned `1.00`), and component2's
matches `FPGA`'s precisely (already the working hypothesis, now this solidly confirms it). This is the
opposite pairing from `dsp_chunks.py`'s internal working names (`front_cpu`→really `dsp_program`;
`dsp_program`→really `dsp_data`) — **those internal filenames/variable names are now known to be swapped
and wrong**, not yet renamed in the tool itself (a real but low-priority cleanup — the extraction offsets
themselves are correct and unaffected, only the labels are wrong).

One genuine surprise this leaves standing: component1 is **confirmed real, executable TMS320C674x object
code** (not a calibration table) yet identity-tracks `DSP Data`, not `DSP Program`. Icom's "Program" vs.
"Data" naming apparently isn't a code-vs-non-code split — more likely "Program" = the main/frequently
revised application image and "Data" = a secondary DSP-side image (boot monitor? resident kernel? a second
overlay?) that happens to have gone unrevised for this product's entire locally-held release history. Not
resolved further; a real follow-up question, not a loose end from bad evidence.

**"Component0 = Front CPU firmware" is retracted.** The original hypothesis (from `FUN_200a94c8`'s 5-field
version-compare struct order — see below — assumed to map 1:1 onto the 3 update chunks) is contradicted two
independent ways: (1) tracing `chunk_transport_send_data`'s full call chain shows **all 3** chunk indices
are transported identically over `SCIF5` — there is no separate path to the front panel's own known link
(SCIF3); (2) `front_cpu.bin` disassembles as genuine TMS320C674x code (real `spdp`/`absdp`/`cmpltdp`
floating-point chains, real branch/call idioms) — not RL78 code, a completely different and much
smaller-flash architecture. So `component0` is genuinely DSP-side, not front-panel-side. **Resolved,
2026-08-30**: it's `DSP Program` specifically, confirmed via version-field correlation against Icom's own
published data — see the table above.

**`IC902` identity — corrected twice, now settled at the pin level.** Final: `IC902` (`EN25QH32A`) is
**`IC901`'s (the DSP's) own dedicated SPI0 boot flash** — its `CS`/`DO`/`DI`/`CLK` pins trace directly to
DSP pins 9/17/18/11, which double as `BOOT[4:0]` mode-select straps, the textbook "boot from SPI flash"
wiring. (Superseded along the way, kept for history only: originally assumed to be the DSP's flash purely
from board proximity; then corrected to "the FPGA's own config flash" based on net-label tracing to a
`DONE`/`STAT`/`CFG` signal cluster.) That FPGA-config observation isn't wrong, just one hop removed: the
FPGA is most likely configured *by the DSP*, once self-booted from `IC902`, not directly by the flash chip.
The DSP self-boots autonomously from `IC902` and reprograms it itself during a firmware update, driven by
commands over `SCIF5` — the main CPU has no other bus access to this flash.

**`SCIF5` (`0xE8009800`) — the real, physical DSP link, fully mapped.** Physical pins: `P8_0`/`P8_1`/`P8_2`
(previously misread as a second DSP McASP1 audio link — corrected, see [[ic7300-signal-chain]]). Armed
unconditionally every boot (`cold_boot_hw_init`). Carries 3 distinct traffic classes over the same
transport:
- **Firmware-update chunk transfer** — `chunk_transport_send_data`/`chunk_transport_send_reload_cmd`
  (single caller each, inside `firmware_update_main`), page-verified via `dsp_page_transfer_verify`
  (checksum + destination-address echo, per 256-byte page).
- **Live parameter sync** (`dsp_param_sync_tick`) — a 24-entry table (`dsp_cmd_table_init`), diffed against
  a shadow copy every tick, changed slots pushed live. Confirms the DSP is an actively-running core in
  normal operation, not held in reset. The ~22 individual parameters (mode/filter/AGC-shaped, per working
  hypothesis) aren't individually named yet. **2026-09-07**: found the real rebuild trigger —
  `dsp_param_table_rebuild_from_settings` (`0x200b232c`, from the RTTY-settings-usage side of
  `notes/kernel-rtos-history.md`'s RTTY/SSTV thread) recomputes all 22 live words from the big DSP-config
  struct at `0x203def00` (the same struct already known to feed `factory_file_load`'s identity check —
  turns out to be a much bigger general settings block, not just 3 identity records) whenever a dirty flag
  (`DAT_200b2b18`) is set, including per-operating-mode-indexed fields and mode-index-4-only global fields
  (a real RTTY candidate). Confirms this mechanism really is how live settings (RTTY's included) reach the
  DSP — no per-field writer found yet (same EEPROM-shadow pattern as elsewhere), but the consumer side of
  the ~22-parameter question is now concretely answered even though individual slot identities still
  aren't all pinned down.
- **Identity/version query** (`dsp_identity_query_cmd0`-`cmd5`) — 3 formatted version-ish records,
  confirmed consumed by `factory_file_load` (gates whether a stored factory/calibration file matches the
  currently-installed DSP) — **not** by `FUN_200a94c8` as originally guessed.

**`FUN_200a94c8`'s version-compare struct — field mapping confirmed, but the storage question dissolved
rather than resolved.** The 5 fields really are, in order, `+0xa0`=Main CPU, `+0xa4`=Front CPU,
`+0xa8`=DSP Program, `+0xac`=DSP Data, `+0xb0`=FPGA (confirmed by dereferencing the live label-string
array). But its backing struct (`0x203ff76c`) isn't a dedicated "update file info" struct at all — once
Ghidra's memory map was extended to cover it, its real writes turned out to belong to unrelated menu
screens (band-scope, keypad entry). It's a generic, heavily-multiplexed "settings candidate" scratch
buffer reused by whichever screen currently owns it, architecturally identical to `DAT_200a9ba8`'s target
— see [[band-scope-state]]. There was never a single dedicated writer to find; "which screen currently
owns this buffer" is the only question that ever made sense here.

**`DRESD` (DSP hardware reset) release — still not located.** Confirmed driven low once at boot
(`port_bulk_gpio_init_pass2`) and never touched again anywhere traced in `body.bin` — checked for a net
inverter (none, purely resistive), a tri-state/direction release via `PM2` (ruled out), and every
neighboring boot-init call (all ruled out). Since the DSP is clearly running and receiving live `SCIF5`
traffic, either the release happens somewhere not yet traced (the `base.dat` boot-ROM stage, which runs
before `body.bin`, is the next candidate — see [[base-loader]]) or it only ever needs releasing once and
the DSP manages everything else autonomously from there.

**DSP disassembly is possible** — Ghidra has no SLEIGH module for TMS320C6000/C674x (confirmed, still an
open upstream feature request), but **binutils' `tic6x` target, Capstone's `TMS320C64X`, and TI's own
`dis6x`** (from the official Code Generation Tools installer) all work against raw dumps once endianness
and silicon-version are set up correctly, and all three agree byte-for-byte on decoded immediates. Recipes
(ELF-wrapping raw binary, hand-built `.c6xabi.attributes` section for `dis6x`) are in the history file.

**Container/companion-chip housekeeping, settled:**
- `SX3765` strings are compatibility/version references to the **front panel's own RL78 MCU** (`IC501`,
  confirmed via the service manual's Display Unit parts list) — not an embedded firmware image.
- The 64 MB SPI flash holds **two complete copies of the container** (A/B slots at `0x18010000`/
  `0x18400000`), and `"SX3765 Vx.xx-xxx"` at `0x187f0000` is a generation/active-slot marker checked by
  both the boot loader and the running firmware (`FUN_20062c64`) — not evidence of a companion-chip image.

## Retracted or superseded along the way (kept for history only)

- Early belief that no C6x disassembler exists anywhere (only custom-disassembler/live-JTAG were viable) —
  retracted; real tools exist upstream (binutils `tic6x`, Capstone `TMS320C64X`, TI's own `dis6x`).
- Reading component2's (internally named `dsp_data.bin`, really `FPGA`) leading 64 bytes as small
  calibration constants — superseded by the whole-file histogram/size/preamble evidence for an Altera FPGA
  bitstream, itself now confirmed by version-field correlation (see above).
- The original boot-loader-only read of the dual-flash-slot mechanism (via `base.dat`'s
  `unpack_from_flash_to_mem`) — superseded by the fuller confirmation via `body.bin`'s own
  `FUN_20062c64` (same conclusion, firmer evidence).
- The `FUN_20025044` runtime-populated RAM-destination-table investigation (whether/where `chunk4`/
  `chunk5-tail`'s bytes land in RAM) — mooted once `SCIF5` tracing showed the data is addressed by the
  DSP-side protocol, not a literal RAM buffer with a findable static destination.
- The `0xb0`/`0xe2` ring-buffer tag range was chased as a possible RSPI2/SSIF-audio/front-panel-UART
  consumer (all 3 ruled out) before the real consumer (`chunk_transport_send_data` →
  `dsp_page_transfer_verify` → `SCIF5`) was found directly.

## Open questions / next steps

- Why component1 (confirmed real DSP object code) identity-tracks the `DSP Data` label rather than
  `DSP Program` — a naming-semantics question, not a structural unknown (see the resolved-identity section
  above).
- Where the front panel's (`IC501`, RL78) own firmware actually lives/updates from, if not these 3 chunks
  — a separate, freshly-opened thread, see [[front-panel-firmware]].
- `DRESD` release mechanism — check the `base.dat` boot-ROM stage (see [[base-loader]]), or live JTAG once
  available.
- Altera's proprietary bitstream compression scheme for component2/`FPGA` — not identified/decoded.
  ~~Component ordering never independently confirmed field-by-field~~ — **resolved, 2026-08-30**: real
  identity of all 3 components confirmed via version-field correlation (component0=`DSP Program`,
  component1=`DSP Data`, component2=`FPGA`) — see above. `tools/icom_fw/dsp_chunks.py`'s internal
  `front_cpu`/`dsp_program`/`dsp_data` names are consequently known to be wrong (swapped for 0/1) and
  worth renaming next time that file is touched.
- The ~22 individual DSP live-sync parameters not yet individually named — trace each `dsp_cmd_table_init`
  slot's real data source.
- `dsp_boot_handshake`'s 2 boot-time command words, and `SCIF5` events `0x9f`/underrun handling, not fully
  decoded.
- `chunk3` (in the main body) has zero found code references anywhere in the version checked — genuinely
  unread by any code Ghidra has resolved, or a static-analysis blind spot.
- `snip` (a 1.5 MB unexplained file with no notes on its origin) — never actually asked about directly;
  worth asking the user rather than continuing to guess.
- Live JTAG access (once available) would likely resolve most of the above quickly — watching `SCIF5`
  traffic and `P2`/`PPR2` bit 6 during boot, or the DSP's own reply classes, live.
