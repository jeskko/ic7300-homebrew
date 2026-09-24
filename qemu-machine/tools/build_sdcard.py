#!/usr/bin/env python3
"""Build an MBR-partitioned FAT32 SD card image for the SDHI0 controller
(sdhi.c). The image includes an MBR partition table and one FAT32 partition
at sector 2048 (1 MiB offset) filling the rest of the card.

With a container argument, copies the firmware data to ::IC-7300/<NAME> as
documented in the official IC-7300 manual, section 15 ("Updating the firmware").
Without a container, builds a blank formatted card; the firmware creates the
IC-7300/{Decode,Voice,Setting,Capture,VoiceTx} folder structure on first insert.

Usage:
    tools/build_sdcard.py [container.dat] [-o OUT] [--name NAME] [--size-mb N]

    # Blank 128 MiB card (power-of-two sizes only):
    tools/build_sdcard.py

    # With firmware container:
    tools/build_sdcard.py firmware.dat -o sdcard.img --size-mb 256

    # Use with the emulator:
    tools/run_gui.py --sd qemu-machine/sdcard.img --icount off
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

DEFAULT_SIZE_MB = 128
UPDATE_FOLDER = "IC-7300"  # exact casing/name per the manual, section 15


def is_power_of_two(n: int) -> bool:
    """Check if n is a power of two."""
    return n > 0 and (n & (n - 1)) == 0


def build(output_path: Path, size_mb: int, container_path: Path | None = None,
          filename: str | None = None) -> None:
    """Build an MBR + FAT32 SD card image.

    Args:
        output_path: Path to write the image file.
        size_mb: Size in MiB (must be a power of two).
        container_path: Optional firmware container to copy to ::IC-7300/<NAME>.
        filename: Override name for the container file on the card.
    """
    if not is_power_of_two(size_mb):
        raise ValueError(f"Size {size_mb} MiB is not a power of two")

    # Create zero-filled image
    total_bytes = size_mb * 1024 * 1024
    output_path.write_bytes(b"\x00" * total_bytes)

    # Create MBR partition table with sfdisk
    # Partition starts at sector 2048 (1 MiB), type 0x0c (FAT32 LBA)
    sfdisk_input = "start=2048, type=c\n"
    subprocess.run(
        ["sfdisk", str(output_path)],
        input=sfdisk_input.encode(),
        check=True,
        capture_output=True,
    )

    # Format the partition with FAT32
    # Partition size in KiB = total size - 1 MiB offset
    partition_size_kib = size_mb * 1024 - 1024
    subprocess.run(
        ["mkfs.vfat", "-F", "32", "-n", "ICOM7300", "--offset", "2048",
         str(output_path), str(partition_size_kib)],
        check=True,
        capture_output=True,
    )

    # If container provided, copy it to the card
    if container_path:
        if filename is None:
            filename = container_path.name.upper()
        # Create IC-7300 directory and copy container
        subprocess.run(
            ["mmd", "-i", f"{output_path}@@1M", f"::{UPDATE_FOLDER}"],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["mcopy", "-i", f"{output_path}@@1M", str(container_path),
             f"::{UPDATE_FOLDER}/{filename}"],
            check=True,
            capture_output=True,
        )
        print(f"wrote {output_path} ({size_mb} MiB, "
              f"MBR+FAT32, {UPDATE_FOLDER}/{filename} = {container_path})")
    else:
        print(f"wrote {output_path} ({size_mb} MiB, MBR+FAT32, blank)")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "container",
        nargs="?",
        help="Optional firmware container to copy to ::IC-7300/<NAME>",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output image path (default: qemu-machine/sdcard.img)",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="Override container filename on the card (default: container's basename, uppercase)",
    )
    parser.add_argument(
        "--size-mb",
        type=int,
        default=DEFAULT_SIZE_MB,
        help=f"Image size in MiB, must be a power of two (default: {DEFAULT_SIZE_MB})",
    )

    args = parser.parse_args(argv)

    # Determine output path
    if args.output:
        output_path = Path(args.output)
    else:
        output_path = Path(__file__).resolve().parent.parent / "sdcard.img"

    # Prepare container arguments
    container_path = None
    filename = args.name
    if args.container:
        container_path = Path(args.container)
        if not container_path.exists():
            print(f"error: container file not found: {args.container}", file=sys.stderr)
            return 1
        if filename is None:
            filename = container_path.name.upper()

    try:
        build(output_path, args.size_mb, container_path, filename)
        return 0
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
