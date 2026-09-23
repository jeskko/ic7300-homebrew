#!/usr/bin/env python3
"""Why is the power-on splash never shown? Single breakpoint on the gate in
`system_mode_request_dispatch` (0x2002abe0, right before the conditional `bl FUN_2002a2a4`,
the boot fade-in/hold/fade-out driver that calls `opening_screen_build_frame` 7x) and dump
every input the condition reads:

    if ((r10[0x6f] != 0 || (mode not in {0,4,5} && req != 10) || r7[0] || r7[1]
         || (r6[1] & 0xc)) && *DAT_2002a09c == 0)
        FUN_2002a2a4();

mode = *DAT_2002a4a4, req = *DAT_2002a158 (both literal-pool pointers read from flash here).

Usage: probe_splash_gate.py [--timeout S] [--no-pwrk]
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp  # noqa: E402
from walk_call_sites import GPIO_PATH, qmp_open, qmp_cmd, pc_qmp  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE  # noqa: E402

GATE = 0x2002abe0
LIT = {"a09c (skip-splash flag)": 0x2002a09c, "a4a4 (idle-loop mode)": 0x2002a4a4,
       "a158 (mode request)": 0x2002a158}


def main():
    import subprocess
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--no-pwrk", action="store_true")
    ap.add_argument("--image", help="override the RIIC2 EEPROM image")
    args = ap.parse_args()

    image = args.image or HERE / ("riic2_eeprom.img" if args.no_pwrk else "riic2_eeprom_pwrk_test.img")
    sock = "/tmp/qemu_splash_gate.sock"
    Path(sock).unlink(missing_ok=True)
    proc = subprocess.Popen(
        [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
         "-serial", "none", "-monitor", "none",
         "-global", f"rza1h-riic.image={image}", "-icount", "shift=auto",
         "-qmp", f"unix:{sock},server,nowait", "-gdb", "tcp::1234"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock)
        if not args.no_pwrk:
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
        g.set_breakpoint(GATE, kind=4)
        t0 = time.time()
        g.cont()
        try:
            g.wait_stop(timeout=args.timeout)
        except Exception:
            print(f"gate 0x{GATE:08x} never reached in {args.timeout:.0f}s")
            return
        r = g.read_registers()
        print(f"gate hit at t={time.time() - t0:.1f}s pc=0x{r['r15']:08x}")
        for k in ("r6", "r7", "r10"):
            print(f"  {k}=0x{r[k]:08x}")
        print(f"  r10[0x6f] = 0x{g.read_memory(r['r10'] + 0x6f, 1)[0]:02x}")
        print(f"  r7[0..1]  = {g.read_memory(r['r7'], 2).hex()}")
        print(f"  r6[1]     = 0x{g.read_memory(r['r6'] + 1, 1)[0]:02x}  (&0xc matters)")
        for name, lit in LIT.items():
            ptr = struct.unpack("<I", g.read_memory(lit, 4))[0]
            print(f"  *{name} @0x{ptr:08x} = 0x{g.read_memory(ptr, 1)[0]:02x}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        Path(sock).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
