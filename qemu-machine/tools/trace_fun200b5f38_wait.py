#!/usr/bin/env python3
"""Handoff prep, 2026-09-09/10 -- NOT yet run to a conclusion, prepared for a fresh session.

The concrete, precisely-localized next step after this session's `force_call_fup.py` retest and
`trace_cold_boot_hw_init_tail.py`'s own smoke tests. Corrects that tool's original assumption
(everything through `FUN_200b5f38`'s own call, 0x2002b064, "already works" since the identical
DMAC function right before it, `FUN_200b5ea4`, was fully resolved earlier the same day) --
`FUN_200b5f38` was never itself independently checked, and a genuinely GDB-free run (no `-S`, no
`-gdb` at all, `RZA1H_DEBUG=dmac,riic,scif,rspi2`) shows only ONE DMAC arm+complete event total,
matching `FUN_200b5ea4`'s own already-confirmed transfer -- `FUN_200b5f38` never reaches its own
`N0TB_0` write at all. Since `FUN_200b5f38` shares the *literal identical* struct base
(`0x203906EC`, confirmed via decompile+listing, not assumed) as `FUN_200b5ea4`, this means it's
stuck in *its own* loop 1 (the shared-slot-idle check, `0x200b5fa4`-`0x200b5fac`) -- the flag
`FUN_200b5ea4`'s own ISR already clears is somehow still non-zero by the time this sibling
function checks it, a genuine, not-yet-diagnosed bug (or a real ordering/race issue), not another
instance of today's GDB-observation artifact (this finding came from a completely GDB-free run).

Exact addresses (raw listing, not decompile, so no ambiguity):
  loop 1: 0x200b5fa4 (check [r4+2]) - 0x200b5fac (bne back)
  loop 1 exit / about to arm: 0x200b5fbc (ldr r0,[r4,#0xc], right before bl 0x200b5cdc/0x200b5dc0)
  loop 2 (own completion wait): 0x200b5fcc - 0x200b5fd4
  real return: 0x200b5fd8 (ldmia sp!,{r4,r5,r6,pc})

This script brackets loop 1's exit and the function's own return with exactly two breakpoints,
nothing at or near either loop itself -- the same near-zero-perturbation technique
`trace_dmac_wait_completion2.py` already proved out on `FUN_200b5ea4`. If `LOOP1_EXIT` never
fires, that's the smoking gun (stuck waiting for the shared flag, never even arms); if it fires
but `OWN_RETURN` doesn't, the bug is instead in this specific transfer's own completion path.

Concrete first things to check once this narrows further:
  - Does `FUN_200b5ea4`'s own ISR (`FUN_200b5b90`) definitely run to completion, clearing
    0x203906EE, BEFORE `FUN_200b5f38` starts checking it? (a real ordering question -- does
    `cold_boot_hw_init`'s own call to `FUN_200b5ea4` truly wait for the ISR, or could there be a
    narrow window where `FUN_200b5f38` starts checking before the async IRQ has actually landed?)
  - Read `0x203906EE`/`0x203906ED` directly (a plain memory read, not a breakpoint-dependent
    PC/register snapshot) once stuck, to see their actual live values -- if non-zero, that's
    direct, GDB-artifact-immune confirmation of which specific bit is stuck set and why.
  - Check `references_to` on `0x203906ED` specifically (the *second* flag, not 0x203906EE) --
    this session never separately verified who's supposed to clear *that* one; it's checked by
    both functions' own loop 1 but wasn't traced as carefully as 0x203906EE was.

Usage: trace_fun200b5f38_wait.py [max_seconds]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

LOOP1_EXIT = 0x200b5fbc      # about to arm -- own N0TB_0 write coming up via FUN_200b5dc0
OWN_RETURN = 0x200b5fd8      # FUN_200b5f38's real return -- only reached post-completion
FLAG_MAIN = 0x203906EE       # loop 1 + loop 2's shared "busy" check (same address FUN_200b5ea4
                              # uses -- literal identical struct, confirmed via listing)
FLAG_SECOND = 0x203906ED     # loop 1's second check -- not as carefully traced this session


def main():
    max_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0

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

        g.set_breakpoint(LOOP1_EXIT)
        g.set_breakpoint(OWN_RETURN)
        g.cont()

        start = time.time()
        print(f"free-running, watching LOOP1_EXIT ({LOOP1_EXIT:#x}) and OWN_RETURN "
              f"({OWN_RETURN:#x}) -- up to {max_seconds:.0f}s, zero interference at/near "
              "either loop")
        try:
            g.wait_stop(timeout=max_seconds)
        except (TimeoutError, OSError):
            elapsed = time.time() - start
            print(f"\nt={elapsed:.1f}s  TIMEOUT -- neither breakpoint fired. Stuck in loop 1 "
                  "(never even arms its own transfer) -- matches the GDB-free dmac.c debug log "
                  "finding (only one arm+complete event ever seen) from earlier this session.")
            print(f"Next: read {FLAG_MAIN:#x} and {FLAG_SECOND:#x} directly (plain memory read, "
                  "not a register/PC snapshot) to see their actual stuck values -- see this "
                  "file's own module comment for what to check next depending on the result.")
            return

        regs = g.read_registers()
        pc = regs["r15"]
        elapsed = time.time() - start
        if pc == LOOP1_EXIT:
            print(f"\nt={elapsed:.1f}s  *** LOOP1_EXIT hit *** -- the shared slot WAS free, "
                  "own transfer about to be armed. This contradicts the GDB-free debug-log "
                  "finding (only one arm event ever seen) -- re-check RZA1H_DEBUG=dmac output "
                  "for this exact run before trusting either result alone.")
            g.remove_breakpoint(LOOP1_EXIT)
            g.cont()
            try:
                g.wait_stop(timeout=max_seconds - elapsed)
                regs = g.read_registers()
                if regs["r15"] == OWN_RETURN:
                    print(f"t={time.time()-start:.1f}s  *** OWN_RETURN hit *** -- FUN_200b5f38 "
                          "genuinely completes. The real blocker is further downstream, inside "
                          "trace_cold_boot_hw_init_tail.py's own waypoint list (0x2002b088 "
                          "onward) after all -- resume there.")
            except (TimeoutError, OSError):
                print(f"t={time.time()-start:.1f}s  own completion wait (loop 2) never clears -- "
                      "own transfer armed fine, but this specific transfer's own completion "
                      "path is the bug (a genuinely different failure mode from FUN_200b5ea4's "
                      "own, already-resolved GDB-artifact story).")
        elif pc == OWN_RETURN:
            print(f"\nt={elapsed:.1f}s  unexpected: OWN_RETURN hit without LOOP1_EXIT firing "
                  "first -- shouldn't be reachable given the code's own straight-line structure, "
                  "worth double-checking the breakpoint addresses against a fresh listing.")
        else:
            print(f"\nt={elapsed:.1f}s  unexpected stop at pc={pc:#010x}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
