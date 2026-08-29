# Firmware body compression: LZSS, not DEFLATE

The container's main body is compressed with a classic **Okumura-style
LZSS**, *not* zlib/DEFLATE. Confirmed by fully tracing the decoder already
written (as Python) in the user's existing `tunk.py`/`tunk3.py` scripts.
(This note originally said "and one auxiliary chunk" — superseded now that
[[container-format]]/[[multi-cpu-images]] have fully mapped that region:
it's actually **three** separately-bounded, MD5-verified LZSS components
— `component0`/`component1`/`component2`, the DSP program/data region —
each using this same algorithm, not just one.)

**Notable: the Python `unpack()` in those scripts reads like a direct
transliteration of Ghidra-decompiled C**, not code written from scratch —
variable names (`piVar7`, `piVar8`, `uVar10`, `bVar1`, `bVar2`, `bVar3`,
`bVar12`, `iVar6`, `iVar9`) match Ghidra's default decompiler auto-naming
convention exactly. That strongly suggested the ARM decompressor function
had already been located and decompiled once in one of the existing
Ghidra projects (most likely `icom_loader.rep` or `icom.rep`, since the
decompressor would live in `base.dat`'s loader code) — **confirmed
correct**: [[base-loader]] found it directly, `unpack_from_flash_to_mem()`
in `base.dat`'s boot chain, byte-for-byte the same ring-buffer/`0xfee`-cursor
algorithm documented below.

## Algorithm

- **Window**: 4096-byte ring buffer (`& 0xfff` masking throughout),
  zero-initialized (not the classic Okumura demo's space-fill).
- **Initial write cursor**: `buf_ptr = 0xfee` (3822) — standard Okumura
  LZSS constant (leaves `0xfee` bytes of buffer "before" the write point
  available as valid back-reference distance from the very start).
- **Control-bit stream**: a 16-bit-ish accumulator (`uVar10`) is refilled
  with `data[pos] | 0xff00` (i.e. next control byte in the low 8 bits, all
  1s in the high 8 bits acting as a shift-countdown sentinel) whenever its
  `0x100` bit has been shifted away; each iteration does `uVar10 >>= 1` and
  tests the LSB:
  - **bit == 1 → literal byte**: copy the next raw byte from the input
    straight to output and into the ring buffer.
  - **bit == 0 → back-reference (match)**: read 2 bytes `b1, b2`.
    - `length = (b2 & 0x0f) + 3` (range 3–18)
    - `offset = b1 | ((b2 & 0xf0) << 4)`, then `& 0xfff` (12-bit distance
      into the ring buffer, i.e. an absolute buffer index, not a
      relative-to-cursor distance)
    - Copy `length` bytes one at a time from `buffer[offset + i]` to
      output, writing each copied byte back into the ring buffer at the
      (auto-incrementing) write cursor as it goes — this is what makes
      overlapping/self-referential runs (RLE-like repeats) work correctly.
- **Termination**: driven by an explicit output-byte counter (`iVar6`,
  set from the container's `length` field for the main body, or a fixed
  `0x10000` for the trailing chunk) — decoding stops once that many bytes
  have been written, not by an in-stream end marker. (There's also a
  fallback `IndexError`/`except` path if the input is exhausted first,
  which the original scripts treat as "ran out of data" rather than a hard
  error — worth keeping as a sanity check but not relying on.)

## Reference pseudocode (cleaned up)

```
buffer = bytearray(0x1000)          # 4096-byte ring buffer, zero-filled
write_cursor = 0xfee
ctrl = 0
pos = 0
remaining = length                  # exact output byte count, from header

while remaining > 0:
    ctrl >>= 1
    if ctrl & 0x100 == 0:
        ctrl = data[pos] | 0xff00
        pos += 1
    if ctrl & 1:                    # literal
        b = data[pos]; pos += 1
        emit(b)
    else:                           # match
        b1, b2 = data[pos], data[pos + 1]; pos += 2
        length_ = (b2 & 0x0f) + 3
        offset = (b1 | ((b2 & 0xf0) << 4)) & 0xfff
        for i in range(length_):
            if remaining == 0: break
            b = buffer[(offset + i) & 0xfff]
            emit(b)
    # emit(b) also does: buffer[write_cursor] = b; write_cursor = (write_cursor+1) & 0xfff; remaining -= 1
```

This is the basis for the rewritten unpacker in `tools/` (see
[[container-format]] for how it's invoked against the container).
