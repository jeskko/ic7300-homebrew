#!/usr/bin/env python3
"""Run the emulated IC-7300 with a live LCD window (vdc5.c's graphic console).

Boots flash.bin with the PWRK-hold EEPROM image, presses and holds PWRK at the
right moment over QMP (the same dance every trace tool does), and leaves QEMU
running with `-display gtk` (or --display sdl/none) until you close it.
--screendump FILE [--after S] instead captures one PPM of the console after S
seconds and exits (a headless check that the console path works).

Usage: run_gui.py [--display gtk|sdl|none] [--screendump out.ppm --after 170]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from screenshot import qmp_cmd, pc  # noqa: E402
from vdc5_framebuffer_peek import GPIO_PATH, qmp_open  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE, DEFAULT_ICOUNT  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--display", default="gtk")
    ap.add_argument("--screendump")
    ap.add_argument("--after", type=float, default=170.0)
    args = ap.parse_args()

    sock = "/tmp/qemu_run_gui.sock"
    Path(sock).unlink(missing_ok=True)
    proc = subprocess.Popen(
        [str(QEMU), "-M", "rz-a1h", "-display", args.display, "-kernel", str(FLASH),
         "-serial", "none", "-monitor", "none",
         "-global", f"rza1h-riic.image={HERE / 'riic2_eeprom_pwrk_test.img'}",
         "-icount", DEFAULT_ICOUNT, "-qmp", f"unix:{sock},server,nowait"],
        stdin=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock)
        t0 = time.time()
        while time.time() - t0 < 15 and pc(s) != 0x20029B18:
            time.sleep(0.1)
        qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print("PWRK pressed and held -- the splash appears after ~40 s, the main screen "
              "after ~2 min (emulated time runs slower than real time).")
        if args.screendump:
            time.sleep(args.after)
            print(qmp_cmd(s, "screendump", filename=str(Path(args.screendump).resolve())))
            return
        proc.wait()
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait()


if __name__ == "__main__":
    main()
