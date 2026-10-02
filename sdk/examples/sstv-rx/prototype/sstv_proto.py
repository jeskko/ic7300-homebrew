#!/usr/bin/env python3
"""Host prototype of the SSTV decoder planned for the sstv-rx app (sdk/sstv-app-design.md).

Checks the algorithm the app will use: a quadrature FM discriminator at 12 kHz (mix by 1900 Hz,
FIR low-pass, phase difference), VIS decode, and per-line sync lock. It is not a port of
slowrx-cli's FFT-per-6-samples estimator. The Scottie family only (VIS 60/56/76); only Scottie 2
is tested (scratch/samples/SSTV.test.au, 12 kHz .au, from qemu-machine/tools/gen_samples.py).

Usage: sstv_proto.py IN.au|IN.wav OUT.png
"""

import sys
import wave

import numpy as np
from PIL import Image
from scipy.signal import firwin, lfilter, resample_poly

FS = 12000
# VIS -> (name, pixel time s, sync s, separator s); line = sep G sep B sync porch R
SCOTTIE = {60: ("Scottie 1", 0.4320e-3, 9e-3, 1.5e-3),
           56: ("Scottie 2", 0.2752e-3, 9e-3, 1.5e-3),
           76: ("Scottie DX", 1.0800e-3, 9e-3, 1.5e-3)}
W, H = 320, 256


def load(path):
    d = open(path, "rb").read()
    if d[:4] == b".snd":
        off, size, enc, rate, ch = (int(v) for v in np.frombuffer(d[4:24], ">u4"))
        x = np.frombuffer(d[off:off + size], ">i2")[::ch]
    else:
        with wave.open(path) as w:
            rate, ch = w.getframerate(), w.getnchannels()
            x = np.frombuffer(w.readframes(w.getnframes()), "<i2")[::ch]
    x = x.astype(np.float32) / 32768
    return resample_poly(x, FS, rate) if rate != FS else x


def discriminator(x):
    """Instantaneous frequency in Hz, per sample."""
    n = np.arange(len(x))
    z = lfilter(firwin(33, 900, fs=FS), 1, x * np.exp(-2j * np.pi * 1900 * n / FS))
    f = 1900 + np.angle(z[1:] * np.conj(z[:-1])) * FS / (2 * np.pi)
    f = np.roll(np.concatenate([[1900], f]), -16)          # undo the FIR's group delay
    return lfilter(np.ones(3) / 3, 1, f)


def find_vis(f):
    """VIS start bit: 30 ms of 1200 Hz right after >= 200 ms of 1900 Hz leader."""
    is1200 = np.abs(f - 1200) < 80
    run = np.convolve(is1200, np.ones(int(0.025 * FS)), "valid")
    for i in np.flatnonzero(run >= 0.9 * 0.025 * FS):
        lead = f[max(0, i - int(0.22 * FS)):i - int(0.02 * FS)]
        if len(lead) and np.mean(np.abs(lead - 1900) < 80) > 0.9:
            bits = [int(np.median(f[i + int((0.030 * (b + 1) + 0.015) * FS) + np.arange(-60, 60)])
                        < 1200) for b in range(8)]
            return i, sum(b << k for k, b in enumerate(bits[:7])), bits
    return None


def decode_scottie(f, vis_start, spec):
    name, pix, sync, sep = spec
    chan = pix * W
    line = 2 * sep + 3 * chan + sync + sep      # porch = sep in Scottie
    syncmask = lfilter(np.ones(int(0.006 * FS)) / int(0.006 * FS), 1, (f < 1350).astype(float))
    lum = lambda fr: np.clip((fr - 1500) / 800 * 255, 0, 255)
    img = np.zeros((H, W, 3), np.uint8)
    pos = vis_start + int(0.300 * FS)                         # VIS = 10 bits x 30 ms
    pred = pos + int((2 * sep + 2 * chan + sync) * FS)        # first sync end
    for y in range(H):
        w0 = max(pred - int(0.02 * FS), 0)
        se = w0 + int(np.argmax(syncmask[w0:pred + int(0.02 * FS)]))   # sync end, re-locked
        tR = se + int(sep * FS)
        tB = se - int(sync * FS) - int(chan * FS)
        tG = tB - int((sep + chan) * FS)
        for c, t in ((1, tG), (2, tB), (0, tR)):              # Scottie sends G, B, R
            img[y, :, c] = lum(np.interp(t + (np.arange(W) + 0.5) * pix * FS,
                                         np.arange(len(f)), f))
        pred = se + int(line * FS)
    return name, img


def main():
    f = discriminator(load(sys.argv[1]))
    found = find_vis(f)
    if not found:
        sys.exit("no VIS header found")
    start, vis, bits = found
    print(f"VIS {vis} at {start / FS:.2f} s, bits {bits}")
    if vis not in SCOTTIE:
        sys.exit(f"VIS {vis}: only the Scottie family is prototyped")
    name, img = decode_scottie(f, start, SCOTTIE[vis])
    Image.fromarray(img).save(sys.argv[2])
    print(f"{name}: wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
