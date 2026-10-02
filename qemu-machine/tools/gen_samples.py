#!/usr/bin/env python3
"""Generate synthetic audio stimulus files: SSTV, RTTY and CW, 12 kHz mono 16-bit Sun .au.

Everything is synthesised here from the published mode specs, so the files carry no third-party
recording or image. Each signal is phase-continuous FM/FSK/keyed tone at half full scale, with an
optional white-noise floor.

    python3 qemu-machine/tools/gen_samples.py                 # all of them into scratch/samples/
    python3 qemu-machine/tools/gen_samples.py --only sstv --sstv-mode "Martin 1" -o /tmp/m1.au

Outputs (default names are what the tests look for):
  SSTV.test.au  Scottie 2 (VIS 56) by default, image = our own test pattern, also written as
                SSTV.test.png so a decode can be compared against it.
  RTTY.test.au  45.45 Bd ITA2, 170 Hz shift, mark 2125 / space 2295 Hz, 1.5 stop bits. Text:
                RTTY_TEXT below (decode_stimulus_test.py --expect "QUICK BROWN FOX").
  CW.test.au    20 WPM Morse at 700 Hz, 5 ms raised-cosine edges. Text: CW_TEXT below.
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

import numpy as np

FS = 12000
REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "scratch" / "samples"

RTTY_TEXT = ("RYRYRYRYRY\r\nCQ CQ CQ DE N0CALL N0CALL N0CALL K\r\n"
             "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789\r\n"
             "IC-7300 HOMEBREW EMULATOR TEST. 73 DE N0CALL SK\r\n")
CW_TEXT = "CQ CQ CQ DE N0CALL N0CALL K"

# ---- output ---------------------------------------------------------------------------------


def write_au(path: Path, x: np.ndarray) -> None:
    """Sun .au, 16-bit linear PCM (encoding 3), big-endian, mono, FS."""
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype(">i2").tobytes()
    path.write_bytes(struct.pack(">6I", 0x2E736E64, 24, len(pcm), 3, FS, 1) + pcm)
    print(f"wrote {path} ({len(x) / FS:.1f} s)")


def fm(freq: np.ndarray, amp: float = 0.5) -> np.ndarray:
    """Phase-continuous tone following a per-sample frequency track."""
    return amp * np.sin(2 * np.pi * np.cumsum(freq) / FS)


def add_noise(x: np.ndarray, level: float, seed: int = 7300) -> np.ndarray:
    return x + level * np.random.default_rng(seed).standard_normal(len(x)) if level else x


class Track:
    """A per-sample frequency track built from timed segments, in absolute time so rounding never
    accumulates: segment i covers samples round(t0*FS) .. round(t1*FS)."""

    def __init__(self):
        self.t = 0.0
        self.parts: list[np.ndarray] = []
        self.n = 0

    def _span(self, dur: float) -> int:
        n1 = round((self.t + dur) * FS)
        k = n1 - self.n
        self.t += dur
        self.n = n1
        return k

    def tone(self, hz: float, dur: float) -> None:
        self.parts.append(np.full(self._span(dur), float(hz)))

    def scan(self, values_hz: np.ndarray, pixel: float) -> None:
        """One scan line: values_hz[i] for pixel i, each `pixel` seconds long."""
        t0 = self.t
        start = self.n
        k = self._span(pixel * len(values_hz))
        idx = np.floor(((start + np.arange(k)) / FS - t0) / pixel).astype(int)
        self.parts.append(values_hz[np.clip(idx, 0, len(values_hz) - 1)])

    def freq(self) -> np.ndarray:
        return np.concatenate(self.parts)


# ---- SSTV -----------------------------------------------------------------------------------

# Timings as in sdk/examples/sstv-rx/sstv_core.c (from the published mode specs), in seconds:
# (vis, family, sync, porch, sep, pixel)
SSTV_MODES = {
    "Scottie 1":  (60, "scottie", 9e-3, 1.5e-3, 1.5e-3, 432.0e-6),
    "Scottie 2":  (56, "scottie", 9e-3, 1.5e-3, 1.5e-3, 275.2e-6),
    "Scottie DX": (76, "scottie", 9e-3, 1.5e-3, 1.5e-3, 1080.53e-6),
    "Martin 1":   (44, "martin", 4.862e-3, 0.572e-3, 0.572e-3, 457.6e-6),
    "Martin 2":   (40, "martin", 4.862e-3, 0.572e-3, 0.572e-3, 228.8e-6),
}
W, H = 320, 256


def font_5x7() -> dict[str, list[int]]:
    """The SDK's own built-in font (sdk/runtime/font.c): ' '..'Z', 7 rows, bit 4 = leftmost."""
    src = (REPO / "sdk" / "runtime" / "font.c").read_text()
    rows = re.findall(r"\{(0b[01]{5}(?:,\s*0b[01]{5}){6})\}", src)
    return {chr(32 + i): [int(b, 2) for b in r.split(",")] for i, r in enumerate(rows)}


def draw_text(img: np.ndarray, x: int, y: int, text: str, scale: int, rgb) -> None:
    font = font_5x7()
    for ch in text.upper():
        for r, bits in enumerate(font.get(ch, font[" "])):
            for c in range(5):
                if bits & (0x10 >> c):
                    img[y + r * scale:y + (r + 1) * scale, x + c * scale:x + (c + 1) * scale] = rgb
        x += 6 * scale


def test_pattern(mode: str) -> np.ndarray:
    """Our own 320x256 test card: colour bars, a grey ramp, a resolution wedge, a colour wheel,
    a grid and labels. Sized so every SSTV mode here sends it unscaled."""
    img = np.zeros((H, W, 3), np.uint8)
    yy, xx = np.mgrid[0:H, 0:W]
    img[:] = (24, 28, 40)
    img[(xx % 32 == 0) | (yy % 32 == 0)] = (70, 75, 90)                     # grid
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    for i, c in enumerate(bars):                                               # colour bars
        img[20:64, i * 40:(i + 1) * 40] = c
    img[72:96, :] = np.repeat(np.linspace(0, 255, W).astype(np.uint8)[None, :, None], 3, 2)  # ramp
    for i in range(8):                                                         # wedge: 1..8 px
        p = i + 1
        x0 = 8 + i * 38
        stripe = ((xx[104:152, x0:x0 + 32] - x0) // p) % 2 == 0
        img[104:152, x0:x0 + 32] = np.where(stripe[..., None], 255, 0)
    cx, cy, rad = 250, 200, 44                                                 # colour wheel
    d = np.hypot(xx - cx, yy - cy)
    ang = (np.arctan2(yy - cy, xx - cx) + np.pi) / (2 * np.pi)
    wheel = d < rad
    h6 = ang * 6
    k = lambda n: np.clip(np.abs((h6 + n) % 6 - 3) - 1, 0, 1)                 # hsv -> rgb, s=v=1
    rgb = (np.stack([k(0), k(4), k(2)], -1) * 255 * np.clip(d / rad * 1.4, 0, 1)[..., None])
    img[wheel] = rgb[wheel].astype(np.uint8)
    img[(np.abs(d - rad) < 1.5)] = (255, 255, 255)
    img[0:12, :] = (40, 60, 120)                                               # title band
    draw_text(img, 4, 3, "IC-7300 HOMEBREW SSTV TEST", 1, (255, 255, 255))
    draw_text(img, 10, 164, mode, 3, (255, 255, 255))
    draw_text(img, 10, 194, "N0CALL", 3, (255, 210, 60))
    draw_text(img, 10, 226, f"{W}X{H}", 2, (150, 200, 255))
    img[H - 4:, :] = (255, 255, 255)                                           # bottom edge marker
    return img


def sstv(mode: str, img: np.ndarray) -> np.ndarray:
    vis, family, sync, porch, sep, pixel = SSTV_MODES[mode]
    hz = lambda v: 1500.0 + v.astype(float) * (800.0 / 255.0)                 # black 1500, white 2300
    t = Track()
    t.tone(0, 0.5)                                                             # lead-in silence
    t.tone(1900, 0.300)                                                        # VIS: leader,
    t.tone(1200, 0.010)                                                        #   break,
    t.tone(1900, 0.300)                                                        #   leader,
    t.tone(1200, 0.030)                                                        #   start bit,
    bits = [(vis >> b) & 1 for b in range(7)]
    for b in bits + [sum(bits) & 1]:                                           #   7 bits LSB first + even parity,
        t.tone(1100 if b else 1300, 0.030)
    t.tone(1200, 0.030)                                                        #   stop bit
    if family == "scottie":
        t.tone(1200, sync)                                                     # Scottie starting sync
    for y in range(H):
        r, g, b = (img[y, :, i] for i in range(3))
        if family == "scottie":                                                # sep G sep B sync porch R
            t.tone(1500, sep); t.scan(hz(g), pixel)
            t.tone(1500, sep); t.scan(hz(b), pixel)
            t.tone(1200, sync); t.tone(1500, porch); t.scan(hz(r), pixel)
        else:                                                                  # sync porch G sep B sep R sep
            t.tone(1200, sync); t.tone(1500, porch); t.scan(hz(g), pixel)
            t.tone(1500, sep); t.scan(hz(b), pixel)
            t.tone(1500, sep); t.scan(hz(r), pixel)
            t.tone(1500, sep)
    t.tone(0, 0.5)
    f = t.freq()
    x = fm(f)
    x[f == 0] = 0.0
    return x


# ---- RTTY -----------------------------------------------------------------------------------

ITA2_LTRS = {"E": 1, "\n": 2, "A": 3, " ": 4, "S": 5, "I": 6, "U": 7, "\r": 8, "D": 9, "R": 10,
             "J": 11, "N": 12, "F": 13, "C": 14, "K": 15, "T": 16, "Z": 17, "L": 18, "W": 19,
             "H": 20, "Y": 21, "P": 22, "Q": 23, "O": 24, "B": 25, "G": 26, "M": 28, "X": 29,
             "V": 30}
ITA2_FIGS = {"3": 1, "-": 3, "8": 6, "7": 7, "4": 10, ",": 12, ":": 14, "(": 15, "5": 16,
             ")": 18, "2": 19, "6": 21, "0": 22, "1": 23, "9": 24, "?": 25, ".": 28, "/": 29}
FIGS, LTRS = 27, 31
SHARED = {"\n", " ", "\r"}                                                     # same code in both shifts


def ita2(text: str) -> list[int]:
    codes, figs, after_space = [LTRS, LTRS], False, False
    for ch in text.upper():
        if ch in SHARED:
            codes.append(ITA2_LTRS[ch])
            after_space = ch == " "
            continue
        if ch in ITA2_LTRS:
            if figs:
                codes.append(LTRS); figs = False
            codes.append(ITA2_LTRS[ch])
        elif ch in ITA2_FIGS:
            # A receiver with unshift-on-space (USOS, the IC-7300's default) drops back to
            # letters after a space, so figures after one get FIGS again, as real senders do.
            if not figs or after_space:
                codes.append(FIGS); figs = True
            codes.append(ITA2_FIGS[ch])
        else:
            raise ValueError(f"no ITA2 code for {ch!r}")
        after_space = False
    return codes


def rtty(text: str, baud: float = 45.45, mark: float = 2125, space: float = 2295) -> np.ndarray:
    bit = 1 / baud
    t = Track()
    t.tone(0, 0.3)
    t.tone(mark, 1.0)                                                          # idle on mark
    for c in ita2(text):
        t.tone(space, bit)                                                     # start bit
        for b in range(5):
            t.tone(mark if (c >> b) & 1 else space, bit)                       # data, LSB first
        t.tone(mark, 1.5 * bit)                                                # 1.5 stop bits
    t.tone(mark, 1.0)
    t.tone(0, 0.3)
    f = t.freq()
    x = fm(f)
    x[f == 0] = 0.0
    return x


# ---- CW -------------------------------------------------------------------------------------

MORSE = {"A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.",
         "H": "....", "I": "..", "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.",
         "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.", "S": "...", "T": "-", "U": "..-",
         "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..", "0": "-----",
         "1": ".----", "2": "..---", "3": "...--", "4": "....-", "5": ".....", "6": "-....",
         "7": "--...", "8": "---..", "9": "----.", "/": "-..-.", "?": "..--..", ".": ".-.-.-"}


def cw(text: str, wpm: float = 20, hz: float = 700, edge: float = 5e-3) -> np.ndarray:
    dot = 1.2 / wpm                                                            # PARIS timing
    keys: list[tuple[bool, float]] = [(False, 0.5)]
    for word in text.upper().split():
        for i, ch in enumerate(word):
            for j, el in enumerate(MORSE[ch]):
                keys.append((True, dot if el == "." else 3 * dot))
                keys.append((False, dot))                                      # inter-element
            keys[-1] = (False, 3 * dot)                                        # inter-character
        keys[-1] = (False, 7 * dot)                                            # inter-word
    keys.append((False, 0.5))
    env = np.concatenate([np.full(round(d * FS), 1.0 if on else 0.0) for on, d in keys])
    n = round(edge * FS)                                                       # raised-cosine edges:
    k = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)                           # smooth the keying
    env = np.convolve(env, k / k.sum(), mode="same")
    return 0.5 * env * np.sin(2 * np.pi * hz * np.arange(len(env)) / FS)


# ---- main -----------------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", choices=["sstv", "rtty", "cw"], help="generate just this one")
    ap.add_argument("-o", "--output", type=Path, help="output file (with --only)")
    ap.add_argument("--sstv-mode", default="Scottie 2", choices=list(SSTV_MODES))
    ap.add_argument("--noise", type=float, default=0.0,
                    help="white-noise std dev, as a fraction of full scale (signal is 0.5 peak)")
    args = ap.parse_args()
    if args.output and not args.only:
        ap.error("-o needs --only")
    OUT.mkdir(parents=True, exist_ok=True)

    def out(default: str) -> Path:
        return args.output or OUT / default

    if args.only in (None, "sstv"):
        img = test_pattern(args.sstv_mode)
        p = out("SSTV.test.au")
        write_au(p, add_noise(sstv(args.sstv_mode, img), args.noise))
        try:
            from PIL import Image
            Image.fromarray(img).save(p.with_suffix(".png"))
            print(f"wrote {p.with_suffix('.png')} (the transmitted image)")
        except ImportError:
            pass
    if args.only in (None, "rtty"):
        write_au(out("RTTY.test.au"), add_noise(rtty(RTTY_TEXT), args.noise))
    if args.only in (None, "cw"):
        write_au(out("CW.test.au"), add_noise(cw(CW_TEXT), args.noise))


if __name__ == "__main__":
    main()
