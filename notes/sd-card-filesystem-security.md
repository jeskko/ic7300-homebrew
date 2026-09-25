# SD-card filesystem: candidate custom-code loading surface for goal (a)

Started 2026-08-30, following up on `sdk/roadmap.md`'s "secondary track" — the user
specifically wanted a no-reflash custom-code loading trigger via SD card or serial port if one could be found,
not just the (already-confirmed-feasible) unauthenticated-firmware-update path. This file tracks that
investigation specifically for the SD-card filesystem angle.

**Deprioritized 2026-09-25** — the SD-card app-loading mechanism (see `sdk/examples/sd-card-app/`,
[[icom-custom-code-goal]] in memory) fully delivered the project's north star without needing this
thread. Kept here as a real, honest record of a real code-level finding; not being actively pursued.

See [notes/sd-card-filesystem-security-history.md](sd-card-filesystem-security-history.md) for the full session-by-session narrative and evidence trail.

## Current bottom line (summary — full derivation below, and in history)

- **This is not the commonly-used small-embedded-FAT reference implementation this thread initially
  assumed.** The `"GRP_FS: ..."` assert strings reveal a proprietary, OS-grade VFS (reference-counted
  buffer cache, per-fd open counts). Long filenames and exFAT both look compiled out.
- **A real bug was found by direct auditing**: `fs_object_release_ref_UNSAFE_NEGATIVE`
  (`FUN_200c7f10`), reachable from the public `vfs_close`, detects a reference count going negative
  but **logs-and-continues** into the full cleanup/unlink/vtable-release path — a genuine
  double-release shape, confirmed at the code level. The buffer-cache layer has the identical
  anti-pattern, but defends itself by zeroing the caller's handle first; the file-object layer has
  no such defense.
- **No trigger has been demonstrated.** Both leading hypotheses took real hits: no path-based
  open-deduplication exists (weakens duplicate-open sharing), and a real, heavily-used
  condition-variable interlock (`fs_task_wait_on_object`, 13 sites) guards the naive close-while-busy
  race. The defect is real and confirmed; a concrete way to reach it is still open.
- **Next step, if ever revisited, is live testing, not more static tracing** — breakpoint the two
  release functions under JTAG (or in `qemu-machine/` once its forced-call path reaches real FS code)
  during heavy concurrent SD use and watch for the `"GRP_FS: negative ..."` log lines. Note: the
  flash-once custom-code path (see [[firmware-update]]/`sdk/roadmap.md`) already gives code exec
  without this bug, so this was always the more elegant no-reflash path, not a critical one — and it's
  now superseded entirely by the SD-card app-loader.

## Why this angle, specifically

A web search for context on `"RENESAS RZ/A1 SD Driver Ver4.01"` (the versioned string already found at
`0x201812e0`, see `notes/kernel-rtos.md`) surfaced a widely-used small embedded FAT filesystem library
(bundled by an enormous range of platforms — Espressif ESP-IDF, STM32Cube, Zephyr, MicroPython,
ArduPilot, RT-Thread, Mbed, TizenRT, SWUpdate) with a public defect-report history that included a
handful of robustness-shaped issues in FAT32 mounting, exFAT parsing, and long-filename handling.
Renesas itself is not named among the platforms in that history, but Renesas's own RZ/A-series FIT/BSP
documentation independently confirms they ship a "FAT file system software library middleware for the
RZ/A Series" — i.e. it was entirely plausible (not yet confirmed) that this exact IC-7300 driver was
based on, or descended from, that same reference implementation.

The most directly relevant reported shape was a long-filename overflow in *wrapper* code around a
`readdir`-style call (not the core library) — meaning any project that writes its own directory-listing
code around such a call, using a fixed-size buffer without re-validating the returned filename's length,
could have the same class of bug independently, whether or not the underlying core library itself was
ever patched. If it existed here, the trigger would be a crafted SD card with a malformed/oversized
long-filename directory entry, inserted into any folder the radio ever reads (SD-card menu file browser,
firmware-update file selection, voice-memory folder listing, etc.) — physical SD-card access only, no
serial port needed, and notably would work against completely stock, unmodified firmware — the "no
reflash" property that made this angle worth checking, unlike the (separately confirmed-feasible)
custom-firmware-via-unauthenticated-update path.

## What's been traced so far

Icom's own small VFS/file-abstraction layer, already partially named from an earlier investigation (the
now-retracted `civ_command_dispatch_task`/`sdcard_file_rpc_dispatch_task` work — see
`notes/kernel-rtos-history.md`): `vfs_open`/`vfs_open_ex`/`vfs_read_record`/`vfs_write_record`/
`vfs_close`/`vfs_rename`/`vfs_read_dir_entry`, all in one dense cluster `0x200c8000`-`0x200cf000`
(~110 functions total, still mostly unnamed `FUN_*` — the sheer density strongly suggests this whole
region *is* the actual filesystem implementation, not just a thin wrapper around something elsewhere).

**`vfs_read_dir_entry`** (`0x200cbcf0`) is the directory-listing entry point, and its shape matched the
reported wrapper-overflow pattern closely enough to be worth the deep look:

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
  length? This was the crux of whether the reported overflow's exact bug class applied here.

Full annotation of this specific finding (with the precise pointer-chase detail) is in Ghidra as a
plate comment on `vfs_read_dir_entry` itself, for whoever picks this up next.

## Open next steps, in order

Superseded by later findings and the bottom-line next step above; full original list in history.

## Version/identity check (2026-08-30, same day)

Built on the (retracted) assumption that this driver was based on the reference FAT implementation
described above — see the correction below; full narrative in history.

## Correction, 2026-08-30, same day — this is not that reference implementation

Tried to fingerprint the mounting code using distinctive constants stable across that reference
implementation's entire history — searched for them as raw literals and as ARM/Thumb
`movw`/`movt`-encoded immediates across the *entire* 3.7 MB image. **Zero hits, anywhere.** That alone
was suspicious given how distinctive these values are; tracing `vfs_open_ex`'s actual callees (rather
than guessing at constants) turned up something conclusive: a family of internal debug/assert strings—

- `"GRP_FS: negative buf ref(dev:0x%x blk:0x%lx ref:%d)\n"` (`0x200c8be4`)
- `"GRP_FS: negative file ref dev:0x%x fid:0x%lx st:0x%x ref:%d\n"` (`0x200c8128`)
- `"GRP_FS: negative FS open dev:0x%x open:%ld\n"` (`0x200c81d4`)
- `"GRP_FS: file still busy dev:0x%x fid:0x%lx st:0x%x\n"` (`0x200c8168`)
- `"GRP_FS: file not blocked dev:0x%x fid:0x%lx st:0x%x\n"` (`0x200c819c`)
- `"GRP_FS: no task environment(%lu)\n"` (`0x200c7cc8`)
- `"GRP_FS: %s failed(dev:0x%x blk:0x%lx blk_shift:%d cnt:%ld)\n"` (`0x200c95ec`)

**This rules out that reference implementation for this layer.** It has no reference-counted buffer
cache, no per-file-descriptor open-count tracking, and nothing resembling this `"GRP_FS: <condition>
dev:%x fid:%x ref:%d"` categorized-assert convention — it's a much simpler, uncached, direct-sector-I/O
design. What's actually here reads like a real, OS-grade VFS: a block-device abstraction (`dev`), a
buffer cache with reference counts (`blk`/`ref`), and file descriptors with their own open-counts and
state (`fid`/`st`/`open`) — structurally far closer to something like eSOL's commercial **PrFILE2** (a
real, widely-used embedded FAT/exFAT middleware — used on the Nintendo Switch among others) or an
in-house Renesas/Icom VFS layer than to the open-source reference this session had been comparing
against. No public source or string match for "PrFILE2"/"GRP_FS" specifically turned up in a web search,
so the exact vendor/product is **not confirmed** — but the negative fingerprint result is solid on its
own regardless of naming it: **matching against that reference implementation's public defect history
does not apply here**, and the earlier long-filename-overflow reasoning, while methodologically sound
(the feature-flag check was real), was built on the same now-doubtful premise.

**What this doesn't undo**: `vfs_read_dir_entry`'s own structural shape (128-byte stack buffer, filled
via a dynamically-dispatched call, whose bounds-respecting behavior is still unconfirmed) is unaffected —
it's still a real, un-closed question, just no longer backed by a named public report. What changes is
the strategy: **this is a proprietary/unidentified codebase, so the productive path is direct auditing
of the actual compiled functions for real bugs, not matching against another project's public defect
history.**

**A new, concrete, self-supplied lead from these very strings**: the developers explicitly instrumented
against reference-count *underflow* ("negative buf ref", "negative file ref", "negative FS open") — i.e.
they were worried about exactly this bug class, which means it's plausible one exists (or existed) in
practice. The real question these asserts don't answer: in a release build, does hitting one of these
conditions actually **deny the operation**, or does it just **log and continue** with an already-corrupted
reference count? If the latter, a ref-count reaching zero/negative while a stale handle is still in use
is a classic use-after-free shape — a genuinely different, and in some ways more promising, bug class
than the buffer-overflow angle this session started with. Worth checking the actual `if` branch
around each of these debug-print call sites for whether an error is actually returned/enforced.

## A real bug, not just a hypothesis: unenforced reference-count underflow (2026-08-30, same day)

Followed up the "log-but-continue" question the correction above raised, on the most directly
reachable of the `GRP_FS` strings: `"negative file ref"`. Traced its one call site
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
unlinking of the same node from a linked list can corrupt the list (the node's neighbor pointers get
overwritten based on already-stale data); combined with the vtable release call being invoked more than
once, this is a genuine double-release shape, confirmed at the code level.

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

This was, at the time, the most concrete, best-evidenced lead in this whole investigation — a real logic
bug found by reading the actual code, independent of any external reference, in a function directly
reachable from ordinary SD-card file operations.

## Further static sweeps (buffer-cache audit, concurrency hypothesis, open-dedup check)

Narrowed the search but did not find a trigger — see the Bottom Line above for current status; full derivation in history.

## Caveat

None of this proves the IC-7300 shares literal compiled code with the reference implementation this
thread initially compared against — the feature-flag matches are circumstantial (consistent with, not
proof of, derivation from that source), and the fingerprint searches are absence-of-evidence, which is
suggestive but not airtight (a sufficiently different fork or a compiler that fully unrolled/inlined the
relevant constants could in principle hide it). Treat "long filenames and exFAT are off" as the working
assumption to build on, not a closed question.
