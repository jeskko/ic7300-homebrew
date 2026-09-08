"""Builds the emulated SPI-NOR flash byte image from a real firmware container.

See /notes/base-loader.md's 2026-09-08 correction section for the derivation of the address
mapping used here. Short version, ground-truthed two independent ways (fresh objdump of the
real `base.dat`'s slot-select code, cross-checked against `tools/icom_fw/container.py`'s
already round-trip-verified offsets):

    flash_address = FLASH_BASE + (container_file_offset - HEADER_LEN)

for container offsets in [HEADER_LEN, HEADER_LEN + size1) -- i.e. the container's own 16-byte
version string + size1..size7 table (44 bytes, `container.CHECKSUM_REGION_START`) is consumed
by the update tooling as in-file metadata only and never itself lands in flash. Everything from
container offset HEADER_LEN onward -- boot loader code, the body's length-prefix + LZSS stream,
and both fonts/chunk3 -- is otherwise a direct byte-for-byte copy into flash starting at
FLASH_BASE, exactly matching what `notes/firmware-update.md`'s traced `firmware_update_main`
orchestrator actually writes.

For the MVP this means: take one real, unmodified `7300_1XX.dat` container, and drop
`container[HEADER_LEN : HEADER_LEN + size1]` directly at flash address FLASH_BASE. No
unpack/repack round-trip through `tools/icom_fw` is needed to *build* the image (that tool
stays relevant for producing a *modified* container to test later, per the Stage 3 roadmap
item in the emulator plan) -- the raw container bytes already are the flash image, once the
one metadata-header skip is accounted for.

Slot B (`0x18400000`) and the active-slot marker (`0x187f0000`) are deliberately left as
erased-flash (`0xff`) for the MVP: the marker's `strncmp` against `"SX3765 V1.00-003"` fails on
all-`0xff` bytes exactly like it does on real never-updated hardware, so the boot ROM's default
"no match -> slot A" path is exercised without needing to populate slot B at all.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

# Match tools/verify_pack.py's own convention: import icom_fw as a top-level package by
# adding tools/ (not the repo root) to sys.path, rather than treating tools/ as a package
# itself (it has no __init__.py).
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from icom_fw import container as _container  # noqa: E402

FLASH_BASE = 0x18000000
FLASH_SIZE = 0x04000000  # 64 MB, per notes/memory-map.md
ERASED_BYTE = 0xFF

HEADER_LEN = _container.CHECKSUM_REGION_START  # 0x2c -- see module docstring


@dataclass
class FlashImage:
    """A flat `bytes`-like flash image plus the metadata needed to sanity-check it."""

    data: bytearray
    size1: int
    version_string: bytes

    def read(self, flash_addr: int, length: int) -> bytes:
        off = flash_addr - FLASH_BASE
        if off < 0 or off + length > len(self.data):
            raise ValueError(
                f"flash read out of range: addr={flash_addr:#x} length={length:#x}"
            )
        return bytes(self.data[off : off + length])


def build_from_container(container_path: Path | str) -> FlashImage:
    """Build a `FlashImage` from one real, unmodified `7300_1XX.dat` container file.

    Also usable with a `tools/icom_fw.container.pack()` output (a modified-body container in
    the exact same shape) for later custom-body testing -- see the module docstring.
    """
    raw = Path(container_path).read_bytes()

    version_string = raw[
        _container.VERSION_STRING_OFFSET : _container.VERSION_STRING_OFFSET
        + _container.VERSION_STRING_LEN
    ]
    size1 = int.from_bytes(
        raw[_container.SIZE_TABLE_OFFSET : _container.SIZE_TABLE_OFFSET + 4], "little"
    )

    flash = bytearray(bytes([ERASED_BYTE]) * FLASH_SIZE)
    payload = raw[HEADER_LEN : HEADER_LEN + size1]
    flash[0 : len(payload)] = payload

    return FlashImage(data=flash, size1=size1, version_string=version_string)
