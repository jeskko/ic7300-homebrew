# SD-card file I/O

Two layers exist: a real, general-purpose VFS, and a simpler internal RPC-style API several existing
tasks already use. Both are documented here; either could plausibly become an app-facing primitive.

All addresses below verified against the live Ghidra project (`body.bin`) 2026-08-30.

## ✅ The VFS layer

A real, proprietary VFS implementation — **confirmed NOT ChaN's FatFs** (see the correction below), an
OS-grade design with a block-device abstraction, a reference-counted buffer cache, and per-file-descriptor
open-count tracking. Dense cluster `0x200c8000`-`0x200cf000`, ~110 functions, still mostly unnamed.
Public API, all verified 2026-08-30:

| Function | Address | Role |
|---|---|---|
| `vfs_open` | `0x200cb278` | open |
| `vfs_open_ex` | `0x200cac48` | open (extended — the redirect-following/refcount-adjusting variant) |
| `vfs_read_record` | `0x200cb5dc` | read |
| `vfs_write_record` | `0x200cb72c` | write |
| `vfs_close` | `0x200cb98c` | close — public entry point into the refcount bug below |
| `vfs_rename` | `0x200cb3d0` | rename |
| `vfs_read_dir_entry` | `0x200cbcf0` | directory listing |

## ⚠️ Known-fragile area: unenforced reference-count underflow

**`fs_object_release_ref_UNSAFE_NEGATIVE`** (`0x200c7f10`, verified), reachable from `vfs_close` via
`fs_close_fd`, detects an over-release (refcount going negative) and logs it — but does **not** prevent the
cleanup path from running anyway, a genuine double-free/use-after-free shape confirmed at the decompiled-
code level (`notes/sd-card-filesystem-security.md`). The identical pattern exists in the block buffer-cache
layer (`fs_buffer_release_ref_UNSAFE_NEGATIVE`, `0x200c8854`, verified) but that side has a real structural
defense (clears the caller's own handle before touching the refcount) that the file-object side lacks.

**Not flagging this as something an app needs to specifically avoid** — no concrete trigger has been found
after real effort (double-close bugs, a duplicate-open-sharing-one-object path, and a DMA-lock-drop
concurrency race were all checked and found weaker than first hoped; see the full trace in
`notes/sd-card-filesystem-security.md`). Worth knowing about if an SDK-provided file-I/O wrapper is ever
built on top of this layer directly — a wrapper that guarantees balanced open/close pairs sidesteps the
whole question.

## ✅ The simpler alternative: `file_rpc_post_command`

**`file_rpc_post_command`** (`0x200bc048`, verified) — already used internally by several catalogued tasks
(`voice_recording_file_task`, `voice_tx_memory_stream_task`, `sdcard_file_rpc_dispatch_task`). Command IDs
known so far: `6` = open, `9` = write/append, `0x13` = read-at-offset, `0x17` = list directory. Individual
payload field layouts within each command are **not decoded**. May be a more SDK-appropriate primitive than
the raw VFS layer — smaller surface, already proven safe by heavy existing internal use — but its own
relationship to the VFS refcount layer above (does it also risk the same bug, or is it insulated by always
balancing its own opens/closes?) hasn't been checked.

## Correction on file identity (don't re-litigate)

An early hypothesis that this driver was ChaN's FatFs (motivated by a real July-2026 public CVE
disclosure) was **investigated and disproved**: `"GRP_FS: ..."` debug strings reveal a reference-counted
buffer cache and per-fd open-count tracking that FatFs simply doesn't have. A feature-flag fingerprint
check also found long filenames and exFAT both compiled out. See `notes/sd-card-filesystem-security.md`
for the full trace — treat the FatFs-CVE-matching angle as a closed, negative result, not something to
re-attempt.
