#!/usr/bin/env python3
"""Drive the IC-7300 emulator's virtual front panel over its control socket.

Transport (implemented on the QEMU/C side; this file only speaks the protocol):
a UNIX-domain stream socket, launched with e.g.
    -chardev socket,id=fpctl,path=/some/fp.sock,server=on,wait=off
    (rz_a1h.c attaches a chardev with id "fpctl" to SCIF3; no -global needed)
Line-based ASCII, '\\n'-terminated, one reply line per command starting "ok" or "err":
    get                      -> ok <64 hex chars>   (the 32-byte report, offset 0..0x1f)
    w <off> <hexbytes>       -> ok                  (write bytes at hex offset)
    bit <off> <bit> <0|1>    -> ok                  (off hex, bit 0..7)
    add8  <off> <n>          -> ok                  (n signed decimal, byte += n mod 256)
    add16 <off> <n>          -> ok                  (n signed decimal, BE u16 += n mod 65536)
    touch <x> <y>            -> ok                  (decimal pixels, 0..479 / 0..271)
    release                  -> ok
Replies can take up to ~50 ms; we use a 2 s timeout per command.

Report layout (see rza1h-scif.c / the front-CPU protocol notes for the source):
  0x01-0x0c  12 ASCII bytes, front-CPU version string
  0x0d-0x11  40-bit key field, active-high, key bit b of byte o (see KEYS below)
  0x13       touch tag: 0x00 = touching, 0xff = not touching
  0x14-0x15  touch X, BE16;  0x16-0x17  touch Y, BE16
  0x18-0x19  MAIN DIAL, BE16 wrapping counter, clockwise = up
  0x1a       dial "fast" flag -- left at 0, no method writes it
  0x1b       TWIN PBT inner counter, u8 wrap
  0x1c       TWIN PBT outer counter, u8 wrap
  0x1d       MULTI knob counter, u8 wrap
  0x1e       AF gain pot, 0..255
  0x1f       RF/SQL pot, 0..255

Library use:
    from fp import FrontPanel
    fp = FrontPanel("/tmp/fp.sock")
    fp.press("MENU")
    fp.dial(20)
    print(fp.state())

CLI use (socket path via --sock, or env RZA1H_FPCTL):
    fp.py press MENU [--hold 0.15]
    fp.py long MENU
    fp.py combo MENU EXIT
    fp.py dial +20
    fp.py multi -3
    fp.py pbt-in 5
    fp.py pbt-out -5
    fp.py af 128
    fp.py rfsql 255
    fp.py touch 240 136 [--hold 0.15]
    fp.py drag 100 100 300 200
    fp.py state
    fp.py keys
    fp.py raw "get"
    fp.py seq "press MENU; wait 1; touch 100 50; dial -5"
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import socket
import sys
import time

# --- key table: canonical name -> (byte offset, bit) --------------------------------------
# M-CH UP/DN and MIC UP/DN bit<->direction assignment is low-confidence (not yet confirmed
# against real hardware or the front-CPU firmware); source notes/front-panel map once written.
KEYS: dict[str, tuple[int, int]] = {
    "TRANSMIT": (0x0D, 0),
    "TUNER": (0x0D, 1),
    "VOX/BK-IN": (0x0D, 2),
    "MENU": (0x0D, 3),
    "FUNCTION": (0x0D, 4),
    "M.SCOPE": (0x0D, 5),
    "QUICK": (0x0D, 6),
    "XFC": (0x0D, 7),
    "P.AMP/ATT": (0x0E, 0),
    "NOTCH": (0x0E, 1),
    "NB": (0x0E, 2),
    "NR": (0x0E, 3),
    "EXIT": (0x0E, 4),
    "AUTO TUNE": (0x0E, 5),
    "SPEECH/LOCK": (0x0E, 6),
    "MPAD": (0x0E, 7),
    "A/B": (0x0F, 0),
    "V/M": (0x0F, 1),
    "M-CH UP": (0x0F, 2),
    "M-CH DN": (0x0F, 3),
    "RIT": (0x0F, 4),
    "ΔTX": (0x0F, 5),  # DELTA TX
    "CLEAR": (0x0F, 6),
    "SPLIT": (0x0F, 7),
    "MULTI": (0x10, 2),  # MULTI knob push
    "PBT-CLR": (0x10, 3),
    "EXT1": (0x11, 5),
    "EXT2": (0x11, 4),
    "EXT3": (0x11, 3),
    "EXT4": (0x11, 2),
    "MIC UP": (0x11, 7),
    "MIC DN": (0x11, 6),
}

# A couple of short-name aliases that aren't derivable by splitting a canonical name on '/'.
_EXTRA_ALIASES = {"scope": "M.SCOPE"}


def _norm(s: str) -> str:
    """Fold a key name down to a bare comparable token: lowercase, delta->d, no separators."""
    s = s.strip().replace("Δ", "d").replace("δ", "d").lower().replace("delta", "d")
    return re.sub(r"[-_./\s()]", "", s)


def _build_aliases() -> dict[str, str]:
    aliases: dict[str, str] = {}
    for canon in KEYS:
        aliases[_norm(canon)] = canon
        for part in canon.split("/"):
            aliases.setdefault(_norm(part), canon)
    for alias, canon in _EXTRA_ALIASES.items():
        aliases[_norm(alias)] = canon
    return aliases


_ALIASES = _build_aliases()


def resolve_key(name: str) -> tuple[int, int]:
    canon = _ALIASES.get(_norm(name))
    if canon is None:
        raise KeyError(f"unknown front-panel key {name!r} (see `fp.py keys`)")
    return KEYS[canon]


# --- library ---------------------------------------------------------------------------------


class FrontPanel:
    def __init__(self, path: str):
        self.path = path
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(2.0)
        self.sock.connect(path)
        self._buf = b""

    def raw(self, cmd: str) -> str:
        """Send one command line, block for its single reply line, return it verbatim."""
        self.sock.sendall((cmd + "\n").encode())
        while b"\n" not in self._buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("front-panel control socket closed")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        reply = line.decode().strip()
        if not reply.startswith("ok"):
            raise RuntimeError(f"{cmd!r} -> {reply!r}")
        return reply

    def get(self) -> bytes:
        return bytes.fromhex(self.raw("get").split()[1])

    def _w(self, off: int, data: bytes) -> None:
        self.raw(f"w {off:02x} {data.hex()}")

    def _bit(self, off: int, bit: int, val: int) -> None:
        self.raw(f"bit {off:02x} {bit} {1 if val else 0}")

    def _add8(self, off: int, n: int) -> None:
        self.raw(f"add8 {off:02x} {n}")

    def _add16(self, off: int, n: int) -> None:
        self.raw(f"add16 {off:02x} {n}")

    def _chunked(self, add_fn, off: int, n: int, chunk: int, delay: float) -> None:
        remaining = n
        while remaining:
            step = max(-chunk, min(chunk, remaining))
            add_fn(off, step)
            remaining -= step
            if remaining:
                time.sleep(delay)

    def press(self, name: str, hold_s: float = 0.15) -> None:
        off, bit = resolve_key(name)
        self._bit(off, bit, 1)
        time.sleep(hold_s)
        self._bit(off, bit, 0)

    def longpress(self, name: str, hold_s: float = 1.5) -> None:
        self.press(name, hold_s=hold_s)

    def combo(self, *names: str, hold_s: float = 0.3) -> None:
        """Press several keys together: set one bit per command, 30 ms apart (the firmware
        only picks up one newly-set bit per byte per scan), hold, then release in reverse."""
        keys = [resolve_key(n) for n in names]
        for off, bit in keys:
            self._bit(off, bit, 1)
            time.sleep(0.03)
        time.sleep(hold_s)
        for off, bit in reversed(keys):
            self._bit(off, bit, 0)

    def dial(self, n: int) -> None:
        self._chunked(self._add16, 0x18, n, 100, 0.02)

    def multi(self, n: int) -> None:
        self._chunked(self._add8, 0x1D, n, 10, 0.02)

    def pbt_inner(self, n: int) -> None:
        self._chunked(self._add8, 0x1B, n, 10, 0.02)

    def pbt_outer(self, n: int) -> None:
        self._chunked(self._add8, 0x1C, n, 10, 0.02)

    def af(self, v: int) -> None:
        self._w(0x1E, bytes([v & 0xFF]))

    def rfsql(self, v: int) -> None:
        self._w(0x1F, bytes([v & 0xFF]))

    def touch(self, x: int, y: int, hold_s: float = 0.15) -> None:
        self.raw(f"touch {x} {y}")
        time.sleep(hold_s)
        self.raw("release")

    def drag(self, x0: int, y0: int, x1: int, y1: int, steps: int = 10, dur_s: float = 0.3) -> None:
        dt = dur_s / steps
        for i in range(steps + 1):
            t = i / steps
            self.raw(f"touch {round(x0 + (x1 - x0) * t)} {round(y0 + (y1 - y0) * t)}")
            if i < steps:
                time.sleep(dt)
        self.raw("release")

    def state(self) -> dict:
        d = self.get()
        version = d[0x01:0x0D].decode("ascii", "replace").rstrip("\x00").strip()
        pressed = sorted(name for name, (off, bit) in KEYS.items() if d[off] & (1 << bit))
        touching = d[0x13] == 0x00
        return {
            "version": version,
            "keys": pressed,
            "touch": {
                "down": touching,
                "x": int.from_bytes(d[0x14:0x16], "big"),
                "y": int.from_bytes(d[0x16:0x18], "big"),
            },
            "dial": int.from_bytes(d[0x18:0x1A], "big"),
            "dial_fast": bool(d[0x1A]),
            "pbt_inner": d[0x1B],
            "pbt_outer": d[0x1C],
            "multi": d[0x1D],
            "af": d[0x1E],
            "rfsql": d[0x1F],
        }


# --- seq mini-language (used by the `seq` subcommand) -----------------------------------------


def run_step(fp: FrontPanel, cmd: str, args: list[str]) -> None:
    if cmd == "press":
        fp.press(args[0])
    elif cmd in ("long", "longpress"):
        fp.longpress(args[0])
    elif cmd == "combo":
        fp.combo(*args)
    elif cmd == "dial":
        fp.dial(int(args[0]))
    elif cmd == "multi":
        fp.multi(int(args[0]))
    elif cmd in ("pbt-in", "pbtin"):
        fp.pbt_inner(int(args[0]))
    elif cmd in ("pbt-out", "pbtout"):
        fp.pbt_outer(int(args[0]))
    elif cmd == "af":
        fp.af(int(args[0]))
    elif cmd == "rfsql":
        fp.rfsql(int(args[0]))
    elif cmd == "touch":
        fp.touch(int(args[0]), int(args[1]))
    elif cmd == "drag":
        fp.drag(*(int(a) for a in args[:4]))
    elif cmd == "state":
        print(json.dumps(fp.state(), indent=2))
    elif cmd == "raw":
        print(fp.raw(" ".join(args)))
    elif cmd == "wait":
        time.sleep(float(args[0]))
    else:
        raise ValueError(f"seq: unknown step {cmd!r}")


def run_seq(fp: FrontPanel, script: str) -> None:
    for step in script.split(";"):
        step = step.strip()
        if not step:
            continue
        parts = shlex.split(step)
        run_step(fp, parts[0], parts[1:])


# --- CLI ---------------------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=os.environ.get("RZA1H_FPCTL"),
                     help="control-socket path (default: env RZA1H_FPCTL)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("press"); p.add_argument("name"); p.add_argument("--hold", type=float, default=0.15)
    p = sub.add_parser("long"); p.add_argument("name"); p.add_argument("--hold", type=float, default=1.5)
    p = sub.add_parser("combo"); p.add_argument("names", nargs="+"); p.add_argument("--hold", type=float, default=0.3)
    p = sub.add_parser("dial"); p.add_argument("n", type=int)
    p = sub.add_parser("multi"); p.add_argument("n", type=int)
    p = sub.add_parser("pbt-in"); p.add_argument("n", type=int)
    p = sub.add_parser("pbt-out"); p.add_argument("n", type=int)
    p = sub.add_parser("af"); p.add_argument("v", type=int)
    p = sub.add_parser("rfsql"); p.add_argument("v", type=int)
    p = sub.add_parser("touch"); p.add_argument("x", type=int); p.add_argument("y", type=int)
    p.add_argument("--hold", type=float, default=0.15)
    p = sub.add_parser("drag")
    for a in ("x0", "y0", "x1", "y1"):
        p.add_argument(a, type=int)
    p.add_argument("--steps", type=int, default=10)
    p.add_argument("--dur", type=float, default=0.3)
    sub.add_parser("state")
    sub.add_parser("keys")
    p = sub.add_parser("raw"); p.add_argument("line")
    p = sub.add_parser("seq"); p.add_argument("script")

    args = ap.parse_args()

    if args.cmd == "keys":
        for name, (off, bit) in sorted(KEYS.items()):
            print(f"{name:14s} {off:02x}.{bit}")
        return

    if not args.sock:
        ap.error("--sock is required (or set RZA1H_FPCTL)")
    fp = FrontPanel(args.sock)

    if args.cmd == "press":
        fp.press(args.name, hold_s=args.hold)
    elif args.cmd == "long":
        fp.longpress(args.name, hold_s=args.hold)
    elif args.cmd == "combo":
        fp.combo(*args.names, hold_s=args.hold)
    elif args.cmd == "dial":
        fp.dial(args.n)
    elif args.cmd == "multi":
        fp.multi(args.n)
    elif args.cmd == "pbt-in":
        fp.pbt_inner(args.n)
    elif args.cmd == "pbt-out":
        fp.pbt_outer(args.n)
    elif args.cmd == "af":
        fp.af(args.v)
    elif args.cmd == "rfsql":
        fp.rfsql(args.v)
    elif args.cmd == "touch":
        fp.touch(args.x, args.y, hold_s=args.hold)
    elif args.cmd == "drag":
        fp.drag(args.x0, args.y0, args.x1, args.y1, steps=args.steps, dur_s=args.dur)
    elif args.cmd == "state":
        print(json.dumps(fp.state(), indent=2))
    elif args.cmd == "raw":
        print(fp.raw(args.line))
    elif args.cmd == "seq":
        run_seq(fp, args.script)


if __name__ == "__main__":
    try:
        main()
    except (KeyError, RuntimeError, ConnectionError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
