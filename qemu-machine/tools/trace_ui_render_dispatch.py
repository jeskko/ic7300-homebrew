#!/usr/bin/env python3
"""Single-shot, zero-perturbation-if-never-hit check: does `ui_graphics_lifecycle_task`'s render
dispatch (`ui_graphics_buffers_init` 0x2007ef08 / `ui_graphics_present_frame` 0x2007ee68) ever
actually run during a long PWRK-hold boot? Follow-up to trace_openvg_command_traffic.py, which
found the OpenVG command FIFO goes completely silent after the ~10s bring-up burst -- this settles
whether that's because the render dispatch itself never fires (the render-request ITRON message
buffer created at FUN_2007ed9c, descriptor 0x20328e0c, never gets a message posted to it), or
because it fires but produces no FIFO traffic (e.g. swapping an already-blank buffer).

Same technique as walk_call_sites.py: QMP-only PC poll to time the PWRK press, then attach
gdbstub, arm both breakpoints, `cont()`, and wait up to --stall-timeout seconds for either to hit.

Usage: trace_ui_render_dispatch.py [--stall-timeout S]
"""

from __future__ import annotations

import argparse
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

SITES = {
    0x2007ef08: "ui_graphics_buffers_init",
    0x2007ee68: "ui_graphics_present_frame",
}


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


def pc_qmp(s):
    for line in hmp(s, "info registers").splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("no R15")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stall-timeout", type=float, default=160.0)
    args = ap.parse_args()

    sock_path = "/tmp/qemu_ui_render_dispatch.sock"
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
        for a in SITES:
            g.set_breakpoint(a, kind=4)
        print(f"{len(SITES)} breakpoints armed ({', '.join(SITES.values())})."
              f" Running for up to {args.stall_timeout:.0f}s...")

        t_run0 = time.time()
        pending = dict(SITES)
        while pending and time.time() - t_run0 < args.stall_timeout:
            g.cont()
            try:
                g.wait_stop(timeout=args.stall_timeout - (time.time() - t_run0))
            except Exception:
                break
            cur = g.read_registers()["r15"]
            elapsed = time.time() - t_run0
            if cur in pending:
                name = pending.pop(cur)
                g.remove_breakpoint(cur, kind=4)
                print(f"*** HIT at t={elapsed:.1f}s: 0x{cur:08x} ({name}) ***")
            else:
                print(f"(unexpected stop at 0x{cur:08x}, t={elapsed:.1f}s)")
        if pending:
            print(f"\nNever hit within {args.stall_timeout:.0f}s: {list(pending.values())}")
        else:
            print("\nBoth sites hit.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
