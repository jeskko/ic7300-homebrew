#!/usr/bin/env python3
"""
Extract the IC-7300 firmware's UI icon/glyph resource set from body.bin.

Format, fully reverse-engineered 2026-08-29 (see notes/bitmaps.md), corrected
2026-08-29 after user-caught decode bugs (channel order + row stride) verified
directly against the real consumer code (icon_blit_by_id_v1, 0x200ae4d4):

  g_icon_table (RAM 0x20335234, 708 entries, 4 bytes each)
      entry[i] = pointer to icon_header

  icon_header (32-byte struct, immediately followed by pixel data):
      +0x00  u32 format       (3 = seen on most icons; 1 = seen on a handful,
                                e.g. the SHARP/SOFT/SCOPE icons -- both use the
                                same pixel layout below, meaning field is not
                                needed for decoding as far as tested; may be
                                significant for something the blitter does
                                that pure extraction doesn't care about)
      +0x04  u32 data_offset  (self-relative offset from this header to the
                                pixel data -- every sample seen so far is 0x20,
                                i.e. the header size itself)
      +0x08  ?
      +0x0c  u16 width
      +0x0e  u16 height
      +0x10..0x1f  ? (unused by the two blit functions found so far)
      +data_offset  pixel data, BGRA8888 (NOT RGBA -- confirmed by the TX
                    icon rendering red only after swapping R/B; ordinary RGBA
                    made it blue), row-major, top row first.

      **Row stride is NOT width*4.** icon_blit_by_id_v1/_v2 both compute it
      as `((width + 3) >> 2) << 4` -- i.e. width padded UP to a 4-pixel
      (16-byte) boundary before multiplying by 4 bytes/pixel. For any width
      already a multiple of 4 this is identical to width*4 (why roughly half
      the icons looked fine even before this was found), but for others
      (e.g. width=59, 90, 137) it silently inserts 4-12 bytes of row padding.
      Decoding with the naive width*4 stride produces a diagonal "shear"
      that gets worse with every row -- the exact symptom that led to this
      fix.

Consumer code (Ghidra names as of this session): icon_blit_by_id_v1 (was
FUN_200ae4d4, at 0x200ae4d4) and icon_blit_by_id_v2 (was FUN_200b0294, at
0x200b0294) both do `header = g_icon_table[icon_id]` (bounds-checked against
0x2c4 = 708, matching the table size exactly), then pass
`header + header->data_offset` as the blit source, the stride formula above,
and `header->width`/`header->height` as the blit dimensions to a small
GPU/2D-blitter API (FUN_200ffdf2/FUN_200ffe66/etc. -- not chased further,
looks like an internal display-controller command queue, not a generic
image codec).

Usage:
    python3 extract_icons.py <body.bin> [--base 0x20005000] [--table 0x20335234]
                              [--count 708] [--out-dir icons/] [--montage montage.png]

Requires Pillow.
"""
import argparse
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    Image = None


def rd32(data, base, addr):
    return struct.unpack_from("<I", data, addr - base)[0]


def rd16(data, base, addr):
    return struct.unpack_from("<H", data, addr - base)[0]


def row_stride(width):
    return ((width + 3) >> 2) << 4


def decode_entry(data, base, table_addr, idx):
    header = rd32(data, base, table_addr + idx * 4)
    data_offset = rd32(data, base, header + 4)
    width = rd16(data, base, header + 0xc)
    height = rd16(data, base, header + 0xe)
    pix_addr = header + data_offset
    return {"idx": idx, "header": header, "data_offset": data_offset,
            "width": width, "height": height, "pix_addr": pix_addr}


def extract_pixels_rgba(data, base, entry):
    """Return tightly-packed RGBA8888 bytes (width*height*4), de-padded and
    channel-swapped from the real BGRA8888/padded-stride source layout."""
    w, h = entry["width"], entry["height"]
    stride = row_stride(w)
    off = entry["pix_addr"] - base
    needed = off + stride * h
    if needed > len(data):
        return None
    out = bytearray(w * h * 4)
    for row in range(h):
        src = off + row * stride
        line = bytearray(data[src:src + w * 4])
        if len(line) < w * 4:
            return None
        for i in range(0, len(line), 4):
            line[i], line[i + 2] = line[i + 2], line[i]  # BGRA -> RGBA
        out[row * w * 4:(row + 1) * w * 4] = line
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binfile")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x20005000)
    ap.add_argument("--table", type=lambda s: int(s, 0), default=0x20335234)
    ap.add_argument("--count", type=int, default=708)
    ap.add_argument("--out-dir", default=None, help="write one PNG per icon here")
    ap.add_argument("--montage", default=None, help="write a single contact-sheet PNG here")
    ap.add_argument("--max-dim", type=int, default=512, help="skip entries with width or height above this (sanity guard against bad decodes)")
    args = ap.parse_args()

    with open(args.binfile, "rb") as f:
        data = f.read()

    entries = []
    for i in range(args.count):
        try:
            e = decode_entry(data, args.base, args.table, i)
        except struct.error:
            continue
        if 0 < e["width"] <= args.max_dim and 0 < e["height"] <= args.max_dim:
            entries.append(e)

    print(f"decoded {len(entries)}/{args.count} entries with sane dimensions", file=sys.stderr)

    if Image is None:
        print("Pillow not installed -- dimensions only, no image output", file=sys.stderr)
        for e in entries:
            print(e)
        return 0

    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)
        for e in entries:
            pix = extract_pixels_rgba(data, args.base, e)
            if pix is None:
                continue
            img = Image.frombytes("RGBA", (e["width"], e["height"]), pix)
            img.save(os.path.join(args.out_dir, f"icon_{e['idx']:04d}_{e['width']}x{e['height']}.png"))

    if args.montage:
        cols = 24
        cell = 64
        rows = (len(entries) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * cell, rows * cell), (32, 32, 32, 255))
        for n, e in enumerate(entries):
            pix = extract_pixels_rgba(data, args.base, e)
            if pix is None:
                continue
            img = Image.frombytes("RGBA", (e["width"], e["height"]), pix)
            img.thumbnail((cell - 4, cell - 4))
            x = (n % cols) * cell + 2
            y = (n // cols) * cell + 2
            sheet.paste(img, (x, y), img)
        sheet.save(args.montage)
        print("montage:", args.montage, sheet.size, file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
