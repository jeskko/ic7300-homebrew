# `base.dat` structure (the ARM boot loader region)

`base.dat` = `container[0x0:0x101d0]` (66000 bytes, constant size across all
10 releases) — see [[container-format]] for how that split was verified.
Ground-truthed directly against `7300_111/base.dat` / `7300_111/base.hex`
(read-only originals) with `xxd`.

| Offset | Content |
|---|---|
| `0x0000`–`0x000f` | 16-byte version string (Shift-JIS), e.g. `3wfU3.092.003.13` |
| `0x0010`–`0x002b` | `size1..size7`, 7 × LE u32 (see [[container-format]] — meaning still unconfirmed) |
| `0x002c`–`0x004b` | **ARM exception vector table**, 8 × 4-byte entries, all `LDR PC, [PC, #imm]` form (bytes `xx f0 9f e5` / `xx f1 9f e5`) — standard ARM reset vector table (Reset, Undef, SWI, PrefetchAbort, DataAbort, Reserved, IRQ, FIQ) |
| `0x004c`–`0x006b` | Literal pool for the vector table: 8 × 4-byte absolute handler addresses, all in the `0x1800xxxx` range (e.g. `0x1800008c`, `0x18000114`, `0x1800011c`, `0x18000120`, `0x18000124`, `0x18000128`, `0x1800012c`) — confirms handlers live in the SPI multi-I/O bus area (channel 0, per [[memory-map]]), consistent with boot-mode-3 serial-flash execute-in-place. |
| `0x0078` onward | First real boot loader function (ARM code), per `tunk.py`'s note ("120 → 0x78, ARM instructions, function prologue") — decimal 120 == 0x78, confirmed by the hex dump: `10 40 2d e9` at `0x78` is `push {r4, lr}`, a textbook ARM prologue. |

## Leads for locating the LZSS decompressor in Ghidra

- It must be reachable from the boot code starting at `0x78`, since that
  code's job (per boot-mode-3 serial-flash boot) is to load and decompress
  the main body before jumping to it.
- Look for the ring-buffer (4096-byte, `& 0xfff` masked) + `write_cursor =
  0xfee` initialization idiom described in [[decompression-lzss]] — that
  magic constant (`0xfee` / 3822) is a strong, fairly unique search target
  in the decompiled listing.
- `tunk.py`'s Python `unpack()` reads like a direct transliteration of an
  already-decompiled Ghidra function (see [[decompression-lzss]]) — check
  the existing `icom_loader.rep`/`icom.rep` projects (read-only reference)
  for a function already renamed/commented along these lines before
  redoing the search from scratch.
- Boot mode 3 GPIO strapping and the `0x1800xxxx` handler addresses are
  detailed in [[memory-map]].
