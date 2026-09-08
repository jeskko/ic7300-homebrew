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


MAX_MATCH_LEN = 18  # (0xf & 4 bits) + MIN_MATCH_LEN
MATCH_LOOKAHEAD = 3  # hash key length -- must equal MIN_MATCH_LEN


def compress(data: bytes) -> bytes:
    """Compress `data` into a stream `decompress()` will reproduce exactly.

    This is a from-scratch encoder for the same Okumura-style LZSS format
    `decompress()` implements -- it does not attempt to reproduce Icom's
    own compressor's byte-for-byte output (their exact match-selection
    heuristic is unknown and irrelevant, and doesn't matter), only to
    produce a *valid* stream: any encoding this module's own decoder
    reproduces `data` from is usable in the real container, since the
    firmware's own decompressor implements this exact same algorithm.
    Correctness is what's required, not matching Icom's compression ratio.

    Design note: rather than simulating the decoder's ring buffer forward
    (which requires careful handling of read-your-own-future-writes timing
    for overlapping/self-referential matches), this works entirely off the
    plain `data` array, which is simpler and easy to reason about as
    correct: for any candidate match source position `p < i`, the decoder
    is guaranteed to have already reproduced `data[p]` at ring-buffer slot
    `(INITIAL_CURSOR + p) & WINDOW_MASK` by the time it reaches position
    `i` (true by induction from the very first byte -- the decoder writes
    every emitted byte, literal or matched, into that same formula's slot,
    so ring content always mirrors `data` for every already-decoded
    position). So a match is valid exactly when `data[p:p+length] ==
    data[i:i+length]` and `p` is still within the 4095-byte window (not
    yet overwritten by wraparound) -- including when `p+length > i`
    (an overlapping/RLE-style repeat), which is well-defined and correct
    against the plain array the same way it is against the ring buffer.

    Uses a simple hash-chain match finder (3-byte prefix -> prior
    positions, most recent first, capped per bucket for speed) and greedy
    longest-match selection (no lazy matching) -- adequate for a
    correctness-first tool; not tuned for best-possible ratio.
    """
    n = len(data)
    out = bytearray()

    MAX_CHAIN = 128
    hash_chains: dict[bytes, list[int]] = {}

    def find_match(i: int) -> tuple[int, int]:
        """Returns (length, source_pos); length==0 if no match >= MIN_MATCH_LEN."""
        if i + MATCH_LOOKAHEAD > n:
            return 0, -1
        best_len = 0
        best_pos = -1
        key = data[i : i + MATCH_LOOKAHEAD]
        max_len = min(MAX_MATCH_LEN, n - i)
        for cand in reversed(hash_chains.get(key, ())):
            if i - cand > WINDOW_MASK:  # would already have been overwritten
                continue
            length = MATCH_LOOKAHEAD  # key match already guarantees the first 3 bytes
            while length < max_len and data[cand + length] == data[i + length]:
                length += 1
            if length > best_len:
                best_len = length
                best_pos = cand
                if best_len >= MAX_MATCH_LEN:
                    break
        return best_len, best_pos

    def insert_positions(start: int, count: int) -> None:
        for j in range(start, start + count):
            if j + MATCH_LOOKAHEAD > n:
                break
            key = data[j : j + MATCH_LOOKAHEAD]
            chain = hash_chains.setdefault(key, [])
            chain.append(j)
            if len(chain) > MAX_CHAIN:
                del chain[0]

    ctrl_bits: list[int] = []
    tokens: list[bytes] = []

    def flush_group() -> None:
        if not ctrl_bits:
            return
        ctrl_byte = 0
        for bit_index, bit in enumerate(ctrl_bits):
            ctrl_byte |= bit << bit_index
        out.append(ctrl_byte)
        for t in tokens:
            out.extend(t)
        ctrl_bits.clear()
        tokens.clear()

    def emit_literal(b: int) -> None:
        ctrl_bits.append(1)
        tokens.append(bytes((b,)))
        if len(ctrl_bits) == 8:
            flush_group()

    def emit_match(ring_offset: int, length: int) -> None:
        b1 = ring_offset & 0xFF
        b2 = ((ring_offset >> 4) & 0xF0) | (length - MIN_MATCH_LEN)
        ctrl_bits.append(0)
        tokens.append(bytes((b1, b2)))
        if len(ctrl_bits) == 8:
            flush_group()

    i = 0
    while i < n:
        length, source_pos = find_match(i)
        if length >= MIN_MATCH_LEN:
            ring_offset = (INITIAL_CURSOR + source_pos) & WINDOW_MASK
            emit_match(ring_offset, length)
            insert_positions(i, length)
            i += length
        else:
            emit_literal(data[i])
            insert_positions(i, 1)
            i += 1

    flush_group()
    return bytes(out)


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
