#!/usr/bin/env python3
"""Fully GDB-free physical-memory read via QMP's `human-monitor-command` -> `xp`.

Built 2026-09-10 while cross-checking `read_fun200b5f38_flags.py`'s finding (a GDB
interrupt()+read_memory sample showed the DMAC busy flag stuck at 0x01). Since this whole
project has already documented one real QEMU gdbstub reliability artifact (the DMAC/icount
"stall", see README-history.md), any surprising "stuck forever" finding is worth cross-checking
against a path with *zero* gdbstub involvement at all before trusting it -- QMP's own
monitor-command interface is a completely separate code path from the `-s -gdb` remote-serial
stub `gdbrsp.py` drives, so a result that agrees across both is on much firmer ground than
either alone. This is what actually confirmed the DMAC completion race (see dmac.c's own
"CORRECTED, 2026-09-10" comment) -- read the flag with this, change the suspect constant,
rebuild, re-read with this again, no breakpoint anywhere near the code in question either time.

Requires QEMU launched with e.g. `-qmp unix:/tmp/some.sock,server,nowait` (no `-S`/`-gdb`
needed at all -- can run fully hands-off, which is the whole point).

Usage: qmp_read_mem.py <socket_path> <addr> [<addr> ...]
  e.g. qmp_read_mem.py /tmp/qemu.sock 0x203906ee 0x203906ed
"""

from __future__ import annotations

import json
import socket
import sys


def qmp_connect(sock_path: str):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    f = s.makefile("rwb")

    def read_json():
        return json.loads(f.readline())

    read_json()  # greeting
    f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode())
    f.flush()
    read_json()
    return f, read_json


def hmp(f, read_json, command_line: str) -> str:
    cmd = {"execute": "human-monitor-command",
           "arguments": {"command-line": command_line}}
    f.write((json.dumps(cmd) + "\n").encode())
    f.flush()
    return read_json().get("return", "")


def read_byte(f, read_json, addr: int) -> int:
    """Physical-address single-byte read, no side effects, no gdbstub involved."""
    reply = hmp(f, read_json, f"xp /1xb 0x{addr:x}")
    # reply looks like "<addr>: 0x<val>\r\n"
    return int(reply.strip().split()[-1], 16)


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    sock_path = sys.argv[1]
    addrs = [int(a, 0) for a in sys.argv[2:]]

    f, read_json = qmp_connect(sock_path)
    for addr in addrs:
        val = read_byte(f, read_json, addr)
        print(f"0x{addr:08x}: 0x{val:02x}")


if __name__ == "__main__":
    main()
