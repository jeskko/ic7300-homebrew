#!/usr/bin/env python3
r"""End-to-end emulator test for the loader's app picker, with sdk/examples/hello-gui and
sdk/examples/about-box as the apps. Boots the loader firmware in qemu-machine, taps through
MENU > SET > SD Card > Homebrew Apps like a user would, and checks guest memory plus a
screenshot at each step.

Build the inputs first (see README.md "Build and test"), then:
    python3 sdk/loader/test_emu.py                     # card with \homebrew\{ABOUT,HELLO}.BIN
    python3 sdk/loader/test_emu.py --sd EMPTY.img --expect-no-apps

Exits non-zero on the first failed check. Screenshots land in --shots
(default scratch/homebrew/shots).
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
REPO = HERE.parent.parent
EXAMPLES = HERE.parent / "examples"
sys.path.insert(0, str(REPO / "qemu-machine" / "tools"))
from fp import FrontPanel  # noqa: E402
from screenshot import qmp_cmd  # noqa: E402
from vdc5_framebuffer_peek import qmp_open  # noqa: E402

QEMU = REPO / "qemu-machine" / "qemu-src" / "build" / "qemu-system-arm"
SCRATCH = REPO / "scratch" / "homebrew"
QMP, FPS = "/tmp/hb_test_qmp.sock", "/tmp/hb_test_fp.sock"

DIALOG_STATE = 0x2039c584
OK_RECORD = 0x2032c91c + 0x53 * 0x4c
OK_RECORD_STOCK_LINE0 = 0x2035f6d4      # "The USB SEND/Keying settings were"
CATALOG = 0x2018ed48
PICKER_REGISTRY = 0x201993e0 + 0x40 * 12
PICKER_REGISTRY_STOCK = (1, 0x20199314)
PICKER_TITLE = 0x2018fe24 + (0x63 - 0x13) * 24 + 8
PICKER_TITLE_STOCK = 0x2035a4f0          # "PLAYER SET"
CURRENT_SCREEN = 0x203de17f
SAVED_CURSOR = 0x203de4cc + 0x288 + 0x40 * 4    # the borrowed category's saved cursor
ROW_Y = (55, 117, 180, 240)
OK_BUTTON = (357, 192)


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
        return self.read(addr, 40).split(b"\0")[0]

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

    def press(self, key: str, settle: float = 1.2) -> None:
        self.fp.press(key)
        time.sleep(settle)


def check(cond: bool, what: str) -> None:
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        sys.exit(1)


def picker_labels(emu: Emu) -> list[bytes]:
    count, lst = struct.unpack("<2I", emu.read(PICKER_REGISTRY, 8))
    labels = []
    for i in range(count):
        val = emu.word(lst + 4 * i) >> 16
        labels.append(emu.cstr(emu.word(CATALOG + val * 20 + 12)))
    return labels


def dismiss_dialog(emu: Emu, runtime: dict[str, int], text: bytes, name: str) -> None:
    emu.shot(name)
    check(emu.read(DIALOG_STATE, 1)[0] == 0x66, f"dialog up: {text.decode()!r}")
    check(emu.cstr(emu.word(OK_RECORD + 4)) == text, "  ...with the app's text")
    check(emu.word(DIALOG_STATE + 8) == runtime["on_ok"], "  ...and the app's OK callback")
    emu.touch(*OK_BUTTON, 2.0)


def launch(emu: Emu, api: int, row: int, app: str, texts: list[bytes]) -> None:
    runtime = nm(EXAMPLES / app / "build" / "app.elf")
    print(f"-- launch {app} (row {row})")
    emu.touch(150, ROW_Y[row], 3.0)
    check(emu.word(api + 8) == runtime["hb_idle"], "app resident, suspended in hb_wait_until")
    for i, text in enumerate(texts):
        dismiss_dialog(emu, runtime, text, f"{app}-dialog-{i + 1}")
    emu.shot(f"{app}-after")
    check(emu.read(DIALOG_STATE, 1)[0] == 0, "dialog closed")
    check(emu.read(runtime["g_app_done"], 1)[0] == 1, "main() returned")
    check(emu.word(api + 8) == 0, "idle hook cleared -- app gone")
    check(emu.word(OK_RECORD + 4) == OK_RECORD_STOCK_LINE0, "stock dialog text restored")
    check(emu.read(CURRENT_SCREEN, 1)[0] == 0x63, "still on the picker")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", type=Path, default=SCRATCH / "shots")
    ap.add_argument("--boot-wait", type=float, default=30.0)
    ap.add_argument("--flash", type=Path, default=SCRATCH / "flash.bin")
    ap.add_argument("--sd", default=str(SCRATCH / "sdcard.img"), help="card image, or 'none'")
    ap.add_argument("--expect-no-apps", action="store_true",
                    help=r"the card has no \homebrew\*.BIN: expect the placeholder row")
    ap.add_argument("--expect-rows", help="comma-separated labels the picker must show; "
                    "then only checks back/restore")
    ap.add_argument("--paging", action="store_true",
                    help=r"the card has \homebrew\A01..A16.BIN, A06 = about-box, the rest "
                         "hello-gui: expect A01..A14, and page 2 row 2 to launch about-box")
    args = ap.parse_args()
    args.shots.mkdir(parents=True, exist_ok=True)

    loader = nm(HERE / "build" / "loader.elf")
    api = loader["g_api"]

    for p in (QMP, FPS):
        Path(p).unlink(missing_ok=True)
    proc = subprocess.Popen([
        str(QEMU), "-M", "rz-a1h", "-display", "none", "-monitor", "none", "-serial", "none",
        "-kernel", str(args.flash),
        "-global", f"rza1h-riic.image={REPO / 'qemu-machine' / 'riic2_eeprom.img'}",
        "-qmp", f"unix:{QMP},server,nowait",
        "-chardev", f"socket,id=fpctl,path={FPS},server=on,wait=off",
    ] + ([] if args.sd == "none" else ["-drive", f"if=sd,format=raw,file={args.sd}"]),
        stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(args.boot_wait)
        emu = Emu(args.shots)
        emu.shot("0-main")
        stock_cursor = emu.word(SAVED_CURSOR)

        emu.press("MENU")
        emu.touch(430, 180)                     # SET
        emu.touch(448, 170)                     # page 2/2
        emu.touch(150, 117)                     # SD Card
        emu.touch(448, 170, 1.0)                # page 2/3
        emu.touch(448, 170)                     # page 3/3
        emu.shot("1-sd-card-menu")
        check(struct.unpack("<2I", emu.read(PICKER_REGISTRY, 8)) == PICKER_REGISTRY_STOCK,
              "borrowed screen is stock before the tap")

        emu.touch(150, 55, 3.0)                 # Homebrew Apps
        emu.shot("2-picker")
        check(emu.read(CURRENT_SCREEN, 1)[0] == 0x63, "picker screen is up")
        check(emu.cstr(emu.word(PICKER_TITLE)) == b"HOMEBREW APPS", "titled HOMEBREW APPS")
        labels = picker_labels(emu)
        print(f"     rows: {labels}")

        if args.expect_no_apps:
            check(labels == [b"No apps in \\homebrew"], "only the placeholder row")
            emu.touch(150, ROW_Y[0], 2.0)
            check(emu.read(DIALOG_STATE, 1)[0] == 0 and emu.word(api + 8) == 0,
                  "tapping the placeholder does nothing")
        elif args.expect_rows:
            want = [r.encode() for r in args.expect_rows.split(",")]
            check(labels == want, f"lists {want}")
        elif args.paging:
            want = [b"A%02d" % i for i in range(1, 15)]
            check(labels == want, "lists the alphabetically-first 14 of 16 apps")
            emu.touch(448, 170)                 # page 2/4
            emu.shot("2-picker-page2")
            launch(emu, api, 1, "about-box",
                   [b"Homebrew SDK for the IC-7300", b"Apps live in \\homebrew"])
            emu.touch(448, 170)                 # page 3/4
            launch(emu, api, 2, "hello-gui", [b"Hello, world!"])
        else:
            check(labels == [b"ABOUT", b"HELLO"], "lists ABOUT, HELLO (sorted, .BIN dropped)")
            launch(emu, api, 1, "hello-gui", [b"Hello, world!"])
            launch(emu, api, 0, "about-box",
                   [b"Homebrew SDK for the IC-7300", b"Apps live in \\homebrew"])
            launch(emu, api, 1, "hello-gui", [b"Hello, world!"])     # relaunch

        emu.press("EXIT", 2.0)                  # back out of the picker
        emu.shot("3-back")
        check(emu.read(CURRENT_SCREEN, 1)[0] == 0x2f, "back returns to SD CARD")
        check(struct.unpack("<2I", emu.read(PICKER_REGISTRY, 8)) == PICKER_REGISTRY_STOCK,
              "borrowed screen's rows restored")
        check(emu.word(PICKER_TITLE) == PICKER_TITLE_STOCK, "borrowed screen's title restored")
        check(emu.word(SAVED_CURSOR) == stock_cursor,
              "borrowed screen's saved cursor restored")

        emu.touch(150, 55, 3.0)                 # Homebrew Apps again: picker reopens
        check(emu.read(CURRENT_SCREEN, 1)[0] == 0x63, "picker reopens")
        emu.press("EXIT", 1.5)
        emu.press("EXIT")
        emu.press("EXIT")
        emu.press("EXIT", 1.5)
        emu.shot("4-main")
        check(emu.read(CURRENT_SCREEN, 1)[0] < 0x13, "back at the main screen")
        check(proc.poll() is None, "emulator still running")
        print(f"all checks passed; screenshots in {args.shots}")
    finally:
        proc.terminate()
        proc.wait()


if __name__ == "__main__":
    main()
