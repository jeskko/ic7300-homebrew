#!/usr/bin/env python3
"""Force a direct call into firmware_update_main (0x20025ae4), bypassing
the whole UI/SD-menu-navigation stack, to see whether it (and, through it,
this project's own file-RPC/VFS layer) actually reaches mmc.c at all --
the concrete next step this session's README flags after finding zero
static or dynamic references to the MMCIF base address during ordinary
boot.

firmware_update_main takes no arguments (confirmed via decompile: it
opens a fixed global path, DAT_200264a0, which has no static writer
anywhere in body.bin -- almost certainly populated at runtime by the
generic SD-card file-browser/selection UI, a separate subsystem this
script deliberately doesn't try to trace or drive). Since it's a pure
function of that one global, this script just writes a plausible path
string into the RAM DAT_200264a0 already points to (real, writable RAM
in this machine, simply past the end of the static image -- confirmed
via tools/icom_fw's own decompressor that this address is genuinely
runtime-only, not a Ghidra memory-map gap) and jumps straight to the
function's entry point.

Return-address trick: since gdbrsp.py has no breakpoint support, LR is
pointed at a scratch RAM word this script writes a `b .` (branch-to-self,
0xEAFFFFFE) into first -- so "did firmware_update_main return" becomes
"is PC sitting at that exact address", unambiguous and pollable the same
way this session's idle-loop-escape checks already are.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdbrsp import GdbRsp

FUP_MAIN = 0x20025ae4
PATH_BUF = 0x203d86c4  # DAT_200264a0's target, see module comment
TRAMPOLINE = 0x209F0000  # arbitrary, well past the static image, unused RAM


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "UPDATE.DAT"
    g = GdbRsp(port=1234)

    regs = g.read_registers()
    print(f"before: PC={regs['r15']:08x} CPSR={regs['cpsr']:08x} SP={regs['r13']:08x}")

    path_bytes = path.encode("ascii") + b"\x00"
    g.write_memory(PATH_BUF, path_bytes)
    print(f"wrote path {path!r} ({len(path_bytes)} bytes) to {PATH_BUF:#x}")

    g.write_u32(TRAMPOLINE, 0xEAFFFFFE)  # b . (branch to self)

    new_regs = dict(regs)
    new_regs["r14"] = TRAMPOLINE
    new_regs["r15"] = FUP_MAIN
    # CPSR: keep mode/flags, just ensure T (bit5) is clear -- firmware_update_main
    # is ARM-mode code (confirmed via listing: 4-byte instructions).
    new_regs["cpsr"] = regs["cpsr"] & ~(1 << 5)
    g.write_registers(new_regs)
    print(f"forced PC={FUP_MAIN:#x} LR={TRAMPOLINE:#x}, continuing...")

    g.cont()
    for i in range(20):
        time.sleep(0.5)
        g.interrupt()
        g.wait_stop()
        r = g.read_registers()
        print(f"  t={(i + 1) * 0.5:.1f}s: PC={r['r15']:08x} CPSR={r['cpsr']:08x}")
        if r["r15"] == TRAMPOLINE:
            print("firmware_update_main RETURNED (PC at trampoline).")
            print(f"  r0 (return value) = {r['r0']:#x}")
            break
        g.cont()
    else:
        print("still running / never returned within 10s -- see above trace.")

    g.close()


if __name__ == "__main__":
    main()
