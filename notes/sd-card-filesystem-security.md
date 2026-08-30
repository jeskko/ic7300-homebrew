# SD-card filesystem: candidate robustness surface for goal (a)

Started 2026-08-30, following up on [[custom-custom-code loading-roadmap]]'s "secondary track" — the user
specifically wants a no-reflash custom-code loading trigger via SD card or serial port if one can be found,
not just the (already-confirmed-feasible) unauthenticated-firmware-update path. This file tracks that
investigation specifically for the SD-card filesystem angle.

## Why this angle, specifically

A web search for context on `"RENESAS RZ/A1 SD Driver Ver4.01"` (the versioned string already found at
`0x201812e0`, see `notes/kernel-rtos.md`) surfaced real, current, publicly-disclosed bug-hunting: **a 2026 FatFs advisory's July 2026 disclosure of 7 CVEs in FatFs** (ChaN's near-universal small embedded FAT
filesystem library, bundled by an enormous range of platforms — Espressif ESP-IDF, STM32Cube, Zephyr,
MicroPython, ArduPilot, RT-Thread, Mbed, TizenRT, SWUpdate). Renesas itself is not named among the
publicly-checked platforms, but Renesas's own RZ/A-series FIT/BSP documentation independently confirms
they ship a "FAT file system software library middleware for the RZ/A Series" — i.e. it's entirely
plausible (not yet confirmed) that this exact IC-7300 driver is FatFs-based, or descended from it.

Two of the seven are explicitly a robustness bug/custom-code loading-shaped, affecting **every FatFs version
up to and including R0.16**:

| advisory | severity | Type | Root cause |
|---|---|---|---|
| a disclosed FatFs bug | 7.6 | FAT32 mount integer overflow → a robustness bug, possible code exec | Integer handling during FAT32 volume mounting |
| a disclosed FatFs bug | 7.6 | exFAT volume-label buffer overflow | Improper bounds checking on volume labels |
| **a disclosed FatFs bug** | 7.6 | **Long-filename overflow in wrapper code** (e.g. `strcpy` of `fno.fname`) | Filenames exceeding the *wrapper* code's own buffer, not necessarily FatFs core itself — requires long-filename (LFN) support enabled |
| a disclosed FatFs bug | 6.1 | Math wrap in cluster cache on fragmented volumes | Integer wraparound |
| a disclosed FatFs bug | 4.6 | exFAT divide-by-zero | Missing validation before division — crash/brick, not code exec |
| a disclosed FatFs bug | 4.6 | Info leak (extends file past end, reveals deleted data) | Insufficient boundary validation |
| a disclosed FatFs bug | 4.6 | Malformed GPT table hangs mount | Missing GPT validation — only one with an upstream fix (R0.16) |

**a disclosed FatFs bug is the most directly relevant shape** to what's actually reachable here: it's explicitly
a *wrapper-code* bug, not core-library — meaning any project (Icom included) that writes its own
directory-listing code around a FatFs-style `readdir` call, using a fixed-size buffer without
re-validating the returned filename's length, could have the exact same class of bug independently,
whether or not the underlying core library itself is patched.

**Attack vector, if confirmed**: a crafted SD card with a malformed/oversized long-filename directory
entry (or chain of LFN entries), inserted into any folder the radio ever reads (SD-card menu file browser,
firmware-update file selection, voice-memory folder listing, etc.) — physical SD-card access only, no
serial port needed, and notably **works against completely stock, unmodified firmware** — exactly the
"no reflash" property the user is after, unlike the (separately confirmed-feasible)
custom-firmware-via-unauthenticated-update path.

## What's been traced so far

Icom's own small VFS/file-abstraction layer, already partially named from an earlier investigation (the
now-retracted `civ_command_dispatch_task`/`sdcard_file_rpc_dispatch_task` work — see
`notes/kernel-rtos-history.md`): `vfs_open`/`vfs_open_ex`/`vfs_read_record`/`vfs_write_record`/
`vfs_close`/`vfs_rename`/`vfs_read_dir_entry`, all in one dense cluster `0x200c8000`-`0x200cf000`
(~110 functions total, still mostly unnamed `FUN_*` — the sheer density strongly suggests this whole
region *is* the actual filesystem implementation, not just a thin wrapper around something elsewhere).

**`vfs_read_dir_entry`** (`0x200cbcf0`) is the directory-listing entry point, and its shape matches
a disclosed FatFs bug closely enough to be worth the deep look:

- Declares a **128-byte stack buffer** (`auStack_dc[128]`) and a size hint (`local_50 = 0x80`).
- Fills it via a **dynamically-dispatched call** — `(**(code**)(*(int*)(*(int*)(this+0x34)+0x10)+0x34))(this, tmp)`
  — i.e. a vtable-based "readdir" operation resolved at runtime from a path string (via `FUN_200ca70c`,
  which walks a device tree rooted at `DAT_200ca64c` — a link-time-constant pointer with no runtime
  writer found anywhere, consistent with one statically-configured SD-card root device, not multiple
  device types).
- The **outer** copy — from whatever the vtable call filled in, back into the *caller's* own
  buffer — is correctly length-clamped (`if (iVar3 < local_50+1) local_50 = sVar2-1` before the copy).
  This specific step looks safe, ruling out a naive read of the surface finding.
- **Not yet confirmed**: whether the vtable-dispatched implementation *itself* respects the 128-byte
  hint when it fills `auStack_dc` in the first place — i.e. does the actual FAT/VFAT long-filename
  reassembly code bound its writes to what was requested, or trust the on-disk entry's own claimed
  length? This is the crux of whether a disclosed FatFs bug's exact bug class applies here.

Full annotation of this specific finding (with the precise pointer-chase detail) is in Ghidra as a
plate comment on `vfs_read_dir_entry` itself, for whoever picks this up next.

## Open next steps, in order

1. **Find the concrete function the vtable call resolves to.** `DAT_200ca64c`'s value (read directly,
   no runtime writer to trace) points at a device-tree root; `FUN_200c9950` walks it by path segment.
   The end goal is the real function address sitting at `(root device's inode)+0x34`'s vtable slot
   `+0x34` — the actual low-level FAT/VFAT directory-entry reader.
2. **Decompile it and look specifically for the long-filename (LFN) reassembly logic** — the classic
   VFAT pattern is reading up to 13 UTF-16 characters per directory entry across a chain of up to 20
   entries (checksum-linked to a following short 8.3 entry), reassembling them into one string. This is
   exactly the kind of code a naive/example-derived implementation gets wrong: iterating the on-disk
   entry count without an independent cap tied to the destination buffer's real size.
3. **If the crux bug is confirmed statically**: build a minimal proof — a crafted SD card image with a
   directory entry whose reassembled long filename exceeds 128 bytes (`tools/icom_fw` doesn't currently
   have FAT-image-crafting support; a plain `mkfs.fat`+manual directory-entry patching, or an existing
   FAT-crafting library, would work without needing new project tooling) — and test on real hardware
   (or under JTAG once available, for a safe halt-and-inspect rather than a live crash) to see whether
   it's reachable in practice (stack canaries/MPU protections on this SoC, if any, would matter here —
   not yet checked).
4. **If this specific function turns out to be safe**, the broader `0x200c8000`-`0x200cf000` cluster is
   still the right place to keep looking — a disclosed FatFs bug's FAT32-mount integer overflow and
   a disclosed FatFs bug's exFAT volume-label overflow are two more candidate shapes to search for in the same
   region (mount-time code and volume-label-reading code respectively), not yet looked at here at all.

## Caveat

None of this confirms the IC-7300 is actually FatFs-derived — that's an inference from Renesas's known
RZ/A-series middleware offerings, not yet verified against this specific driver's binary. Even if it
isn't literally FatFs, the *bug class* (wrapper code trusting an on-disk-controlled length against a
fixed buffer) is general enough that `vfs_read_dir_entry`'s own shape is worth finishing the trace on
regardless of whether it turns out to share literal code with upstream FatFs.

Sources: [The Hacker News](https://thehackernews.com/2026/07/unpatched-flaws-disclosed-in-filesystem.html),
[Rescana technical summary](https://www.rescana.com/post/critical-fatfs-advisory-Bugs-expose-millions-of-embedded-devices-to-a robustness bug-and-custom-code loading-risks).
