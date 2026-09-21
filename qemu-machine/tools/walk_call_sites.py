#!/usr/bin/env python3
"""Generic 'which call site is the last one reached' breakpoint walker.

Arms a GDB breakpoint on every address given, then repeatedly continues; each hit is
recorded, its breakpoint removed, and execution resumed.  When no further breakpoint is
hit within --stall-timeout, the last recorded hit tells you which call the boot is stuck
inside.  Built 2026-09-21 for the icom-main-idle-loop-not-reached thread, generalized from
trace_cold_boot_hw_init_walk.py.

Addresses come from a file (one hex address per line, '#' comments allowed) so the
disassembly-derived site list can be regenerated per function.

Usage: walk_call_sites.py SITEFILE [--stall-timeout S] [--no-pwrk]
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
GPIO_PATH = "/machine/unattached/device[14]"


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
    ap.add_argument("sitefile")
    ap.add_argument("--stall-timeout", type=float, default=40.0)
    ap.add_argument("--no-pwrk", action="store_true")
    args = ap.parse_args()

    sites = []
    labels = {}
    for line in Path(args.sitefile).read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("#", 1)
        a = parts[0].strip()
        if not a:
            continue
        addr = int(a, 16)
        sites.append(addr)
        labels[addr] = parts[1].strip() if len(parts) > 1 else ""

    image = HERE / ("riic2_eeprom.img" if args.no_pwrk else "riic2_eeprom_pwrk_test.img")
    sock_path = "/tmp/qemu_walk_sites.sock"
    Path(sock_path).unlink(missing_ok=True)

    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={image}",
        "-icount", "shift=auto",
        "-qmp", f"unix:{sock_path},server,nowait",
        "-gdb", "tcp::1234",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)
        if not args.no_pwrk:
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
        pending = set(sites)
        for a in sites:
            g.set_breakpoint(a, kind=4)
        print(f"{len(sites)} breakpoints armed. Walking...\n")

        last = None
        hits = []
        while pending:
            g.cont()
            try:
                g.wait_stop(timeout=args.stall_timeout)
            except Exception:
                print(f"\n*** STALLED: no further breakpoint in {args.stall_timeout:.0f}s ***")
                break
            cur = g.read_registers()["r15"]
            if cur in pending:
                pending.discard(cur)
                g.remove_breakpoint(cur, kind=4)
                hits.append(cur)
                last = cur
                print(f"  hit 0x{cur:08x}  {labels.get(cur, '')}")
            else:
                print(f"  (unexpected stop at 0x{cur:08x})")
        print("\n--- summary ---")
        if last is not None:
            print(f"last site reached: 0x{last:08x}  {labels.get(last, '')}")
        missed = [a for a in sites if a not in hits]
        print(f"never reached ({len(missed)}):")
        for a in missed[:10]:
            print(f"    0x{a:08x}  {labels.get(a,'')}")
        try:
            g.interrupt()
        except Exception:
            pass
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
