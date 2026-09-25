#!/usr/bin/env python3
r"""RAM marker sweep: which parts of a candidate RAM range does the firmware ever write?

Fills the range with an address-keyed pattern *at reset* (QEMU's generic loader device, so it
is in place before the first firmware instruction), boots, then dumps the range after each
step of a scenario and reports every run of words that no longer hold their marker. Anything
the firmware allocates and touches -- zeroing it counts -- shows up; RAM it reserves but never
writes during the scenario does not, so the answer is only as good as the scenario's coverage.

    python3 qemu-machine/tools/ram_marker_sweep.py \
        --flash scratch/homebrew/flash.bin --sd scratch/homebrew/sdcard.img

--flash is the sdk/loader/ firmware and --sd a card with \homebrew\{ABOUT,CUBE,HELLO,MINES}.BIN
(sdk/loader/README.md, "Build and test"): the SDK apps are the positive control. The run takes
about 10 minutes, and exits non-zero if the control fails or the radio stops answering CI-V.
Library use: Sweep(..., launch=False) attaches to one already running, for interactive work.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
from civ import Civ  # noqa: E402
from fp import FrontPanel  # noqa: E402
from screenshot import qmp_cmd  # noqa: E402
from vdc5_framebuffer_peek import qmp_open  # noqa: E402

QEMU = REPO / "qemu-machine" / "qemu-src" / "build" / "qemu-system-arm"
EEPROM = REPO / "qemu-machine" / "riic2_eeprom.img"

# Default candidate range: everything that read all-zero after an idle boot of the loader
# firmware (2026-09-25), from the page after the loader up to the first firmware-used page.
START, END = 0x20601000, 0x2080b000
KEY = 0x5A3C96E1


def marker_words(start: int, end: int) -> bytes:
    return b"".join(struct.pack("<I", (a ^ KEY) & 0xffffffff) for a in range(start, end, 4))


def diff_runs(a: bytes, b: bytes, start: int, merge: int = 256) -> list[tuple[int, int]]:
    """[(first, end)] address runs of words that differ between a and b (same range, based at
    `start`); gaps shorter than `merge` bytes are folded into one run."""
    out: list[list[int]] = []
    for blk in range(0, len(b), 4096):
        if a[blk:blk + 4096] == b[blk:blk + 4096]:
            continue
        for off in range(blk, min(blk + 4096, len(b)), 4):
            if a[off:off + 4] != b[off:off + 4]:
                addr = start + off
                if out and addr - out[-1][1] < merge:
                    out[-1][1] = addr + 4
                else:
                    out.append([addr, addr + 4])
    return [(x, y) for x, y in out]


def runs(dump: bytes, start: int, merge: int = 256) -> list[tuple[int, int]]:
    """Runs of words that no longer hold their marker."""
    return diff_runs(marker_words(start, start + len(dump)), dump, start, merge)


def union(rs: list[tuple[int, int]], merge: int = 256) -> list[tuple[int, int]]:
    out: list[list[int]] = []
    for a, b in sorted(rs):
        if out and a - out[-1][1] < merge:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return [(a, b) for a, b in out]


def fmt_runs(rs: list[tuple[int, int]]) -> str:
    if not rs:
        return "  (untouched)"
    return "\n".join(f"  {a:#010x}-{b - 1:#010x}  {b - a:>8} B" for a, b in rs)


class Sweep:
    def __init__(self, flash: Path, sd: Path | None, work: Path, start: int = START,
                 end: int = END, sock_dir: str = "/tmp", launch: bool = True,
                 markers: bool = True):
        self.start, self.end, self.work = start, end, work
        work.mkdir(parents=True, exist_ok=True)
        self.qmp = f"{sock_dir}/rms_qmp.sock"
        self.fps = f"{sock_dir}/rms_fp.sock"
        self.civs = f"{sock_dir}/rms_civ.sock"
        self.proc = self.fp = self.civ = None
        if not launch:                  # attach to one already running (interactive use)
            return
        for p in (self.qmp, self.fps, self.civs):
            Path(p).unlink(missing_ok=True)
        markers_bin = work / "markers.bin"
        markers_bin.write_bytes(marker_words(start, end))
        self.proc = subprocess.Popen([
            str(QEMU), "-M", "rz-a1h", "-display", "none", "-monitor", "none",
            "-kernel", str(flash),
        ] + (["-device", f"loader,file={markers_bin},addr={start:#x},force-raw=on"]
             if markers else []) + [
            "-global", f"rza1h-riic.image={EEPROM}",
            "-qmp", f"unix:{self.qmp},server,nowait",
            "-chardev", f"socket,id=fpctl,path={self.fps},server=on,wait=off",
            "-chardev", f"socket,id=civ,path={self.civs},server=on,wait=off",
            "-serial", "chardev:civ",
        ] + ([] if sd is None else ["-drive", f"if=sd,format=raw,file={sd}"]),
            stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def connect(self) -> None:
        self.fp = FrontPanel(self.fps)
        self.civ = Civ(self.civs)

    def alive(self) -> bool:
        return self.proc.poll() is None

    def dump(self, name: str) -> bytes:
        path = self.work / f"{name}.bin"
        s = qmp_open(self.qmp)
        qmp_cmd(s, "pmemsave", val=self.start, size=self.end - self.start, filename=str(path))
        s.close()
        for _ in range(50):
            if path.exists() and path.stat().st_size == self.end - self.start:
                break
            time.sleep(0.1)
        return path.read_bytes()

    def read(self, addr: int, n: int) -> bytes:
        path = self.work / "_read.bin"
        path.unlink(missing_ok=True)
        s = qmp_open(self.qmp)
        qmp_cmd(s, "pmemsave", val=addr, size=n, filename=str(path))
        s.close()
        time.sleep(0.1)
        return path.read_bytes()

    def shot(self, name: str) -> Path:
        ppm = self.work / f"{name}.ppm"
        s = qmp_open(self.qmp)
        qmp_cmd(s, "screendump", filename=str(ppm))
        s.close()
        png = ppm.with_suffix(".png")
        subprocess.run(["magick", str(ppm), str(png)], check=True)
        ppm.unlink()
        return png

    def touch(self, x: int, y: int, settle: float = 1.5, hold: float = 0.15) -> None:
        self.fp.touch(x, y, hold)
        time.sleep(settle)

    def press(self, key: str, settle: float = 1.2, hold: float = 0.15) -> None:
        self.fp.press(key, hold)
        time.sleep(settle)

    def stop(self) -> None:
        self.proc.terminate()
        self.proc.wait()


# ---- the scenario ------------------------------------------------------------------------
# Screen coordinates are for the 1.42 UI at 480x272; list rows are at y = 55/117/180/236 and a
# list's page-down/up buttons at (448, 170)/(448, 55).

APP_REGION = (0x20610000, 0x20630000)       # sdk/include/hb/abi.h
APP_FBS = (0x20640000, 0x20640000 + 2 * 0x40000 - 0x400)   # HB_FB0..HB_FB1 + 480*272*2


def wait_civ(sw: Sweep, timeout: float = 90) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        if sw.civ.cmd(0x03):
            return True
        time.sleep(2)
    return False


def menu_page1(sw: Sweep) -> None:
    for _ in range(4):
        sw.press("EXIT", 0.8)
    sw.press("MENU", 2)
    sw.touch(180, 244, 1.5)                 # page dot 1: MENU reopens on its last page


def list_top(sw: Sweep, pages: int) -> None:
    """Lists reopen on the page they were left on, and paging doesn't wrap: page up once per
    page to be sure of page 1."""
    for _ in range(pages):
        sw.touch(448, 55, 1.0)


def open_sd_card(sw: Sweep) -> None:
    """MENU > SET > SD Card, on page 1/3."""
    menu_page1(sw)
    sw.touch(430, 180, 2)                   # SET
    list_top(sw, 2)
    sw.touch(448, 170, 1.5)                 # page 2/2
    sw.touch(150, 117, 2)                   # SD Card
    list_top(sw, 3)


def step_scope(sw):
    sw.press("M.SCOPE", 3)


def step_modes_bands(sw):
    for mode in (4, 3, 2, 5, 7, 8, 1):      # RTTY CW AM FM CW-R RTTY-R USB
        sw.civ.cmd(0x06, mode, 1)
        time.sleep(2)
    for f in ([0, 0, 0x10, 0x07, 0], [0, 0, 0x00, 0x21, 0], [0, 0, 0x10, 0x14, 0]):
        sw.civ.cmd(0x05, *f)                # 7.1, 21.0, back to 14.1 MHz
        time.sleep(2)


def step_menu_tiles(sw):
    for x, y in ((146, 90), (240, 90), (335, 90), (430, 90),        # AUDIO VOICE METER SWR
                 (52, 180), (146, 180), (240, 180), (335, 180)):    # MEMORY SCAN MPAD RECORD
        menu_page1(sw)
        sw.touch(x, y, 3)


def step_keyer(sw):
    sw.civ.cmd(0x06, 3, 1)
    menu_page1(sw)
    sw.touch(240, 90, 3)                    # KEYER (CW mode)
    sw.touch(40, 200, 6)                    # M1: send it


def step_rtty_decode(sw):
    sw.civ.cmd(0x06, 4, 1)
    menu_page1(sw)
    sw.touch(240, 90, 12)                   # DECODE (RTTY mode)


def step_tx(sw):
    for _ in range(4):
        sw.press("EXIT", 0.8)
    for mode in (1, 4, 3):
        sw.civ.cmd(0x06, mode, 1)
        time.sleep(1)
        sw.civ.cmd(0x1c, 0, 1)              # TX on
        time.sleep(4)
        sw.civ.cmd(0x1c, 0, 0)
        time.sleep(2)
    sw.civ.cmd(0x06, 1, 1)
    sw.press("TUNER", 8, hold=1.5)          # hold = start a tune


def step_qso_recorder(sw):
    menu_page1(sw)
    sw.touch(335, 180, 3)                   # RECORD
    sw.touch(150, 55, 16)                   # <<REC Start>>, record ~15 s
    sw.touch(150, 55, 3)                    # <<REC Stop>>
    sw.touch(150, 117, 3)                   # Play Files
    sw.touch(150, 55, 3)                    # the folder
    sw.touch(250, 55, 12)                   # the file: play ~10 s


def step_sd_save_load(sw):
    open_sd_card(sw)
    sw.touch(150, 117, 2.5)                 # Save Setting
    sw.touch(150, 55, 4)                    # <<New File>>
    sw.touch(440, 193, 3)                   # ENT (default name)
    sw.touch(232, 192, 8)                   # YES; back to SD CARD when done
    sw.touch(150, 55, 2.5)                  # Load Setting
    sw.touch(150, 55, 2.5)                  # the file
    sw.touch(150, 55, 2.5)                  # ALL
    sw.touch(232, 192, 10)                  # YES


def step_scope_expanded(sw):
    menu_page1(sw)
    sw.touch(52, 90, 3)                     # SCOPE
    sw.touch(430, 254, 3)                   # EXPD
    for _ in range(3):
        sw.touch(146, 254, 1.2)             # SPAN
    sw.touch(335, 254, 2)                   # CENT/FIX
    sw.touch(335, 254, 1.5)
    sw.touch(430, 254, 2)


def step_apps(sw):
    """Positive control: SDK apps write the app region and framebuffers, nothing else."""
    open_sd_card(sw)
    sw.touch(448, 170, 1.0)
    sw.touch(448, 170, 1.5)                 # page 3/3
    sw.touch(150, 55, 3)                    # Homebrew Apps: ABOUT CUBE HELLO MINES
    sw.touch(150, 117, 5)                   # CUBE
    sw.touch(456, 24, 3)                    # X
    sw.touch(150, 236, 3)                   # MINES
    sw.touch(123, 123, 2)                   # dig
    sw.press("EXIT", 3)                     # quit


def step_voice_tx_rec(sw):
    """Voice TX memory recording. NOTE: as of 2026-09-25 this opens the record screen but does
    not start a recording (VoiceTx/ stays empty) -- the record button's position is wrong."""
    menu_page1(sw)
    sw.touch(240, 90, 3)                    # VOICE
    sw.touch(448, 196, 3)                   # REC/SET
    sw.touch(200, 55, 3)                    # REC: VOICE TX RECORD (T1)
    sw.touch(111, 220, 6)                   # record ~6 s
    sw.touch(327, 220, 3)                   # stop


SCENARIO = [
    ("scope", step_scope), ("modes+bands", step_modes_bands), ("menu tiles", step_menu_tiles),
    ("CW keyer send", step_keyer), ("RTTY decode", step_rtty_decode), ("TX + tune", step_tx),
    ("QSO record+play", step_qso_recorder), ("scope expanded", step_scope_expanded),
    ("voice TX REC", step_voice_tx_rec), ("apps (control)", step_apps),
    # Last: in the emulator the card drops out during this ("SD Card was removed.", an
    # SD-model gap found 2026-09-25), so nothing that needs the card can follow it.
    ("SD save+load", step_sd_save_load),
]


def inside(rs, ranges) -> bool:
    return all(any(lo <= a and b <= hi for lo, hi in ranges) for a, b in rs)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--flash", type=Path, required=True)
    ap.add_argument("--sd", type=Path, required=True,
                    help="card image with \\homebrew\\{ABOUT,CUBE,HELLO,MINES}.BIN; a copy is used")
    ap.add_argument("--work", type=Path, default=REPO / "scratch" / "ram_sweep")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=START)
    ap.add_argument("--end", type=lambda s: int(s, 0), default=END)
    args = ap.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    sd = args.work / "sd.img"
    sd.write_bytes(args.sd.read_bytes())

    sw = Sweep(args.flash, sd, args.work, args.start, args.end)
    ok = True
    try:
        time.sleep(1.0)
        sw.connect()
        pre = runs(sw.dump("00-reset"), sw.start)
        print(f"markers at reset: {'intact' if not pre else 'DAMAGED'}")
        if not wait_civ(sw):
            sys.exit("no CI-V reply: boot failed")
        time.sleep(8)
        sw.shot("01-boot")
        prev = sw.dump("01-boot")
        firmware = runs(prev, sw.start)
        print(f"boot: {sum(b - a for a, b in firmware)} B written\n{fmt_runs(firmware)}")
        for n, (name, fn) in enumerate(SCENARIO, 2):
            fn(sw)
            sw.shot(f"{n:02d}-{name.split()[0]}{n}")
            cur = sw.dump(f"{n:02d}")
            new = diff_runs(prev, cur, sw.start)    # what this step wrote
            prev = cur
            if not wait_civ(sw, 20):        # a tune can keep CI-V busy for a few seconds
                print(f"{name}: the radio stopped answering CI-V -- run INVALID (seen once "
                      f"2026-09-25 after a band change, not reproduced)")
                ok = False
                break
            if fn is step_apps:
                good = (inside(new, [APP_REGION, APP_FBS])
                        and any(inside([r], [APP_REGION]) for r in new)
                        and any(inside([r], [APP_FBS]) for r in new))
                print(f"{name}: {'PASS' if good else 'FAIL'} -- the apps' writes land in the "
                      f"app region and both framebuffers, and only there\n{fmt_runs(new)}")
                ok &= good
                continue                    # the apps' writes, not the firmware's
            firmware = union(firmware + new)
            print(f"{name}: {sum(b - a for a, b in new)} B written")
            if new:
                print(fmt_runs(new))
        print(f"\nfirmware writes into {sw.start:#x}-{sw.end - 1:#x}, whole scenario: "
              f"{sum(b - a for a, b in firmware)} B\n{fmt_runs(firmware)}")
    finally:
        sw.stop()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
