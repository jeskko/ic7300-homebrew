#!/usr/bin/env python3
"""Dumps guest RAM 0x20000000-0x20004fff (the "early-boot blind spot" this session's hotblocks
heat-map found dominating >80% of every non-trap capture window, but which body.bin's own Ghidra
project has zero coverage of -- see README.md's Status section) via QMP, fully GDB-free (no
vm_stop(), matching this project's own established zero-perturbation technique for exactly this
kind of question -- see qmp_read_mem.py's own header comment for the precedent).

Polls `info registers` at a tight interval (safe here -- QMP's human-monitor-command path is
confirmed, in this project's own prior sessions, not to call vm_stop() at all, unlike GDB) until
PC first lands inside the target range, then immediately `pmemsave`s the whole 0x5000-byte region
to a local file and reports the exact PC/registers at that moment for context.

Usage: dump_early_boot_ram.py [output_file] [max_seconds]
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"

RANGE_LO = 0x20000000
RANGE_HI = 0x20004FFF
RANGE_SIZE = RANGE_HI - RANGE_LO + 1  # 0x5000


def qmp_connect(sock_path: str):
    # Line-buffered file object, not raw recv() -- a raw recv(65536) doesn't guarantee getting
    # exactly one complete QMP JSON message per call (each message IS newline-terminated, but
    # the kernel can hand back a read in more than one chunk); found needing this the hard way,
    # live: a raw-recv version occasionally handed a *stale* buffered "info registers" reply
    # back as the *next* command's own response. Same fix qmp_read_mem.py already uses.
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    f = s.makefile("rwb")

    def read_reply():
        # QMP interleaves asynchronous "event" messages (e.g. RESUME, triggered by our own
        # "cont") with synchronous command replies on the SAME socket -- found live, the hard
        # way: a naive single-readline() read after "cont" got the RESUME event instead of
        # cont's own {"return": ...}, silently offsetting every later reply by one exchange
        # (the *next* command's reply looked like the wrong command's answer). Skip any message
        # that isn't a command reply (no "return"/"error" key) until we find one.
        while True:
            msg = json.loads(f.readline())
            if "return" in msg or "error" in msg:
                return msg

    f.readline()  # greeting -- has neither "return" nor "error", read directly, not via
                  # read_reply()'s skip-events loop (which would otherwise block forever
                  # waiting for a line that will never come before the next real command)
    f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode())
    f.flush()
    read_reply()
    return f, read_reply


def hmp(conn, command_line: str) -> str:
    f, read_reply = conn
    cmd = {"execute": "human-monitor-command",
           "arguments": {"command-line": command_line}}
    f.write((json.dumps(cmd) + "\n").encode())
    f.flush()
    return read_reply().get("return", "")


def parse_pc(regs_text: str) -> int | None:
    for line in regs_text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    return None


def main():
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/tmp/early_boot_ram.bin")
    max_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

    sock_path = "/tmp/qemu_earlyram.sock"
    Path(sock_path).unlink(missing_ok=True)
    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
        "-S",  # start paused -- otherwise the guest runs during our own connect-time delay and
               # this early phase (short enough that a 1s warmup sleep alone was long enough to
               # miss it entirely, confirmed live) is gone before polling even starts
    ]
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        # Connect as soon as the socket exists -- CPU is paused (-S) so there's no race yet.
        s = None
        for _ in range(100):
            try:
                s = qmp_connect(sock_path)
                break
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.05)
        if s is None:
            print("Could not connect to QMP socket")
            return 1

        hmp(s, "cont")
        start = time.time()
        hit_pc = None
        samples = 0
        while time.time() - start < max_seconds:
            regs = hmp(s, "info registers")
            pc = parse_pc(regs)
            samples += 1
            if pc is not None and RANGE_LO <= pc <= RANGE_HI:
                hit_pc = pc
                break
        elapsed = time.time() - start
        if hit_pc is None:
            print(f"Never observed PC inside 0x{RANGE_LO:08x}-0x{RANGE_HI:08x} within "
                  f"{max_seconds:.0f}s ({samples} samples) -- dumping the range anyway for a "
                  f"look, but it may not reflect what was executing there.")
        else:
            print(f"PC=0x{hit_pc:08x} at t={elapsed:.2f}s ({samples} samples to find it) -- "
                  f"dumping now.")

        # Filename must be quoted -- confirmed live: an unquoted path makes the monitor's
        # expression parser choke on the first non-hex-digit character in it ("invalid char
        # 't' in expression", from "/tmp/..."), even though args_type says filename is a
        # plain string ('s'). Not documented anywhere obvious; found by direct trial.
        dump_reply = hmp(s, f'pmemsave 0x{RANGE_LO:x} 0x{RANGE_SIZE:x} "{out_path}"')
        if dump_reply.strip():
            print(f"pmemsave reply: {dump_reply.strip()}")
        s[0].close()

        if out_path.exists() and out_path.stat().st_size == RANGE_SIZE:
            print(f"Wrote {RANGE_SIZE} bytes to {out_path}")
        else:
            print(f"WARNING: {out_path} missing or wrong size "
                  f"({out_path.stat().st_size if out_path.exists() else 'missing'} bytes, "
                  f"expected {RANGE_SIZE})")
            return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
