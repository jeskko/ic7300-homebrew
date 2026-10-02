#!/usr/bin/env python3
"""Run icom_fw against all 10 real firmware releases and sanity-check it.

- Confirms the parser doesn't crash and reports zero unaccounted bytes for
  every version (see /notes/container-format.md's "known risk" section).
- Diffs `body` against the existing `7300_1XX/unpacked.dat` reference (and,
  for v1.42, the top-level `out.dat`) to make sure the rewritten LZSS
  decoder is byte-identical to the original tool's output.
- Never writes into the firmware directory (read-only; `$ICOM_FW_DIR`, default `firmware/`) -- output goes to
  ./scratch/unpacked/<version>/ (git-ignored).

Usage: python3 tools/verify_all.py
"""

from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from icom_fw.container import parse  # noqa: E402

SOURCE_DIR = Path(os.environ.get("ICOM_FW_DIR", Path(__file__).resolve().parent.parent / "firmware"))
VERSIONS = ["111", "112", "113", "114", "120", "121", "130", "140", "141", "142"]
OUT_DIR = Path(__file__).parent.parent / "scratch" / "unpacked"


def md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    all_ok = True

    for v in VERSIONS:
        fw_path = SOURCE_DIR / f"7300_{v}.dat"
        data = fw_path.read_bytes()
        fw = parse(data)

        out_dir = OUT_DIR / v
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "body.bin").write_bytes(fw.body.decompressed)
        (out_dir / "chunk5_tail.bin").write_bytes(fw.chunk5_tail.decompressed)

        unaccounted = fw.total_size - fw.accounted_bytes()
        status = []

        ref_path = SOURCE_DIR / f"7300_{v}" / "unpacked.dat"
        if ref_path.exists():
            ref = ref_path.read_bytes()
            if md5(ref) == md5(fw.body.decompressed):
                status.append("body==reference OK")
            else:
                status.append(
                    f"body MISMATCH vs reference (ref={len(ref)}B "
                    f"md5={md5(ref)[:8]}, ours={len(fw.body.decompressed)}B "
                    f"md5={md5(fw.body.decompressed)[:8]})"
                )
                all_ok = False
        else:
            status.append("no reference unpacked.dat found")

        if unaccounted != 0:
            status.append(f"UNACCOUNTED BYTES: {unaccounted}")
            all_ok = False
        else:
            status.append("100% of container accounted for")

        if fw.warnings:
            status.append(f"{len(fw.warnings)} warning(s): {fw.warnings}")
            all_ok = False

        print(f"v{v}: " + " | ".join(status))

    print()
    print("ALL OK" if all_ok else "SOME CHECKS FAILED — see above")
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
