#!/usr/bin/env python3
"""Build a homebrew app (C sources + sdk/runtime) into an APP.BIN for the SD card.

Usage:
    python3 sdk/tools/build_app.py -o APP.BIN main.c [more.c ...]

Put the output at C:\\IC-7300\\APP.BIN on the card and start it from
MENU > SET > SD Card > Homebrew Apps (needs the loader firmware from sdk/loader/).
Requires arm-none-eabi-gcc on PATH.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SDK = Path(__file__).resolve().parent.parent
RUNTIME = SDK / "runtime"
INCLUDE = SDK / "include"

APP_REGION = 0x20610000
APP_REGION_SIZE = 0x20000
APP_MAGIC = 0x31304248

CFLAGS = [
    "-mcpu=cortex-a9", "-marm", "-mfloat-abi=soft",
    "-Os", "-std=c11", "-Wall", "-Wextra",
    "-ffreestanding", "-fno-builtin", "-nostdlib", "-fno-pic",
    "-ffunction-sections", "-fdata-sections",
    f"-I{INCLUDE}",
]
RUNTIME_SOURCES = ["crt0.S", "runtime.c", "ui_dialog.c", "libc.c"]


def build(sources: list[Path], out: Path, keep_dir: Path | None = None) -> bytes:
    work = keep_dir or Path(tempfile.mkdtemp(prefix="hbapp-"))
    work.mkdir(parents=True, exist_ok=True)
    elf = work / "app.elf"
    all_sources = [RUNTIME / s for s in RUNTIME_SOURCES] + sources
    subprocess.run(
        ["arm-none-eabi-gcc", *CFLAGS, "-T", str(RUNTIME / "app.ld"), "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments",
         "-Wl,-Map," + str(work / "app.map"), *map(str, all_sources), "-lgcc", "-o", str(elf)],
        check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", str(elf), str(out)], check=True)

    image = out.read_bytes()
    magic, abi, entry, image_end = struct.unpack_from("<4I", image)
    assert magic == APP_MAGIC, f"bad header magic {magic:#x} -- .hb_header not first?"
    assert APP_REGION <= entry < APP_REGION + len(image), f"entry {entry:#x} outside the file"
    assert image_end <= APP_REGION + APP_REGION_SIZE, f"image end {image_end:#x} overflows region"
    print(f"{out}: {len(image)} bytes, entry {entry:#x}, "
          f"RAM footprint {image_end - APP_REGION:#x} of {APP_REGION_SIZE:#x}")
    return image


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--keep", type=Path, help="keep the ELF/map in this directory")
    ap.add_argument("sources", type=Path, nargs="+")
    args = ap.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        build(args.sources, args.output, args.keep)
    except subprocess.CalledProcessError as e:
        sys.exit(e.returncode)


if __name__ == "__main__":
    main()
