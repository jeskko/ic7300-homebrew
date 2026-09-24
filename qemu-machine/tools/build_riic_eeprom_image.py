#!/usr/bin/env python3
"""Build a real, ROM-sourced virtual RIIC2 EEPROM image -- the one that
lets a natural boot clear body.bin's entire cold-boot-vs-power-state
branch decision (see qemu-machine/README.md's "The 0x2002b540 mystery
resolved..." section for the full derivation). Without this image (or an
equivalent), a fresh boot's virtual EEPROM answers every read with 0x00,
FUN_2002b29c takes the power-state/watchdog branch instead, and none of
this project's feature tasks (including sdcard_file_rpc_dispatch_task)
ever get created -- the single biggest lever on how far this emulator's
boot progresses.

Every byte here is either a literal ROM-embedded reference string pulled
directly out of body.bin via Ghidra (not guessed, not a real captured
EEPROM dump -- see below) or a value confirmed by testing.

Real, confirmed offsets (all EEPROM-parameter-ID-relative, i.e. literal
byte offsets into this flat image, matching FUN_2001e510's own
`(param_id, dest, length)` getter convention -- see
notes/eeprom-catalogue.md):

  0x3e00  (1 byte)   read by FUN_2002b274; its top bit feeds directly
                     into FUN_2002b29c's own bVar8 -- not otherwise
                     identified, 0xff (bit 7 set) is what this project
                     tested with, not confirmed to be the *real* stored
                     value on real hardware.
  0x3e80  (16 bytes) compared by FUN_20029224 against the real ROM
                     reference "SX3765 V4.81-000" (DAT_2002a08c ->
                     0x2018d796) -- the *current native* EEPROM format
                     signature, distinct from FUN_20024738's own
                     4-entry legacy-compat table (V3.41/V4.41/V4.61/
                     V4.71, see notes/eeprom-catalogue.md).
  0x3fc0  (16 bytes) compared by FUN_200291d8 against the real ROM
                     reference "SX3765 V0.30-000" (DAT_2002a088 ->
                     0x2018d786).

**Offsets are nominal (2026-09-24).** Until then the image stored everything
at +1: riic.c served real EEPROM data on the RIIC's post-address dummy read
(FUN_2001dbcc discards it), so reads came back one address late and the image
compensated. The firmware's own writes land at nominal offsets, though, so
nothing it saved ever read back correctly. riic.c now models the dummy read
(it returns the address byte, no EEPROM data consumed) and the image uses the
firmware's real offsets.
"""

from __future__ import annotations

import sys
from pathlib import Path

DEFAULT_SIZE = 0x4000  # comfortably covers every offset below

# (nominal EEPROM parameter-ID offset, bytes to write) -- the +1 dummy-
# read shift is applied automatically, see module docstring.
ENTRIES: list[tuple[int, bytes]] = [
    (0x3e00, bytes([0xFF])),
    (0x3e80, b"SX3765 V4.81-000"),
    (0x3fc0, b"SX3765 V0.30-000"),
    # SET > DISPLAY > "Opening Message" (factory-reset item 0x70, record 0x20192acc: live
    # value 0x203de53b = NVRAM region 2 base 0x203de4cc + 0x6f -> EEPROM 0x1a20+0x6f; type
    # byte 1, default +0x08 = 1 = "ON"). With the blank image's 0 ("OFF"),
    # system_mode_request_dispatch skips the boot splash (FUN_2002a2a4) entirely -- and that
    # splash is the only thing a real radio draws at power-on without user input.
    (0x1a8f, bytes([0x01])),
    # (VFO state at 0x2000.. and its slot index 0x3e40 used to be hand-written here; the
    # factory base image now carries the firmware's own values, which also fill a slot
    # field the hand-written copy zeroed.)
]

DUMMY_READ_SHIFT = 0  # see module docstring (was 1 before riic.c modelled the dummy read)


FACTORY_BASE = Path(__file__).resolve().parent / "eeprom_factory_defaults.bin"


def build(output_path: Path, size: int = DEFAULT_SIZE, pwrk_hold: bool = False) -> None:
    # 2026-09-24: start from the firmware's own factory defaults (captured by
    # tools/capture_factory_eeprom.py via a real All Reset), so every setting -- CI-V address
    # 0x94, etc. -- has its real default instead of 0. ENTRIES then only pin what the boot
    # path needs (they already agree with the factory image; kept for documentation).
    data = bytearray(FACTORY_BASE.read_bytes()[:size]) if FACTORY_BASE.exists() else bytearray(size)
    data += bytes(size - len(data))
    entries = list(ENTRIES)
    if pwrk_hold:
        # riic2_eeprom_pwrk_test.img: bit 7 of 0x3e00 clear takes the PWRK-hold power-on
        # branch (FUN_2002b29c), see README.md
        entries.append((0x3e00, bytes([0x7F])))
    for nominal_offset, content in entries:
        real_offset = nominal_offset + DUMMY_READ_SHIFT
        end = real_offset + len(content)
        if end > size:
            raise ValueError(
                f"entry at {nominal_offset:#x} (len {len(content)}) needs "
                f"size >= {end:#x}, got {size:#x}"
            )
        data[real_offset:end] = content
    output_path.write_bytes(bytes(data))
    print(f"wrote {output_path} ({size:#x} bytes)")
    for nominal_offset, content in entries:
        print(f"  {nominal_offset + DUMMY_READ_SHIFT:#06x}: {content!r}")


def main(argv: list[str]) -> int:
    pwrk = "--pwrk-hold" in argv
    argv = [a for a in argv if a != "--pwrk-hold"]
    output_path = Path(argv[0]) if argv else (
        Path(__file__).resolve().parent.parent /
        ("riic2_eeprom_pwrk_test.img" if pwrk else "riic2_eeprom.img")
    )
    build(output_path, pwrk_hold=pwrk)
    print(f"\nUsage: pass this as RIIC2's backing image, e.g.\n"
         f"  qemu-src/build/qemu-system-arm -M rz-a1h ... \\\n"
         f"    -global rza1h-riic.image={output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
