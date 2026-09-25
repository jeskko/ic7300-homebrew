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

## ✅ The simpler alternative: `file_rpc_post_command` — now a proven, working app-facing primitive

**`file_rpc_post_command`** (`0x200bc048`, verified) — already used internally by several catalogued tasks
(`voice_recording_file_task`, `voice_tx_memory_stream_task`, `sdcard_file_rpc_dispatch_task`). Not meant to
be called directly with a hand-built message, though — `sdcard_file_rpc_dispatch_task`'s own retraction
comment already flagged a family of 26 tiny per-command wrapper functions (`0x200bc0fc`-`0x200bca08`) that
hide the raw RPC message-building entirely behind a plain C calling convention. **Confirmed and live-tested,
2026-09-25** (`sdk/examples/sd-card-app/`), by reading `firmware_update_main`'s own real, working file-read
code (it opens the SD-card update container this exact way) rather than guessing from `file_rpc_post_command`
cold:

| Wrapper | Address | Command ID | Signature | Role |
|---|---|---|---|---|
| — | `0x200bc5f4` | `0xf` | `(path, flags, &handle_out, scratch36)` | open |
| — | `0x200bc6a4` | `0x11` | `(handle, dest_buf, len, &actual_out)` | read |
| — | `0x200bc754` | `0x13` | `(handle, offset, 0, &actual_out)` | seek |
| — | `0x200bc64c` | `0x10` | `(handle, 0)` | close |

All four: call, then `FUN_200214b0(return_value, 0x46)` waits for the RPC to complete and returns 0 on
success — the exact pattern `firmware_update_main` uses for every one of its own file operations. `path` is
a plain null-terminated ASCII string (`"C:\IC-7300\..."` — confirmed against a real literal elsewhere in the
image). This closes the "is file I/O safe/simple enough for an app to call" open question — it is, and
`sdk/examples/sd-card-app/`'s loader hook calls these four directly to load and run a file from the SD card.
**Correction**: this file previously guessed `6`/`9`/`0x13`/`0x17` for open/write/read/list — `0x13` for
seek checks out, the rest don't match this now-confirmed cluster; those other IDs may still be valid for
different operations (the dispatch table has 26 entries total, `0`-`0x1a`), just not re-verified since.

## Correction on file identity (don't re-litigate)

An early hypothesis that this driver was ChaN's FatFs was **investigated and disproved**: `"GRP_FS: ..."`
debug strings reveal a reference-counted buffer cache and per-fd open-count tracking that FatFs simply
doesn't have. A feature-flag fingerprint check also found long filenames and exFAT both compiled out. See
`notes/sd-card-filesystem-security.md` for the full trace — treat the FatFs-matching angle as a closed,
negative result, not something to re-attempt.
