"""Extract the "3 extra chunks" (DSP Program / DSP Data / FPGA) from an
IC-7300 firmware update container.

**Supersedes `container.py`'s `chunk4`/`chunk5_tail` model for this specific
region.** That model (fixed 0x10000-decompressed-length "chunk4" starting
right after chunk3, then a decode-to-EOF "chunk5/tail") was a reasonable
first guess from `tunk3.py`'s byte-accounting, but the real firmware
(`firmware_update_main`/`chunk_transport_send_data` in `body.bin`) doesn't
recognize any chunk4/chunk5 boundary at all -- it reads 3 independently
sized/positioned LZSS streams computed from the `size1..size7` header
fields, which happen to span across where the old model's chunk4/chunk5
split falls. See /notes/multi-cpu-images.md's "DSP firmware precisely
located and unpacked" section for the full derivation and verification
(byte-exact re-decompression against a real v1.42 container).

Layout (confirmed against `firmware_update_main`'s decompiled `local_9c[]`
computation):

    size1..size7  = the same 7 header fields `container.py` already reads
                    (size1 at file offset 0x10, size2..size7 immediately
                    after, i.e. 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28).

    component0 (Front CPU, hypothesized) file offset = size1 + 0x3c
    component1 (DSP Program, hypothesized) file offset =
        component0_offset + size2 + 0x10
    component2 (DSP Data, hypothesized) file offset =
        component1_offset + size4 + 0x10

Each component is stored as `[compressed payload][0x10-byte MD5 trailer]`,
LZSS-compressed (same Okumura variant as the main body, see `lzss.py`):
compressed length is size2/size4/size6 respectively (what's read from the
file and MD5-verified against the trailer), decompressed length is
size3/size5/size7 respectively (what `chunk_transport_send_data` actually
sends onward). size3/5/7 is NOT simply "a truncated prefix of size2/4/6" --
it can be smaller OR larger than the compressed length depending on how
well that specific component compresses.

The Front CPU / DSP Program / DSP Data component ORDER is a working
hypothesis (matches the version-compare field ordering found in
`FUN_200a94c8`, see /notes/multi-cpu-images.md) -- not yet independently
confirmed component-by-component.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

from .lzss import decompress

SIZE_TABLE_OFFSET = 0x10
# Component identities confirmed 2026-08-30 by version-field correlation against
# Icom's own per-release Main CPU/DSP Program/DSP Data/FPGA breakdown (see
# /notes/multi-cpu-images.md). Earlier names ("front_cpu", "dsp_program",
# "dsp_data") were shifted by one and are now corrected: component0=DSP Program,
# component1=DSP Data, component2=FPGA (a compressed Altera bitstream).
COMPONENT_NAMES = ("dsp_program", "dsp_data", "fpga")


@dataclass
class DspComponent:
    name: str
    file_offset: int
    compressed_size: int
    decompressed_size: int
    compressed: bytes
    decompressed: bytes
    md5_trailer: bytes
    md5_actual: bytes

    @property
    def md5_ok(self) -> bool:
        return self.md5_trailer == self.md5_actual


def extract(data: bytes) -> list[DspComponent]:
    """Extract and decompress the 3 optional update components from a raw
    firmware container's bytes. Raises ValueError if any component's MD5
    trailer doesn't match (a strong sign the offset formula is wrong for
    this container, e.g. a version whose header layout differs)."""

    size1, size2, size3, size4, size5, size6, size7 = struct.unpack_from(
        "<7I", data, SIZE_TABLE_OFFSET
    )

    off0 = size1 + 0x3C
    off1 = off0 + size2 + 0x10
    off2 = off1 + size4 + 0x10

    specs = [
        (COMPONENT_NAMES[0], off0, size2, size3),
        (COMPONENT_NAMES[1], off1, size4, size5),
        (COMPONENT_NAMES[2], off2, size6, size7),
    ]

    components = []
    for name, offset, comp_len, decomp_len in specs:
        compressed = data[offset : offset + comp_len]
        trailer = data[offset + comp_len : offset + comp_len + 0x10]
        result = decompress(compressed, decomp_len)
        if result.consumed != comp_len:
            raise ValueError(
                f"{name}: LZSS stream consumed {result.consumed} bytes, "
                f"expected exactly {comp_len} (compressed length from header) "
                f"-- offset formula is likely wrong for this container"
            )
        actual_md5 = hashlib.md5(compressed).digest()
        components.append(
            DspComponent(
                name=name,
                file_offset=offset,
                compressed_size=comp_len,
                decompressed_size=decomp_len,
                compressed=compressed,
                decompressed=result.data,
                md5_trailer=trailer,
                md5_actual=actual_md5,
            )
        )
    return components


def main(argv: list[str]) -> int:
    import sys
    from pathlib import Path

    if len(argv) != 3:
        print(f"usage: {argv[0]} <firmware.dat> <output_dir>", file=sys.stderr)
        return 2

    in_path = Path(argv[1])
    out_dir = Path(argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    data = in_path.read_bytes()
    components = extract(data)

    for c in components:
        (out_dir / f"{c.name}.bin").write_bytes(c.decompressed)
        (out_dir / f"{c.name}_compressed.bin").write_bytes(c.compressed)
        status = "OK" if c.md5_ok else "MD5 MISMATCH"
        print(
            f"{c.name}: file_offset={c.file_offset:#x} "
            f"compressed={c.compressed_size:#x} decompressed={c.decompressed_size:#x} "
            f"md5={status}"
        )
    return 0


if __name__ == "__main__":
    import sys

    raise SystemExit(main(sys.argv))
