"""Parser for the IC-7300 firmware update container format (`7300_1XX.dat`).

See /notes/container-format.md for the full derivation. Summary of the
layout this implements:

    0x0000  16 B    version string (Shift-JIS)
    0x0010  7x4 B   size1..size7 (LE u32) -- meaning only partly understood,
                    see /notes/firmware-versions.md
    0x1002c  4 B    `length`: decompressed size of the main body
    0x10030  ...    main body, LZSS-compressed, `length` bytes decompressed
    0x21002c 4 B+N   chunk1 (font1, .ttf): LE u32 size prefix + raw bytes
                     -- FIXED SLOT, offset is constant regardless of size
    0x24002c 4 B+N   chunk2 (font2, .ttf): same shape -- FIXED SLOT
    0x25002c 4 B+N   chunk3 (unidentified): same shape -- FIXED SLOT
    (immediately after chunk3, no gap)
             0x10000 chunk4 (unidentified): LZSS-compressed, fixed 65536
                     decompressed bytes, tightly packed (no offset seek)
    (immediately after chunk4, no gap, no length prefix)
             to EOF  chunk5/tail: LZSS-compressed, decode runs until input
                     is exhausted. NOT read by the original tunk3.py at
                     all -- see /notes/multi-cpu-images.md.

Every offset below is a *hardcoded* constant, matching the original
firmware's own build tool as far as we can tell (see
/notes/firmware-versions.md for the evidence these are fixed, oversized
slots rather than tightly-packed, size-table-driven regions) -- but this
module validates that assumption at parse time rather than trusting it
blindly, and surfaces every byte of the container as a named component so
nothing is silently dropped the way the original script drops chunk5.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .lzss import decompress

VERSION_STRING_OFFSET = 0x0
VERSION_STRING_LEN = 16

SIZE_TABLE_OFFSET = 0x10
SIZE_TABLE_COUNT = 7

BODY_LENGTH_OFFSET = 0x1002C
BODY_OFFSET = 0x10030

CHUNK1_OFFSET = 0x21002C  # font1 (.ttf)
CHUNK2_OFFSET = 0x24002C  # font2 (.ttf)
CHUNK3_OFFSET = 0x25002C  # unidentified, raw

CHUNK4_DECOMPRESSED_LEN = 0x10000  # unidentified, tightly packed after chunk3

# `base.dat`/`data.dat` (the per-version derived files already on disk) are
# just this container split at BODY_OFFSET -- see /notes/container-format.md.
BASE_SPLIT_OFFSET = BODY_OFFSET


@dataclass
class SizedChunk:
    """A `size-prefix + raw bytes` chunk read from a fixed offset."""

    offset: int
    size: int
    data: bytes


@dataclass
class CompressedChunk:
    """An LZSS-compressed chunk."""

    offset: int
    decompressed: bytes
    compressed_size: int  # input bytes actually consumed


@dataclass
class Firmware:
    version_string: str
    size_table: tuple[int, ...]
    body_length: int

    body: CompressedChunk
    chunk1_font: SizedChunk
    chunk2_font: SizedChunk
    chunk3: SizedChunk
    chunk4: CompressedChunk
    chunk5_tail: CompressedChunk  # see /notes/multi-cpu-images.md

    total_size: int
    warnings: list[str] = field(default_factory=list)

    def accounted_bytes(self) -> int:
        """Total container bytes covered by a known component (compressed
        sizes for LZSS chunks, since that's what actually occupies the
        file; sizes/prefixes counted where relevant)."""
        return (
            BODY_OFFSET
            + self.body.compressed_size
            + (CHUNK1_OFFSET - (BODY_OFFSET + self.body.compressed_size))  # slot slack
            + 4
            + self.chunk1_font.size
            + (CHUNK2_OFFSET - (CHUNK1_OFFSET + 4 + self.chunk1_font.size))
            + 4
            + self.chunk2_font.size
            + (CHUNK3_OFFSET - (CHUNK2_OFFSET + 4 + self.chunk2_font.size))
            + 4
            + self.chunk3.size
            + self.chunk4.compressed_size
            + self.chunk5_tail.compressed_size
        )


def _read_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _read_sized_chunk(data: bytes, offset: int) -> SizedChunk:
    size = _read_u32(data, offset)
    start = offset + 4
    return SizedChunk(offset=offset, size=size, data=data[start : start + size])


def parse(data: bytes) -> Firmware:
    warnings: list[str] = []

    version_string = data[
        VERSION_STRING_OFFSET : VERSION_STRING_OFFSET + VERSION_STRING_LEN
    ].decode("shift-jis", errors="replace")

    size_table = struct.unpack_from(
        f"<{SIZE_TABLE_COUNT}I", data, SIZE_TABLE_OFFSET
    )

    body_length = _read_u32(data, BODY_LENGTH_OFFSET)
    body_result = decompress(data, body_length, start=BODY_OFFSET)
    body_end = BODY_OFFSET + body_result.consumed
    if body_end > CHUNK1_OFFSET:
        warnings.append(
            f"main body overran its fixed slot: ends at {body_end:#x}, "
            f"chunk1 slot starts at {CHUNK1_OFFSET:#x}"
        )

    chunk1 = _read_sized_chunk(data, CHUNK1_OFFSET)
    chunk1_end = CHUNK1_OFFSET + 4 + chunk1.size
    if chunk1_end > CHUNK2_OFFSET:
        warnings.append(
            f"chunk1 overran its fixed slot: ends at {chunk1_end:#x}, "
            f"chunk2 slot starts at {CHUNK2_OFFSET:#x}"
        )

    chunk2 = _read_sized_chunk(data, CHUNK2_OFFSET)
    chunk2_end = CHUNK2_OFFSET + 4 + chunk2.size
    if chunk2_end > CHUNK3_OFFSET:
        warnings.append(
            f"chunk2 overran its fixed slot: ends at {chunk2_end:#x}, "
            f"chunk3 slot starts at {CHUNK3_OFFSET:#x}"
        )

    chunk3 = _read_sized_chunk(data, CHUNK3_OFFSET)
    chunk4_start = CHUNK3_OFFSET + 4 + chunk3.size
    chunk4_result = decompress(data, CHUNK4_DECOMPRESSED_LEN, start=chunk4_start)
    chunk4 = CompressedChunk(
        offset=chunk4_start,
        decompressed=chunk4_result.data,
        compressed_size=chunk4_result.consumed,
    )

    chunk5_start = chunk4_start + chunk4_result.consumed
    chunk5_result = _decompress_to_eof(data, chunk5_start)
    chunk5 = CompressedChunk(
        offset=chunk5_start,
        decompressed=chunk5_result.data,
        compressed_size=chunk5_result.consumed,
    )
    chunk5_end = chunk5_start + chunk5_result.consumed
    if chunk5_end != len(data):
        warnings.append(
            f"chunk5/tail did not consume the whole file: ended at "
            f"{chunk5_end:#x}, file is {len(data):#x} bytes "
            f"({len(data) - chunk5_end} bytes left over)"
        )

    return Firmware(
        version_string=version_string,
        size_table=size_table,
        body_length=body_length,
        body=CompressedChunk(
            offset=BODY_OFFSET,
            decompressed=body_result.data,
            compressed_size=body_result.consumed,
        ),
        chunk1_font=chunk1,
        chunk2_font=chunk2,
        chunk3=chunk3,
        chunk4=chunk4,
        chunk5_tail=chunk5,
        total_size=len(data),
        warnings=warnings,
    )


def _decompress_to_eof(data: bytes, start: int):
    """Like lzss.decompress, but instead of a fixed output length, keeps
    decoding until the input is exhausted -- this is chunk5/tail's
    termination mode, see /notes/multi-cpu-images.md. Returns the same
    shape as lzss.DecompressResult.
    """
    from .lzss import WINDOW_SIZE, WINDOW_MASK, INITIAL_CURSOR, MIN_MATCH_LEN
    from .lzss import DecompressResult

    buffer = bytearray(WINDOW_SIZE)
    cursor = INITIAL_CURSOR
    ctrl = 0
    pos = start
    out = bytearray()
    n = len(data)

    def emit(b: int) -> None:
        nonlocal cursor
        out.append(b)
        buffer[cursor] = b
        cursor = (cursor + 1) & WINDOW_MASK

    while True:
        ctrl >>= 1
        if not (ctrl & 0x100):
            if pos >= n:
                break
            ctrl = data[pos] | 0xFF00
            pos += 1
        if ctrl & 1:
            if pos >= n:
                break
            emit(data[pos])
            pos += 1
        else:
            if pos + 1 >= n:
                break
            b1, b2 = data[pos], data[pos + 1]
            pos += 2
            match_len = (b2 & 0x0F) + MIN_MATCH_LEN
            offset = (b1 | ((b2 & 0xF0) << 4)) & WINDOW_MASK
            for i in range(match_len):
                emit(buffer[(offset + i) & WINDOW_MASK])

    return DecompressResult(data=bytes(out), consumed=pos - start)
