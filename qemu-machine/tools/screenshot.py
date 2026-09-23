#!/usr/bin/env python3
"""Boot, wait, and save what the emulated LCD would show -- plus any extra guest surface.

Built 2026-09-23 once openvg.c started rasterizing the real command stream. Unlike
vdc5_framebuffer_peek.py (which dumps the moment a plane pointer first appears, long before
anything is drawn), this lets the boot run for `seconds`, pauses the VM, then:
  - dumps every VDC50 graphics plane with a nonzero FLM2 as a 480x272 PNG (format/stride from
    FLM3/FLM6, same decoding as vdc5_framebuffer_peek.py);
  - dumps each --surface ADDR,STRIDE,W,H,FMT (FMT: 565 | a8 | argb; STRIDE may be negative,
    matching the OpenVG driver's bottom-up surfaces, e.g. the 960x552 pixmap
    0x20974860,-1920,960,552,565).
Uses QMP pmemsave (fast, GDB-free).

Usage: screenshot.py [seconds] [--surface ...]... [--out DIR] [--no-pwrk]
"""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vdc5_framebuffer_peek import PLANES, VDC50_BASE, GPIO_PATH, qmp_open  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE, DEFAULT_ICOUNT  # noqa: E402

LCD_H = 272
_rx = b""


def qmp_cmd(s, execute: str, **arguments):
    """Send one QMP command; return its reply, skipping asynchronous events."""
    global _rx
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
    while True:
        while b"\n" not in _rx:
            _rx += s.recv(1 << 20)
        line, _rx = _rx.split(b"\n", 1)
        msg = json.loads(line)
        if "return" in msg or "error" in msg:
            return msg


def hmp(s, cmd: str) -> str:
    return qmp_cmd(s, "human-monitor-command", **{"command-line": cmd})["return"]


def read_u32(s, addr: int) -> int:
    return int(hmp(s, f"xp /1wx 0x{addr:x}").split()[1], 16)


def pc(s) -> int:
    for tok in hmp(s, "info registers").split():
        if tok.startswith("R15="):
            return int(tok[4:], 16)
    return 0
BPP = {"565": 2, "a8": 1, "argb": 4}
VDC_FMT = {0: "565", 4: "argb"}


def to_rgb(fmt: str, px: bytes) -> tuple[int, int, int]:
    if fmt == "565":
        v = px[0] | px[1] << 8
        return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
    if fmt == "a8":
        return (px[0],) * 3
    v = struct.unpack("<I", px)[0]
    return (v >> 16) & 255, (v >> 8) & 255, v & 255


def grab(s, tmpdir: Path, addr: int, stride: int, w: int, h: int, fmt: str, out: Path):
    lo = addr + min(0, stride * (h - 1))
    size = abs(stride) * (h - 1) + w * BPP[fmt]
    raw_path = tmpdir / f"surf_{addr:08x}.bin"
    qmp_cmd(s, "pmemsave", val=lo, size=size, filename=str(raw_path))
    time.sleep(0.3)
    raw = raw_path.read_bytes()
    img = Image.new("RGB", (w, h))
    b = BPP[fmt]
    for y in range(h):
        row = addr + y * stride - lo
        for x in range(w):
            img.putpixel((x, y), to_rgb(fmt, raw[row + x * b: row + x * b + b]))
    img.save(out)
    print(f"  saved {out} ({w}x{h} {fmt} @0x{addr:08x} stride {stride})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=float, default=150.0)
    ap.add_argument("--surface", action="append", default=[])
    ap.add_argument("--out", default="/tmp")
    ap.add_argument("--no-pwrk", action="store_true")
    ap.add_argument("--poke", action="append", default=[],
                    help="ADDR=VALUE (hex, u32) written via the gdbstub after `seconds`; "
                         "the VM then runs --after more seconds before the capture")
    ap.add_argument("--after", type=float, default=20.0)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    image = HERE / ("riic2_eeprom.img" if args.no_pwrk else "riic2_eeprom_pwrk_test.img")
    sock = "/tmp/qemu_screenshot.sock"
    Path(sock).unlink(missing_ok=True)
    proc = subprocess.Popen(
        [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
         "-serial", "none", "-monitor", "none", "-global", f"rza1h-riic.image={image}",
         "-icount", DEFAULT_ICOUNT, "-qmp", f"unix:{sock},server,nowait"]
        + (["-gdb", "tcp::1234"] if args.poke else []),
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock)
        if not args.no_pwrk:
            t0 = time.time()
            while time.time() - t0 < 15 and pc(s) != 0x20029B18:
                time.sleep(0.1)
            qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print(f"running {args.seconds:.0f}s...")
        time.sleep(args.seconds)
        if args.poke:
            # QEMU's monitor has no physical-memory write, so use the gdbstub.
            from gdbrsp import GdbRsp
            g = GdbRsp(port=1234)
            g.handshake()
            try:        # attaching usually halts the VM already
                g.interrupt()
                g.wait_stop(timeout=5)
            except Exception:
                pass
            for spec in args.poke:
                a, v = (int(x, 16) for x in spec.split("="))
                g.write_u32(a, v)
                print(f"poked 0x{a:08x} = 0x{v:08x} (reads back 0x{g.read_u32(a):08x})")
            g.cont()
            g.close()
            time.sleep(args.after)
        qmp_cmd(s, "stop")
        for name, block in PLANES:
            flm2 = read_u32(s, VDC50_BASE + block + 0x0C)
            if not flm2:
                continue
            flm3 = read_u32(s, VDC50_BASE + block + 0x10)
            flm6 = read_u32(s, VDC50_BASE + block + 0x1C)
            fmt = VDC_FMT.get((flm6 >> 28) & 0xF)
            width = ((flm6 >> 16) & 0x7FF) + 1
            stride = (flm3 >> 16) & 0x7FFF
            print(f"{name}: FLM2=0x{flm2:08x} width={width} stride={stride} fmt={fmt}")
            if fmt and stride:
                grab(s, out, flm2, stride, width, LCD_H, fmt, out / f"lcd_{name}.png")
        for spec in args.surface:
            a, st, w, h, f = spec.split(",")
            addr = int(a, 16)
            grab(s, out, addr, int(st), int(w), int(h), f, out / f"surf_{addr:08x}.png")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
