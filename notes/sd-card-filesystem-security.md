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

## Version/identity check (2026-08-30, same day)

User's ask: is it easy to pin down the actual FatFs version this firmware uses, before digging further?
No embedded runtime version *string* was found (`strings` on `body.bin` turns up zero hits for
`ChaN`/`FatFs`/`R0.`/`exFAT` etc. — expected, since FatFs's `R0.13a`-style version marker is a source
comment, not a compiled string, so its absence proves nothing either way). But this project already has
something better sitting in `scratch/` from the earlier FreeRTOS-identification work: the **full Renesas
RZ/A1H reference source**, including its FatFs middleware —
`scratch/r01an5093ej0170-rza1-swpkg/.../RZA1H_Sample/src/renesas/middleware/fatfs/` (`ff.c`/`ff.h` = real
ChaN FatFs **R0.13a**, well before the R0.16 cutoff so all 7 disclosed CVEs' version range covers it,
plus Renesas's own `r_fatfs_abstraction.c` wrapper).

**Found the exact a disclosed FatFs bug pattern in the reference source itself**:
`r_fatfs_abstraction.c`'s `map_filinfo_to_fatentry()` does `strcpy(p_fat_entry->FileName, info->fname)`
— literally the bug the advisory describes, sitting in Renesas's own official sample wrapper code. If Icom's
firmware derives from this (plausible, not proven — this project's kernel work already independently
confirmed the FreeRTOS *port* itself matches this exact software package via direct source comparison,
which is real precedent for the SD/FatFs side deriving from it too), this is the literal, named function
to look for compiled into `body.bin`.

**But a feature-flag check narrows things down significantly, using this reference `ffconf.h` as a map
of what to search for in the binary**:

| Feature | Reference default (`ffconf.h`) | Found in `body.bin`? | Verdict |
|---|---|---|---|
| Long filenames (`FF_USE_LFN`) | `0` (disabled) | **No** — FatFs's `LfnOfs[]` table (`{1,3,5,7,9,14,16,18,20,22,24,28,30}`), a distinctive 13-byte fingerprint stable across FatFs versions, was searched for as a literal byte sequence across the whole image and **not found anywhere** | Long filenames very likely **compiled out** — `FILINFO.fname` would be capped at 12+1 bytes (8.3 short name only), which fits trivially in any reasonably-sized destination buffer. **This means a disclosed FatFs bug as literally described (LFN overflow) most likely does NOT apply** — a real, honest negative result, not just an assumption |
| exFAT (`FF_FS_EXFAT`) | `0` (disabled) | **No** — searched for the `"EXFAT"` boot-sector label string, zero hits (only plain `"FAT32"`/`"FAT12"`/`"FAT16"` found, at `0x200c5238`, inside a boot-sector-*building* function, `FUN_200c4d84` — this firmware can format FAT12/16/32 volumes, consistent with genuine FatFs, but this specific function constructs Icom's *own* valid boot sector rather than parsing an The owner-supplied one, so it's not itself the target) | exFAT compiled out too — **rules out a disclosed FatFs bug and a disclosed FatFs bug** (both exFAT-specific) entirely |

**Net effect**: this firmware's SD/FAT support looks consistent with Renesas's reference `ffconf.h`
defaults essentially unmodified (LFN off, exFAT off, plain FAT12/16/32 only) — a real, useful narrowing.
Of the 7 disclosed CVEs, the ones that *don't* depend on an optional feature flag are now the better
targets: **a disclosed FatFs bug** (FAT32 mount integer overflow — core mounting logic, always compiled in),
**a disclosed FatFs bug** (math wrap in cluster cache on fragmented volumes — core), and **a disclosed FatFs bug** (info
leak on files extended past end — core). a disclosed FatFs bug (GPT hang) needs `FF_MULTI_PARTITION`/GPT
handling, also `0` in the reference config, so likely inapplicable too, same reasoning as exFAT.

**Concrete next step, sharpened by this**: find the actual *mount-time* volume-parsing function (reads
an existing, potentially The owner-crafted boot sector and computes sector/cluster/FAT-size counts from
its fields — this is where a disclosed FatFs bug's integer overflow lives, in FatFs's `f_mount`/`find_volume`
equivalent) rather than continuing to chase the long-filename angle in `vfs_read_dir_entry` — that specific
function's own risk just dropped substantially given LFN appears to be off. The boot-sector-*building*
function (`FUN_200c4d84`) just found is in the right neighborhood; the parsing counterpart is very
likely nearby in the same `0x200c4000`-`0x200c8000` region (not yet located).

## Correction, 2026-08-30, same day — this is NOT ChaN's FatFs

Tried to trace a disclosed FatFs bug (FAT32 mount integer overflow) by fingerprinting `find_volume`'s distinctive
`MAX_FAT16`/`MAX_FAT32` constants (`0xFFF5`/`0x0FFFFFF5`, stable across FatFs's entire history) — searched
for both as raw literals and as ARM/Thumb `movw`/`movt`-encoded immediates across the *entire* 3.7 MB
image. **Zero hits, anywhere.** That alone was suspicious given how distinctive these values are; tracing
`vfs_open_ex`'s actual callees (rather than guessing at constants) turned up something conclusive: a
family of internal debug/assert strings—

- `"GRP_FS: negative buf ref(dev:0x%x blk:0x%lx ref:%d)\n"` (`0x200c8be4`)
- `"GRP_FS: negative file ref dev:0x%x fid:0x%lx st:0x%x ref:%d\n"` (`0x200c8128`)
- `"GRP_FS: negative FS open dev:0x%x open:%ld\n"` (`0x200c81d4`)
- `"GRP_FS: file still busy dev:0x%x fid:0x%lx st:0x%x\n"` (`0x200c8168`)
- `"GRP_FS: file not blocked dev:0x%x fid:0x%lx st:0x%x\n"` (`0x200c819c`)
- `"GRP_FS: no task environment(%lu)\n"` (`0x200c7cc8`)
- `"GRP_FS: %s failed(dev:0x%x blk:0x%lx blk_shift:%d cnt:%ld)\n"` (`0x200c95ec`)

**This rules out ChaN's FatFs for this layer.** FatFs has no reference-counted buffer cache, no
per-file-descriptor open-count tracking, and nothing resembling this `"GRP_FS: <condition> dev:%x
fid:%x ref:%d"` categorized-assert convention — it's a much simpler, uncached, direct-sector-I/O design.
What's actually here reads like a real, OS-grade VFS: a block-device abstraction (`dev`), a buffer cache
with reference counts (`blk`/`ref`), and file descriptors with their own open-counts and state
(`fid`/`st`/`open`) — structurally far closer to something like eSOL's commercial **PrFILE2** (a
real, widely-used embedded FAT/exFAT middleware — used on the Nintendo Switch among others) or an
in-house Renesas/Icom VFS layer than to the open-source reference this session had been comparing
against. No public source or string match for "PrFILE2"/"GRP_FS" specifically turned up in a web search,
so the exact vendor/product is **not confirmed** — but the negative a disclosed FatFs bug fingerprint result is
solid on its own regardless of naming it: **the FatFs-advisory-matching approach for the mount-time path does
not apply here**, and the earlier a disclosed FatFs bug (LFN) reasoning, while methodologically sound (the
feature-flag check was real), was built on the same now-doubtful FatFs premise.

**What this doesn't undo**: `vfs_read_dir_entry`'s own structural shape (128-byte stack buffer, filled
via a dynamically-dispatched call, whose bounds-respecting behavior is still unconfirmed) is unaffected —
it's still a real, un-closed question, just no longer backed by a named public advisory. What changes is the
strategy: **this is a proprietary/unidentified codebase, so the productive path is now direct auditing
of the actual compiled functions for real bugs, not matching against public FatFs bug-hunting.**

**A new, concrete, self-supplied lead from these very strings**: the developers explicitly instrumented
against reference-count *underflow* ("negative buf ref", "negative file ref", "negative FS open") — i.e.
they were worried about exactly this bug class, which means it's plausible one exists (or existed) in
practice. The real question these asserts don't answer: in a release build, does hitting one of these
conditions actually **deny the operation**, or does it just **log and continue** with an already-corrupted
reference count? If the latter, a ref-count reaching zero/negative while a stale handle is still in use
is the classic shape of a use-after-free — a genuinely different, and in some ways more promising, bug
class than the buffer-overflow angle this session started with. Worth checking the actual `if` branch
around each of these debug-print call sites for whether an error is actually returned/enforced.

## A real bug, not just a hypothesis: unenforced reference-count underflow (2026-08-30, same day)

Followed up the "log-but-continue" question the correction above raised, on the most directly
The owner-reachable of the `GRP_FS` strings: `"negative file ref"`. Traced its one call site
(`FUN_200c7f10`, renamed **`fs_object_release_ref_UNSAFE_NEGATIVE`**) and confirmed a real bug, visible
directly in the decompiled logic — not speculation:

```c
iVar1 = *(param_1+8) - 1;      // decrement the object's reference count
*(param_1+8) = iVar1;          // stored even if it goes negative
if (iVar1 < 1) {
    if (iVar1 < 0) {
        fs_debug_log("GRP_FS: negative file ref dev:0x%x fid:0x%lx st:0x%x ref:%d\n", ...);
        // no return, no error propagated, no abort
    }
    *(param_1+8) = 0;          // clamped only AFTER logging
    // ... unconditionally proceeds with full "last reference released" cleanup:
    //     unlinks the object from a doubly-linked active-object list,
    //     calls its release vtable slot, decrements a second, outer
    //     "FS open count" (which has the exact same log-but-continue
    //     pattern for its own "negative FS open" case) ...
}
```

**The bug**: an over-release (this function invoked one more time than the object was ever genuinely
referenced) is detected and logged, but not prevented. The code still runs the *entire* "final reference
gone" cleanup path every time it's called again on an already-zero-or-negative count — including
unlinking the object from an active-object linked list and calling a release/free vtable slot. Repeated
unlinking of the same node from a linked list is a classic a robustness bug primitive (the node's
neighbor pointers get overwritten based on already-stale/The owner-influenceable data); combined with the
vtable release call being invoked more than once, this is a genuine **double-free/use-after-free shape**,
confirmed at the code level, not inferred from a advisory database.

**Confirmed reachable from the public API**: `vfs_close` (`0x200cb98c`) → **`fs_close_fd`**
(renamed `FUN_200c7724`) → `fs_object_release_ref_UNSAFE_NEGATIVE`. `vfs_close` is exactly the kind of
function every SD-card file operation in this firmware (voice memory, RTTY logs, firmware-update staging,
the SD menu, the factory-file loader) ultimately calls when done with a file handle.

**What's still open — the concrete next step**: a *trigger*. This bug needs the release path invoked one
extra, unbalanced time relative to a matching acquire. Candidates, not yet individually checked:
- A double-close bug in Icom's own calling code somewhere (closing the same handle twice on an error path,
  a classic and common real-world bug pattern in C).
- A bug in this filesystem layer's own open/alias/hard-link-style handling that lets two different
  "opens" resolve to the same underlying object without both being counted, so one real close() over-releases.
- The shared `"negative buf ref"` sibling (block-cache buffer ref count, 13 call sites across this same
  cluster — not yet individually traced) might have an easier-to-reach trigger than the file-level one.

This is now the most concrete, best-evidenced lead in this whole investigation — a real logic bug found
by reading the actual code, independent of any external advisory database, in a function directly reachable
from ordinary SD-card file operations.

## Caveat

None of this proves the IC-7300 shares literal compiled code with this reference package — the feature-flag
matches are circumstantial (consistent with, not proof of, derivation from this exact source), and the
`LfnOfs`/`"EXFAT"` searches are absence-of-evidence, which is suggestive but not airtight (a sufficiently
different FatFs fork or a compiler that fully unrolled/inlined the LFN offset table could in principle
hide it). Treat "LFN and exFAT are off" as the working assumption to build on, not a closed question.

Sources: [The Hacker News](https://thehackernews.com/2026/07/unpatched-flaws-disclosed-in-filesystem.html),
[Rescana technical summary](https://www.rescana.com/post/critical-fatfs-advisory-Bugs-expose-millions-of-embedded-devices-to-a robustness bug-and-custom-code loading-risks).
