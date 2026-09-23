#!/usr/bin/env python3
"""Live-confirms (or refutes) the static-traced mechanism in README.md's Status section: does
`dsp_cmd_table_init`'s own SCIF5-TX-ring drain (paced by the same ~82ms MTU2 tick that produces
the outer ring's doorbell) actually overlap with the outer ring's climb from 0 to overflow?

Polls, every `poll_interval` seconds, fully GDB-free (QMP `xp` only):
  - the outer job-ring header (0x20420120): write/read/pending/capacity
  - the SCIF5 TX ring's own state: active flag (0x203906ed, set while draining a queued burst),
    write/read indices (0x20415310/0x20415311, base 0x20414da0 + 0x570/0x571)

Usage: trace_dsp_param_burst.py [seconds] [poll_interval_s]
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
RIIC_IMAGE = HERE / "riic2_eeprom.img"

RING_BASE = 0x20420120
SCIF5_ACTIVE_FLAG = 0x203906ED
SCIF5_RING_WRITE_IDX = 0x20415310  # 0x20414da0 + 0x570
SCIF5_RING_READ_IDX = 0x20415311   # 0x20414da0 + 0x571


def qmp_open(sock_path: str):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.recv(65536)
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    return s


def xp(s: socket.socket, addr: int, n: int) -> list[int]:
    s.send(json.dumps({"execute": "human-monitor-command",
                        "arguments": {"command-line": f"xp /{n}xb 0x{addr:x}"}}).encode())
    reply = json.loads(s.recv(65536).decode())["return"]
    return [int(x, 16) for x in reply.strip().split()[1:]]


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.15

    sock_path = "/tmp/qemu_dspburst.sock"
    Path(sock_path).unlink(missing_ok=True)
    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)
        start = time.time()
        while time.time() - start < seconds:
            time.sleep(poll_interval)
            elapsed = time.time() - start
            try:
                ring = xp(s, RING_BASE, 4)
                active = xp(s, SCIF5_ACTIVE_FLAG, 1)[0]
                s5w = xp(s, SCIF5_RING_WRITE_IDX, 1)[0]
                s5r = xp(s, SCIF5_RING_READ_IDX, 1)[0]
            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                print(f"t={elapsed:6.2f}s  QMP read failed, retrying")
                continue
            outer = f"write={ring[0]:3d} read={ring[1]:3d} pending={ring[2]:3d}"
            scif5 = f"active={active} write={s5w:3d} read={s5r:3d} depth={((s5w - s5r) % 256):3d}"
            print(f"t={elapsed:6.2f}s  outer[{outer}]  scif5[{scif5}]")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
