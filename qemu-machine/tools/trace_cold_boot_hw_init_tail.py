#!/usr/bin/env python3
"""Handoff prep, 2026-09-09 -- NOT yet run, prepared for a fresh session to pick up.

Corrects a wrong conclusion from earlier the same day: `force_call_fup.py`'s retest attributed
the SD-card path's blocker to `FUN_2002b29c`'s branch decision (the 2026-09-08 session's own
diagnosis) never routing to `cold_boot_mode_dispatch`. That's now shown to be WRONG --
`cold_boot_mode_dispatch`'s own decompile (`0x2002b1c8`) shows `cold_boot_hw_init()` called as
its very first, plain synchronous statement, and `cold_boot_hw_init` demonstrably DOES run (all
of today's DMAC/MTU2/RIIC/SCIF/RSPI2 findings happen inside it) -- so `cold_boot_mode_dispatch`,
and therefore `FUN_2002b29c`'s gate, has already been entered and passed. `system_mode_request_
dispatch()` (which creates `sdcard_file_rpc_dispatch_task`, per the 2026-09-08 session) is only
reached AFTER `cold_boot_hw_init()` returns -- and it never does. The real, current blocker is
somewhere in `cold_boot_hw_init`'s own remaining body, specifically the ~25 calls after
`FUN_200b5f38()` (0x2002b064, the last of the DMAC-related calls already fully traced and
confirmed working today) through its own real return at 0x2002b1c4.

Method: the exact "waypoint breakpoint sweep across a function's own body" technique the
2026-09-08 session already used successfully to localize `FUN_2002b29c`'s own stall (see
README-history.md's "riic2_driver_init... FUN_2001dd58 is the real stall" section) -- one
breakpoint at the address immediately following each `bl`/`blx` call site in this remaining
stretch (not inside any callee -- these are one-shot, called-once-per-boot init/precheck steps,
not busy-wait loops, so no per-iteration masking risk the way DMAC's own tight loop had), plus
one at the function's own real return (0x2002b1c4). Free-runs and reports which waypoints
actually get reached, in what order and at what real elapsed time, before either reaching the
final return or timing out -- directly localizing which specific call is the one that never
returns, the same way the 2026-09-08 sweep found `riic2_driver_init`'s own internal busy-wait.

Waypoints listed in address order as they appear in `cold_boot_hw_init`'s raw listing
(`0x2002afc0`-`0x2002b1c4`); some are on conditional branches taken only for specific EEPROM/
mode-combo outcomes, included anyway since which path is actually taken is itself part of what
this sweep should reveal. Real, human-readable names filled in where already established
elsewhere in this project's notes (`scif5_dsp_link_driver_init`, `dsp_boot_handshake`,
`rspi2_driver_init`, `tuner_jack_signal_precheck`, etc.); everything else is still `FUN_xxx` in
Ghidra as of this session and should probably get named as this sweep narrows things down.

Usage: trace_cold_boot_hw_init_tail.py [seconds]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

# (address, label) -- the address right after each call site from 0x2002b064 onward.
#
# IMPORTANT, added after this tool's own first smoke tests (2026-09-09): the very first version
# of this list started at 0x2002b088, on the assumption that everything through FUN_200b5f38's
# own call (0x2002b064) "already works" since 0x2002b060 (FUN_200b5ea4's own call, the DMAC
# function this whole session's GDB-artifact investigation covered) does. That assumption was
# never actually checked -- FUN_200b5f38 is a SIBLING of FUN_200b5ea4 (same "arm a transfer,
# wait for completion" shape, confirmed via references_to on the shared 0x203906EE flag), and
# was never independently verified to complete. Two smoke tests (15s, 60s) with the original
# waypoint-88-onward list got ZERO hits at all, including the very first one -- far more likely
# explained by FUN_200b5f38 itself never returning than by all ~25 downstream calls somehow being
# unreachable. 0x2002b068 (right after FUN_200b5f38's own call) is now the first waypoint for
# exactly this reason -- check this one first.
WAYPOINTS = [
    (0x2002b068, "after FUN_200b5f38 (the DMAC function's own untested sibling -- CONFIRMED "
                 "2026-09-09/10 to never reach here at all, via a GDB-free RZA1H_DEBUG=dmac run "
                 "showing only one arm+complete event ever. Use tools/trace_fun200b5f38_wait.py "
                 "instead of this waypoint -- it brackets FUN_200b5f38's own internal loop 1/"
                 "loop 2 directly and is the real next step, not this coarser sweep)"),
    (0x2002b088, "after scif5_dsp_link_driver_init (bl 0x200b2bb0)"),
    (0x2002b08c, "after scif5_wait_hsk1_ready (bl 0x200b48e4)"),
    (0x2002b090, "after dsp_boot_handshake (bl 0x200b5aa0)"),
    (0x2002b094, "after dsp_cmd_table_init (bl 0x200b1540)"),
    (0x2002b098, "after dsp_identity_query_record0 (bl 0x200b26b4)"),
    (0x2002b09c, "after dsp_identity_query_record1 (bl 0x200b27f0)"),
    (0x2002b0a0, "after dsp_identity_query_record2 (bl 0x200b292c)"),
    (0x2002b0a4, "after rspi2_driver_init (bl 0x200b665c)"),
    (0x2002b0a8, "after FUN_200b7020"),
    (0x2002b0ac, "after FUN_2005f8ac"),
    (0x2002b0b0, "after FUN_2005fcb4"),
    (0x2002b0b4, "after FUN_2005f9b0"),
    (0x2002b0b8, "after FUN_2005fac4"),
    (0x2002b0bc, "after FUN_200605fc"),
    (0x2002b0c0, "after FUN_200291d8 (3rd EEPROM signature check, 0x3fc0)"),
    # 0x2002b0c8-0x2002b0dc: only taken if the above returned nonzero
    (0x2002b0cc, "after FUN_200291a8 (conditional branch)"),
    (0x2002b0d0, "after FUN_200291c8 (conditional branch)"),
    (0x2002b0d4, "after FUN_20008328 (conditional branch)"),
    (0x2002b0dc, "after FUN_2000790c (conditional branch)"),
    (0x2002b0e0, "after FUN_20029178 (conditional branch) / branch target"),
    (0x2002b0e8, "after FUN_20006c74(1)"),
    (0x2002b0f0, "after FUN_2000a0a8"),
    (0x2002b0f4, "after FUN_2000a264"),
    (0x2002b0f8, "after FUN_200609c0"),
    (0x2002b0fc, "after FUN_2006756c"),
    (0x2002b100, "after FUN_20035af4"),
    (0x2002b104, "after FUN_2001a104"),
    (0x2002b108, "after FUN_2001f6bc"),
    (0x2002b114, "after FUN_20029308"),
    (0x2002b118, "after FUN_2003c530 (mode-combo dispatch)"),
    (0x2002b124, "after FUN_2002aeb4 (conditional branch)"),
    (0x2002b134, "after FUN_20029224 (SX3765 V4.81-000 signature check)"),
    (0x2002b140, "after FUN_2002a698 (alt branch)"),
    (0x2002b148, "after FUN_2002aeac (conditional branch)"),
    (0x2002b158, "after FUN_20029270 ('Partial' signature check)"),
    (0x2002b164, "after FUN_2002a678 (alt branch) / branch target"),
    (0x2002b168, "after FUN_200292bc"),
    (0x2002b174, "after FUN_2002c1e0 (conditional branch)"),
    (0x2002b178, "after FUN_2001a2f4 (conditional branch)"),
    (0x2002b17c, "after FUN_20029198 (conditional branch) / branch target"),
    (0x2002b180, "after tuner_jack_signal_precheck (bl 0x20066154)"),
    (0x2002b184, "after FUN_2002ae6c (emergency_screen_checkbox_state_sync?)"),
    (0x2002b1a8, "after FUN_2002ae18 (conditional branch)"),
    (0x2002b1ac, "after FUN_2002add0 (conditional branch)"),
    (0x2002b1b0, "after FUN_2002ad18 (conditional branch)"),
    (0x2002b1c4, "*** cold_boot_hw_init RETURNS *** (real function exit, ldmia sp!,{r4,r5,r6,pc})"),
]


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0

    if not RIIC_IMAGE.exists():
        print(f"missing {RIIC_IMAGE} -- run tools/build_riic_eeprom_image.py first",
              file=sys.stderr)
        sys.exit(1)

    proc = launch_qemu(["-global", f"rza1h-riic.image={RIIC_IMAGE}"])
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, f"not a fresh boot, PC={regs['r15']:#x}"

        addr_to_label = dict(WAYPOINTS)
        for addr in addr_to_label:
            g.set_breakpoint(addr)
        g.cont()

        start = time.time()
        seen = []
        print(f"free-running, watching {len(WAYPOINTS)} waypoints across cold_boot_hw_init's "
              f"remaining body -- up to {total_seconds:.0f}s")
        while True:
            remaining = total_seconds - (time.time() - start)
            if remaining <= 0:
                print(f"\nTIMEOUT after {total_seconds:.0f}s -- last waypoint reached: "
                      f"{seen[-1] if seen else '(none)'}")
                if not seen:
                    print("**ZERO waypoints hit at all, including the very first one right "
                          "after already-confirmed-working code (FUN_200b5f38) -- confirmed "
                          "live 2026-09-09, twice (15s and 60s smoke tests). Before concluding "
                          "the code itself is stuck there: this project's whole session that "
                          "same day found GDB observation overhead can itself distort timing "
                          "under -icount (the DMAC false-stall). 46 SIMULTANEOUS breakpoints is "
                          "a lot more than any single trial that worked cleanly earlier that "
                          "session (2-3 at most) -- try a much smaller subset first (e.g. just "
                          "the first 5-10 waypoints, or a coarse-to-fine approach like "
                          "trace_job_ring_overflow.py's own [poll_interval]/[fine_start] "
                          "pattern) before trusting this as a real finding, not an artifact.**")
                else:
                    print("The call immediately after the last-reached waypoint is the "
                          "concrete next place to look -- decompile/trace it directly, the same "
                          "way FUN_2001dd58 was found inside riic2_driver_init's own call chain "
                          "on 2026-09-08.")
                return
            try:
                g.wait_stop(timeout=remaining)
            except (TimeoutError, OSError):
                continue

            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            label = addr_to_label.get(pc)
            if label is None:
                print(f"t={elapsed:7.3f}s  unexpected stop pc={pc:#010x}")
                g.cont()
                continue

            seen.append(f"{pc:#010x} ({label})")
            print(f"t={elapsed:7.3f}s  {pc:#010x}  {label}")
            if pc == 0x2002b1c4:
                print("\ncold_boot_hw_init genuinely returns -- if this happens, the earlier "
                      "conclusion (this function itself never returns) is WRONG and the real "
                      "blocker is further downstream, inside system_mode_request_dispatch's own "
                      "~80-call pass instead -- re-check sdcard_file_rpc_dispatch_task's struct "
                      "fields (0x203907f0+0x20/+0x28) again live before assuming otherwise.")
                return
            g.cont()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
