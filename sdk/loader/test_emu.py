#!/usr/bin/env python3
r"""End-to-end emulator test for the loader's app picker, with sdk/examples/hello-gui,
sdk/examples/about-box, sdk/examples/cube and sdk/examples/minesweeper as the apps. Boots the loader firmware in qemu-machine, taps through
MENU > SET > SD Card > Homebrew Apps like a user would, and checks guest memory plus a
screenshot at each step.

Build the inputs first (see README.md "Build and test"), then:
    python3 sdk/loader/test_emu.py                     # card with \homebrew\{ABOUT,CUBE,HELLO,MINES}.BIN
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
GR3 = 0xfcff7780
FB = (0x20710000, 0x20750000)           # HB_FB0/1 (hb/abi.h, v3)
CUBE_BG = 0x0863                        # HB_RGB(10, 12, 24)
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

    def touch(self, x: int, y: int, settle: float = 1.5, hold: float = 0.15) -> None:
        self.fp.touch(x, y, hold)
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


def center_bg_pixels(emu: Emu) -> int:
    """How many of a 5x5 grid of pixels around the screen centre show the cube's background
    colour, read from the framebuffer GR3 is currently displaying."""
    base = emu.word(GR3 + 0x0c)
    n = 0
    for dy in range(-20, 21, 10):
        row = emu.read(base + (136 + dy) * 960 + (240 - 20) * 2, 82)
        for dx in range(0, 41, 10):
            n += struct.unpack_from("<H", row, dx * 2)[0] == CUBE_BG
    return n


def run_cube(emu: Emu, api: int, row: int) -> None:
    runtime = nm(EXAMPLES / "cube" / "build" / "app.elf")
    print(f"-- launch cube (row {row})")
    emu.touch(150, ROW_Y[row], 2.5)
    emu.shot("cube-1")
    check(emu.word(GR3 + 0x04) & 1 == 1, "GR3 read enabled")
    check(emu.word(GR3 + 0x20) & 3 == 2, "GR3 DISP_SEL = CURRENT (overlay shown)")
    check(emu.word(GR3 + 0x0c) in FB, "GR3 shows one of the SDK framebuffers")
    check(emu.word(api + 12) == 1, "input grabbed")
    f0, t0 = emu.word(runtime["g_frames"]), time.time()
    time.sleep(2.0)
    f1, t1 = emu.word(runtime["g_frames"]), time.time()
    fps = (f1 - f0) / (t1 - t0)
    print(f"     {fps:.1f} frames/s (wall clock, --icount off)")
    check(fps > 10, "animating")
    check(center_bg_pixels(emu) >= 20, "wireframe: centre mostly background")

    cursor = emu.read(0x20390222, 2)
    emu.touch(30, ROW_Y[row + 1], 1.5)         # a picker row underneath, clear of the cube
    check(emu.read(0x20390222, 2) == cursor and emu.word(api + 8) == runtime["hb_idle"],
          "tap outside the cube: picker underneath didn't react")

    emu.touch(240, 136, 1.5)                    # the cube
    emu.shot("cube-2-filled")
    check(center_bg_pixels(emu) == 0, "tap on the cube: filled faces")
    emu.touch(240, 136, 1.5)
    check(center_bg_pixels(emu) >= 20, "tap again: back to wireframe")

    emu.touch(456, 24, 2.5)                     # the X
    emu.shot("cube-3-exit")
    check(emu.word(GR3 + 0x04) & 1 == 0 and emu.word(GR3 + 0x20) & 3 == 1,
          "exit: GR3 back to read-off / LOWER")
    check(emu.word(api + 12) == 0 and emu.word(api + 8) == 0, "grab released, app gone")
    check(emu.read(runtime["g_app_done"], 1)[0] == 1, "main() returned")
    check(emu.read(CURRENT_SCREEN, 1)[0] == 0x63, "back on the picker")


MINES_N, MINES_CELL, MINES_GRID = 10, 26, 6
READY, PLAYING, WON, LOST = range(4)
HIDDEN, DUG, FLAGGED = range(3)
MINES_MODE, MINES_NEW = (329, 190), (429, 190)


def mines_xy(i: int) -> tuple[int, int]:
    return (MINES_GRID + i % MINES_N * MINES_CELL + 13, MINES_GRID + i // MINES_N * MINES_CELL + 13)


def run_mines(emu: Emu, api: int, row: int) -> None:
    rt = nm(EXAMPLES / "minesweeper" / "build" / "app.elf")
    state = lambda: emu.read(rt["g_state"], 1)[0]
    cells = lambda: emu.read(rt["g_cell"], 100)
    print(f"-- launch minesweeper (row {row})")
    emu.touch(150, ROW_Y[row], 2.5)
    cursor = emu.read(0x20390222, 2)            # the launch tap itself moves it
    emu.shot("mines-1-new")
    check(emu.word(GR3 + 0x20) & 3 == 2 and emu.word(GR3 + 0x0c) in FB, "GR3 overlay shown")
    check(emu.word(api + 12) == 1, "input grabbed")
    check(state() == READY and cells() == bytes(100), "new board: all hidden, no mines laid")

    first = 44
    emu.touch(*mines_xy(first), 1.0)
    mine = emu.read(rt["g_mine"], 100)
    c = cells()
    near = [i for i in range(100) if abs(i % 10 - first % 10) <= 1 and abs(i // 10 - first // 10) <= 1]
    print(f"     mines at {[i for i in range(100) if mine[i]]}, {c.count(DUG)} cells dug")
    check(state() == PLAYING and sum(mine) == 12, "first dig: 12 mines laid, playing")
    check(not any(mine[i] for i in near) and all(c[i] == DUG for i in near),
          "  ...none on or next to the first cell, which opened an area")
    emu.shot("mines-2-first-dig")

    hidden = [i for i in range(100) if c[i] == HIDDEN]
    m, safe = next(i for i in hidden if mine[i]), next(i for i in hidden if not mine[i])
    emu.touch(*mines_xy(m), 1.0, hold=1.0)
    check(cells()[m] == FLAGGED, "hold: flag planted")
    emu.touch(*mines_xy(m), 1.0, hold=1.0)
    check(cells()[m] == HIDDEN, "hold again: flag pulled")
    emu.touch(*MINES_MODE, 1.0)
    check(emu.read(rt["g_flag_mode"], 1)[0] == 1, "DIG/FLAG button: flag mode")
    emu.touch(*mines_xy(m), 1.0)
    check(cells()[m] == FLAGGED, "  ...a tap flags")
    emu.touch(*mines_xy(safe), 1.0, hold=1.0)
    check(cells()[safe] == DUG and state() == PLAYING, "  ...a hold digs")
    emu.touch(*MINES_MODE, 1.0)
    check(emu.read(rt["g_flag_mode"], 1)[0] == 0, "back to dig mode")
    emu.shot("mines-3-flag")

    taps = 0
    while state() == PLAYING:
        c = cells()
        todo = [i for i in range(100) if c[i] == HIDDEN and not mine[i]]
        if not todo:
            break
        emu.touch(*mines_xy(todo[0]), 0.4)
        taps += 1
        if taps > 100:
            break
    time.sleep(1.0)
    emu.shot("mines-4-won")
    c = cells()
    check(state() == WON, f"digging every safe cell ({taps} taps) wins")
    check(all(c[i] == (FLAGGED if mine[i] else DUG) for i in range(100)),
          "  ...every mine flagged, every other cell dug")

    emu.touch(*MINES_NEW, 1.0)
    check(state() == READY and cells() == bytes(100), "NEW: fresh board")
    emu.touch(*mines_xy(0), 1.0)
    mine = emu.read(rt["g_mine"], 100)
    c = cells()
    m = next(i for i in range(100) if mine[i] and c[i] == HIDDEN)
    emu.touch(*mines_xy(m), 1.5)
    emu.shot("mines-5-lost")
    check(state() == LOST and emu.read(rt["g_boom"], 1)[0] == m, "digging a mine loses")
    before = cells()
    emu.touch(*mines_xy(next(i for i in range(100) if before[i] == HIDDEN)), 1.0)
    check(cells() == before, "  ...and the board is frozen")

    check(emu.read(0x20390222, 2) == cursor and emu.read(CURRENT_SCREEN, 1)[0] == 0x63,
          "picker underneath never reacted")
    emu.press("EXIT", 2.5)
    emu.shot("mines-6-exit")
    check(emu.word(GR3 + 0x04) & 1 == 0 and emu.word(GR3 + 0x20) & 3 == 1,
          "EXIT key: GR3 back to read-off / LOWER")
    check(emu.word(api + 12) == 0 and emu.word(api + 8) == 0, "grab released, app gone")
    check(emu.read(rt["g_app_done"], 1)[0] == 1, "main() returned")
    check(emu.read(CURRENT_SCREEN, 1)[0] == 0x63, "still on the picker: EXIT didn't reach it")


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
    ap.add_argument("--big", action="store_true",
                    help=r"the card has \homebrew\BIG.BIN (sdk/loader/test_apps/big: a ~770 KB "
                         r"image and a heap workout) and TOOBIG.BIN (0x100001 bytes): expect "
                         "only BIG listed, and its 'BIG OK' dialog")
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
        elif args.big:
            check(labels == [b"BIG"], "lists BIG, not TOOBIG (1 MB + 1 byte, over the region)")
            rt = nm(HERE / "test_apps" / "big" / "build" / "app.elf")
            t0 = time.time()
            emu.touch(150, ROW_Y[0], 0.2)
            while emu.read(DIALOG_STATE, 1)[0] != 0x66 and time.time() - t0 < 30:
                time.sleep(0.5)
            print(f"     dialog up {time.time() - t0:.1f} s after the tap (load + test)")
            emu.shot("big-dialog")
            step = struct.unpack("<i", emu.read(rt["g_fail_step"], 4))[0]
            check(step == 0, f"image and heap checks passed (g_fail_step = {step})")
            check(emu.cstr(emu.word(OK_RECORD + 4)) == b"BIG OK", "  ...and it says BIG OK")
            emu.touch(*OK_BUTTON, 2.0)
            check(emu.read(rt["g_app_done"], 1)[0] == 1 and emu.word(api + 8) == 0,
                  "main() returned, app gone")
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
            check(labels == [b"ABOUT", b"CUBE", b"HELLO", b"MINES"],
                  "lists ABOUT, CUBE, HELLO, MINES (sorted)")
            launch(emu, api, 2, "hello-gui", [b"Hello, world!"])
            launch(emu, api, 0, "about-box",
                   [b"Homebrew SDK for the IC-7300", b"Apps live in \\homebrew"])
            run_cube(emu, api, 1)
            run_mines(emu, api, 3)
            launch(emu, api, 2, "hello-gui", [b"Hello, world!"])     # relaunch after the cube

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
