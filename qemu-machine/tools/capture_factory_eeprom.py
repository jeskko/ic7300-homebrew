#!/usr/bin/env python3
"""Capture the IC-7300 firmware's own factory-default EEPROM image (SET > Others > Reset > All).

Built 2026-09-24. Boots a copy of the current riic2_eeprom.img as a *persistent* EEPROM
(-drive + -global rza1h-riic.eeprom-drive, riic.c), waits for the main screen, then writes
request value 5 into the system-mode request byte 0x20390310 -- exactly what the All Reset
confirm dialog does -- so system_mode_request_dispatch runs all_reset_system_mode_action()
(factory_reset_apply_defaults(1), writeback flush, format signature) in its own task context.
When the request byte reads 0 again the reset is done; the backing file then holds the
firmware-written defaults. Needs riic.c's dummy-read and NACK/repeated-start fixes
(2026-09-24): before them only the first EEPROM write of the reset ever completed.

Usage: capture_factory_eeprom.py [OUT]   (default tools/eeprom_factory_defaults.bin)
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE  # noqa: E402

SYSTEM_MODE_REQUEST = 0x20390310
REQ_ALL_RESET = 5


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "eeprom_factory_defaults.bin"
    tmp = Path(tempfile.mkdtemp(prefix="eecap"))
    img = tmp / "ee.img"
    shutil.copy(HERE / "riic2_eeprom.img", img)
    proc = subprocess.Popen(
        [str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH), "-serial", "none",
         "-monitor", "none", "-drive", f"if=none,id=ee,format=raw,file={img}",
         "-global", "rza1h-riic.eeprom-drive=ee", "-icount", "shift=1,sleep=off",
         "-gdb", "tcp::1234"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    try:
        time.sleep(15)  # main screen is up after ~8 s at sleep=off
        g = GdbRsp()
        g.status()
        g.write_memory(SYSTEM_MODE_REQUEST, bytes([REQ_ALL_RESET]))
        g.cont()
        for _ in range(24):
            time.sleep(5)
            g.interrupt()
            g.wait_stop(5)
            done = g.read_memory(SYSTEM_MODE_REQUEST, 1) == b"\x00"
            g.cont()
            if done:
                time.sleep(10)  # let the writeback finish
                break
        else:
            sys.exit("All Reset did not complete")
    finally:
        proc.terminate()
        proc.wait(timeout=10)
    data = img.read_bytes()
    out.write_bytes(data)
    print(f"wrote {out}: {sum(1 for b in data if b)} nonzero bytes, "
          f"CI-V address {data[0x1a79]:#04x}, signature {data[0x3e80:0x3e90]!r}")


if __name__ == "__main__":
    main()
