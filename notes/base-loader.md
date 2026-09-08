# `base.dat` structure (the ARM boot loader region)

`base.dat` = `container[0x0:0x101d0]` (66000 bytes, constant size across all
10 releases) — see [[container-format]] for how that split was verified.
Ground-truthed directly against `7300_111/base.dat` / `7300_111/base.hex`
(read-only originals) with `xxd`.

**Correction, 2026-09-08 (while planning [[emu]]): the true flash/CPU-address mapping has a constant
`0x2c`-byte shift the offsets below don't show.** The table's offsets are *raw byte offsets within the
`base.dat` file*, historically written with a `0x18000000 +` prefix as if file offset 0 == flash address
`0x18000000` — but the real hardware mapping is **`flash_address = 0x18000000 + (file_offset - 0x2c)`**.
Confirmed two independent ways: (1) fresh `arm-none-eabi-objdump` of the real `base.dat` bytes shows the
boot code treats the hardcoded slot-select literals `0x18010000`/`0x18400000` (see below) as pointing
*directly* at the compressed body's 4-byte length prefix — which only lines up with
[[container-format]]'s independently-verified (round-trip-tested against all 10 releases)
`BODY_LENGTH_OFFSET = 0x1002c` if the shift is applied (`0x18000000 + (0x1002c - 0x2c) = 0x18010000`,
exactly); (2) [[firmware-update]]'s own already-recorded trace of `firmware_update_main`'s checksum
computation independently found it "seeks back to `0x2c` (start of the boot-loader region in the file)"
— i.e. file offset `0x2c`, not `0`, is where the update mechanism's flash-bound byte stream actually
begins. The live Ghidra project's own `base.dat` memory block was in fact already based correctly at
`0x17ffffd4`–`0x180101a3` (i.e. file offset 0 at `0x17ffffd4`, **not** `0x18000000` — exactly a `0x2c`
shift) per the "Confirmed: does not touch..." section below; only this table's prose never caught up to
that. **Net effect**: every absolute flash address quoted in the table below (and in the "found boot
sequence" section) is `0x2c` too high versus true hardware/flash addresses — read the offsets as
file-relative, not as literal flash addresses, until each is individually corrected.

| Offset | Content |
|---|---|
| `0x0000`–`0x000f` | 16-byte version string (Shift-JIS), e.g. `3wfU3.092.003.13` |
| `0x0010`–`0x002b` | `size1..size7`, 7 × LE u32 — **field meanings now confirmed** (2026-08-29 session, see [[container-format]]): `size1` is the fixed boot-loader+body+chunks slot size, `size2`/`size3` are component0's compressed/decompressed lengths, `size4`/`size5` component1 (DSP Program)'s, `size6`/`size7` component2 (DSP Data)'s |
| `0x002c`–`0x004b` | **ARM exception vector table**, 8 × 4-byte entries, all `LDR PC, [PC, #imm]` form (bytes `xx f0 9f e5` / `xx f1 9f e5`) — standard ARM reset vector table (Reset, Undef, SWI, PrefetchAbort, DataAbort, Reserved, IRQ, FIQ). **True flash address of this table is `0x18000000`** (file offset `0x2c`, minus the `0x2c` shift above) — this is genuinely the very first thing at the CPU's XIP boot address, not `0x1800002c`. |
| `0x004c`–`0x006b` | Literal pool for the vector table: 8 × 4-byte absolute handler addresses, all in the `0x1800xxxx` range (e.g. `0x1800008c`, `0x18000114`, `0x1800011c`, `0x18000120`, `0x18000124`, `0x18000128`, `0x1800012c`) — confirms handlers live in the SPI multi-I/O bus area (channel 0, per [[memory-map]]), consistent with boot-mode-3 serial-flash execute-in-place. (True flash addresses are each `0x2c` lower than quoted, e.g. `0x18000060` not `0x1800008c` — the quoted values are internally self-consistent as file offsets since the vector table's own literal pool references them the same way, so the "handlers live in the SPI bus area" conclusion still holds either way.) |
| `0x0078` onward | First real boot loader function (ARM code), per `tunk.py`'s note ("120 → 0x78, ARM instructions, function prologue") — decimal 120 == 0x78, confirmed by the hex dump: `10 40 2d e9` at `0x78` is `push {r4, lr}`, a textbook ARM prologue. |

## Found: full boot sequence, already named in the prior `icom_loader.rep`
## Ghidra project (`base.dat`, read-only reference at `/data/misc/icom/7300/`)

The prior session had already named and partially documented the whole
chain. Confirmed by reading it (not re-derived from scratch), and now
re-confirmed byte-for-byte, 2026-09-08, by directly disassembling the
real `base.dat` (`arm-none-eabi-objdump -D -b binary -m arm
--adjust-vma=0x18000000 7300_142/base.dat` — note this reproduces the
table's pre-correction, `0x2c`-too-high addressing, kept below as-is since
it's what the raw file-offset disassembly shows; subtract `0x2c` for true
flash addresses per the correction above) while planning [[emu]]:

`FUN_180045c4` (**true reset-path entry — corrects the address below**;
what the file-offset disassembly shows as `FUN_180045d0` is actually a
tail-called sub-block reached via `b`, not the function's own start; no
static callers found either way, presumably reached directly from the
vector table above) runs, in order:

1. `cpu_init_stuff()` → `set_translation_table()` sets up the MMU: identity
   maps `0x18000000`–`0x1bffffff` (64 MB, flash/XIP, cacheable flags
   `0x8dc06`), `0x20000000`–`0x209fffff` (10 MB, RAM, flags `0x85c06`), and
   two device/uncacheable (flags `0xc12`) I/O regions
   (`0x3fe00000`+1 MB, `0xfcf00000`+49 MB) — **this independently confirms
   the region boundaries in [[memory-map]]** directly from boot code, not
   just from the hand-derived `memmap.txt`. Translation table itself lives
   at `0x20000000`. Confirmed this session: the routine also explicitly
   *enables* the MMU and I/D caches here (`SCTLR` bits M/C/I set via
   `mcr p15,0,r0,c1,c0,0`) — but see step 5, this doesn't last.
2. Waits on an SPI status register at flash-XIP-independent MMIO address
   **`0x3fefa048`, bit 0** (confirmed by direct disassembly this session —
   a tight poll loop re-reads this address and masks/shifts bit 0 until
   it's `1`), then `setup_port9_io()` / `config_spi()` / `setup_spi()`
   bring up the SPI flash controller.
3. `unpack_from_flash_to_mem()` — **this is the LZSS decompressor**,
   already found, byte-for-byte the same ring-buffer/`0xfee`-cursor
   algorithm as [[decompression-lzss]]. Before decompressing, it checks 16
   bytes at flash address `0x187f0000` against the literal string
   `"SX3765 V1.00-003"`; picks compressed source `0x18010000` if it
   *doesn't* match, `0x18400000` if it *does* — **always decompresses to
   RAM at `0x20005000`** (see [[multi-cpu-images]] for what the two
   sources probably mean). Confirmed this session: the chosen slot address
   is used directly as a pointer to a 4-byte decompressed-length field,
   with the real LZSS stream starting exactly 4 bytes later (slot base
   `+0x4`) — matches [[container-format]]'s `BODY_LENGTH_OFFSET`/
   `BODY_OFFSET` exactly once the `0x2c` shift above is applied.
4. `set_vector_base_to_ram()` — writes ARM `VBAR` (coprocessor 15) =
   `0x20005000`.
5. **New finding, 2026-09-08**: immediately before the final jump (step
   below), the MMU and I/D caches and branch prediction that step 1 turned
   on are explicitly turned back **off** again — a direct
   `mrc p15,0,r0,c1,c0,0` / `bic` (clears `SCTLR` bits `M`/`C`/`I`/`Z`,
   i.e. `0x1`/`0x4`/`0x1000`/`0x800`) / `mcr` / `dsb`/`isb` sequence, not a
   call to a shared helper. **`body.bin` therefore starts executing its
   very first instruction at `0x20005000` with the MMU off and both caches
   off — flat physical addressing throughout `base.dat`'s own hand-off,
   no working translation table needs to be modeled to reach that point.**
   (Confirmed directly reachable from the reset entry above; not yet
   confirmed whether `body.bin` re-enables the MMU itself later — out of
   scope for this pass.)
6. Cache/control cleanup, barriers, then `(*0x20005000)()` — an indirect
   call straight into the just-decompressed image (confirmed via a final
   PC-relative `ldr r0, =0x20005000` immediately followed by `bx r0`).

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
