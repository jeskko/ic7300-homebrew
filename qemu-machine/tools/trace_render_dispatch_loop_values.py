#!/usr/bin/env python3
"""Breakpoints the compare instruction inside ui_graphics_lifecycle_task's own dispatch loop
(0x2007f020, right after `ldr r0,[r5,#0]` loads the dispatch flag at 0x2039064c into r0, before
`cmp r0,#1`) and logs r0 on every hit -- more direct than a write watchpoint on the flag address
itself (which, run this same session, never caught a write of 1 or 2 despite the dispatch
demonstrably firing both branches -- see trace_render_state_flag_writes.py's own puzzling result).
This reads the value the loop ACTUALLY branches on, sidestepping whatever's wrong with the
watchpoint approach.

Usage: trace_render_dispatch_loop_values.py [n_hits]
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp  # noqa: E402

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom_pwrk_test.img"
GPIO_PATH = "/machine/gpio"
CMP_ADDR = 0x2007F020


def qmp_open(sock_path):
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


def pc_qmp(s):
    for line in hmp(s, "info registers").splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("no R15")


def main():
    n_hits = int(sys.argv[1]) if len(sys.argv) > 1 else 30

    sock_path = "/tmp/qemu_render_loop_values.sock"
    Path(sock_path).unlink(missing_ok=True)
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
        "-gdb", "tcp::1234",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)
        t0 = time.time()
        while time.time() - t0 < 15:
            time.sleep(0.1)
            if pc_qmp(s) == 0x20029B18:
                break
        qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print("PWRK pressed and held; attaching gdbstub...")

        g = GdbRsp(port=1234)
        g.handshake()
        try:
            g.interrupt()
            g.wait_stop(timeout=5)
        except Exception:
            pass
        g.set_breakpoint(CMP_ADDR, kind=4)
        print(f"Breakpoint armed at 0x{CMP_ADDR:x}. Running for up to {n_hits} hits"
              f" (60s stall timeout)...")

        t_run0 = time.time()
        hits = 0
        while hits < n_hits:
            g.cont()
            try:
                g.wait_stop(timeout=60)
            except Exception:
                print("\n*** STALLED: no further hit within 60s ***")
                break
            regs = g.read_registers()
            elapsed = time.time() - t_run0
            hits += 1
            print(f"  hit #{hits:2d}  t={elapsed:6.1f}s  r0(flag value)={regs['r0']:#x}"
                  f"  LR={regs['r14']:#010x}")
        try:
            g.remove_breakpoint(CMP_ADDR, kind=4)
        except Exception as e:
            print(f"(remove_breakpoint failed, ignoring: {e})")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
