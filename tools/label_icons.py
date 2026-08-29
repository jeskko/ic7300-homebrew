#!/usr/bin/env python3
"""
Build labeled contact sheets of the IC-7300 firmware's icon set (see
extract_icons.py / notes/bitmaps.md for the format) with the icon_id printed
under every cell, chunked into multiple sheets so each one stays readable.

Uses the same corrected decode as extract_icons.py: BGRA8888 pixels, row
stride padded to a 4-pixel (16-byte) boundary (`((width+3)>>2)<<4`), not
naive width*4 -- see extract_icons.py's docstring for how that was found.

Usage:
    python3 label_icons.py <body.bin> --out-dir /tmp/icon_sheets --per-sheet 100
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = [
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
]


def load_font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


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
    return {"idx": idx, "header": header, "width": width, "height": height,
            "pix_addr": header + data_offset}


def extract_pixels_rgba(data, base, e):
    w, h = e["width"], e["height"]
    stride = row_stride(w)
    off = e["pix_addr"] - base
    if off + stride * h > len(data):
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
    ap = argparse.ArgumentParser()
    ap.add_argument("binfile")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x20005000)
    ap.add_argument("--table", type=lambda s: int(s, 0), default=0x20335234)
    ap.add_argument("--count", type=int, default=708)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--per-sheet", type=int, default=100)
    ap.add_argument("--cols", type=int, default=10)
    ap.add_argument("--cell", type=int, default=96)
    ap.add_argument("--ids", default=None, help="comma-separated explicit id list instead of 0..count")
    args = ap.parse_args()

    with open(args.binfile, "rb") as f:
        data = f.read()

    os.makedirs(args.out_dir, exist_ok=True)
    font = load_font(12)

    id_list = [int(x) for x in args.ids.split(",")] if args.ids else list(range(args.count))

    entries = []
    for i in id_list:
        e = decode_entry(data, args.base, args.table, i)
        if 0 < e["width"] <= 300 and 0 < e["height"] <= 300:
            entries.append(e)

    cell = args.cell
    cols = args.cols
    rows_per_sheet = (args.per_sheet + cols - 1) // cols

    for sheet_start in range(0, len(entries), args.per_sheet):
        chunk = entries[sheet_start:sheet_start + args.per_sheet]
        sheet = Image.new("RGB", (cols * cell, rows_per_sheet * cell), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for n, e in enumerate(chunk):
            x = (n % cols) * cell
            y = (n // cols) * cell
            pix = extract_pixels_rgba(data, args.base, e)
            if pix is None:
                continue
            img = Image.frombytes("RGBA", (e["width"], e["height"]), pix)
            bg = Image.new("RGBA", img.size, (60, 60, 60, 255))
            img = Image.alpha_composite(bg, img).convert("RGB")
            img.thumbnail((cell - 4, cell - 16))
            px = x + (cell - img.width) // 2
            py = y + 2
            sheet.paste(img, (px, py))
            draw.rectangle([x, y, x + cell - 1, y + cell - 1], outline=(90, 90, 90))
            draw.text((x + 3, y + cell - 13), f"{e['idx']}", fill=(255, 255, 0), font=font)
        path = os.path.join(args.out_dir, f"sheet_{sheet_start:04d}.png")
        sheet.save(path)
        print(path)


if __name__ == "__main__":
    sys.exit(main())
