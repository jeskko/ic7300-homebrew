# `base.dat` structure (the ARM boot loader region)

`base.dat` = `container[0x0:0x101d0]` (66000 bytes, constant size across all
10 releases) — see [[container-format]] for how that split was verified.
Ground-truthed directly against `7300_111/base.dat` / `7300_111/base.hex`
(read-only originals) with `xxd`.

| Offset | Content |
|---|---|
| `0x0000`–`0x000f` | 16-byte version string (Shift-JIS), e.g. `3wfU3.092.003.13` |
| `0x0010`–`0x002b` | `size1..size7`, 7 × LE u32 — **field meanings now confirmed** (2026-08-29 session, see [[container-format]]): `size1` is the fixed boot-loader+body+chunks slot size, `size2`/`size3` are component0's compressed/decompressed lengths, `size4`/`size5` component1 (DSP Program)'s, `size6`/`size7` component2 (DSP Data)'s |
| `0x002c`–`0x004b` | **ARM exception vector table**, 8 × 4-byte entries, all `LDR PC, [PC, #imm]` form (bytes `xx f0 9f e5` / `xx f1 9f e5`) — standard ARM reset vector table (Reset, Undef, SWI, PrefetchAbort, DataAbort, Reserved, IRQ, FIQ) |
| `0x004c`–`0x006b` | Literal pool for the vector table: 8 × 4-byte absolute handler addresses, all in the `0x1800xxxx` range (e.g. `0x1800008c`, `0x18000114`, `0x1800011c`, `0x18000120`, `0x18000124`, `0x18000128`, `0x1800012c`) — confirms handlers live in the SPI multi-I/O bus area (channel 0, per [[memory-map]]), consistent with boot-mode-3 serial-flash execute-in-place. |
| `0x0078` onward | First real boot loader function (ARM code), per `tunk.py`'s note ("120 → 0x78, ARM instructions, function prologue") — decimal 120 == 0x78, confirmed by the hex dump: `10 40 2d e9` at `0x78` is `push {r4, lr}`, a textbook ARM prologue. |

## Found: full boot sequence, already named in the prior `icom_loader.rep`
## Ghidra project (`base.dat`, read-only reference at `/data/misc/icom/7300/`)

The prior session had already named and partially documented the whole
chain. Confirmed by reading it (not re-derived from scratch):

`FUN_180045d0` (the real reset-path routine — no static callers, presumably
reached directly from the vector table above) runs, in order:

1. `cpu_init_stuff()` → `set_translation_table()` sets up the MMU: identity
   maps `0x18000000`–`0x1bffffff` (64 MB, flash/XIP, cacheable flags
   `0x8dc06`), `0x20000000`–`0x209fffff` (10 MB, RAM, flags `0x85c06`), and
   two device/uncacheable (flags `0xc12`) I/O regions
   (`0x3fe00000`+1 MB, `0xfcf00000`+49 MB) — **this independently confirms
   the region boundaries in [[memory-map]]** directly from boot code, not
   just from the hand-derived `memmap.txt`. Translation table itself lives
   at `0x20000000`.
2. Waits on an SPI status register, then `setup_port9_io()` /
   `config_spi()` / `setup_spi()` bring up the SPI flash controller.
3. `unpack_from_flash_to_mem()` — **this is the LZSS decompressor**,
   already found, byte-for-byte the same ring-buffer/`0xfee`-cursor
   algorithm as [[decompression-lzss]]. Before decompressing, it checks 16
   bytes at flash address `0x187f0000` against the literal string
   `"SX3765 V1.00-003"`; picks compressed source `0x18010000` if it
   *doesn't* match, `0x18400000` if it *does* — **always decompresses to
   RAM at `0x20005000`** (see [[multi-cpu-images]] for what the two
   sources probably mean).
4. `set_vector_base_to_ram()` — writes ARM `VBAR` (coprocessor 15) =
   `0x20005000`.
5. Cache/control cleanup, barriers, then `(*0x20005000)()` — an indirect
   call straight into the just-decompressed image.

**This settles the main-body load address: `0x20005000`, not
`0x18000000`.** The `0x1800xxxx` vector table above is *only* the
first-stage (still-in-flash) vector table; `body.bin`/`out.dat`'s own
internal vector table (its first 0x20 bytes, `8× LDR PC,[PC,#0x18]`) is the
**second-stage** one, active only after step 4 above repoints `VBAR` — its
literal pool resolves to `0x20005050`-range addresses, consistent with this.
Re-import/re-base any decompressed body in Ghidra at `0x20005000`.

## Confirmed: does not touch the diode matrix or the EEPROM

Already implicit in the 5-step boot sequence above (no GPIO/I2C peripheral
access of any kind — just MMU setup, SPI flash read, LZSS decompress,
`VBAR` repoint, jump), but verified directly rather than just inferred by
absence: searched all 66,000 bytes of `base.dat` (loaded as a memory block
in the live Ghidra project, `0x17ffffd4`-`0x180101a3`) for any literal
reference to the EEPROM's peripheral register block. The EEPROM (`IC351`,
see [[diode-matrix]]) sits on **RIIC2** (`P1_4`/`P1_5`, `ECK`/`EDT`) —
register block `RIIC2CR1`..`RIIC2DRR` = `0xFCFEE800`-`0xFCFEE840` per the
RZ/A1H hardware manual. A hex search for that address's byte prefix (both
byte orders checked) across the entire `base.dat` region returns **zero
matches** — validated against a known-good literal first (the documented
vector-table target `0x1800008c`, found correctly at `0x18000020`), so
this is a real negative, not a broken search. Combined with the
already-confirmed absence of any P5 (diode scan port) reference: **`base.dat`
touches neither the diode matrix nor the EEPROM at any point during boot**
— both are purely `body.bin`'s (the application firmware's) concern, never
touched before the jump into RAM.
