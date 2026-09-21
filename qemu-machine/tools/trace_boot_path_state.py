#!/usr/bin/env python3
"""GDB-free (pure QMP) probe of the whole power_state_dispatch -> cold_boot_mode_dispatch ->
main_idle_loop boot-path state chain, for the icom-main-idle-loop-not-reached thread.

Static facts this probe tests (all re-derived 2026-09-21 from Ghidra + literal-pool dumps):

  power_state_dispatch (0x2002b29c)
      *0x2039030f = 0                       <- boot/power state byte
      ... then either ...
      cold_boot_mode_dispatch (0x2002b1c8): *0x2039030f = 1 ; cold_boot_hw_init() ;
          loop { if (*0x20390310 == 0) switch (*0x203902de) { 1,4,5 -> svc idle loops ;
                                                              default -> main_idle_loop() } }
          (loop only entered at all if *0x20390311 == 0)
      ... or ...
      power_state_pwrk_wait_and_bringup (0x20029ca4): *0x2039030f = 2 while waiting for
          PWRK; 3 = press serviced; finally *0x20390311 = 1 and RETURNS.

  main_idle_loop (0x20052e30) entry:
      *(0x203def00+0x37c) = *0x203df27c = 0          (cond A cleared)
      FUN_20062c28(): *0x20390315 = 0 ; *0x20390316 = 1   (cond B made FALSE!)
      *0x20390326 = 0                                 (u16 per-iteration counter)
      while (A || ((*0x20390315 < 2 && *0x20390316 == 0) || C)) rtos_wait_obj1_forever();
      ... body ... ; *0x20390326 += 1 each pass

  NOTE the polarity of cond B: the loop KEEPS WAITING while 0x20390315 < 2 AND
  0x20390316 == 0.  Both reading zero forever means "no work ticks ever arrived",
  NOT "condition satisfied".

  cond C = FUN_200b521c() != 0 <=> *0x203906ed != 0 || *0x203906ee != 0.

Usage: trace_boot_path_state.py [seconds] [--no-pwrk] [--interval S]
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
GPIO_PATH = "/machine/unattached/device[14]"

U8 = [
    ("state_30f", 0x2039030F),   # 0=init 1=cold-boot path 2=PWRK wait 3=press serviced
    ("pwrk_1ef", 0x203901EF),    # DAT_2002a0dc+6, debounced PWRK active
    ("b_305", 0x20390305),
    ("modereq_310", 0x20390310), # DAT_2002a158 / DAT_2002b4fc
    ("pwrdn_311", 0x20390311),   # DAT_2002a104 / DAT_2002b4f8 / main_idle_loop break flag
    ("idlesel_2de", 0x203902DE), # DAT_2002a4a4 idle-loop variant selector
    ("condA_df27c", 0x203DF27C),
    ("condB_315", 0x20390315),
    ("condB_316", 0x20390316),
    ("condC_6ed", 0x203906ED),
    ("condC_6ee", 0x203906EE),
]
U16 = [
    ("iter_326", 0x20390326),    # main_idle_loop per-iteration counter
]


def qmp_open(sock_path: str) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
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


def read_u8(s, addr):
    return int(hmp(s, f"xp /1xb 0x{addr:x}").strip().split()[1], 16)


def read_u16(s, addr):
    return int(hmp(s, f"xp /1xh 0x{addr:x}").strip().split()[1], 16)


def pc(s: socket.socket) -> int:
    text = hmp(s, "info registers")
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("R15 not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timeout_s", nargs="?", type=float, default=90.0)
    ap.add_argument("--no-pwrk", action="store_true")
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--riic-log", action="store_true")
    args = ap.parse_args()

    image = HERE / ("riic2_eeprom.img" if args.no_pwrk else "riic2_eeprom_pwrk_test.img")
    sock_path = "/tmp/qemu_boot_path_state.sock"
    log_path = Path("/tmp/qemu_boot_path_state.log")
    Path(sock_path).unlink(missing_ok=True)
    log_path.unlink(missing_ok=True)

    env = os.environ.copy()
    if args.riic_log:
        env["RZA1H_DEBUG"] = "riic"
    log_f = open(log_path, "wb")

    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={image}",
        "-icount", "shift=auto",
        "-qmp", f"unix:{sock_path},server,nowait",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL, stdout=log_f, stderr=log_f,
                            env=env)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)

        if not args.no_pwrk:
            t0 = time.time()
            while time.time() - t0 < 15:
                time.sleep(0.1)
                if pc(s) == 0x20029B18:
                    break
            qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
            print("PWRK pressed and held")

        print("t(s)   " + "  ".join(n for n, _ in U8 + U16) + "   PC")
        t0 = time.time()
        last = None
        while time.time() - t0 < args.timeout_s:
            vals = [read_u8(s, a) for _, a in U8] + [read_u16(s, a) for _, a in U16]
            cur_pc = pc(s)
            if vals != last:
                el = time.time() - t0
                cells = "  ".join(f"{v:>{max(len(n),3)}d}" for (n, _), v in zip(U8 + U16, vals))
                print(f"{el:6.1f} {cells}   0x{cur_pc:08x}")
                last = vals
            time.sleep(args.interval)
        print("done.")
        if args.riic_log:
            log_f.flush()
            text = log_path.read_text(errors="replace")
            print("riic1 lines:", sum(1 for l in text.splitlines() if "riic1" in l.lower()))
            print("riic2 lines:", sum(1 for l in text.splitlines() if "riic2" in l.lower()))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log_f.close()


if __name__ == "__main__":
    main()
