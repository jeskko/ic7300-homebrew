#!/usr/bin/env python3
"""End-to-end emulator test for hello-gui: boots the loader firmware in qemu-machine with an SD
card holding APP.BIN, taps through MENU > SET > SD Card > Homebrew Apps like a user would, and
checks the dialog appears, OK dismisses it, and the app exits cleanly.

Usage (after sdk/loader/build.py and the build step in README.md):
    python3 sdk/examples/hello-gui/test_emu.py [--shots DIR]

Exits non-zero on the first failed check. Screenshots of each step land in --shots
(default scratch/hello-gui/shots).
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TOOLS = REPO / "qemu-machine" / "tools"
sys.path.insert(0, str(TOOLS))
from fp import FrontPanel  # noqa: E402
from screenshot import qmp_cmd  # noqa: E402
from vdc5_framebuffer_peek import qmp_open  # noqa: E402

QEMU = REPO / "qemu-machine" / "qemu-src" / "build" / "qemu-system-arm"
SCRATCH = REPO / "scratch" / "hello-gui"
QMP, FPS = "/tmp/hb_test_qmp.sock", "/tmp/hb_test_fp.sock"

DIALOG_STATE = 0x2039c584
OK_RECORD = 0x2032c91c + 0x53 * 0x4c
OK_RECORD_STOCK_LINE0 = 0x2035f6d4      # "The USB SEND/Keying settings were"


def nm(elf: Path) -> dict[str, int]:
    out = subprocess.run(["arm-none-eabi-nm", str(elf)], check=True, capture_output=True,
                         text=True).stdout
    return {m.group(2): int(m.group(1), 16)
            for m in (re.match(r"^([0-9a-fA-F]+)\s+\S+\s+(\S+)$", l) for l in out.splitlines()) if m}


class Emu:
    def __init__(self, shots: Path):
        self.shots = shots
        self.fp = FrontPanel(FPS)

    def read(self, addr: int, n: int) -> bytes:
        s = qmp_open(QMP)
        r = qmp_cmd(s, "human-monitor-command", **{"command-line": f"xp /{n}xb {addr:#x}"})
        s.close()
        return bytes(int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})\b", r["return"]))

    def word(self, addr: int) -> int:
        return struct.unpack("<I", self.read(addr, 4))[0]

    def cstr(self, addr: int) -> bytes:
        return self.read(addr, 32).split(b"\0")[0]

    def shot(self, name: str) -> None:
        ppm = self.shots / f"{name}.ppm"
        s = qmp_open(QMP)
        qmp_cmd(s, "screendump", filename=str(ppm))
        s.close()
        subprocess.run(["magick", str(ppm), str(ppm.with_suffix(".png"))], check=True)
        ppm.unlink()

    def touch(self, x: int, y: int, settle: float = 1.5) -> None:
        self.fp.touch(x, y)
        time.sleep(settle)


def check(cond: bool, what: str) -> None:
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", type=Path, default=SCRATCH / "shots")
    ap.add_argument("--boot-wait", type=float, default=30.0)
    ap.add_argument("--sd", type=Path, default=SCRATCH / "sdcard.img")
    ap.add_argument("--expect-no-app", action="store_true",
                    help="the card has no valid APP.BIN: check the tap does nothing, safely")
    args = ap.parse_args()
    args.shots.mkdir(parents=True, exist_ok=True)

    loader = nm(REPO / "sdk" / "loader" / "build" / "loader.elf")
    app = nm(HERE / "build" / "app.elf")
    api = loader["g_api"]

    for p in (QMP, FPS):
        Path(p).unlink(missing_ok=True)
    proc = subprocess.Popen([
        str(QEMU), "-M", "rz-a1h", "-display", "none", "-monitor", "none", "-serial", "none",
        "-kernel", str(SCRATCH / "flash.bin"),
        "-global", f"rza1h-riic.image={REPO / 'qemu-machine' / 'riic2_eeprom.img'}",
        "-qmp", f"unix:{QMP},server,nowait",
        "-chardev", f"socket,id=fpctl,path={FPS},server=on,wait=off",
        "-drive", f"if=sd,format=raw,file={args.sd}",
    ], stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(args.boot_wait)
        emu = Emu(args.shots)
        emu.shot("0-main")

        emu.fp.press("MENU"); time.sleep(1.5)
        emu.touch(430, 180)                     # SET
        emu.touch(448, 170)                     # page 2/2
        emu.touch(150, 117)                     # SD Card
        emu.touch(448, 170, 1.0)                # page 2/3
        emu.touch(448, 170)                     # page 3/3
        emu.shot("1-sd-card-menu")
        check(emu.word(api + 8) == 0, "no app resident before the tap")

        if args.expect_no_app:
            emu.touch(150, 55, 3.0)             # Homebrew Apps
            emu.shot("2-no-app")
            check(emu.read(DIALOG_STATE, 1)[0] == 0, "no dialog")
            check(emu.word(api + 8) == 0, "nothing resident")
            emu.fp.press("EXIT"); time.sleep(1.5)
            check(emu.read(DIALOG_STATE, 1)[0] == 0 and proc.poll() is None, "still responsive")
            print("all checks passed")
            return

        for run in (1, 2):                      # twice: the runtime must re-initialize cleanly
            print(f"-- launch {run}")
            launch_and_dismiss(emu, api, app, run)

        emu.fp.press("EXIT"); time.sleep(1)
        emu.fp.press("EXIT"); time.sleep(1)
        emu.fp.press("EXIT"); time.sleep(1.5)
        emu.shot("4-back-to-main")
        check(proc.poll() is None, "emulator still running")
        print(f"all checks passed; screenshots in {args.shots}")
    finally:
        proc.terminate()
        proc.wait()


def launch_and_dismiss(emu: Emu, api: int, app: dict[str, int], run: int) -> None:
    emu.touch(150, 55, 3.0)                 # Homebrew Apps
    emu.shot(f"2-hello-dialog-{run}")
    state = emu.read(DIALOG_STATE, 16)
    check(state[0] == 0x66, f"dialog item 0x66 up (active_item={state[0]:#x})")
    check(struct.unpack_from("<I", state, 8)[0] == app["on_ok"], "OK callback is the app's")
    check(emu.cstr(emu.word(OK_RECORD + 4)) == b"Hello, world!", "line 0 is our text")
    check(emu.cstr(emu.word(OK_RECORD + 4 + 7 * 4)) == b"OK", "button label is OK")
    check(emu.word(api + 8) == app["hb_idle"], "app suspended in hb_wait_until, idle hook set")
    check(emu.read(app["g_app_done"], 1)[0] == 0, "main() still running (bss re-zeroed)")

    emu.touch(357, 192, 2.0)                # the OK button
    emu.shot(f"3-after-ok-{run}")
    check(emu.read(DIALOG_STATE, 1)[0] == 0, "dialog closed")
    check(emu.read(app["g_ok_tapped"], 1)[0] == 1, "ui_message_box saw the OK tap")
    check(emu.read(app["g_app_done"], 1)[0] == 1, "main() returned")
    check(emu.word(api + 8) == 0, "idle hook cleared -- app gone")
    check(emu.word(OK_RECORD + 4) == OK_RECORD_STOCK_LINE0, "stock dialog text restored")


if __name__ == "__main__":
    main()
