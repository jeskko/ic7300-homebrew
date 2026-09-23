#!/usr/bin/env python3
"""Long-duration (default 280s) PWRK-hold capture with RZA1H_DEBUG=riic, to directly check
whether RIIC1 (real-time-clock) traffic EVER starts -- the ground-truth signal that
main_idle_loop has actually been reached (it calls a real RTC read over RIIC1 every iteration).

This is the decisive test for the icom-main-idle-loop-not-reached thread's current working
hypothesis: that the DSP identity-query protocol's own ~6-command chain (each bounded to an
~18-try, ~30s retry budget against qemu-machine's own always-wrong canned DSP ack) accounts for
up to ~3 minutes of legitimate boot delay, not a real deadlock -- and that no prior capture in
this project's history ever ran long enough (60-90s) to see past it. The earlier flag-polling
capture (trace_main_idle_wait_flags.py, 300s, 3s poll interval) was inconclusive: flag_ed
apparently toggles faster than a 3s poll can reliably catch, so it just kept reading "1" the
whole time regardless of whether real progress was happening underneath. This script instead
watches the actual RIIC1 log output directly, which is unambiguous.

Usage: trace_riic1_long_capture.py [seconds]
"""

from __future__ import annotations

import json
import os
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


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 280.0

    sock_path = "/tmp/qemu_riic1_long.sock"
    log_path = Path("/tmp/qemu_riic1_long.log")
    Path(sock_path).unlink(missing_ok=True)
    log_path.unlink(missing_ok=True)

    env = os.environ.copy()
    env["RZA1H_DEBUG"] = "riic"

    log_f = open(log_path, "wb")
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
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
              f"(RZA1H_DEBUG=riic, logging to {log_path})...")

        t0 = time.time()
        last_report = 0
        while time.time() - t0 < total_seconds:
            time.sleep(10)
            elapsed = time.time() - t0
            log_f.flush()
            text = log_path.read_text(errors="replace")
            riic1_lines = sum(1 for line in text.splitlines() if "riic1" in line.lower())
            riic2_lines = sum(1 for line in text.splitlines() if "riic2" in line.lower())
            print(f"t={elapsed:6.1f}s  riic1_lines={riic1_lines}  riic2_lines={riic2_lines}")
            if riic1_lines > 0 and last_report == 0:
                print("*** RIIC1 traffic detected -- main_idle_loop reached! ***")
                # capture a few sample lines
                for line in text.splitlines():
                    if "riic1" in line.lower():
                        print("  " + line)
                        last_report += 1
                        if last_report >= 5:
                            break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log_f.close()
        text = log_path.read_text(errors="replace")
        riic1_total = sum(1 for line in text.splitlines() if "riic1" in line.lower())
        riic2_total = sum(1 for line in text.splitlines() if "riic2" in line.lower())
        print(f"\nFINAL: riic1_lines={riic1_total}  riic2_lines={riic2_total}  "
              f"(full log at {log_path})")


if __name__ == "__main__":
    main()
