"""Okumura-style LZSS decompressor used by the IC-7300 firmware container.

See /notes/decompression-lzss.md for the derivation of this algorithm from
the existing tunk.py/tunk3.py scripts (whose variable naming strongly
suggests they were themselves transliterated from Ghidra-decompiled ARM
code — i.e. this almost certainly matches the real ARM decompressor
bit-for-bit, not just a reimplementation from a spec).

Parameters, for reference:
- 4096-byte (0x1000) ring buffer, zero-initialized.
- Initial write cursor: 0xfee.
- Control-bit stream: one byte read at a time, LSB-first; bit == 1 means
  "literal follows", bit == 0 means "match follows".
- Match encoding: 2 bytes (b1, b2). length = (b2 & 0xf) + 3 (3..18).
  offset = (b1 | ((b2 & 0xf0) << 4)) & 0xfff — an absolute index into the
  ring buffer, not a relative back-distance.
"""

from __future__ import annotations

from dataclasses import dataclass

WINDOW_SIZE = 0x1000
WINDOW_MASK = WINDOW_SIZE - 1
INITIAL_CURSOR = 0xFEE
MIN_MATCH_LEN = 3


@dataclass
class DecompressResult:
    data: bytes
    #: number of input bytes actually consumed to produce `data` — i.e. the
    #: *compressed* size of this stream. Useful for locating where the next
    #: field starts without relying on a hardcoded offset.
    consumed: int


def decompress(data: bytes, out_len: int, start: int = 0) -> DecompressResult:
    """Decompress `out_len` bytes of LZSS-compressed data from `data[start:]`.

    Mirrors tunk.py/tunk3.py's unpack() precisely, but returns how many
    input bytes were consumed (which those scripts discarded), and raises
    instead of silently truncating if the input runs out early.
    """
    buffer = bytearray(WINDOW_SIZE)
    cursor = INITIAL_CURSOR
    ctrl = 0
    pos = start
    out = bytearray()

    def read_byte() -> int:
        nonlocal pos
        if pos >= len(data):
            raise ValueError(
                f"LZSS stream ran out of input at offset {pos:#x} "
                f"(produced {len(out)}/{out_len} bytes)"
            )
        b = data[pos]
        pos += 1
        return b

    def emit(b: int) -> None:
        nonlocal cursor
        out.append(b)
        buffer[cursor] = b
        cursor = (cursor + 1) & WINDOW_MASK

    while len(out) < out_len:
        ctrl >>= 1
        if not (ctrl & 0x100):
            ctrl = read_byte() | 0xFF00
        if ctrl & 1:
            emit(read_byte())
        else:
            b1 = read_byte()
            b2 = read_byte()
            match_len = (b2 & 0x0F) + MIN_MATCH_LEN
            offset = (b1 | ((b2 & 0xF0) << 4)) & WINDOW_MASK
            for i in range(match_len):
                if len(out) >= out_len:
                    break
                emit(buffer[(offset + i) & WINDOW_MASK])

    return DecompressResult(data=bytes(out), consumed=pos - start)
