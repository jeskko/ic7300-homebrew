#!/usr/bin/env python3
"""Answers "do we see any activity on the CPU<->FPGA differential-I/O pins right now" (user's
own schematic-derived question, 2026-09-21): FPDX/FPSX/FPSR (P8_11/14/15, FPGA-only, no DSP pin)
and SCPCK/SCPSS/SCPX/SCPR (P8_3/4/5/6, RSPI channel 2's alternate function, real confirmed driver
-- see notes/ic7300-signal-chain.md's "RSPI2 confirmed as a real, actively-used SPI link to the
FPGA"). RSPI2 is modeled (rspi2.c, RZA1H_DEBUG=rspi2, raw TX-byte logging only, no command
decoding). FPDX itself is SCIF5's 3rd pin dynamically rerouted there when scif5_arm_retry_timer's
hidden param_1 is nonzero (notes/multi-cpu-images-history.md) -- never observed taken in any
static sample, so this also captures RZA1H_DEBUG=scif (all channels, includes scif5's DSP-link
responder) to see the volume/shape of the traffic that pin-mux switch would apply to, even though
this device model has no way to detect the mux switch itself (PMC8 bits aren't traced by any
debug channel today).

Usage: trace_fpga_link_activity.py [seconds]
"""

from __future__ import annotations

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


def pc(s):
    for line in hmp(s, "info registers").splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("no R15")


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 150.0

    sock_path = "/tmp/qemu_fpga_link.sock"
    log_path = Path("/tmp/qemu_fpga_link.log")
    Path(sock_path).unlink(missing_ok=True)
    log_path.unlink(missing_ok=True)

    env = os.environ.copy()
    env["RZA1H_DEBUG"] = "rspi2,scif"

    log_f = open(log_path, "wb")
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=auto",
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
              f"(RZA1H_DEBUG=rspi2,scif, logging to {log_path})...")

        t0 = time.time()
        while time.time() - t0 < total_seconds:
            time.sleep(10)
            elapsed = time.time() - t0
            log_f.flush()
            text = log_path.read_text(errors="replace")
            rspi2_n = sum(1 for l in text.splitlines() if "rza1h:rspi2" in l)
            scif5_n = sum(1 for l in text.splitlines() if "scif5:" in l)
            scif_other_n = sum(1 for l in text.splitlines()
                               if "rza1h:scif" in l and "scif5:" not in l)
            print(f"t={elapsed:6.1f}s  rspi2_lines={rspi2_n}  scif5_lines={scif5_n}"
                  f"  other_scif_lines={scif_other_n}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log_f.close()

    text = log_path.read_text(errors="replace")
    lines = text.splitlines()
    rspi2_lines = [l for l in lines if "rza1h:rspi2" in l]
    scif5_lines = [l for l in lines if "scif5:" in l]

    print(f"\nFINAL: {len(rspi2_lines)} rspi2 (FPGA/SCP*) lines, {len(scif5_lines)} scif5"
          f" (DSP-link, dynamically reroutable onto FPDX) lines. Full log: {log_path}")

    if rspi2_lines:
        print("\nFirst 5 rspi2 lines (each is one raw TX byte, no command decoding exists):")
        for l in rspi2_lines[:5]:
            print("  " + l)
        print("Last 5 rspi2 lines:")
        for l in rspi2_lines[-5:]:
            print("  " + l)
    else:
        print("\nNo rspi2 (FPGA-facing SCP*/RSPI2) activity at all in this window.")

    if scif5_lines:
        ack_lines = [l for l in scif5_lines if "canned class-0xf ack" in l]
        print(f"\n{len(ack_lines)} of {len(scif5_lines)} scif5 lines are the canned,"
              f" content-blind class-0xf ack (same reply regardless of what command was sent).")


if __name__ == "__main__":
    main()
