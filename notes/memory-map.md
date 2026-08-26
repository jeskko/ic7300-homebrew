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
- Whether `0x18000000` is where the *whole* container is mapped, or just
  the main-CPU sub-image after the multi-image split (see
  [[multi-cpu-images]]) — `tunk.py` has a note "`187f000 5dd38 - image
  alkais 18212c8`" (Finnish "image starts at 18212c8") that looks like a
  lead on a *second* image start address within this space; not yet
  verified.
