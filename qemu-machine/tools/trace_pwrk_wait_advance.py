#!/usr/bin/env python3
"""Drives the PWRK-wait boot branch (`power_state_pwrk_wait_and_bringup`, forced via
`riic2_eeprom_pwrk_test.img`) with one or more live button presses via the `pwrk-pressed` QOM
property, then traces PC at fine granularity over a longer window to see whether firmware
actually advances *past* the `0x20029b18` `wfi` landing point after being woken -- toward its own
finalization or the shared `idle_loop_wfe_spin` (`0x200b939c`) the auto-power-on branch reaches --
or just re-enters the identical `wfi` unchanged. Fully QMP/HMP-only, no GDB, no breakpoints (this
project's own established practice: GDB/heavy polling has repeatedly been shown to perturb timing
in this codebase -- see README.md).

Usage: trace_pwrk_wait_advance.py [window_s] [poll_interval_s] [--presses N] [--press-gap S]
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom_pwrk_test.img"

GPIO_PATH = "/machine/unattached/device[14]"  # confirmed via `info qom-tree` -- (rza1h-gpio)

WFI_LANDING = 0x20029B18
PWRK_WAIT_LO, PWRK_WAIT_HI = 0x20029914, 0x20029DE7
IDLE_LOOP_LO, IDLE_LOOP_HI = 0x200B939C, 0x200B93B0

# Live flag addresses -- resolved 2026-09-20 by reading the actual pointer constants each
# DAT_2002axxx symbol holds (Ghidra shows these as double-indirected pool loads: the pool slot
# at e.g. 0x2002a0dc itself holds a pointer, dereferenced by the decompiled C). Confirmed live via
# QMP `xp /1xw <pool addr>` against a running boot -- these are RAM/BSS targets, not further pool
# slots. See README.md's PWRK section for the source-level derivation.
FLAG_PRESS_ACTIVE = 0x203901EF   # *(DAT_2002a0dc + 6): debounced "PWRK still active" byte,
                                  # set 1 on initial press, updated by pwrk_irq7_isr on release
FLAG_CIV_STATE = 0x2039030F      # *DAT_2002a0e8: '\x02' set before the wait loop, '\x03' needed
                                  # to actually run the CI-V/SCIF1 servicing sub-loop after wake
FLAG_FINALIZED = 0x20390311      # *DAT_2002a104: set to 1 on successful bring-up completion


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
    return json.loads(s.recv(65536).decode())["return"]


def qmp_cmd(s: socket.socket, execute: str, **arguments):
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
    return json.loads(s.recv(65536).decode())


def read_pc(s: socket.socket) -> int:
    text = hmp(s, "info registers")
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("R15 not found in `info registers` output")


def read_byte(s: socket.socket, addr: int) -> int:
    reply = hmp(s, f"xp /1xb 0x{addr:x}")
    return int(reply.strip().split()[1], 16)


def read_flags(s: socket.socket) -> tuple[int, int, int]:
    return (read_byte(s, FLAG_PRESS_ACTIVE), read_byte(s, FLAG_CIV_STATE),
            read_byte(s, FLAG_FINALIZED))


def set_pwrk_pressed(s: socket.socket, pressed: bool):
    qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=pressed)


def describe(pc: int) -> str:
    if pc == WFI_LANDING:
        return "AT the known wfi landing point"
    if PWRK_WAIT_LO <= pc <= PWRK_WAIT_HI:
        return "elsewhere inside power_state_pwrk_wait_and_bringup"
    if IDLE_LOOP_LO <= pc <= IDLE_LOOP_HI:
        return "*** in idle_loop_wfe_spin -- the shared RTOS idle task! ***"
    return "OUTSIDE both known ranges -- new territory"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("window_s", nargs="?", type=float, default=30.0,
                     help="how long to trace PC after each press cycle")
    ap.add_argument("poll_interval_s", nargs="?", type=float, default=0.2)
    ap.add_argument("--presses", type=int, default=1,
                     help="number of press/release cycles to try, each followed by its own"
                          " window_s observation")
    ap.add_argument("--press-gap", type=float, default=0.2,
                     help="seconds pwrk-pressed stays true before release")
    args = ap.parse_args()

    sock_path = "/tmp/qemu_pwrk_advance.sock"
    Path(sock_path).unlink(missing_ok=True)
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=auto",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)

        print("Waiting to reach the known wfi landing point (0x20029b18)...")
        t0 = time.time()
        pc = None
        while time.time() - t0 < 10.0:
            time.sleep(0.1)
            pc = read_pc(s)
            if pc == WFI_LANDING:
                break
        print(f"t={time.time() - t0:.2f}s  pc=0x{pc:08x}  {describe(pc)}")
        if pc != WFI_LANDING:
            print("Never reached the expected landing point -- aborting trace, check the image"
                  " and boot path.")
            return

        flags = read_flags(s)
        print(f"flags before any press: press_active={flags[0]} civ_state={flags[1]}"
              f" finalized={flags[2]}")

        for cycle in range(1, args.presses + 1):
            print(f"\n--- press/release cycle {cycle}/{args.presses} ---")
            set_pwrk_pressed(s, True)
            time.sleep(args.press_gap)
            set_pwrk_pressed(s, False)
            print("released -- rising edge should fire IRQ7 now")

            start = time.time()
            last_pc = None
            last_flags = None
            reached_idle = False
            reached_final = False
            while time.time() - start < args.window_s:
                time.sleep(args.poll_interval_s)
                elapsed = time.time() - start
                pc = read_pc(s)
                flags = read_flags(s)
                if pc != last_pc or flags != last_flags:
                    print(f"  t={elapsed:6.2f}s  pc=0x{pc:08x}  {describe(pc)}"
                          f"  press_active={flags[0]} civ_state={flags[1]}"
                          f" finalized={flags[2]}")
                    last_pc, last_flags = pc, flags
                if IDLE_LOOP_LO <= pc <= IDLE_LOOP_HI:
                    reached_idle = True
                if flags[2] != 0:
                    reached_final = True
            if reached_final:
                print(f"cycle {cycle}: FINALIZED (DAT_2002a104=1) -- bring-up completed!")
            if reached_idle:
                print(f"cycle {cycle}: reached idle_loop_wfe_spin -- stable steady-state hit.")
            elif last_pc == WFI_LANDING:
                print(f"cycle {cycle}: settled right back at the identical wfi landing point"
                      f" (flags: press_active={last_flags[0]} civ_state={last_flags[1]}"
                      f" finalized={last_flags[2]}).")
            else:
                print(f"cycle {cycle}: window ended at pc=0x{last_pc:08x} ({describe(last_pc)}).")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        Path(sock_path).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
