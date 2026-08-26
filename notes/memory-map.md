# IC-7300 main CPU memory map (Renesas RZ/A1H)

Source: hand-derived by the user in `/data/misc/icom/memmap.txt` (read-only
original), cross-referenced against the Renesas RZ/A1H/RZ/A1M Hardware
User's Manual shipped alongside the service manuals in
`/data/misc/icom/7300/doc/REN_r01uh0403ej0600_rz_a1h_MAT_20210129-2931443.pdf`.

The main CPU is an ARM Cortex-A9-based **Renesas RZ/A1H**.

| Range | Size | Region |
|---|---|---|
| `0x00000000`–`0x03ffffff` | 64 MB | CS0 space |
| `0x04000000`–`0x07ffffff` | 64 MB | CS1 space |
| `0x08000000`–`0x0bffffff` | 64 MB | CS2 space |
| `0x0c000000`–`0x0fffffff` | 64 MB | CS3 space |
| `0x10000000`–`0x13ffffff` | 64 MB | CS4 space |
| `0x14000000`–`0x17ffffff` | 64 MB | CS5 space |
| `0x18000000`–`0x1bffffff` | 64 MB | SPI multi-I/O bus area, channel 0 |
| `0x1c000000`–`0x1fffffff` | 64 MB | SPI multi-I/O bus area, channel 1 |
| `0x20000000`–`0x209fffff` | 10 MB | On-chip RAM page (`0x20000000`–`0x2001ffff` may be data-retention RAM) |
| `0x20a00000`–`0x3fef9fff` | — | reserved |
| `0x3fefa000`–`0x3fefbfff` | 8 KB | I/O |
| `0x3fefc000`–`0x3fffbfff` | — | reserved |
| `0x3fffc000`–`0x3fffffff` | 16 KB | I/O |
| `0xe8000000`–`0xe801ffff` | 128 KB | I/O |
| `0xe8030000`–`0xe804ffff` | 128 KB | I/O |
| `0xe8100000`–`0xe813ffff` | 256 KB | I/O |
| `0xe8200000`–`0xe822ffff` | 192 KB | I/O |
| `0xfc000000`–`0xfc07ffff` | 512 KB | I/O |
| `0xfcfe0000`–`0xfcffffff` | 128 KB | I/O |
| `0xffff0000`–`0xffffffff` | 64 KB | I/O (also: exception vector table location at boot, see below) |

Note (original memmap.txt, Finnish): "mirroreita välissä 0x4000-0x609f" —
mirrored regions somewhere in the `0x4000`–`0x609f` range within one of the
above windows; not yet pinned down to a specific base.

## Boot configuration (from `tunk.py` notes, GPIO strapping)

- `P0_0` / `MD_BOOT0` pulled up
- `P0_1` / `MD_BOOT1` pulled down
- `P7_0` / `MD_BOOT2` pulled up
- → **Boot mode 3: serial flash booting**
- Exception vector table lives at `0xffff0000` at boot.
- Code execution for the loaded image starts around `0x18000000` (start of
  the SPI multi-I/O bus area, channel 0) — consistent with boot-mode-3
  serial (SPI) flash XIP.

## Ground-truthed via Ghidra (`icom1` project, `body.bin` + prior projects) — see [[base-loader]], [[firmware-update]]

**Two-stage execution, two different base addresses — don't conflate
them:**

1. **Boot loader (`base.dat`), runs in place, XIP from flash at
   `0x18000000`.** This is the *only* stage that actually executes
   directly out of the `0x18000000`+ SPI window. It sets up the MMU, SPI
   controller, and copies+decompresses the main body into RAM.
2. **Main firmware (`body.bin`/`out.dat`/`unpacked.dat`), decompressed
   into and executed from RAM at `0x20005000`.** This is where real
   analysis of the main firmware belongs — confirmed three independent
   ways (derived from `base.dat`'s `unpack_from_flash_to_mem` destination
   constant; matches two prior Ghidra projects' own basing; and confirmed
   by clean disassembly of the reset handler and its cache-disable/SCTLR
   idiom once rebased there). **`0x18000000` was a wrong guess for where
   to load the decompressed body in Ghidra — corrected in this session.**

**MMU table (`set_translation_table` in `base.dat`) confirms the region
boundaries above directly from boot code**, not just from
`memmap.txt`/the manual: identity-maps `0x18000000`–`0x1bffffff` (64 MB,
flash/XIP, cacheable), `0x20000000`–`0x209fffff` (10 MB, RAM), and two
device/uncacheable I/O windows (`0x3fe00000`+1 MB, `0xfcf00000`+49 MB).
Translation table itself lives at `0x20000000` (RAM base).

**SPI Multi-I/O Bus Controller (SPIBSC) register base, read directly from
`base.dat`'s literal pool: `SPI_BASE = 0x3fefa000`** — confirms this is
what lives in the "8 KB I/O" region listed above (`0x3fefa000`–`0x3fefbfff`).
Status register at `SPI_BASE+0x48` (`0x3fefa048`). `PORT_BASE = 0xfcfe7000`,
inside the `0xfcfe0000`–`0xfcffffff` I/O region.

**Flash slot layout (the resolution of the old "second image" open
question below) — see [[firmware-update]] for the full derivation and
[[multi-cpu-images]] for how it was found:**

| Region | Flash address | Notes |
|---|---|---|
| boot loader (`base.dat`) | `0x18000000` | shared, not duplicated per slot |
| slot A body (LZSS-compressed) | `0x18010000` | `FLASH_1801`; decompresses to RAM `0x20005000` |
| slot A chunk1 (font, raw `.ttf`) | `0x18210000` | verified: exact match to code (`FUN_20062c64`) |
| slot A chunk2 (font, raw `.ttf`) | `0x18240000` | verified: exact match to code |
| slot A chunk3 (raw, unidentified) | `0x18250000` | |
| slot B body (LZSS-compressed) | `0x18400000` | `FLASH_1840` |
| slot B chunk1/2/3 | `0x18600000` / `0x18630000` / `0x18640000` | same relative offsets as slot A |
| active-slot marker (16 B) | `0x187f0000` | `"SX3765 Vx.xx-xxx"`-shaped string, checked identically at boot and at runtime |

Verified in Ghidra: `body.bin`'s program now has real backing memory
blocks at all of the slot-A/boot-loader addresses (imported from
`base.dat`/`chunk1.ttf`/`chunk2.ttf`/`chunk3.dat`) and slot B's three
chunk addresses (same three files) — every XIP flash reference the
running firmware makes resolves to real, inspectable content instead of
unmapped memory.

## External flash / peripherals (from `tunk.py` notes)

- **IC391**: 64 MB SPI flash — pins `SFLCK`/`SFLSS`/`SFLD0-3` on
  `P9_2`–`P9_7` (`SPBCLK_0`/`SPBSSL_0`/`SPBIO{0,1,2,3}0_0`).
- **IC902**: 32 MB flash, physically next to the DSP.
- **IC351**: RTC + EEPROM, I2C: `RTC_IRQ`/`RTC_SCL`/`RTC_SDA` on
  `P1_1`–`P1_3` (RIIC0/1), EEPROM `ECK`/`EDT` on `P1_4`–`P1_5` (RIIC2).
- A UART link between the main board and a secondary board: main unit
  `LRXD`(pin 37)/`LTXD`(pin 36) ↔ secondary board `TOOLTxD`/`TOOLRxD`
  (pins 34/33) — likely the debug/programming path to the companion chip
  (see [[multi-cpu-images]]).
- FPGA-adjacent signals noted but not mapped: `spdo stat done spck cfg`,
  and an I/O block `0xE8008800`–`0xE8008828`.

## Open questions
- Exact base addresses for the two "mirrors" and the FPGA config block.
- ~~Whether `0x18000000` is where the *whole* container is mapped~~ —
  resolved, see the "Ground-truthed via Ghidra" section above and
  [[firmware-update]]: `0x18000000` is XIP flash boot-loader execution
  only; the main body runs from RAM at `0x20005000`, and flash holds two
  full A/B "body + chunks" slots plus a shared boot loader, not a single
  container. The `tunk.py` `"187f000"`/`"18212c8"` number pair itself
  remains unexplained (see [[multi-cpu-images]]) — it's numerically too
  small to be a `0x18xxxxxx`-range address, so it likely indexes into a
  different data source (possibly a raw flash dump) we haven't located.
