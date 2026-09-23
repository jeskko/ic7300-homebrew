#!/usr/bin/env python3
"""Finds who writes to ui_graphics_lifecycle_task's own render-dispatch flag (0x2039064c --
resolved this session from DAT_2007f1d8's own stored pointer value) with a live GDB write
watchpoint, since static address-literal cross-referencing found only ONE writer (ui_request_post,
0x200378cc, sets it to 1 once at graphics startup -- the param_1!=0 branch is dead code, does
nothing) and NO writer of state=2 anywhere in body.bin, despite ui_graphics_present_frame
empirically firing once (confirmed 2026-09-21, trace_ui_render_dispatch.py). The likely
explanation: whatever sets it to 2 reaches this address via a caller-supplied pointer parameter
(pointer arithmetic on an argument, not a literal constant this project's own xref-by-address
method can see) -- a live watchpoint sidesteps that entirely by catching the actual write, with LR
telling us the real caller regardless of how the address was computed.

Usage: trace_render_state_flag_writes.py [n_hits]
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
GPIO_PATH = "/machine/unattached/device[14]"
FLAG_ADDR = 0x2039064C


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
    n_hits = int(sys.argv[1]) if len(sys.argv) > 1 else 20

    sock_path = "/tmp/qemu_render_flag_watch.sock"
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
        g.set_watchpoint(FLAG_ADDR, length=1, kind=2)
        print(f"Write watchpoint armed on 0x{FLAG_ADDR:x}. Running for up to {n_hits} hits"
              f" (120s stall timeout)...")

        t_run0 = time.time()
        hits = 0
        while hits < n_hits:
            g.cont()
            try:
                g.wait_stop(timeout=120)
            except Exception:
                print("\n*** STALLED: no further write within 120s ***")
                break
            regs = g.read_registers()
            elapsed = time.time() - t_run0
            val = g.read_memory(FLAG_ADDR, 1)[0]
            hits += 1
            print(f"  hit #{hits:2d}  t={elapsed:6.1f}s  PC={regs['r15']:#010x}"
                  f"  LR={regs['r14']:#010x}  new_value={val:#04x}")
        try:
            g.remove_watchpoint(FLAG_ADDR, length=1, kind=2)
        except Exception as e:
            print(f"(remove_watchpoint failed, ignoring: {e})")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
