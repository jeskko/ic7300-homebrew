#!/usr/bin/env python3
"""Checks live whether the spectrum-scope FFT task's real-time sample-ring buffer ever actually
receives new data (user's own question: "do some tasks expect answers from peripherals our
emulation can't provide"). Static trace (this session): spectrum_scope_fft_task's producer chain
(FUN_20008868 -> FUN_2005fb64) is a classic ring-buffer consumer at a fixed RAM struct (found via
DAT_20060700's own stored pointer, resolved this session to 0x203fbdc0): write_idx at +0x240,
read_idx at +0x241, 8 slots of 0x48 bytes each. If write_idx == read_idx, FUN_2005fb64 returns an
all-zero sample block instead of real audio/IQ data. notes/ic7300-signal-chain.md already ties a
"0x20060700 setup table" to SSICR_0/SSICR_1 (SSIF0/1) and DMAC-channel-shaped register addresses
-- i.e. this ring is meant to be filled by a real DMA-driven audio/IQ stream from the DSP, which
this project's dmac.c only models channel 0 for (every other channel is plain storage). If SSIF's
own DMA channel isn't channel 0, write_idx would never move and the FFT/spectrum-scope would
process silence forever, no matter how long boot runs.

QMP-only, zero perturbation. Usage: trace_spectrum_scope_ring.py [seconds]
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom_pwrk_test.img"
GPIO_PATH = "/machine/gpio"

RING_BASE = 0x203FBDC0
WRITE_IDX_ADDR = RING_BASE + 0x240
READ_IDX_ADDR = RING_BASE + 0x241


def qmp_open(sock_path: str) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.recv(65536)
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    return s


def hmp(s, cmd):
    s.send(json.dumps({"execute": "human-monitor-command",
                       "arguments": {"command-line": cmd}}).encode())
    return json.loads(s.recv(1 << 20).decode())["return"]


def qmp_cmd(s, execute, **arguments):
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
    return json.loads(s.recv(65536).decode())


def read_byte(s, addr):
    reply = hmp(s, f"xp /1xb 0x{addr:x}")
    return int(reply.strip().split()[1], 16)


def pc(s):
    for line in hmp(s, "info registers").splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("no R15")


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0

    sock_path = "/tmp/qemu_scope_ring.sock"
    Path(sock_path).unlink(missing_ok=True)
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)

        t0 = time.time()
        while time.time() - t0 < 15:
            time.sleep(0.1)
            if pc(s) == 0x20029B18:
                break
        qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print(f"PWRK pressed and held; polling write_idx/read_idx at 0x{WRITE_IDX_ADDR:x}/"
              f"0x{READ_IDX_ADDR:x} for {total_seconds:.0f}s...")

        t0 = time.time()
        seen = set()
        while time.time() - t0 < total_seconds:
            time.sleep(1.0)
            elapsed = time.time() - t0
            w = read_byte(s, WRITE_IDX_ADDR)
            r = read_byte(s, READ_IDX_ADDR)
            seen.add(w)
            print(f"t={elapsed:6.1f}s  write_idx={w}  read_idx={r}"
                  f"  {'(EQUAL -- consumer would see all-zero sample)' if w == r else '(differ)'}")
        print(f"\ndistinct write_idx values observed: {sorted(seen)}"
              f" -- {'NEVER moved, ring genuinely idle' if len(seen) <= 1 else 'DID move, real data flowing'}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
