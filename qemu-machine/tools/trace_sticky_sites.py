#!/usr/bin/env python3
"""Sticky-breakpoint hit logger: like walk_call_sites.py, but every breakpoint stays armed for
the whole run and EVERY hit is logged (remove / single-step / re-arm / continue, per the
icom-gdb-perturbation-resolved gotcha). Built 2026-09-23 to find which of the OpenVG driver's
wait-event-flag call sites the render task is parked in: give each call site and its return
address (call+4) in SITEFILE; the last "call" line with no matching "ret" afterwards is the
stuck wait. walk_call_sites.py can't answer that -- it drops each breakpoint after its first
hit, so a site hit many times and then finally parked on looks identical to one that returned.

SITEFILE format is the same as walk_call_sites.py (hex address, optional '# label').
Also dumps r0-r3 on each hit (the wait's flag pointer / pattern / mode for FUN_20153ec2).

Usage: trace_sticky_sites.py SITEFILE [--stall-timeout S] [--max-hits N] [--image IMG]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp  # noqa: E402
from walk_call_sites import GPIO_PATH, qmp_open, qmp_cmd, pc_qmp  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sitefile")
    ap.add_argument("--stall-timeout", type=float, default=60.0)
    ap.add_argument("--max-hits", type=int, default=5000)
    ap.add_argument("--image", help="override the RIIC2 EEPROM image")
    args = ap.parse_args()

    labels = {}
    for line in Path(args.sitefile).read_text().splitlines():
        a, _, lab = line.partition("#")
        if a.strip():
            labels[int(a.strip(), 16)] = lab.strip()

    image = args.image or HERE / "riic2_eeprom_pwrk_test.img"
    sock = "/tmp/qemu_sticky_sites.sock"
    Path(sock).unlink(missing_ok=True)
    proc = subprocess.Popen(
        [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
         "-serial", "none", "-monitor", "none",
         "-global", f"rza1h-riic.image={image}", "-icount", "shift=1",
         "-qmp", f"unix:{sock},server,nowait", "-gdb", "tcp::1234"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock)
        t0 = time.time()
        while time.time() - t0 < 15:
            time.sleep(0.1)
            if pc_qmp(s) == 0x20029B18:
                break
        qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        g = GdbRsp(port=1234)
        g.handshake()
        try:
            g.interrupt()
            g.wait_stop(timeout=5)
        except Exception:
            pass
        for a in labels:
            g.set_breakpoint(a, kind=4)
        print(f"{len(labels)} sticky breakpoints armed.", flush=True)
        t0 = time.time()
        hits = 0
        while hits < args.max_hits:
            g.cont()
            try:
                g.wait_stop(timeout=args.stall_timeout)
            except Exception:
                print(f"*** STALLED: no hit in {args.stall_timeout:.0f}s ***")
                break
            r = g.read_registers()
            pc = r["r15"]
            hits += 1
            print(f"t={time.time() - t0:7.2f} 0x{pc:08x} {labels.get(pc, '?'):<16} "
                  f"r0={r['r0']:08x} r1={r['r1']:08x} r2={r['r2']:08x} r3={r['r3']:08x}",
                  flush=True)
            if pc in labels:
                g.remove_breakpoint(pc, kind=4)
                g.step()
                g.set_breakpoint(pc, kind=4)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        Path(sock).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
