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

**The +1 offset below is not a typo.** riic.c's own comment (and this
project's own live tracing) found a real "dummy read after switching to
receive mode" in the driver's own RI interrupt handler (FUN_2001dbcc) --
a dead store the decompiler drops entirely from the visible C. The byte
the driver actually *keeps* for destination position 0 of any multi-byte
RIIC2 read comes from `mem_addr + 1`, not `mem_addr` -- confirmed by
watching a byte-perfect reference string arrive shifted left by one with
trailing garbage until this offset was applied. So the *content* goes at
0x3e01/0x3e81/0x3fc1, one byte past each nominal parameter-ID offset.
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
]

DUMMY_READ_SHIFT = 1


def build(output_path: Path, size: int = DEFAULT_SIZE) -> None:
    data = bytearray(size)
    for nominal_offset, content in ENTRIES:
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
    for nominal_offset, content in ENTRIES:
        print(f"  {nominal_offset:#06x} (+{DUMMY_READ_SHIFT} dummy-read shift "
             f"-> {nominal_offset + DUMMY_READ_SHIFT:#06x}): {content!r}")


def main(argv: list[str]) -> int:
    output_path = Path(argv[0]) if argv else (
        Path(__file__).resolve().parent.parent / "riic2_eeprom.img"
    )
    build(output_path)
    print(f"\nUsage: pass this as RIIC2's backing image, e.g.\n"
         f"  qemu-src/build/qemu-system-arm -M rz-a1h ... \\\n"
         f"    -global rza1h-riic.image={output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
