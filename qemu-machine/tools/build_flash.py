#!/usr/bin/env python3
"""Build a flat flash image for the `rz-a1h` QEMU machine.

Thin wrapper around the already-existing, already-tested `emu/flash_image.py`
-- no reimplementation of the container-offset-correction logic (the `0x2c`
header shift, see notes/base-loader.md's correction) here. QEMU's machine
code (`qemu-machine/src/rz_a1h.c`) loads the resulting file directly via
`rom_add_file_fixed()` at the real flash base address.

Usage:
    python3 qemu-machine/tools/build_flash.py [/path/to/7300_142.dat] [output.bin]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

from emu import flash_image  # noqa: E402

DEFAULT_CONTAINER = Path(os.environ.get("ICOM_FW_DIR", Path(__file__).resolve().parent.parent.parent / "firmware")) / "7300_142.dat"
DEFAULT_OUTPUT = Path(__file__).resolve().parent.parent / "flash.bin"


def main(container_path: Path, output_path: Path) -> int:
    image = flash_image.build_from_container(container_path)
    output_path.write_bytes(bytes(image.data))
    print(f"wrote {len(image.data)} bytes to {output_path} (size1={image.size1:#x}, "
          f"version={image.version_string!r})")
    return 0


if __name__ == "__main__":
    container = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CONTAINER
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT
    raise SystemExit(main(container, output))
