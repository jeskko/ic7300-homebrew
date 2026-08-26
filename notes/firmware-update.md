# The firmware update mechanism (SD-card based, dual A/B flash slots)

Traced from `body.bin` (rebased to `0x20005000`, see [[base-loader]]) by
following live code from the reset handler onward, then jumping to the
SPI-flash driver once literal-pool xrefs to `SPI_BASE` (`0x3fefa000`, read
directly from `base.dat` on disk — same register `base.dat`'s own
`setup_spi`/`config_spi` use, see [[base-loader]]) turned up real callers.
All addresses below are v1.42; offsets should hold across versions.

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

**No code-execution-from-RAM-without-flashing path exists in what's been
traced.** `FUN_20024db8` always stages each chunk through a RAM buffer
only as a step before `erase`+`program`ing it into flash — nothing in the
path executes that buffer directly. Every successful update is a
permanent flash write; there's no "run once from SD card, no trace left"
shortcut via this mechanism.

**What *is* notable: no signature verification was found anywhere in
`FUN_20025ae4`'s orchestration.** The only validation against the update
file's content is structural/size sanity checks plus a self-contained
checksum (`FUN_2003c860`/`FUN_2003d38c`/`FUN_2003d458`, compared via
`memcmp`-equivalent `FUN_2017c81e` against a 16-byte expected value read
from the file itself) — no RSA/ECDSA/HMAC-shaped call sequence turned up.
**Not yet confirmed**: haven't decompiled the checksum algorithm itself,
so whether it's a simple unkeyed hash (forgeable by anyone, since the
attacker controls both the payload and can compute the matching checksum
for it) or something stronger is still open. If it turns out to be
unkeyed, the practical implication is that custom firmware satisfying the
container structure and its own checksum would plausibly flash and boot
as legitimate — a real, persistent capability, just not the RAM-only one
originally guessed.

**Sharper version of the implication: `base.dat` itself is not
protected.** Step 3 of `FUN_20025ae4` writes flash offset `0`–`0x10000`
(`base.dat`, the boot loader/unpacker) through the *exact same*
unauthenticated path as everything else — read, checksum, `memcmp`
against current flash, erase+program if different. `base.dat` is the
first code the CPU executes on reset (boot-mode-3 XIP straight into its
vector table, see [[base-loader]]); nothing in Icom's own firmware sits
above it to validate it, since its own marker check only runs *after*
it's already executing. So if the checksum turns out to be unkeyed: a
malicious update replaces the boot loader itself, unconditionally, and
that replacement has first-instruction control on every subsequent boot —
including the ability to fake or remove all update-time checks going
forward.

**Not yet checked, and the one thing that could still stop this:**
whether the RZ/A1H SoC's own on-chip, immutable boot ROM (a *silicon*
feature, not anything in Icom's firmware or in any of our `.dat`
containers) performs a signature check before jumping into SPI flash for
boot-mode-3. That would be documented in the Renesas hardware manual
(`/data/misc/icom/7300/doc/REN_r01uh0403ej0600...`), not discoverable from
anything reverse-engineered so far. **Next steps if this is worth
pursuing, in order: (1) check the Renesas manual for boot-mode-3 secure
boot / signature behavior — settles whether this matters at all; (2) if
no hardware-level check exists, decompile `FUN_2003d38c`/`FUN_2003d458` to
confirm the checksum algorithm is actually unkeyed.**

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
