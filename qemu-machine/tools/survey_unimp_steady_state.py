#!/usr/bin/env python3
"""Full `-d unimp,guest_errors` survey over a long PWRK-hold boot, run well past the point
main_idle_loop/the render dispatch is reached (t~24s), to directly check the user's hypothesis:
some OTHER task (the one that would eventually post a real message to the render-request ITRON
mailbox, or something else downstream) might itself be stalled waiting on a peripheral this
machine still doesn't model at all -- exactly the pattern that VDC50/LVDS, MTU2 TGI4D, and the
RX-8803LC RTC each turned out to be, earlier in this project's history. Same established
methodology as README.md's 2026-09-20 "-d unimp survey" section, just re-run now that OpenVG,
VDC50, and the RTC are all fixed, to see what's left.

Usage: survey_unimp_steady_state.py [seconds]
"""

from __future__ import annotations

import collections
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom_pwrk_test.img"

LINE_RE = re.compile(r"^([\w.-]+): (?:unimplemented device|Unimplemented device) (\S+)"
                      r".*?(?:offset|addr) (0x[0-9a-fA-F]+)", re.IGNORECASE)


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0

    log_path = Path("/tmp/qemu_unimp_survey.log")
    log_path.unlink(missing_ok=True)

    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-d", "unimp,guest_errors", "-D", str(log_path),
    ]
    # PWRK-hold requires QMP; but for this pure log survey we don't need to press it
    # precisely -- reuse the GPIO device's default (auto power-on branch already known to
    # converge on the same downstream code per the 2026-09-20 byte-for-byte tail match).
    # Simpler: launch with QMP anyway, so we CAN press PWRK, matching every other tool here.
    import json
    import socket

    sock_path = "/tmp/qemu_unimp_survey.sock"
    Path(sock_path).unlink(missing_ok=True)
    qemu_args += ["-qmp", f"unix:{sock_path},server,nowait"]
    GPIO_PATH = "/machine/unattached/device[14]"

    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(sock_path)
        s.recv(65536)
        s.send(b'{"execute":"qmp_capabilities"}')
        s.recv(65536)

        def hmp(cmd):
            s.send(json.dumps({"execute": "human-monitor-command",
                                "arguments": {"command-line": cmd}}).encode())
            return json.loads(s.recv(1 << 20).decode())["return"]

        def qmp_cmd(execute, **arguments):
            s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
            return json.loads(s.recv(65536).decode())

        def pc():
            for line in hmp("info registers").splitlines():
                for tok in line.split():
                    if tok.startswith("R15="):
                        return int(tok[4:], 16)
            raise RuntimeError("no R15")

        t0 = time.time()
        while time.time() - t0 < 15:
            time.sleep(0.1)
            if pc() == 0x20029B18:
                break
        qmp_cmd("qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print(f"PWRK pressed and held; running for {total_seconds:.0f}s "
              f"(-d unimp,guest_errors, logging to {log_path})...")

        t0 = time.time()
        last_count = 0
        while time.time() - t0 < total_seconds:
            time.sleep(10)
            elapsed = time.time() - t0
            text = log_path.read_text(errors="replace")
            n = len(text.splitlines())
            growing = " (still growing)" if n > last_count else " (flat)"
            print(f"t={elapsed:6.1f}s  unimp/guest_error lines={n}{growing}")
            last_count = n
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        Path(sock_path).unlink(missing_ok=True)

    text = log_path.read_text(errors="replace")
    lines = text.splitlines()
    print(f"\nFINAL: {len(lines)} total unimp/guest_error lines. Full log: {log_path}")

    region_counter = collections.Counter()
    for line in lines:
        m = re.match(r"^([\w.-]+):", line)
        if m:
            region_counter[m.group(1)] += 1
    print("\nBy region:")
    for region, count in region_counter.most_common(30):
        print(f"  {region}: {count}")

    print("\nLast 20 lines (whatever's most recent, i.e. steady-state, if any):")
    for line in lines[-20:]:
        print("  " + line)


if __name__ == "__main__":
    main()
