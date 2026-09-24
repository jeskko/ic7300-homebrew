#!/usr/bin/env python3
"""Boot benchmark: how fast does the emulator reach the main screen, and at what speed ratio?

Built 2026-09-24 for the emulation-speed work. Launches QEMU like screenshot.py (PWRK-hold boot
by default), then polls over QMP every --poll wall seconds:
  - emulated time = OSTM0 CNT (32 MHz, counts emulated time since the guest started it; the
    32-bit wrap at ~134 s is unwrapped here), giving the emulated/wall speed ratio per interval;
  - the LCD plane GR2 framebuffer, compared with the reference main-screen screenshot.
Reports wall and emulated time of the first exact match ("time to main screen").

Extra QEMU args after `--`, e.g. `bench_boot.py -- -icount shift=2`. Default icount comes
from qemu_launch.DEFAULT_ICOUNT unless the extra args contain -icount (or --no-icount).

Usage: bench_boot.py [--timeout S] [--poll S] [--no-pwrk] [--no-icount] [--label L] [-- qemu args]
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_launch import QEMU, FLASH, HERE, DEFAULT_ICOUNT  # noqa: E402
from vdc5_framebuffer_peek import GPIO_PATH  # noqa: E402

OSTM0_CNT = 0xFCFEC004
OSTM_HZ = 32_000_000
REF = HERE / "screenshots" / "2026-09-24-main-screen-factory-defaults.png"  # factory-default EEPROM
FB_ADDR, FB_STRIDE, W, H = 0x20974FE0, 960, 480, 272


class Qmp:
    def __init__(self, path: str):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, execute: str, **arguments):
        self.f.write(json.dumps({"execute": execute, "arguments": arguments}) + "\n")
        self.f.flush()
        while True:
            m = json.loads(self.f.readline())
            if "return" in m or "error" in m:
                return m

    def hmp(self, line: str) -> str:
        return self.cmd("human-monitor-command", **{"command-line": line})["return"]

    def u32(self, addr: int) -> int:
        return int(self.hmp(f"xp /1wx 0x{addr:x}").split()[1], 16)

    def pc(self) -> int:
        for tok in self.hmp("info registers").split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
        return 0


def ref_565() -> np.ndarray:
    a = np.asarray(Image.open(REF).convert("RGB")).astype(np.uint16)
    return ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=400.0)
    ap.add_argument("--poll", type=float, default=2.0)
    ap.add_argument("--no-pwrk", action="store_true")
    ap.add_argument("--no-icount", action="store_true")
    ap.add_argument("--label", default="")
    ap.add_argument("qemu_args", nargs="*")
    args = ap.parse_args()

    ref = ref_565()
    tmp = Path(tempfile.mkdtemp(prefix="bench"))
    sock = str(tmp / "q.sock")
    image = HERE / ("riic2_eeprom.img" if args.no_pwrk else "riic2_eeprom_pwrk_test.img")
    extra = list(args.qemu_args)
    if "-icount" not in extra and not args.no_icount:
        extra += ["-icount", DEFAULT_ICOUNT]
    cmd = [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
           "-serial", "none", "-monitor", "none", "-global", f"rza1h-riic.image={image}",
           "-qmp", f"unix:{sock},server,nowait"] + extra
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    result = {"label": args.label, "args": extra, "match_wall": None, "match_emu": None}
    try:
        time.sleep(0.5)
        q = Qmp(sock)
        t0 = time.time()
        if not args.no_pwrk:
            while time.time() - t0 < 30 and q.pc() != 0x20029B18:
                time.sleep(0.05)
            q.cmd("qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        last_cnt, emu_s, last_t = q.u32(OSTM0_CNT), 0.0, time.time()
        samples = []
        while time.time() - t0 < args.timeout:
            time.sleep(args.poll)
            cnt, now = q.u32(OSTM0_CNT), time.time()
            d = (cnt - last_cnt) & 0xFFFFFFFF
            emu_s += d / OSTM_HZ
            ratio = (d / OSTM_HZ) / (now - last_t)
            last_cnt, last_t = cnt, now
            raw = tmp / "fb.bin"
            q.cmd("pmemsave", val=FB_ADDR, size=FB_STRIDE * H, filename=str(raw))
            fb = np.frombuffer(raw.read_bytes(), dtype="<u2").reshape(H, W)
            diff = int(np.count_nonzero(fb != ref))
            samples.append((round(now - t0, 1), round(emu_s, 2), round(ratio, 3), diff))
            print(f"wall {now - t0:6.1f}s  emu {emu_s:7.2f}s  speed {ratio:6.3f}x  "
                  f"fb-diff-pixels {diff}", flush=True)
            if diff == 0 and result["match_wall"] is None:
                result["match_wall"], result["match_emu"] = round(now - t0, 1), round(emu_s, 2)
                break
        result["samples"] = samples
        steady = [r for _, _, r, _ in samples[len(samples) // 2:]]
        result["median_speed_2nd_half"] = sorted(steady)[len(steady) // 2] if steady else None
        print("RESULT " + json.dumps(result))
    finally:
        proc.terminate()
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
