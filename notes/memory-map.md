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
- **Not the same pins as JTAG** — `P0_0`/`P0_1` (general Port 0, these boot straps) are physically separate
  from `JP0_0`/`JP0_1` (JTAG Port 0, `TDI`/`TDO`) despite the similar naming; see
  [[hardware-debug-access]]'s "Firmware readiness check" section for the full manual-sourced distinction —
  an earlier hedge in that file conflating the two has been corrected.
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

**SD card: SD Host Interface channel 0 (SDHI0), `0xe804e000`** — not MMCIF. `body.bin` passes
this base to Renesas' SD driver library (`system_mode_request_dispatch` ->
`FUN_2017df40(0, 0xE804E000, work)`, the library's `sd_init`); sector reads/writes use DMAC ch7
with `0xE804E000` as the fixed peripheral side (the manual's 64-byte-unit DMA address). GIC
302/303/304 = card detect / card access / SDIO. Registers per the RZ/A1H manual chapter 50;
modelled in `qemu-machine/src/sdhi.c` (confirmed live 2026-09-25: mount, folder creation,
settings file write). SD_INFO1.INFO7 = 1 reads as *writable* to this firmware.

**MMC Host Interface (MMCIF), `MMC_BASE = 0xe804c800`** — present on the chip but unused by
`body.bin`: zero references anywhere in the image (the long-open "where is the SD driver"
question, answered above). `qemu-machine/src/mmc.c` still models it standalone.

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
- **IC351**/**IC381**: two separate chips on separate I2C pairs, not one combined RTC+EEPROM part
  as originally guessed here — **corrected, see [[ic7300-hardware]]**: `IC381` (`RX-8803LC`) is the
  RTC, `RTC_IRQ`/`RTC_SCL`/`RTC_SDA` on `P1_1`–`P1_3` (RIIC0/1, narrowed to **specifically RIIC1** by
  the SVD xref sweep below — no RIIC0 driver was found); `IC351` (`GT24C128B`) is the EEPROM,
  `ECK`/`EDT` on `P1_4`–`P1_5` (RIIC2, confirmed — see below).
- A UART link between the main board and a secondary board: main unit
  `LRXD`(pin 37)/`LTXD`(pin 36) ↔ secondary board `TOOLTxD`/`TOOLRxD`
  (pins 34/33) — likely the debug/programming path to the companion chip
  (see [[multi-cpu-images]]).
- FPGA-adjacent signals noted but not mapped: `spdo stat done spck cfg`,
  and an I/O block `0xE8008800`–`0xE8008828`.

## Peripheral SVD import + xref sweep (2026-08-29)

Imported a community-sourced `rza1.svd` (70 peripherals, real Renesas register names) into the live
Ghidra project via a patched copy of [leveldown-security/SVD-Loader-Ghidra](https://github.com/leveldown-security/SVD-Loader-Ghidra)
staged at `~/ghidra_scripts/SVD-Loader.py` (outside the repo, not committed — Jython script run from
Ghidra's Script Manager, not reachable through the MCP bridge). The stock script crashed twice on
this particular SVD and needed local patches (also not committed, live only in the staged copy):
this SVD omits `<addressBlock>` entirely for every peripheral, and a handful of registers (`DMAC`'s
`DMARS0-15`, one `INTC` register, several `CPG` registers) encode `addressOffset` as *(their real
absolute address − this peripheral's base)* rather than a small in-block offset, because the
datasheet documents them under one peripheral's chapter while they physically live elsewhere —
naively trusting those inflates a memory block to hundreds of MB. All 70 peripherals now have real
memory blocks, labels, and register-field structs in the live project (`icom1`, git-ignored).

**Important tooling gotcha, worth remembering before trusting a similar sweep again**: the `ghidra`
MCP's `references_to` on a peripheral's base address is **not reliable** when the real code reaches
that peripheral through an indirect pointer cell (`ldr r0, [some_global]; ...; str r1, [r0, #offset]`)
rather than a direct literal load — which is nearly all serial/I2C driver code in this firmware, all
routed through shared `FUN_20360aXX`-style bitfield-write helpers taking a runtime base pointer.
Observed twice: querying RIIC0's base (`0xFCFEE000`) surfaced a function that, once traced through
its actual pointer cell via a raw `memory read`, turned out to configure RIIC1 (`0xFCFEE400`)
instead — and the same one-bank-off pattern repeated querying RIIC1's base, surfacing what's really
RIIC2's driver. **Don't trust an xref hit on one of these base addresses without confirming the real
target via a raw `memory read` of whatever pointer cell the code actually dereferences** — the
decompiled `DAT_xxxxxxxx` pseudo-names give no indication when this has happened. Confirmed
trustworthy: hits on `DMAC`/`INTC`/`WDT` all checked out exactly against a direct raw memory read.

**Findings that survived verification**:
- **`riic1_driver_init`** (`0x200511bc`, base `0xFCFEE400`) and **`riic2_driver_init`**
  (`0x2001e120`, base `0xFCFEE800`) — structurally identical I2C driver-init functions (CPG
  clock-enable, 7-port GPIO pin-mux fan-out, direct register config, 6 consecutive interrupt-event
  IDs each: 197–202 for RIIC1, 205–210 for RIIC2). Matches and narrows the `IC351`/`IC381` I2C
  assignment above. No RIIC0 driver found in this pass — open question, not confirmed absent.
- **`gic_distributor_disable`** (`0x200b8210`) / its enable counterpart (inlined prologue at
  `0x200b81fc` inside `FUN_200b848c`) — clear/set bit 0 (`EnableGrp0`) of the GIC Distributor
  Control Register, `0xE8201000`, confirmed exactly against `INTC`'s SVD base. Refines, doesn't
  discover: `FUN_200b848c`/`FUN_200b8630` were already identified as "GIC initialization" in an
  earlier session's boot-chain audit (see [[multi-cpu-images-history]]'s "checked for a bulk
  BSS-clear/init loop" section) — this just pins the specific register/bit within that already-known
  GIC-init code.
- **`DMAC`/`SSIF0`/`SSIF1` hits both re-surfaced already-known ground** — `0x200b5e08` and the
  `SSIF0`/`SSIF1` register-config hits both fall inside `FUN_200b5cdc`/`FUN_200b5dc0`, already named
  and described in detail in [[ic7300-signal-chain]] as "the SSIF/DMAC cache-flush-and-transfer
  function." No new information here — flagged so a future sweep doesn't re-spend effort re-deriving
  it a third time.
- **Corroboration, not a new finding, for [[firmware-update]]'s already-fully-traced
  `"Fup_AutoEnd_3765"`/watchdog-reset mechanism**: this sweep independently re-found the same
  `FUN_20029ca4` watchdog-register writes documented there in detail already (register names,
  values, the `fup_autoend_marker_write()` call, all matching exactly). The only actual new
  information is that the SVD's own register names (`WRCSR`/`WTCNT`/`WTCSR`) match the names
  [[firmware-update]] already derived from the hardware manual — an independent cross-check of
  those manually-transcribed names, nothing more. Added a PRE comment at `0x20029a10` in Ghidra
  pointing back to [[firmware-update]] for anyone landing on that address cold.
- **`SSIF0`/`SSIF1`** (`0xE820B000`/`0xE820B800`): one shared init function (`0x2005fdb4`)
  configures both at matching relative offsets (dual-channel audio init) — noted, not named or
  chased further.

## RAM above the homebrew loader: never written by the firmware (marker sweep, 2026-09-25) ✅ emulator only

`0x20601000`–`0x2080afff` (2088 KB, from the page after `sdk/loader/` up to the first page the
firmware uses) is **never written by the firmware**, in the emulator, across a boot plus a
scenario covering most of the radio's features. How it was measured:
`qemu-machine/tools/ram_marker_sweep.py` writes an address-keyed pattern over the whole range at
reset (QEMU's `loader` device, before the first instruction), then dumps the range after each
scenario step and diffs it against the previous dump. Any write, zeroing included, counts.

- **Features covered (0 bytes written in each):**
  - Boot.
  - Scope on, including the expanded view and SPAN/CENT/FIX.
  - Seven modes and three bands.
  - All eight page-1 MENU screens.
  - CW keyer send and RTTY decode.
  - Transmit over CI-V in USB/RTTY/CW, and a tune.
  - QSO recorder: 15 s recorded to SD, then played back.
  - SD Save Setting and Load Setting.
- **Positive control:** the SDK apps (CUBE, MINES) are launched in the same run. Their writes
  show up exactly where expected, in the app region and both framebuffers
  (`0x20640000`–`0x206bfbff`), so the harness does catch writes.
- **Not covered:**
  - Screen capture: a POWER tap doesn't trigger it in the emulator.
  - Voice TX memory recording: the scenario's taps didn't start it.
  - Firmware update.
  - Real audio in TX: the emulator doesn't show TX on screen, and TX is only partly modelled.
  - Long sessions.
  - Real hardware.
- **Above the range** the firmware does use RAM: non-zero pages start at `0x2080b000`, and GR2's
  UI framebuffer sits at `0x20974fe0`. A zero-page scan underestimates use there: GR2 read only
  88 KB non-zero of its 255 KB, because black pixels read as zero.

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
