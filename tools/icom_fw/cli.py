#!/usr/bin/env python3
"""Unpack/repack an IC-7300 firmware update container.

Usage:
    python3 -m icom_fw.cli unpack <firmware.dat> <output_dir>
    python3 -m icom_fw.cli pack <original.dat> <new_body.bin> <output.dat>

`unpack` writes: body.bin, chunk1_font1.ttf, chunk2_font2.ttf, chunk3.bin,
chunk4.bin, chunk5_tail.bin, and a manifest.json describing offsets/sizes/
warnings. See /notes/container-format.md for what each component is (and
its docstring's note on chunk4/chunk5_tail being superseded by
`dsp_chunks.py`'s model for that region).

`pack` takes an original container plus a (possibly hand-edited)
*decompressed* main-body file, and produces a new container with that body
recompressed, re-inserted into its fixed slot, and the update mechanism's
own checksum recomputed -- everything else copied byte-for-byte from the
original. This is what `sdk/roadmap.md`'s Phase 0 test needs: unpack a
release, edit body.bin, `pack` it back into a container, and test whether
a real IC-7300 accepts it via the normal SD-card update flow.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from .container import pack, parse


def _unpack(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {sys.argv[0]} unpack <firmware.dat> <output_dir>", file=sys.stderr)
        return 2

    in_path = Path(argv[0])
    out_dir = Path(argv[1])
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
          f"({fw.chunk4.compressed_size} compressed) -- NOTE: superseded/likely-garbage, "
          f"see container.py's module docstring")
    print(f"chunk5/tail: {len(fw.chunk5_tail.decompressed)} decompressed "
          f"({fw.chunk5_tail.compressed_size} compressed)")
    print(f"Accounted: {manifest['accounted_bytes']} / {fw.total_size} bytes "
          f"({manifest['unaccounted_bytes']} unaccounted)")
    if fw.warnings:
        print("WARNINGS:")
        for w in fw.warnings:
            print(f"  - {w}")
    return 0


def _pack(argv: list[str]) -> int:
    if len(argv) != 3:
        print(
            f"usage: {sys.argv[0]} pack <original.dat> <new_body.bin> <output.dat>",
            file=sys.stderr,
        )
        return 2

    original_path = Path(argv[0])
    new_body_path = Path(argv[1])
    output_path = Path(argv[2])

    original_data = original_path.read_bytes()
    new_body = new_body_path.read_bytes()

    try:
        packed = pack(original_data, new_body)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    output_path.write_bytes(packed)

    # Report what changed, for a quick sanity glance -- re-parse both to
    # confirm the new container is self-consistent before the caller
    # trusts it (e.g. before flashing it at a real radio).
    fw_before = parse(original_data)
    fw_after = parse(packed)
    print(f"Wrote {output_path} ({len(packed)} bytes, same size as input: "
          f"{len(packed) == len(original_data)})")
    print(f"Body: {len(fw_before.body.decompressed)} -> {len(fw_after.body.decompressed)} "
          f"decompressed bytes, {fw_before.body.compressed_size} -> "
          f"{fw_after.body.compressed_size} compressed bytes")
    print(f"Body actually matches requested new_body.bin: "
          f"{fw_after.body.decompressed == new_body}")
    if fw_after.warnings:
        print("WARNINGS (re-parsing the packed output):")
        for w in fw_after.warnings:
            print(f"  - {w}")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    subcommand = argv[1]
    if subcommand == "unpack":
        return _unpack(argv[2:])
    if subcommand == "pack":
        return _pack(argv[2:])

    # Backward compatibility: the original CLI took exactly
    # `<firmware.dat> <output_dir>` with no subcommand at all.
    if len(argv) == 3 and Path(argv[1]).is_file():
        return _unpack(argv[1:])

    print(f"unknown subcommand {subcommand!r}", file=sys.stderr)
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
