#!/usr/bin/env python3
"""Minimal CI-V client for the emulated IC-7300 (SCIF0 on a UNIX-socket chardev).

Built 2026-09-24. Launch QEMU with
    -chardev socket,id=civ,path=/some/civ.sock,server=on,wait=off -serial chardev:civ
(the first -serial is serial_hd(0) = SCIF0 = CI-V). The radio's default CI-V address is 0x94;
we act as controller 0xE0.

Library use: Civ(path).cmd(0x03) -> list of reply frames (bytes, without FE FE/FD).
CLI: civ.py SOCK HEX... e.g. `civ.py /tmp/civ.sock 03` (read frequency) or `27 10 01`.
"""

from __future__ import annotations

import socket
import sys
import time

RADIO, CTRL = 0x94, 0xE0


class Civ:
    def __init__(self, path: str, radio: int = RADIO):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.s.settimeout(0.05)
        self.radio = radio
        self.buf = b""

    def send(self, *payload: int) -> None:
        self.s.sendall(bytes([0xFE, 0xFE, self.radio, CTRL, *payload, 0xFD]))

    def frames(self, timeout: float = 1.0) -> list[bytes]:
        """Collect complete frames addressed to us (echoes of our own frames are dropped)."""
        out, t0 = [], time.time()
        while time.time() - t0 < timeout:
            try:
                chunk = self.s.recv(4096)
                if chunk:
                    self.buf += chunk
            except socket.timeout:
                pass
            while b"\xfd" in self.buf:
                raw, self.buf = self.buf.split(b"\xfd", 1)
                i = raw.rfind(b"\xfe\xfe")
                if i < 0:
                    continue
                f = raw[i + 2:]
                if len(f) >= 2 and f[0] == CTRL:
                    out.append(f)
            if out:
                # a reply arrived; drain briefly for multi-frame answers
                timeout = min(timeout, time.time() - t0 + 0.2)
        return out

    def cmd(self, *payload: int, timeout: float = 1.0) -> list[bytes]:
        self.send(*payload)
        return self.frames(timeout)


def main():
    c = Civ(sys.argv[1])
    for f in c.cmd(*[int(x, 16) for x in sys.argv[2:]]):
        print(f.hex(" "))


if __name__ == "__main__":
    main()
