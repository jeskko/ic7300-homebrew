#!/usr/bin/env python3
"""Run the emulated IC-7300 with a live LCD window (vdc5.c's graphic console).

Boots flash.bin with the PWRK-hold EEPROM image, presses and holds PWRK at the
right moment over QMP (the same dance every trace tool does), and leaves QEMU
running with `-display gtk` (or --display sdl/none) until you close it.
--screendump FILE [--after S] instead captures one PPM of the console after S
seconds and exits (a headless check that the console path works).

With the 2026-09-24 system-tick fix, the main screen comes up after roughly
4s of emulated time (~12s wall with the default real-time pacing; less with
--fast). --no-pwrk skips the power-key dance and boots straight up instead.

Front panel: click in the window to touch the screen (scif.c maps the mouse to the
touch panel), and drive keys/knobs with tools/fp.py over the control socket this
starts at /tmp/qemu_run_gui_fp.sock, e.g. `RZA1H_FPCTL=/tmp/qemu_run_gui_fp.sock
tools/fp.py press MENU`.

FRONT_PANEL_HELP below is printed after start (see that string for the key map).

Usage: run_gui.py [--display gtk|sdl|none] [--screendump out.ppm --after 170]
                   [--fast | --icount SPEC] [--no-pwrk] [--civ PATH]
                   [--no-audio] [--tone HZ:LEVEL] [--noise LEVEL]
                   [--af N] [--rfsql N] [--fpga-sweep-hz N] [--fpga-signals SPEC]
                   [--no-mouse] [--no-keys] [--debug DEVS] [--log PATH]
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from screenshot import qmp_cmd, pc  # noqa: E402
from vdc5_framebuffer_peek import GPIO_PATH, qmp_open  # noqa: E402
from qemu_launch import QEMU, FLASH, HERE, DEFAULT_ICOUNT  # noqa: E402

# Kept in sync with scif.c's rza1h_scif3_fp_input_event() comment (fp_keymap[] there is the
# source of truth for the codes; this is just the human-readable map).
FRONT_PANEL_HELP = """\
Front panel (click the LCD window first so it has keyboard focus):
  mouse click/drag = touch screen    wheel = MAIN DIAL (5 steps/notch)
  shift+wheel = MULTI   ctrl+wheel = TWIN PBT inner   ctrl+shift+wheel = PBT outer
  left/right = MAIN DIAL -1/+1   +/- = AF gain   ]/[ = RF/SQL
  M MENU  F FUNCTION  Q QUICK  Esc EXIT  S M.SCOPE  T TRANSMIT  U TUNER  O AUTO TUNE
  V VOX/BK-IN  P P.AMP/ATT  N NOTCH  B NB  R NR  X XFC  K SPEECH/LOCK  D MPAD
  W A/B  E V/M  I RIT  J dTX  C CLEAR  L SPLIT  Z PBT-CLR  Enter MULTI push
  PgUp/PgDn M-CH UP/DN  up/down MIC UP/DN  F1-F4 ext keypad 1-4
  Keys are held while pressed (hold = long press). Scripted control: tools/fp.py
  (RZA1H_FPCTL=<fp socket> tools/fp.py press MENU); GTK's own shortcuts need ctrl+alt.
"""


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--display", default="gtk")
    ap.add_argument("--screendump")
    ap.add_argument("--after", type=float, default=170.0)
    ap.add_argument("--fast", action="store_true",
                     help="-icount shift=1,sleep=off (emulated clock may run ahead of real "
                          "time; the on-screen clock runs fast). Mutually exclusive with "
                          "--icount.")
    ap.add_argument("--icount",
                     help="pass -icount SPEC verbatim (e.g. shift=2, shift=auto); "
                          "'off' = no -icount at all. Mutually exclusive with --fast.")
    ap.add_argument("--no-pwrk", action="store_true",
                     help="boot straight up: use riic2_eeprom.img and skip the QMP PWRK "
                          "press/wait (default is the realistic PWRK-hold power-on)")
    ap.add_argument("--civ", help="also expose CI-V (SCIF0) on this unix socket path "
                                   "(talk to it with tools/civ.py PATH ...; radio address 0x94)")
    ap.add_argument("--no-audio", action="store_true",
                     help="RZA1H_SSIF=off: the CPU<->DSP audio link never starts (slightly faster)")
    ap.add_argument("--tone", help="RZA1H_AF_TONE: fake RX audio tone HZ:LEVEL "
                                    "(e.g. 1000:0.25; 'none' = silence)")
    ap.add_argument("--noise", help="RZA1H_AF_NOISE level")
    ap.add_argument("--af", type=lambda x: int(x, 0),
                     help="RZA1H_FP_AF: front-panel AF pot position at power-on (0..255)")
    ap.add_argument("--rfsql", type=lambda x: int(x, 0),
                     help="RZA1H_FP_RFSQL: front-panel RF/SQL pot position at power-on (0..255); "
                          "RF/SQL >= 0x66 closes the squelch")
    ap.add_argument("--fpga-sweep-hz", help="RZA1H_FPGA_SWEEP_HZ")
    ap.add_argument("--fpga-signals",
                     help="RZA1H_FPGA_SIGNALS: band-scope test carriers "
                          "'off_hz:raw,...' ('none' = none)")
    ap.add_argument("--no-mouse", action="store_true",
                     help="RZA1H_FP_NO_MOUSE=1: disable window mouse-as-touch")
    ap.add_argument("--no-keys", action="store_true",
                     help="RZA1H_FP_NO_KEYS=1: disable keyboard-as-front-panel")
    ap.add_argument("--debug", help="RZA1H_DEBUG devices to log (comma list, "
                                     "e.g. fpga,ssif,scif3fp); sends QEMU stderr to --log")
    ap.add_argument("--log", default="/tmp/qemu_run_gui.log",
                     help="where to write QEMU's stderr when --debug is given")
    args = ap.parse_args()

    if args.fast and args.icount:
        ap.error("--fast and --icount are mutually exclusive")

    if args.fast:
        icount = "shift=1,sleep=off"
    elif args.icount:
        icount = None if args.icount == "off" else args.icount
    else:
        icount = DEFAULT_ICOUNT

    env = dict(os.environ)
    if args.no_audio:
        env["RZA1H_SSIF"] = "off"
    if args.tone is not None:
        env["RZA1H_AF_TONE"] = args.tone
    if args.noise is not None:
        env["RZA1H_AF_NOISE"] = args.noise
    if args.af is not None:
        env["RZA1H_FP_AF"] = str(args.af)
    if args.rfsql is not None:
        env["RZA1H_FP_RFSQL"] = str(args.rfsql)
    if args.fpga_sweep_hz is not None:
        env["RZA1H_FPGA_SWEEP_HZ"] = args.fpga_sweep_hz
    if args.fpga_signals is not None:
        env["RZA1H_FPGA_SIGNALS"] = args.fpga_signals
    if args.no_mouse:
        env["RZA1H_FP_NO_MOUSE"] = "1"
    if args.no_keys:
        env["RZA1H_FP_NO_KEYS"] = "1"
    if args.debug:
        env["RZA1H_DEBUG"] = args.debug

    sock = "/tmp/qemu_run_gui.sock"
    fpsock = "/tmp/qemu_run_gui_fp.sock"
    Path(sock).unlink(missing_ok=True)
    Path(fpsock).unlink(missing_ok=True)

    image = HERE / ("riic2_eeprom.img" if args.no_pwrk else "riic2_eeprom_pwrk_test.img")
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-display", args.display, "-kernel", str(FLASH),
        "-monitor", "none",
        "-global", f"rza1h-riic.image={image}",
        "-qmp", f"unix:{sock},server,nowait",
        "-chardev", f"socket,id=fpctl,path={fpsock},server=on,wait=off",
    ]
    if icount:
        qemu_args += ["-icount", icount]
    if args.civ:
        Path(args.civ).unlink(missing_ok=True)
        qemu_args += ["-chardev", f"socket,id=civ,path={args.civ},server=on,wait=off",
                       "-serial", "chardev:civ"]
    else:
        qemu_args += ["-serial", "none"]

    stderr = open(args.log, "w") if args.debug else subprocess.DEVNULL
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL, stderr=stderr, env=env)
    try:
        time.sleep(1.0)
        s = qmp_open(sock)
        if not args.no_pwrk:
            t0 = time.time()
            while time.time() - t0 < 15 and pc(s) != 0x20029B18:
                time.sleep(0.1)
            qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
            print("PWRK pressed and held -- the main screen comes up after roughly 4s of "
                  "emulated time (~12s wall with the default pacing; less with --fast).")
        else:
            print("--no-pwrk: booting straight up (no power-key dance).")
        if args.debug:
            print(f"QEMU stderr (RZA1H_DEBUG={args.debug}) -> {args.log}")
        if args.civ:
            print(f"CI-V on {args.civ} -- talk to it with tools/civ.py {args.civ} ... "
                  f"(radio address 0x94)")
        print(FRONT_PANEL_HELP, end="")
        print(f"Front panel: click the screen to touch; keys/knobs: "
              f"RZA1H_FPCTL={fpsock} tools/fp.py press MENU")
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
