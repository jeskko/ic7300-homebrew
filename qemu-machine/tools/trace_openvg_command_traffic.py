#!/usr/bin/env python3
"""Captures real OpenVG command-FIFO traffic (RZA1H_DEBUG=openvg) over a long PWRK-hold boot, to
scope what `ui_graphics_lifecycle_task`'s real present-frame loop actually asks the GPU to do --
step 3 of icom-openvg-rendering's suggested approach (memory: "scope down what's actually drawn
before committing to an approach"). openvg.c's own model just logs "cmd %08x (#N)" per FIFO write
with no decoding; this tool aggregates the raw command words themselves (frequency by value, and
by putative opcode-tag nibble) instead of decoding hardware semantics, since nothing in this
project has decoded the real command-word format yet (see FUN_2014f818's header-word encoding,
TAG(0xA)<<28 | (count-1)<<16 | opcode, found this session by decompiling it directly).

Usage: trace_openvg_command_traffic.py [seconds]
"""

from __future__ import annotations

import collections
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom_pwrk_test.img"
GPIO_PATH = "/machine/unattached/device[14]"

CMD_RE = re.compile(r"cmd ([0-9a-f]{8}) \(#(\d+)\)")


def qmp_open(sock_path: str) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.recv(65536)
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    return s


def hmp(s: socket.socket, cmd: str) -> str:
    s.send(json.dumps({"execute": "human-monitor-command",
                        "arguments": {"command-line": cmd}}).encode())
    return json.loads(s.recv(1 << 20).decode())["return"]


def qmp_cmd(s: socket.socket, execute: str, **arguments):
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
    return json.loads(s.recv(65536).decode())


def pc(s: socket.socket) -> int:
    text = hmp(s, "info registers")
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("R15 not found")


def analyze(log_path: Path, t0_wall: float):
    text = log_path.read_text(errors="replace")
    cmds = []
    for line in text.splitlines():
        m = CMD_RE.search(line)
        if m:
            cmds.append(int(m.group(1), 16))
    total = len(cmds)
    header_like = [c for c in cmds if (c >> 28) == 0xA]
    tag_counter = collections.Counter((c >> 28) for c in cmds)
    opcode_counter = collections.Counter((c & 0xFFFF) for c in header_like)
    return total, header_like, tag_counter, opcode_counter


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
    image = sys.argv[2] if len(sys.argv) > 2 else RIIC_IMAGE  # optional EEPROM image override

    sock_path = "/tmp/qemu_openvg_trace.sock"
    log_path = Path("/tmp/qemu_openvg_trace.log")
    Path(sock_path).unlink(missing_ok=True)
    log_path.unlink(missing_ok=True)

    env = os.environ.copy()
    env["RZA1H_DEBUG"] = os.environ.get("RZA1H_DEBUG", "openvg")

    log_f = open(log_path, "wb")
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={image}",
        "-icount", os.environ.get("RZA1H_ICOUNT", "shift=auto"),
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL, stdout=log_f, stderr=log_f,
                             env=env)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)

        t0 = time.time()
        while time.time() - t0 < 15:
            time.sleep(0.1)
            if pc(s) == 0x20029B18:
                break
        qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print(f"PWRK pressed and held; running for {total_seconds:.0f}s "
              f"(RZA1H_DEBUG=openvg, logging to {log_path})...")

        t0 = time.time()
        while time.time() - t0 < total_seconds:
            time.sleep(10)
            elapsed = time.time() - t0
            log_f.flush()
            total, header_like, tag_counter, opcode_counter = analyze(log_path, t0)
            print(f"t={elapsed:6.1f}s  total_cmd_words={total}  header_words={len(header_like)}"
                  f"  tag_nibbles={dict(tag_counter)}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log_f.close()
        total, header_like, tag_counter, opcode_counter = analyze(log_path, 0)
        print(f"\nFINAL: total_cmd_words={total}")
        print(f"  tag nibble (bits 31:28) distribution: {dict(tag_counter)}")
        print(f"  header-word (tag==0xA) opcode (bits 15:0) distribution, top 20:")
        for opcode, count in opcode_counter.most_common(20):
            print(f"    opcode=0x{opcode:04x}  count={count}")
        print(f"\nfull log at {log_path}")


if __name__ == "__main__":
    main()
