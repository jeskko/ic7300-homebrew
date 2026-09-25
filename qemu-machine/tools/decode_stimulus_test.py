#!/usr/bin/env python3
"""Feed stimulus audio to the CPU's DX_FMT consumers (the "decoders") and check they react.

Built 2026-09-25. The CPU reads the DSP's DX_FMT stream (SSIF1 RX, demod output) in exactly two
places (notes/dsp-protocol.md, "DX_FMT consumers"):
  - RTTY decode screen tuning scope, FUN_200211c4 -> FUN_20020e18/FUN_20020d80/FUN_20020a50:
    1024-point FFT at 6 kHz; bins 0x145..0x1ad (1.90-2.52 kHz) -> 105 bytes at 0x203a2bdd.
    Runs only in RTTY/RTTY-R with the DECODE screen open (0x203de180 == 10 or 12);
    enable flag 0x203a2de5.
  - CTCSS tone detector, FUN_200636b0 -> FUN_200635ec/FUN_20063f08: armed by FUN_200527b8 via
    FUN_20063548(tone index) in FM with TSQL on; state byte 0x203fc6e7 (1 = running);
    "tone present" flag 0x203fc6f3.
RTTY *text* doesn't come from DX_FMT: the DSP demodulates FSK and drives the mark/space bit on
pin P8_7 (RTD), which the CPU samples at 1 kHz from MTU2 ch1 TGI1A (GIC 146) -- neither is
modelled yet, so no text decodes in the emulator (see notes/dsp-protocol.md).

Test 1: RTTY mode, MENU > DECODE, RTTY file on DX_FMT: expect the scope enabled and FFT peaks
at mark 2125 / space 2295 Hz. Test 2: FM, TSQL 88.5 Hz, tones on DX_FMT: expect the detector
to flag 88.5 Hz only.

Usage: decode_stimulus_test.py RTTY_FILE [--out DIR]
  RTTY_FILE: any RTTY recording (.au/.wav, 16-bit). Its two FSK tones are found by FFT and the
  signal is shifted (SSB, Hilbert) so they sit at 2125/2295 Hz, written as a 12 kHz WAV.
  e.g. decode_stimulus_test.py "../scratch/samples/rtty 10 seconds.wav"
"""

from __future__ import annotations

import argparse
import os
import wave
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from scipy.signal import hilbert, resample_poly

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_boot import Qmp  # noqa: E402
from civ import Civ  # noqa: E402
from fp import FrontPanel  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE, DEFAULT_ICOUNT  # noqa: E402
from vdc5_framebuffer_peek import GPIO_PATH  # noqa: E402

SSIF = "/machine/ssif"
SCREEN_STATE = 0x203DE180
FFT_ENABLE, FFT_BINS, FFT_BIN0 = 0x203A2DE5, 0x203A2BDD, 0x145
CTCSS_STATE, CTCSS_PRESENT = 0x203FC6E7, 0x203FC6F3


def shift_to_mark(src: Path, dst: Path) -> tuple[float, float]:
    """Write src, resampled to 12 kHz and shifted so its lower FSK tone is at 2125 Hz, to dst.
    Returns the two tones found. Centres them on 2210 Hz, i.e. mark 2125 / space 2295 Hz for
    a 170 Hz shift."""
    d = src.read_bytes()
    if d[:4] == b".snd":
        off, size, enc, rate, ch = (int(v) for v in np.frombuffer(d[4:24], ">u4"))
        x = np.frombuffer(d[off:off + min(size, len(d) - off)], ">i2")[::ch]
    else:
        with wave.open(str(src)) as w:
            rate, ch = w.getframerate(), w.getnchannels()
            x = np.frombuffer(w.readframes(w.getnframes()), "<i2")[::ch]
    y = resample_poly(x.astype(float) / 32768, 12000, rate)
    spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    f = np.fft.rfftfreq(len(y), 1 / 12000)
    peaks = []
    for i in np.argsort(spec)[::-1]:
        if all(abs(f[i] - p) > 60 for p in peaks):
            peaks.append(f[i])
        if len(peaks) == 2:
            break
    # refine each tone to the power centroid within +-60 Hz (keyed tones have broad peaks)
    p2 = spec ** 2
    lo, hi = sorted(float(np.sum(f[m] * p2[m]) / np.sum(p2[m]))
                    for m in (np.abs(f - pk) < 60 for pk in peaks))
    # centre the pair on 2210 Hz (mark 2125 / space 2295 for a 170 Hz shift)
    z = np.real(hilbert(y) * np.exp(2j * np.pi * (2210 - (lo + hi) / 2) * np.arange(len(y)) / 12000))
    z *= 0.5 / np.abs(z).max()
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(12000)
        w.writeframes((z * 32767).astype("<i2").tobytes())
    return lo, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rtty_file", type=Path)
    ap.add_argument("--out", type=Path, default=None, help="directory for screenshots")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="decodetest"))
    out = (args.out or tmp).resolve()
    out.mkdir(parents=True, exist_ok=True)
    stim = tmp / "rtty_2125.wav"
    lo, hi = shift_to_mark(args.rtty_file, stim)
    print(f"RTTY tones {lo:.0f}/{hi:.0f} Hz (shift {hi - lo:.0f}), centred on 2210 Hz")
    qsock, fsock, csock = (str(tmp / n) for n in ("q.sock", "fp.sock", "civ.sock"))
    cmd = [str(QEMU), "-M", "rz-a1h", "-display", "none", "-kernel", str(FLASH),
           "-monitor", "none",
           "-global", f"rza1h-riic.image={HERE / 'riic2_eeprom_pwrk_test.img'}",
           "-qmp", f"unix:{qsock},server,nowait",
           "-chardev", f"socket,id=fpctl,path={fsock},server=on,wait=off",
           "-chardev", f"socket,id=civ,path={csock},server=on,wait=off", "-serial", "chardev:civ",
           "-icount", DEFAULT_ICOUNT]
    env = dict(os.environ, RZA1H_AF_TONE="none")
    proc = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    ok = True

    def check(name, cond, detail):
        nonlocal ok
        ok &= bool(cond)
        print(f"{'PASS' if cond else 'FAIL'}  {name}: {detail}", flush=True)

    try:
        time.sleep(0.5)
        q = Qmp(qsock)
        mem = lambda a, n=1: bytes(int(q.hmp(f"xp /1xb 0x{a + i:x}").split()[-1], 16)
                                   for i in range(n))
        ssif = lambda prop, val: q.cmd("qom-set", path=SSIF, property=prop, value=val)
        shot = lambda name: q.cmd("screendump", filename=str(out / f"{name}.ppm"))
        t0 = time.time()
        while time.time() - t0 < 30 and q.pc() != 0x20029B18:
            time.sleep(0.05)
        q.cmd("qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        time.sleep(25)  # main screen, ~4 s emulated
        civ, fp = Civ(csock), FrontPanel(fsock)

        def civ_set(*payload):
            # CI-V commands are sometimes lost (first one after boot, right after a mode
            # change): retry until the radio acks with FB
            for _ in range(5):
                if any(f[2:3] == b"\xfb" for f in civ.cmd(*payload)):
                    return True
                time.sleep(1)
            return False

        def set_mode(code):
            if not civ_set(0x06, code, 0x01):
                return False
            for _ in range(3):
                if any(f[2:4] == bytes([0x04, code]) for f in civ.cmd(0x04)):
                    return True
            return False

        # --- Test 1: RTTY decode screen scope ---
        check("mode RTTY", set_mode(0x04), "CI-V 06 04 read back")
        fp.press("MENU")
        time.sleep(2)
        fp.touch(245, 80)  # MENU page 1: DECODE
        time.sleep(2)
        state = mem(SCREEN_STATE)[0]
        check("RTTY decode screen open", state in (10, 12), f"screen state {state}")
        ssif("fmt-file", f"{stim},loop")
        time.sleep(6)
        check("scope capture enabled", mem(FFT_ENABLE)[0] == 1, f"flag {mem(FFT_ENABLE)[0]}")
        bins = mem(FFT_BINS, 105)
        hz = lambda i: (FFT_BIN0 + i) * 6000 / 1024
        lo = max(range(0, 52), key=lambda i: bins[i])
        hi = max(range(52, 105), key=lambda i: bins[i])
        # +-3 bins: a 170 ms window of keyed FSK moves the peaks around a little
        check("FFT peaks at mark/space", abs(hz(lo) - 2125) < 18 and abs(hz(hi) - 2295) < 18,
              f"{hz(lo):.0f} Hz ({bins[lo]}), {hz(hi):.0f} Hz ({bins[hi]}); "
              f"median bin {sorted(bins)[52]}")
        shot("rtty-decode")
        ssif("fmt-file", "none")
        fp.press("EXIT")
        fp.press("EXIT")

        # --- Test 2: CTCSS detector under FM + TSQL 88.5 Hz ---
        check("mode FM", set_mode(0x05), "CI-V 06 05 read back")
        check("TSQL 88.5 Hz on", civ_set(0x1B, 0x01, 0x00, 0x08, 0x85) and
              civ_set(0x16, 0x43, 0x01), "CI-V 1B 01 / 16 43 acked")
        time.sleep(2)
        check("CTCSS detector running", mem(CTCSS_STATE)[0] == 1, f"state {mem(CTCSS_STATE)[0]}")
        for tone, want in (("none", 0), ("88.5:0.1", 1), ("85.4:0.1", 0), ("91.5:0.1", 0),
                           ("100:0.1", 0), ("88.5:0.01", 1)):
            ssif("fmt-tone", tone)
            time.sleep(3)
            got = mem(CTCSS_PRESENT)[0]
            check(f"CTCSS with DX_FMT tone {tone}", got == want, f"present={got}, want {want}")
        shot("fm-tsql")
        print("RESULT", "PASS" if ok else "FAIL", f"(screenshots in {out})")
    finally:
        proc.terminate()
        proc.wait(timeout=10)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
