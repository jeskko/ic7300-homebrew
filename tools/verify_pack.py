#!/usr/bin/env python3
"""Verify `icom_fw`'s LZSS compressor + container packer against all 10 real
firmware releases.

For each release: unpack the real body, patch a few bytes (start, middle,
end, and a length-changing append) of the decompressed body, repack a new
container, then check:

- the packer accepts it (fits in the fixed body slot -- our from-scratch
  encoder tends to compress *better* than Icom's own, per
  /notes/decompression-lzss.md, so this should never trip for a same-size
  or smaller edit);
- re-decompressing the packed container's body reproduces the patched
  bytes exactly;
- every other component (fonts, chunk3, and everything from `CHUNK1_OFFSET`
  onward except the 16-byte checksum field itself) is byte-for-byte
  identical to the original container;
- the checksum field is internally self-consistent (MD5 of the checksummed
  region matches what's stored) and actually changed vs. the original
  (proving it's freshly computed over the new content, not just copied).

This does not (and cannot, without real hardware) verify that an actual
IC-7300 accepts the repacked container via its SD-card update flow --
see /sdk/roadmap.md's Phase 0 for that live test.

Usage: python3 tools/verify_pack.py
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from icom_fw.container import (  # noqa: E402
    CHECKSUM_REGION_START,
    CHUNK1_OFFSET,
    pack,
    parse,
)

SOURCE_DIR = Path("/data/misc/icom/7300")
VERSIONS = ["111", "112", "113", "114", "120", "121", "130", "140", "141", "142"]


def patch_body(body: bytes) -> bytes:
    patched = bytearray(body)
    patched[0] ^= 0xFF  # very first byte
    patched[len(patched) // 2] ^= 0xFF  # middle
    patched[-1] ^= 0xFF  # last byte
    patched.append(0xAB)  # also change the decompressed length
    return bytes(patched)


def main() -> int:
    all_ok = True

    for v in VERSIONS:
        fw_path = SOURCE_DIR / f"7300_{v}.dat"
        original = fw_path.read_bytes()
        fw = parse(original)
        new_body = patch_body(fw.body.decompressed)

        status = []
        try:
            packed = pack(original, new_body)
        except ValueError as e:
            print(f"v{v}: PACK FAILED: {e}")
            all_ok = False
            continue

        if len(packed) != len(original):
            status.append(f"UNEXPECTED SIZE CHANGE ({len(original)} -> {len(packed)})")
            all_ok = False
        else:
            status.append("same total size OK")

        fw2 = parse(packed)
        if fw2.warnings:
            status.append(f"re-parse warnings: {fw2.warnings}")
            all_ok = False

        if fw2.body.decompressed == new_body:
            status.append("body round-trip OK")
        else:
            status.append("BODY MISMATCH")
            all_ok = False

        if (
            fw2.chunk1_font.data == fw.chunk1_font.data
            and fw2.chunk2_font.data == fw.chunk2_font.data
            and fw2.chunk3.data == fw.chunk3.data
        ):
            status.append("fonts/chunk3 unchanged OK")
        else:
            status.append("FONTS/CHUNK3 CHANGED UNEXPECTEDLY")
            all_ok = False

        size1 = fw.size_table[0]
        digest_offset = CHECKSUM_REGION_START + size1
        rel = digest_offset - CHUNK1_OFFSET
        tail_orig = bytearray(original[CHUNK1_OFFSET:])
        tail_packed = bytearray(packed[CHUNK1_OFFSET:])
        tail_orig[rel : rel + 16] = b"\x00" * 16
        tail_packed[rel : rel + 16] = b"\x00" * 16
        if tail_orig == tail_packed:
            status.append("tail (excl. checksum) unchanged OK")
        else:
            status.append("TAIL CHANGED UNEXPECTEDLY (outside checksum field)")
            all_ok = False

        region = packed[CHECKSUM_REGION_START:digest_offset]
        expected_digest = hashlib.md5(region).digest()
        actual_digest = packed[digest_offset : digest_offset + 16]
        orig_digest = original[digest_offset : digest_offset + 16]
        if expected_digest == actual_digest and actual_digest != orig_digest:
            status.append("checksum self-consistent + updated OK")
        else:
            status.append("CHECKSUM WRONG OR UNCHANGED")
            all_ok = False

        print(f"v{v}: " + " | ".join(status))

    print()
    print("ALL OK" if all_ok else "SOME CHECKS FAILED — see above")
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
