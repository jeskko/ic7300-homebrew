#!/usr/bin/env python3
"""Live check for the icom-main-idle-loop-not-reached thread (2026-09-21 session).

Static trace found main_idle_loop's own entry wait-loop (`while (A||B||C) FUN_20062c1c();`)
blocks on a real SVC-based kernel wait (confirmed via arm-none-eabi-objdump against
scratch/unpacked/142/body.bin, correcting Ghidra's own known ARM/Thumb-disassembly-bug
corruption of FUN_20062c1c's decompile). Condition C (`FUN_200b521c`) is gated on two byte
flags, resolved via their real (double-indirected) addresses: 0x203906ed and 0x203906ee --
the loop only proceeds once BOTH read zero.

0x203906ed turned out to already be a known quantity: it's the "shared job ring" active flag
(`DAT_200b1cac` in `shared_job_ring_dispatch`, `0x200b0f68`) -- a *different* ring from the
already-fixed RIIC2/EEPROM overflow ring (`0x20420120`, see trace_job_ring_overflow.py) --
this one backs the SCIF5 DSP-link command/reply protocol (`scif5_send_and_wait_reply` and
friends, see scif.c's own module comment). scif.c's history already documents (2026-09-09)
that `scif5_cmd_transmit_now`'s own busy-wait on this identical flag was previously observed
stuck, then fixed via a real virtual-DSP-ack timing fix + MTU2 modeling ("the fourth [MTU2
event] unblocks scif5_cmd_transmit_now" per the current peripheral table). Given how much has
changed since (ring-overflow fix, MTU2/DMAC/riic timing fixes), it's not safe to assume the
flag is *still* stuck now without checking live -- that's what this script does.

Usage: trace_main_idle_wait_flags.py [seconds] [--hold-pwrk]
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
GPIO_PATH = "/machine/gpio"

FLAG_ED = 0x203906ed  # shared job ring "active/non-empty" flag
FLAG_EE = 0x203906ee  # sibling flag, no static writer found yet


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
    return json.loads(s.recv(1 << 20).decode())["return"]


def qmp_cmd(s: socket.socket, execute: str, **arguments):
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
    return json.loads(s.recv(65536).decode())


def read_u8(s: socket.socket, addr: int) -> int:
    reply = hmp(s, f"xp /1xb 0x{addr:x}")
    return int(reply.strip().split()[1], 16)


def pc(s: socket.socket) -> int:
    text = hmp(s, "info registers")
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("R15 not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timeout_s", nargs="?", type=float, default=60.0)
    ap.add_argument("--poll-interval", type=float, default=1.0)
    ap.add_argument("--hold-pwrk", action="store_true")
    args = ap.parse_args()

    riic_image = "riic2_eeprom_pwrk_test.img" if args.hold_pwrk else "riic2_eeprom.img"
    sock_path = "/tmp/qemu_idle_wait_flags.sock"
    Path(sock_path).unlink(missing_ok=True)
    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={HERE / riic_image}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)

        if args.hold_pwrk:
            t0 = time.time()
            while time.time() - t0 < 10:
                time.sleep(0.1)
                if pc(s) == 0x20029B18:
                    break
            qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
            print("PWRK pressed and held")

        print(f"Polling flag_ed=0x{FLAG_ED:x} flag_ee=0x{FLAG_EE:x} + PC for up to "
              f"{args.timeout_s:.0f}s (every {args.poll_interval}s)...")
        t0 = time.time()
        last = None
        while time.time() - t0 < args.timeout_s:
            time.sleep(args.poll_interval)
            ed = read_u8(s, FLAG_ED)
            ee = read_u8(s, FLAG_EE)
            cur_pc = pc(s)
            state = (ed, ee)
            if state != last:
                elapsed = time.time() - t0
                print(f"t={elapsed:5.1f}s  flag_ed={ed}  flag_ee={ee}  PC=0x{cur_pc:08x}")
                last = state
        print("done.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
