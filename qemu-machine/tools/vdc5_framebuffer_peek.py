#!/usr/bin/env python3
"""Polls VDC50's real graphics-plane registers (GRn_FLM2/FLM3/FLM6) over a live boot, and as
soon as any plane's FLM2 (framebuffer base address) becomes nonzero, dumps that region of guest
RAM and decodes it into a real PNG using the pixel format/stride/width VDC50's own registers
describe -- an actual peek at what the radio would be showing on its LCD, without modeling VDC5's
compositing/scaling/timing pipeline at all (see rz_a1h.c's own comment on why VDC50 is a plain,
register-storage-only RAM region: this project doesn't emulate the display pipeline, it just makes
sure firmware's real register writes are no longer silently discarded, so they can be read back).

Register layout confirmed against `~/Downloads/rza1.svd` (offsets) and the vendored Renesas VDC5
driver in `scratch/r01an5093ej0170-rza1-swpkg/.../r_vdc_l_register.c` (bit-field packing):
  GRn_FLM2 = framebuffer base address (guest RAM), low 3 bits reserved/masked.
  GRn_FLM3 bits [30:16] = line offset (stride) -- units per the driver: same as gr_ln_off,
      passed straight from the app's own stride-in-bytes calculation in every caller checked,
      so treated here as bytes-per-line directly (not re-derived from pixel width * bpp,
      since real hardware allows padding).
  GRn_FLM6 bits [31:28] = format (matches VDC_GR_FORMAT_* enum: 0=RGB565, 1=RGB888,
      2=ARGB1555, 3=ARGB4444, 4=ARGB8888, 5=CLUT8, 6=CLUT4, 7=CLUT1, 8=YCbCr422,
      9=YCbCr444, 10=RGBA5551, 11=RGBA8888). Only the non-CLUT/non-YCbCr RGB formats are
      decoded here -- CLUT/YCbCr are reported but not rendered (would need the CLUT table or
      a YCbCr->RGB conversion this tool doesn't implement yet).
  GRn_FLM6 bits [26:16] = buffer width in pixels, minus 1.
  Height isn't in these registers -- estimated from the dumped region's own size budget (see
  MAX_DUMP_BYTES) since VDC5's actual display-height register wasn't identified this session;
  if the decoded image looks truncated/wrong, that's the likely reason, not a tool bug.

Usage: vdc5_framebuffer_peek.py [--auto-boot] [--hold-pwrk] [timeout_s]
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"

VDC50_BASE = 0xFCFF7400
GPIO_PATH = "/machine/gpio"

# (name, FLM_block_offset) -- FLM1 is block+8, FLM2 is block+0xc, FLM3 is block+0x10,
# FLM6 is block+0x1c, for every plane except GR_VIN/GR_OIR which use a slightly different
# sub-layout (checked against the SVD offsets directly, not assumed uniform).
PLANES = [
    ("GR0", 0x200),
    ("GR1", 0x900),
    ("GR2", 0x300),
    ("GR3", 0x380),
    ("GR_VIN", 0xA00),
    ("GR_OIR", 0xB80),
]

FORMAT_NAMES = {
    0: "RGB565", 1: "RGB888", 2: "ARGB1555", 3: "ARGB4444", 4: "ARGB8888",
    5: "CLUT8", 6: "CLUT4", 7: "CLUT1", 8: "YCbCr422", 9: "YCbCr444",
    10: "RGBA5551", 11: "RGBA8888",
}
BYTES_PER_PIXEL = {0: 2, 1: 3, 2: 2, 3: 2, 4: 4, 10: 2, 11: 4}

MAX_DUMP_BYTES = 4 * 1024 * 1024  # generous cap; real panels here are well under this


def qmp_open(sock_path: str) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.recv(65536)
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    return s


def hmp(s: socket.socket, cmd: str) -> str:
    s.send(json.dumps({"execute": "human-monitor-command",
                        "arguments": {"command-line": cmd}}).encode())
    return json.loads(s.recv(1 << 20).decode())["return"]


def qmp_cmd(s: socket.socket, execute: str, **arguments):
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
    return json.loads(s.recv(65536).decode())


def read_u32(s: socket.socket, addr: int) -> int:
    reply = hmp(s, f"xp /1xw 0x{addr:x}")
    return int(reply.strip().split()[1], 16)


def pc(s: socket.socket) -> int:
    text = hmp(s, "info registers")
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("R15 not found")


def dump_ram(sock_path_dir: Path, s: socket.socket, addr: int, length: int) -> bytes:
    out = sock_path_dir / "vdc5_dump.bin"
    out.unlink(missing_ok=True)
    hmp(s, f'pmemsave 0x{addr:x} 0x{length:x} "{out}"')
    return out.read_bytes()


def decode_and_save(name: str, fmt: int, width: int, stride: int, raw: bytes, out_path: Path):
    bpp = BYTES_PER_PIXEL.get(fmt)
    if bpp is None:
        print(f"  format {FORMAT_NAMES.get(fmt, fmt)} not decodable by this tool yet"
              f" (CLUT needs the palette, YCbCr needs a color-space conversion) --"
              f" raw dump saved instead")
        raw_path = out_path.with_suffix(".raw")
        raw_path.write_bytes(raw)
        print(f"  raw bytes: {raw_path}")
        return
    height = max(1, len(raw) // stride) if stride else 0
    try:
        from PIL import Image
    except ImportError:
        print("  Pillow not installed (pip install pillow) -- can't decode to PNG,"
              " raw bytes saved instead")
        raw_path = out_path.with_suffix(".raw")
        raw_path.write_bytes(raw)
        print(f"  raw bytes: {raw_path}  (format={FORMAT_NAMES.get(fmt)},"
              f" width={width}, stride={stride}, height~={height})")
        return

    mode_map = {0: "RGB565", 1: "RGB", 2: "ARGB1555", 3: "ARGB4444", 4: "RGBA",
                10: "RGBA5551", 11: "RGBA"}
    img = None
    if fmt == 0:  # RGB565
        pixels = []
        for row in range(height):
            line = raw[row * stride: row * stride + width * 2]
            for i in range(0, len(line) - 1, 2):
                v = struct.unpack_from("<H", line, i)[0]
                r = (v >> 11) & 0x1F
                g = (v >> 5) & 0x3F
                b = v & 0x1F
                pixels.append(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
        img = Image.new("RGB", (width, height))
        img.putdata(pixels)
    elif fmt == 1:  # RGB888
        img = Image.frombuffer("RGB", (width, height), raw, "raw", "RGB", stride, 1)
    elif fmt == 4:  # ARGB8888
        img = Image.frombuffer("RGBA", (width, height), raw, "raw", "ARGB", stride, 1)
    elif fmt == 11:  # RGBA8888
        img = Image.frombuffer("RGBA", (width, height), raw, "raw", "RGBA", stride, 1)
    else:
        print(f"  format {FORMAT_NAMES.get(fmt, fmt)} recognized but this tool's decoder"
              f" doesn't handle it yet -- raw dump saved instead")
        raw_path = out_path.with_suffix(".raw")
        raw_path.write_bytes(raw)
        print(f"  raw bytes: {raw_path}")
        return
    img.save(out_path)
    print(f"  saved: {out_path}  ({width}x{height}, {FORMAT_NAMES.get(fmt)})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timeout_s", nargs="?", type=float, default=120.0,
                     help="how long to keep polling for a nonzero framebuffer pointer")
    ap.add_argument("--poll-interval", type=float, default=1.0)
    ap.add_argument("--hold-pwrk", action="store_true",
                     help="force the PWRK-wait branch and hold the button, instead of the"
                          " default auto-power-on branch")
    args = ap.parse_args()

    riic_image = "riic2_eeprom_pwrk_test.img" if args.hold_pwrk else "riic2_eeprom.img"
    sock_path = "/tmp/qemu_vdc5_peek.sock"
    Path(sock_path).unlink(missing_ok=True)
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={HERE / riic_image}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)

        if args.hold_pwrk:
            t0 = time.time()
            while time.time() - t0 < 10:
                time.sleep(0.1)
                if pc(s) == 0x20029B18:
                    break
            qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
            print("PWRK pressed and held")

        print(f"Polling {len(PLANES)} graphics planes' FLM2 for up to {args.timeout_s:.0f}s"
              f" (every {args.poll_interval}s)...")
        seen_nonzero = set()
        t0 = time.time()
        while time.time() - t0 < args.timeout_s:
            time.sleep(args.poll_interval)
            for name, block in PLANES:
                flm2 = read_u32(s, VDC50_BASE + block + 0x0C)
                if flm2 != 0 and name not in seen_nonzero:
                    seen_nonzero.add(name)
                    flm3 = read_u32(s, VDC50_BASE + block + 0x10)
                    flm6 = read_u32(s, VDC50_BASE + block + 0x1C)
                    stride = (flm3 >> 16) & 0x7FFF
                    fmt = (flm6 >> 28) & 0xF
                    width = ((flm6 >> 16) & 0x7FF) + 1
                    elapsed = time.time() - t0
                    print(f"\nt={elapsed:.1f}s  {name}: FLM2 (base)=0x{flm2:08x}"
                          f"  format={FORMAT_NAMES.get(fmt, fmt)} width={width}"
                          f" stride={stride}")
                    dump_len = min(MAX_DUMP_BYTES, stride * 4096 if stride else 0)
                    if dump_len == 0:
                        print("  stride is 0 -- can't size the dump, skipping")
                        continue
                    raw = dump_ram(Path("/tmp"), s, flm2, dump_len)
                    out_path = Path(f"/tmp/vdc5_{name}_{elapsed:.0f}s.png")
                    decode_and_save(name, fmt, width, stride, raw, out_path)
            if len(seen_nonzero) == len(PLANES):
                break
        if not seen_nonzero:
            print(f"\nNo plane ever got a nonzero framebuffer pointer within"
                  f" {args.timeout_s:.0f}s -- real UI content may need more boot time, or a"
                  f" boot path this tool didn't drive far enough (try --hold-pwrk, or a"
                  f" longer timeout).")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
