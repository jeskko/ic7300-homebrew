# The firmware update mechanism (SD-card based, dual A/B flash slots)

Traced from `body.bin` (rebased to `0x20005000`, see [[base-loader]]) by
following live code from the reset handler onward, then jumping to the
SPI-flash driver once literal-pool xrefs to `SPI_BASE` (`0x3fefa000`, read
directly from `base.dat` on disk — same register `base.dat`'s own
`setup_spi`/`config_spi` use, see [[base-loader]]) turned up real callers.
All addresses below are v1.42; offsets should hold across versions.

See [notes/firmware-update-history.md](firmware-update-history.md) for the full session-by-session narrative and evidence trail.

## Confirmed: user-facing messages establish the flow

Message-table strings (data-only region `~0x2032f000`–`0x20360000`, no
static xrefs — reached via a computed/indexed table, not direct pointers,
so found by string search rather than xref-walking):

1. *"...firmware update. Making a backup file of programmed contents and
   settings onto the SD Card before updating is recommended... Do you
   agree to all of the above?"*
2. *"...restart. NEVER turn OFF the IC-7300 until the frequency screen is
   displayed. Also, NEVER remove the SD Card... Do you wish to start the
   firmware update?"*

Confirms: update source is a file on the **SD card**, with a two-step
confirmation UX and an explicit backup-recommendation step.

## Real hardware confirmation of the whole sequence, from a YouTube recording (2026-09-08)

User found a video of a real IC-7300 firmware update (one where the DSP version actually changes) and
reported the exact on-screen sequence. This lines up cleanly with the traced code, message-for-message, and
is a genuine, valuable external confirmation of this session's own conclusion that the DSP/FPGA push happens
*before* any reboot, in the same session as the main-body flash write — not a separate boot-time
reconciliation step (see `notes/front-panel-firmware-history.md`'s "Does a version mismatch actually trigger
a firmware push?" thread, which reached the same conclusion from static analysis alone).

Observed sequence, mapped onto the traced code:

1. *"...Do you wish to start the firmware update?"* — hold the on-screen YES button ~1 sec to confirm.
   Matches the two-step confirmation UX already documented above.
2. **"Checking the file. Please wait..." with a progress bar.** Maps to the header read (`size1`..`size7`)
   plus the up-front MD5 verification pass over the file before any flash write begins — see "Precise
   checksum timing in `firmware_update_main`" below. A real progress bar here makes sense: hashing a
   multi-hundred-KB-to-MB file over SD-card I/O takes real, visible time.
3. **"Updating MAIN CPU firmware. Please wait for 15sec." with a countdown.** Maps to the two
   `flash_write_chunked_from_file` calls: the shared bootloader region (`0`-`0x10000`, always rewritten)
   then the main body to whichever A/B slot isn't currently active.
4. **At ~9 seconds into that countdown, the dialog switches to "Updating DSP/FPGA firmware. Please wait for
   29sec." with its own countdown.** This is the direct, physical confirmation: the `chunk_needs_update`
   loop and `chunk_transport_send_data` (the 3 "extra chunks") run in the *same update session*, right
   after the main-body write, with **no intervening reboot** — exactly what this project's static analysis
   found independently. Also confirms component0/1/2 are presented to the user as one combined
   "DSP/FPGA" phase, not separate sub-phases (matching the single shared progress percentage,
   `*(DAT_2002649c+0x50)`, `firmware_update_main` updates throughout this whole loop).
5. **"Firmware updating has completed. The IC-7300 will automatically restart."** Maps to
   `chunk_transport_send_reload_cmd` (sent when component0 or DSP Data changed) plus
   `flash_write_active_slot_marker`, followed by the genuine watchdog-forced reset already confirmed as
   this firmware's restart primitive (see "The system restart mechanism" in history).
6. **The radio power-cycles (power LED goes dark briefly) then returns to the frequency/home screen.** A
   short LED-dark blip rather than a full cold power-cycle is consistent with a watchdog-triggered *warm*
   reset, not a full power-off — and gives real-world grounding to the `"Fup_AutoEnd_3765"` marker
   mechanism (written to the top of RAM just before this exact kind of reset, checked for on the very next
   boot; full derivation in history): this is precisely the reset event that marker is
   designed to survive and detect. Its consumer side is still an unresolved dead end (see history),
   but this confirms the reset event itself is real and exactly as hypothesized, not a full power-cycle
   that would wipe RAM.

## `FUN_200aa750` IS the real update-dialog renderer — full chain confirmed, correcting the section below (2026-09-08)

**This corrects the "not it" conclusion originally written in this section (kept below, struck through in
spirit but left for the record) — the user pushed further with direct memory reads and found the real
connection this session's own investigation missed.**

User's hunch: `FUN_200aa750` (called from `FUN_200ab148`) might render the update-progress dialogs from
the video sequence above, since it has progress-bar-shaped code and is called from the same top-level
screen-dispatcher tail that also runs `ui_version_screen_draw_and_compare`. First pass traced its dispatch
state to `FUN_20038450`'s menu-item-property mapping and concluded (wrongly) that this was unrelated to
firmware updates — a coincidental value match confused two different numbering axes (menu-*item-index* vs.
item-*property-byte*, which happen to share small integers like `0x1c` for unrelated items).

**User found the real evidence directly**: read `DAT_200ab2d0` (the array `FUN_200aa750`'s specific
`uVar9 = *(byte*)(DAT_200ab2d0 + property_byte*0x4c)` line indexes) and found it resolves to `0x2032c91c`
— exactly the base of the real message-string table this file's opening section already knew held
`"Checking the file"`/`"Updating MAIN CPU firmware"`/`"Updating DSP/FPGA firmware"`. Computing the exact
record offsets confirmed it precisely: property byte `0x1c` (record index 28, stride `0x4c`) lands exactly
on `"Updating MAIN CPU firmware."`; `0x1d` (record 29) lands exactly on `"Updating DSP/FPGA firmware."` —
both at the record's `+4` field, both landing at the identical remainder within the stride, no fudging.

**User's second find, mid-session: the dialogs are genuinely bilingual.** Found a Shift-JIS string at
`0x2035fb94` decoding to `"DSP/FPGAのファームウェアを書き換えて"` ("...is rewriting the DSP/FPGA
firmware...") and noticed the record at `0x2032d1bc` (= record 29's own body) has *two* language pointers,
not one. Dumping the full 76-byte record for both items confirmed the real layout — `{flag(4), english_ptr
(+4), ...unused (7 slots)..., japanese_ptr(+0x24), japanese_suffix_ptr(+0x28), ...unused (7 slots)...}` —
and decoding the Japanese pointers directly gives an exact match for both items:
*(Layout corrected 2026-09-25, see `notes/ui-menu.md` "Popup message dialogs": the record is
`{flags, en[9] at +0x04, jp[9] at +0x28}`; the "Japanese pointer + suffix" below are simply
Japanese text lines 0 and 1.)*
- Item/record `0x1c` (`0x47`, see below): English `"Updating MAIN CPU firmware."`, Japanese
  `"メインCPUのファームウェアを書き換えて"` + shared suffix `"います。"`.
- Item/record `0x1d` (`0x48`): English `"Updating DSP/FPGA firmware."`, Japanese
  `"DSP/FPGAのファームウェアを書き換えて"` + the same shared suffix `"います。"`.

**Resolved the earlier confusion by checking the real numbering axes separately.** The property byte
(`0x1c`/`0x1d`) that indexes into `g_status_message_table` is a *different* number from the menu-*item
index* passed to the activation APIs. Scanned the per-item table (`0x2018b8f0`, stride `0x10`) for items
whose own property byte (`+6`) equals `0x1c`/`0x1d`/`0x1e`, and found a clean, consecutive trio:

| Item index | Property byte | Message (English) |
|---|---|---|
| `0x47` | `0x1c` | `"Updating MAIN CPU firmware."` |
| `0x48` | `0x1d` | `"Updating DSP/FPGA firmware."` |
| `0x49` | `0x1e` | `"Firmware updating has completed."` |

**Found the real activation call sites** by searching the raw disassembly for `mov r0, #0x47`/`#0x48`
immediately followed by a call to the activation API (the same literal-value-search technique used
elsewhere this session) — both land inside `FUN_2005b2a0`, the same function already found this session
while tracing the `"Fup_AutoEnd_3765"` marker's trigger-setter (previously named
`mode_transition_sequencer_fup_autoend_setter`, **renamed `firmware_update_progress_dialog_sequencer`** —
its real identity was this the whole time). Its states map exactly onto the observed sequence:
- State `0x3b`→`0x3c` transition activates item `0x47` ("Updating MAIN CPU firmware").
- State `0x3c` activates item `0x48` ("Updating DSP/FPGA firmware") — but only when a flag,
  `g_fup_dsp_phase_sync_flag` (`0x20390308` — a byte in the *same* small cluster as
  `g_fup_autoend_trigger_flag`), equals `2`. State `0x3c` then unconditionally advances to `0x3d` and *also*
  sets `g_fup_autoend_trigger_flag` — the two threads (update-progress dialogs, post-reboot marker) turn out
  to be driven by the exact same function, one state apart.
- State `0x3d` activates item `0x49` ("Firmware updating has completed.").

**The full synchronization with `firmware_update_main` itself, confirmed by direct listing, not inferred**:
`g_fup_dsp_phase_sync_flag`'s real writer is inside `firmware_update_main`'s own body (`0x20025f44`) —
`*(byte*)0x20390308 = 2`, written right after the `chunk_needs_update` loop, gated so it only fires when a
DSP/FPGA chunk actually needs updating. `firmware_update_main` then **busy-waits** on this same byte
(`0x20025f5c`-`64`: `do {} while (*(byte*)0x20390308 == 2);`) until `firmware_update_progress_dialog_
sequencer` (running as a separate task) consumes it in its own state `0x3c`. This is a real, literal
task-to-task handshake — not a guess.

**This is now a fully closed loop**, tying together three previously-separate threads from this project:
the real update-dialog message table (bilingual, exact-match confirmed), the real dialog-activation call
chain (`ui_show_message_dialog`/`ui_show_message_dialog_with_timeout` → `ui_activate_menu_item` →
`ui_active_item_category_dispatch` → `ui_status_notification_tick` → `ui_status_message_render_rows`,
functions renamed this session), and the `"Fup_AutoEnd_3765"` post-update-reboot marker (whose trigger-
setter turns out to be this exact same sequencer function). Renamed 8 functions/globals in Ghidra to match;
see each one's own plate comment for the full derivation.

**What's still open**: the "Checking the file." message (property/index `0x61`) hasn't been traced to its
own activating item or call site the same way — a natural, well-scoped next step using the exact same
technique (find which item has property byte `0x61`, then search the disassembly for `mov r0, #<that item>`
followed by a call to the activation APIs). Also open: exactly what sets `g_fup_dsp_phase_sync_flag` to `1`
(for the "Main CPU" phase, item `0x47`) — only the `=2` write (DSP/FPGA phase) was found inside
`firmware_update_main`; the "Main CPU" activation might be triggered unconditionally by a different,
earlier point in the same function, or by whatever calls `firmware_update_main` in the first place.

## Checked a real lead for the update-progress dialog renderer — not it, but real infrastructure found (2026-09-08) — SUPERSEDED, see section above

Superseded — see the confirmed `FUN_200aa750` chain above; full derivation in history.

## The SPI-NOR flash driver (`0x20024xxx`)

- `FUN_20024850` — driver init (SPI controller bring-up, same register
  base as `base.dat`'s boot-time `setup_spi`).
- `FUN_20024a60(dest_addr, src_data, len)` — **page program**: for each
  4-byte word that isn't `0xffffffff` (skips words already matching the
  erased state), issues write-enable, sets target address, writes data
  respecting the 256-byte page boundary (`addr & 0xff < 0xfc`), polling
  the controller ready bit throughout.
- `FUN_20024bb8(start_addr, end_addr)` — **block erase**: write-enable,
  then issues SPI opcode `0xD8` (standard 64 KB block erase) at each
  address, advancing by `0x10000` until past `end_addr`.
- `FUN_20024cd0` — driver deinit, restores XIP/read mode.

## The chunked writer: `FUN_20024db8(file, flash_offset, len, ctx, changed_flag)`

Per 64 KB chunk, until `len` is exhausted:
1. Fill a RAM scratch buffer with `0xff` (handles a short final chunk).
2. `FUN_200bc6a4` — read up to 64 KB from the open SD-card file handle
   into the scratch buffer.
3. `FUN_2003d38c` — fold the chunk into a running checksum/CRC context.
4. **`FUN_2017c81e(scratch, flash_offset | 0x18000000, 0x10000)`** —
   `memcmp` the new chunk against the flash's *current* content at that
   XIP address — **skip erase+program entirely if unchanged.**
5. If different: `FUN_20024cd0` → `FUN_20024bb8(flash_offset, flash_offset)`
   (erase this one block) → `FUN_20024a60(flash_offset, scratch, 0x10000)`
   (program it) → `FUN_20024850` (reinit) → set `*changed_flag = 1`.
6. `flash_offset += 0x10000`.

## The marker writer: `FUN_20024d60(slot_flag, marker16)`

Erases and reprograms exactly flash offset **`0x7f0000`**
(`0x18000000 + 0x7f0000 = 0x187f0000` — the same address `base.dat`
checks at boot, see [[base-loader]] and [[multi-cpu-images]]) with a
16-byte value. This is the "commit"/active-slot-flip step.

## The orchestrator: `FUN_20025ae4`

The full update sequence:

1. Open the SD-card update file (`FUN_200bc5f4`), then read/seek through
   its header exactly like [[container-format]] documents: 16-byte
   version string, `size1..size7` at offset `0x10`. **Unlike `tunk3.py`,
   this code computes chunk offsets dynamically from the `size*` fields**
   (`local_9c[0] = size1 + 0x3c`, etc.) rather than hardcoding them —
   answers [[container-format]]'s "known risk" open question: the real
   firmware does derive offsets from the header, confirming `size1..size7`
   really are chunk-boundary/size fields, just not yet mapped 1:1 by name.
2. Seeks to file offset **`0x4f2c`**, reads 16 bytes into a buffer. This
   is the file offset the old `tunk.py` note flagged (`"187f000 5dd38 -
   image alkais 18212c8"`, see [[multi-cpu-images]]) — now explained:
   it's **not** an address inside the decompressed body, it's the offset
   *inside the update file* of the 16-byte string this same function later
   flashes as the new `0x187f0000` marker via `FUN_20024d60`. (Falls
   within the boot-loader portion of the container, i.e. inside the new
   `base.dat` blob being shipped — consistent with it being that build's
   own embedded version tag, the same kind of literal pool constant found
   in the *current* `base.dat` at `~0x18004564`.)
3. `FUN_20024db8(file, 0, 0x10000, ...)` — **always** rewrites flash
   offset `0`–`0x10000` first: the shared boot-loader region
   (`base.dat`'s 66000 bytes live here). This region is **not**
   A/B-duplicated — only one boot loader exists in flash.
4. **`if (*DAT_200264a4 == 0) dest = 0x10000; else dest = 0x400000;`** —
   picks the write destination based on a slot flag (same shape and same
   marker-derived logic as [[multi-cpu-images]]'s `FUN_20062c64`, though
   not yet traced to the exact same storage location) — **writes the new
   main body + fonts + chunks into whichever slot ISN'T currently
   active.**
5. `FUN_20024db8(file, dest, size1 - 0x10000, ...)` — writes the rest of
   the body to the chosen slot.
6. If anything actually changed: `FUN_20024d60` writes the new marker at
   `0x187f0000`, flipping the active slot for the *next* boot.
7. Verifies a checksum, then writes up to 3 more optional
   chunks/components (fonts etc., via the same read/compare/write
   pattern) with their own per-chunk checksum verification
   (`FUN_20025044`) before finishing.

## `firmware_update_main`'s own calling convention, and the real SD-card path convention (2026-09-08, `qemu-machine/` dynamic-testing session)

Found while trying to *drive* `firmware_update_main` directly (forcing a call via GDB against
the `qemu-machine/` QEMU port, rather than reaching it through the real SD-menu UI — see
`qemu-machine/README.md`'s "Forcing `firmware_update_main` directly" section for the full
dynamic-testing narrative; this note covers only the durable RE facts that came out of it).

**`firmware_update_main` (`0x20025ae4`) takes no arguments at all.** It opens a single fixed
global path, `DAT_200264a0`, via `FUN_200bc5f4` — not anything passed in by its caller
(`sd_menu_dispatch_task`'s case `0xb`, an ordinary SD-menu dispatch entry — full entry-point trace in history). **`DAT_200264a0` has zero
static writers anywhere in `body.bin`** — genuinely surprising at first (matches this
project's recurring "computed/indexed write defeats direct xref tracing" pattern), until
cross-checked against `tools/icom_fw`'s own LZSS decompressor: the address it points to
(`0x203d86c4`) falls **past the end of the real decompressed body** (`3,738,392` bytes,
ending at RAM `0x20395b18` — confirmed exactly, not approximately). So this isn't a missed
static reference at all — it's genuinely a **runtime-only RAM buffer** (BSS/heap), populated
at runtime by the SD-card file-browser/file-selection UI (an entirely separate subsystem,
not traced here) with whatever full path the user picked from a displayed list, before
`firmware_update_main` ever runs. `firmware_update_main` itself is a pure function of that
one buffer's content at call time.

**The real path convention, found directly in Icom's own published manual** (not derived
from firmware code at all — `pdftotext` of `/data/misc/icom/7300/doc/
IC-7300_ENG_FM_12b.pdf`, section 15 "Updating the firmware"): *"Copy the downloaded firmware
data into the IC-7300 folder on an SD card"*, followed by a file-selection screen showing
the available firmware files by name (e.g. "7300_101") for the user to pick from. This
matches — and now explains the shared convention behind — the already-documented
`C:\IC-7300\Voice`/`C:\IC-7300\VoiceTx` folders `notes/kernel-rtos.md`'s task catalog
records for the voice-recording/playback features: **`IC-7300\` is this firmware's one
shared SD-card feature-folder root**, with per-feature subfolders (`Voice`, `VoiceTx`) for
some features and the update file dropped directly in the root `IC-7300\` folder itself for
firmware updates specifically (per the manual — not independently confirmed against code,
since the actual join happens inside the untraced file-browser UI, not
`firmware_update_main` itself).

**Genuinely still open, not attempted by tracing further**: whether the browser scans
`IC-7300\` for *any* file matching a recognizable firmware-container shape (size/header
sanity, matching "Touch the Firmware (Example: 7300_101)" implying multiple candidates can
be listed) or expects one specific fixed filename — the manual's own wording ("the
Firmware") reads as the former. Also open: the drive-letter/path-separator convention this
custom (non-FatFs, see [[sd-card-filesystem-security]]) VFS layer actually expects for a
directly-supplied path string (`\IC-7300\...` vs `C:\IC-7300\...` vs no prefix at all) —
`qemu-machine/`'s own forced-call testing tried the backslash-only form and got real
progress into the file-RPC layer either way (blocked on something unrelated to path
parsing — see that README section), so this specific detail wasn't pinned down conclusively.

## Precise checksum timing in `firmware_update_main` (traced in full)

The full sequence, addresses/offsets exact:

1. Read `size1` (`local_54`), then `size2..size7` (`local_9c[]`, used to
   compute chunk offsets — dynamically, per [[container-format]]).
2. Seek to file offset `size1 + 0x2c`, read 16 bytes → this is the
   **expected combined checksum** covering the boot-loader region *and*
   the main body together (confirmed by the byte range the running
   checksum actually covers below — it ends exactly here).
3. Seek to `0x4f2c`, read the new active-slot marker value (see above).
4. Seek back to `0x2c` (start of the boot-loader region in the file),
   init a running checksum context (`FUN_2003c860`).
5. `flash_write_chunked_from_file(file, 0, 0x10000, ctx, &changed)` —
   writes flash offset `0`–`0x10000` (the boot loader/`base.dat` region).
   Every chunk read is folded into `ctx` as it goes.
6. `flash_write_chunked_from_file(file, dest, size1-0x10000, ctx,
   &changed)` — writes the body to whichever A/B slot is currently
   inactive. Continues folding into the *same* checksum context — the
   two writes together cover exactly `[0x2c, size1+0x2c)`, matching where
   the expected checksum was read from in step 2.
7. **`if (bVar12)` (`bVar12` = "did the boot-loader chunk actually
   differ from what was already in flash") → `flash_write_active_slot_marker`
   is called HERE — before the checksum is finalized or compared at
   all.**
8. Only now: `FUN_2003d458` finalizes the checksum, then it's compared
   (`memcmp`-equivalent) against the expected value from step 2.
9. If the boot loader was unchanged (`!bVar12`), the marker-write is
   instead deferred to *after* the checksum check passes (further down,
   after additional font/chunk writes with their own per-chunk
   checksums) — correctly gated in this case.

**Conclusion, precise:** flash writes always happen before the checksum
is checked in every case (a failed checksum never rolls back a write —
matches the earlier finding). But the *boot decision* (the marker flip)
is only actually gated by the checksum when the boot loader itself is
unchanged by the update.

**Correction — checked directly, this is not a rare edge case:**
`md5sum`-compared `base.dat` across all 10 real releases
(`/data/misc/icom/7300/7300_1XX/base.dat`) — **every single one has a
different hash.** This isn't necessarily the boot-loader *code*
differing — `base.dat` = `container[0x0:0x101d0]`, which includes the
`size1..size7` header fields, and `size1` (the compressed body's length)
changes on essentially every release regardless of whether the executable
logic changed. Either way, the practical result is the same:
**`bVar12` (marker-flips-before-checksum-check) is true for essentially
every real Icom update, not an occasional corner case.** The
unverified-boot-into-new-slot behavior described above is the normal
path, not a rare one.

## Checksum algorithm: CONFIRMED as plain MD5 (no keying, no signature)

Decompiled `FUN_2003c860`/`FUN_2003d38c`/`FUN_2003d458` (init/update/final
— the checksum context used throughout `firmware_update_main` and
`flash_write_chunked_from_file`). Textbook MD5, confirmed unambiguously:
4×32-bit state, 64-byte block buffer, bit-count length tracking, padding
to 56-mod-64 before an 8-byte length field, little-endian digest output —
and the init constants at `0x2003d280` are MD5's exact standard IV
(`0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476`), byte for byte.

**This settles the security question from "preliminary" to confirmed.**
Not just "MD5 is weak" — structurally, both the payload and its expected
checksum are read from the same attacker-controlled update file
(`auStack_64`, read at `size1+0x2c`, per the exact sequence documented
below). An attacker doesn't need to find an MD5 collision at all: they
compute MD5 of their own malicious payload and write that value into the
checksum field themselves. Any unkeyed hash — MD5, SHA-256, anything —
would fail identically here; the algorithm choice isn't the weakness,
the lack of a keyed MAC or signature is. Combined with the earlier,
already-confirmed finding that no signature/MAC verification exists
anywhere in `firmware_update_main`'s call graph: **custom firmware can be
crafted to pass every validation step in the update process.** No
cryptographic authentication exists anywhere in this path.

## Security implication (superseded by the confirmed section above — kept for history)

Superseded — see the confirmed MD5/no-signature-check finding above ("Checksum algorithm: CONFIRMED as plain MD5"); full narrative in history.

## Conclusion

**Confirms and extends [[multi-cpu-images]]'s dual-slot theory with a
complete, concrete mechanism:**
- One shared boot-loader region at flash `0x0`–`0x10000`
  (`0x18000000`–`0x18010000`).
- Two full "body + fonts + chunks" regions, slot A at `0x10000`
  (`0x18010000`) and slot B at `0x400000` (`0x18400000`).
- A 16-byte generation marker at `0x7f0000` (`0x187f0000`) recording which
  slot is active, written/read identically by the boot loader, the
  running firmware's resource-locator (`FUN_20062c64`), and the updater's
  own slot-selection logic.
- Update always targets the *inactive* slot, verifies as it goes
  (memcmp-skip-if-unchanged, checksums), and only flips the marker after a
  successful write — a standard safe A/B update scheme.

**No embedded companion-chip ("SX3765") firmware image found anywhere in
this path** — the "SX3765 Vx.xx-xxx" string is conclusively a repurposed
marker/tag, not evidence of a second processor's code bundled in the
container. The `chunk5`/tail region ([[multi-cpu-images]]) remains
unexplained by this investigation and is the one open thread left if the
companion-chip question is still worth pursuing — otherwise this closes
the updater investigation and firmware analysis proper can start from a
known-good `body.bin` at the correct base.

## 2026-09-24 — end-to-end repack-and-boot test PASSES in the emulator

First actual round-trip of the whole unpack→edit→repack→boot chain against a real container, not just
the `verify_pack.py` self-consistency check. Confirms the repack tooling produces a container the
base-loader boots and decompresses correctly.

Procedure (all reproducible; system `python3` needs PIL for the screenshot step):

    W=/tmp/fwtest; mkdir -p $W
    # 1. unpack a real release
    python3 -m tools.icom_fw.cli unpack /data/misc/icom/7300/7300_142.dat $W/orig
    # 2. same-length, non-executable string edit in the DECOMPRESSED body:
    #    "IC-7300 Ver\x001.42" -> "IC-7300 Ver\x009.99" (offset 0x64674, the version display literal)
    # 3. repack: recompresses body (our LZSS), re-inserts, recomputes the update MD5
    python3 -m tools.icom_fw.cli pack /data/misc/icom/7300/7300_142.dat $W/body_mod.bin $W/7300_142_mod.dat
    # 4. build the flat flash image and boot it
    python3 qemu-machine/tools/build_flash.py $W/7300_142_mod.dat $W/flash_mod.bin
    cp $W/flash_mod.bin qemu-machine/flash.bin   # (back up the original first)
    python3 qemu-machine/tools/screenshot.py 20 --icount shift=1,sleep=off --out /tmp/fwshot_mod

Results:
- `pack` reports body 3738392 decompressed both ways; compressed 1676645 (Icom's) -> **1464387** (ours),
  so our re-encode is *smaller* than the original and comfortably fits the fixed body slot. Container
  stays the exact same 3954089 bytes; re-parse is warning-free and the MD5 field is freshly valid.
- The modified image **boots to the main screen**, and that screen is **byte-for-byte identical**
  (same PNG SHA256, `ImageChops.difference` bbox = None) to the unmodified-142 build captured with the
  same settings — i.e. a repacked container changes nothing on-screen, exactly as a hidden-string edit
  should.
- The edit is **live in guest RAM**: reading 0x20069674 (body RAM base 0x20005000 + body offset 0x64674)
  in the running modified guest returns `IC-7300 Ver\x009.99`, proving the recompressed LZSS stream
  decompressed correctly during boot.

What this does and doesn't prove: it exercises LZSS round-trip + MD5 fixup + base-loader body
decompression against a genuine repacked container end to end. It does NOT exercise the SD-card *update
flow's* own acceptance path (that reads the .dat off a FAT card, checks the MD5, and writes flash) — that
check runs only on real hardware and is the remaining part of `sdk/roadmap.md` Phase 0. `flash_image.py`
drops the container's body slot straight into flash, which is what the loader boots from, so the update
flow's write step is bypassed in the emulator.

## 2026-09-25 — the stock SD updater installs a modified firmware in the emulator

Full procedure: repack with `tools/icom_fw` (here: `1.42`→`9.99` at body offsets 0x327f4,
0x3e72c and 0x64680 — the splash, VERSION-screen and `IC-7300 Ver` literals), put it on a card
(`qemu-machine/tools/build_sdcard.py X.dat -o sd.img --name 7300_999.dat`), boot with
`-drive if=sd,...` and no `-icount`, then MENU > SET > (page 2) SD Card > (page 2) Firmware Update
> agree YES > backup NO > pick 7300_999 > hold YES ~1.5 s. Results:
- The file browser lists every container in `IC-7300/` by basename; the MD5 check passes for our
  repack; slot B (0x400000-) is written byte-identical to the repacked body, slot A untouched,
  and the marker at 0x7f0000 becomes `SX3765 V1.00-003`. The boot region (0-0x10000) is skipped
  because it's unchanged. No DSP/FPGA phase ran (components unchanged).
- **The restart after "completed" (previously open, now traced end to end)**: the progress
  sequencer (state 0x3c) and `firmware_update_main` set the restart flag `0x20390306` (and
  `0x20390307`, the Fup_AutoEnd trigger). The normal-operation loop hands over to
  `power_state_pwrk_wait_and_bringup`, which: clears TX[1] bit 0 (POWER LED) and TX[3] in the
  front-panel buffer and sends it (`fe 01 00 00 00 fd` on SCIF3); sets `0x203901ec = 0xff` and
  waits for `ui_graphics_lifecycle_task` to leave its render loop and tear down EGL
  (eglTerminate path -> `FUN_2007ebd0`: STBREQ2 bit 0, wait STBACK2 bit 0, then module stop;
  also STBREQ2 bit 5 = VDC5 ch0); shuts peripherals down; writes `Fup_AutoEnd_3765`; waits a
  fixed delay; and, with the restart flag set, arms the watchdog (0x5a5f/0x5afe/0xa57f). The next
  boot's `power_state_dispatch` sees WRCSR.WOVF and clears it.
- After the watchdog reset the boot loader picks slot B and the radio runs the modified image:
  splash `9.99`, VERSION "Main CPU: 9.99".
So the whole acceptance path runs in the emulator; what's left for Phase 0 is doing it on the
real radio.
