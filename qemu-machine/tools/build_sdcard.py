#!/usr/bin/env python3
"""Build a real FAT16-formatted virtual SD card image for `rza1h-mmc`,
with an update container placed at the real path the official IC-7300
manual documents: copy the downloaded firmware data into an "IC-7300"
folder on the SD card (manual section 15, "Updating the firmware" --
`pdftotext` of `/data/misc/icom/7300/doc/IC-7300_ENG_FM_12b.pdf`,
searched 2026-09-08 while chasing why body.bin's own SD driver couldn't
be found statically -- DAT_200264a0, the path firmware_update_main
opens, has no static writer anywhere in the image, which now makes sense:
the full path is built at runtime by the file-selection UI code this
script's caller (tools/force_call_fup.py) deliberately bypasses).

Thin wrapper around `mkfs.vfat` + `mtools` (`mmd`/`mcopy`) -- no from-
scratch FAT writer needed, both are already available in this
environment. Does not require root or loopback mounting (mtools operates
directly on the image file).

Usage:
    tools/build_sdcard.py <container.dat> [output.img] [--name 7300_142.DAT]
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

DEFAULT_SIZE_MB = 64
UPDATE_FOLDER = "IC-7300"  # exact casing/name per the manual, section 15


def build(container_path: Path, output_path: Path, filename: str,
         size_mb: int = DEFAULT_SIZE_MB) -> None:
    output_path.write_bytes(b"\x00" * (size_mb * 1024 * 1024))

    subprocess.run(["mkfs.vfat", "-F", "16", "-n", "ICOM7300", str(output_path)],
                   check=True, capture_output=True)
    subprocess.run(["mmd", "-i", str(output_path), f"::{UPDATE_FOLDER}"], check=True)
    subprocess.run(
        ["mcopy", "-i", str(output_path), str(container_path), f"::{UPDATE_FOLDER}/{filename}"],
        check=True,
    )
    print(f"wrote {output_path} ({size_mb} MiB FAT16, "
         f"{UPDATE_FOLDER}/{filename} = {container_path})")


def main(argv: list[str]) -> int:
    if len(argv) < 1:
        print(__doc__)
        return 1
    container_path = Path(argv[0])
    output_path = Path(argv[1]) if len(argv) > 1 and not argv[1].startswith("--") else \
        Path(__file__).resolve().parent.parent / "sdcard.img"
    filename = container_path.name.upper()
    if "--name" in argv:
        filename = argv[argv.index("--name") + 1]

    build(container_path, output_path, filename)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
