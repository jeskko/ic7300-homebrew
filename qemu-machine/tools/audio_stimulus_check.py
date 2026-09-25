#!/usr/bin/env python3
"""Check that a fake-DSP stimulus file reaches the firmware's RX-audio ring intact.

Built 2026-09-25 with ssif.c's stimulus files (RZA1H_AF_FILE, /machine/ssif af-file). Boots
the emulator (PWRK-hold boot, like bench_boot.py) with the file on DX_REC L, then takes
--snapshots snapshots of the firmware's 48 kHz RX-audio ring 0x203fbdc0 (8 blocks x 36 int16,
pushed by ssif0_rx_pump_dx_rec 0x20060614 from every 2nd frame's top 16 bits). Each block is
matched against the file resampled to 48 kHz: the best normalized cross-correlation, the
offset it's at and the amplitude ratio. A block that is the file scores ~1 (the fake DSP's
noise floor of 0.01 FS costs a little on quiet passages).

--runtime arms the file with qom-set after boot instead of the env knob.

Usage: audio_stimulus_check.py FILE [--wait S] [--snapshots N] [--runtime] [--gain G]
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from math import gcd
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_boot import Qmp  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE, DEFAULT_ICOUNT  # noqa: E402
from vdc5_framebuffer_peek import GPIO_PATH  # noqa: E402

RING, BLOCKS, BLOCK = 0x203FBDC0, 8, 36
SSIF_PATH = "/machine/ssif"


def load_pcm(path: Path) -> tuple[np.ndarray, int]:
    d = path.read_bytes()
    if d[:4] == b".snd":
        off, size, enc, rate, ch = np.frombuffer(d[4:24], ">u4")
        assert enc == 3, enc
        pcm = np.frombuffer(d[off:off + min(size, len(d) - off)], ">i2")
    else:
        import wave
        with wave.open(str(path)) as w:
            rate, ch = w.getframerate(), w.getnchannels()
            pcm = np.frombuffer(w.readframes(w.getnframes()), "<i2")
    return pcm[::ch].astype(float) / 32768, int(rate)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", type=Path)
    ap.add_argument("--wait", type=float, default=45.0, help="wall s after PWRK")
    ap.add_argument("--snapshots", type=int, default=3)
    ap.add_argument("--runtime", action="store_true")
    ap.add_argument("--gain", type=float, default=1.0)
    args = ap.parse_args()

    pcm, rate = load_pcm(args.file)
    g = gcd(rate, 48000)
    ref = resample_poly(pcm, 48000 // g, rate // g) * args.gain
    spec = f"{args.file.resolve()},gain={args.gain},loop"

    tmp = Path(tempfile.mkdtemp(prefix="audiochk"))
    sock = str(tmp / "q.sock")
    env = dict(os.environ, RZA1H_DEBUG="ssif")
    if not args.runtime:
        env["RZA1H_AF_FILE"] = spec
    cmd = [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
           "-serial", "none", "-monitor", "none",
           "-global", f"rza1h-riic.image={HERE / 'riic2_eeprom_pwrk_test.img'}",
           "-qmp", f"unix:{sock},server,nowait", "-icount", DEFAULT_ICOUNT]
    log = open(tmp / "qemu.log", "w")
    proc = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    worst = 1.0
    try:
        time.sleep(0.5)
        q = Qmp(sock)
        t0 = time.time()
        while time.time() - t0 < 30 and q.pc() != 0x20029B18:
            time.sleep(0.05)
        q.cmd("qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        time.sleep(args.wait)
        if args.runtime:
            r = q.cmd("qom-set", path=SSIF_PATH, property="af-file", value=spec)
            print("qom-set af-file:", r)
            time.sleep(3)
        for n in range(args.snapshots):
            q.cmd("stop")
            status = q.cmd("qom-get", path=SSIF_PATH, property="af-status").get("return")
            raw = tmp / "ring.bin"
            q.cmd("pmemsave", val=RING, size=BLOCKS * BLOCK * 2, filename=str(raw))
            q.cmd("cont")
            ring = np.frombuffer(raw.read_bytes(), "<i2").astype(float).reshape(BLOCKS, BLOCK)
            ring /= 32768
            print(f"snapshot {n}: af-status '{status}'")
            for b, blk in enumerate(ring):
                rms = np.sqrt(np.mean(blk ** 2))
                if rms < 0.02:
                    print(f"  block {b}: quiet (rms {rms:.4f})")
                    continue
                xc = np.correlate(ref, blk, "valid")
                energy = np.sqrt(np.convolve(ref ** 2, np.ones(BLOCK), "valid"))
                ncc = xc / (energy * np.linalg.norm(blk) + 1e-12)
                i = int(np.argmax(ncc))
                ratio = np.linalg.norm(blk) / (energy[i] + 1e-12)
                worst = min(worst, ncc[i])
                print(f"  block {b}: ncc {ncc[i]:.4f} at {i / 48000:8.4f} s, "
                      f"amplitude ratio {ratio:.3f}, rms {rms:.3f}")
            time.sleep(2)
        print(f"RESULT worst ncc {worst:.4f}")
    finally:
        proc.terminate()
        proc.wait(timeout=10)
        log.close()
        print("ssif log:")
        for line in (tmp / "qemu.log").read_text().splitlines():
            if "ssif" in line:
                print("  " + line)


if __name__ == "__main__":
    main()
