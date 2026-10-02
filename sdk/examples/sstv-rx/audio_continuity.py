#!/usr/bin/env python3
"""Check that the app's 12 kHz audio ring is a gap-free copy of the stimulus file.

Run against an emulator where the SSTV app is resident and the file is playing (for example
test_emu.py --keep-running). This stops the VM, reads the runtime's ring (hb/audio.h) plus
its write index, and resumes. It then matches each 90-sample chunk against the file by
normalized cross-correlation. Consecutive chunks must land exactly 90 samples apart; a
dropped or repeated 0.75 ms DMA block shows up as a jump of about 9 samples.

    audio_continuity.py QMP_SOCKET APP_ELF FILE.au
"""

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "qemu-machine" / "tools"))
from screenshot import qmp_cmd  # noqa: E402
from vdc5_framebuffer_peek import qmp_open  # noqa: E402

RING = 16384
CHUNK = 90


def syms(elf):
    out = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True).stdout
    return {m.group(2): int(m.group(1), 16)
            for m in (re.match(r"^([0-9a-fA-F]+)\s+\S+\s+(\S+)$", l) for l in out.splitlines()) if m}


def main():
    sock, elf, path = sys.argv[1:4]
    s = syms(elf)
    q = qmp_open(sock)
    qmp_cmd(q, "stop")
    tmp = Path(tempfile.mkdtemp(prefix="hb_ring-")) / "ring.bin"   # QEMU writes it (pmemsave)
    qmp_cmd(q, "pmemsave", val=s["g_ring"], size=RING * 2, filename=str(tmp))
    w = int(qmp_cmd(q, "human-monitor-command",
                    **{"command-line": f"xp /1wx {s['g_w']:#x}"})["return"].split()[-1], 16)
    qmp_cmd(q, "cont")
    ring = np.frombuffer(tmp.read_bytes(), "<i2").astype(float)
    y = np.roll(ring, -(w % RING))                 # oldest first

    x = load(path)
    # track: the first chunk globally, then each next one within +-400 samples of where the
    # previous one said it should be; a jump there is a lost or repeated stretch
    res, prev = [], None
    for c in range(0, RING - CHUNK, CHUNK):
        blk = y[c:c + CHUNK]
        if blk.std() < 100:
            prev = None
            continue
        if prev is None:
            i, n = best(x, blk, 0, len(x) - CHUNK)
        else:
            i, n = best(x, blk, prev + CHUNK - 400, prev + CHUNK + 400)
            if n < 0.8:
                i, n = best(x, blk, 0, len(x) - CHUNK)
        res.append((c, i - c, n))
        prev = i
    ok = [r for r in res if r[2] > 0.8]
    jumps = [(res[k][0], res[k][1] - res[k - 1][1]) for k in range(1, len(res))
             if res[k][1] != res[k - 1][1]]
    print(f"{len(ok)}/{len(res)} chunks matched (ncc > 0.8), median ncc "
          f"{np.median([r[2] for r in res]):.3f}; {len(jumps)} discontinuities "
          f"(expect 0): {jumps[:20]}")


def load(path):
    d = open(path, "rb").read()
    if d[:4] == b".snd":
        off, size, enc, rate, ch = struct.unpack(">5I", d[4:24])
        x = np.frombuffer(d[off:off + size], ">i2")[::ch].astype(float)
    else:
        import wave
        with wave.open(path) as w:
            rate, ch = w.getframerate(), w.getnchannels()
            x = np.frombuffer(w.readframes(w.getnframes()), "<i2")[::ch].astype(float)
    if rate != 12000:
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(rate, 12000)
        x = resample_poly(x, 12000 // g, rate // g)
    return x


def best(x, blk, lo, hi):
    lo, hi = max(lo, 0), min(hi, len(x) - CHUNK)
    seg = x[lo:hi + CHUNK]
    b = blk - blk.mean()
    xc = np.correlate(seg, b, "valid")
    e = np.sqrt(np.convolve(seg ** 2, np.ones(CHUNK), "valid"))
    n = xc / (e * np.linalg.norm(b) + 1e-9)
    i = int(np.argmax(n))
    return lo + i, n[i]


if __name__ == "__main__":
    main()
