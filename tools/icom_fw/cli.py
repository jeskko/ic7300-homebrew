#!/usr/bin/env python3
"""Unpack an IC-7300 firmware container into its component files.

Usage:
    python3 -m icom_fw.cli <firmware.dat> <output_dir>

Writes: body.bin, chunk1_font1.ttf, chunk2_font2.ttf, chunk3.bin,
chunk4.bin, chunk5_tail.bin, and a manifest.json describing offsets/sizes/
warnings. See /notes/container-format.md for what each component is.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from .container import parse


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <firmware.dat> <output_dir>", file=sys.stderr)
        return 2

    in_path = Path(argv[1])
    out_dir = Path(argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    data = in_path.read_bytes()
    fw = parse(data)

    (out_dir / "body.bin").write_bytes(fw.body.decompressed)
    (out_dir / "chunk1_font1.ttf").write_bytes(fw.chunk1_font.data)
    (out_dir / "chunk2_font2.ttf").write_bytes(fw.chunk2_font.data)
    (out_dir / "chunk3.bin").write_bytes(fw.chunk3.data)
    (out_dir / "chunk4.bin").write_bytes(fw.chunk4.decompressed)
    (out_dir / "chunk5_tail.bin").write_bytes(fw.chunk5_tail.decompressed)

    manifest = {
        "source": str(in_path),
        "total_size": fw.total_size,
        "version_string": fw.version_string,
        "size_table": list(fw.size_table),
        "body_length": fw.body_length,
        "components": {
            "body": {
                "offset": fw.body.offset,
                "compressed_size": fw.body.compressed_size,
                "decompressed_size": len(fw.body.decompressed),
            },
            "chunk1_font1": {
                "offset": fw.chunk1_font.offset,
                "size": fw.chunk1_font.size,
            },
            "chunk2_font2": {
                "offset": fw.chunk2_font.offset,
                "size": fw.chunk2_font.size,
            },
            "chunk3": {
                "offset": fw.chunk3.offset,
                "size": fw.chunk3.size,
            },
            "chunk4": {
                "offset": fw.chunk4.offset,
                "compressed_size": fw.chunk4.compressed_size,
                "decompressed_size": len(fw.chunk4.decompressed),
            },
            "chunk5_tail": {
                "offset": fw.chunk5_tail.offset,
                "compressed_size": fw.chunk5_tail.compressed_size,
                "decompressed_size": len(fw.chunk5_tail.decompressed),
            },
        },
        "warnings": fw.warnings,
        "accounted_bytes": fw.accounted_bytes(),
        "unaccounted_bytes": fw.total_size - fw.accounted_bytes(),
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    print(f"Version: {fw.version_string!r}")
    print(f"Total size: {fw.total_size}")
    print(f"Body: {len(fw.body.decompressed)} decompressed "
          f"({fw.body.compressed_size} compressed)")
    print(f"chunk1 (font1): {fw.chunk1_font.size} bytes")
    print(f"chunk2 (font2): {fw.chunk2_font.size} bytes")
    print(f"chunk3: {fw.chunk3.size} bytes")
    print(f"chunk4: {len(fw.chunk4.decompressed)} decompressed "
          f"({fw.chunk4.compressed_size} compressed)")
    print(f"chunk5/tail: {len(fw.chunk5_tail.decompressed)} decompressed "
          f"({fw.chunk5_tail.compressed_size} compressed)")
    print(f"Accounted: {manifest['accounted_bytes']} / {fw.total_size} bytes "
          f"({manifest['unaccounted_bytes']} unaccounted)")
    if fw.warnings:
        print("WARNINGS:")
        for w in fw.warnings:
            print(f"  - {w}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
