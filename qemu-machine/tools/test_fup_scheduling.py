#!/usr/bin/env python3
"""Does sdcard_file_rpc_dispatch_task actually wake up and dispatch the
command posted by a hand-hijacked firmware_update_main call?

README.md's "Forcing firmware_update_main directly" section left this as
the open next thread: force_call_fup.py gets real, confirmed execution
into file_rpc_post_command (command ID 0xf, "open") consumed by
sdcard_file_rpc_dispatch_task, but never observes any MMCIF access --
identically whether given a blank or a real, valid disk image. Since
image content provably doesn't matter, the blocker sits upstream of any
actual filesystem/card access, and three hypotheses were left
undistinguished: (1) the consumer task never gets scheduled to run at
all against a hijacked (not properly kernel-created) calling context,
(2) it runs but blocks forever on a real precondition, (3) something
about the hijack confuses the RTOS's own wait/signal primitives so the
post is silently lost.

This settles it with two real breakpoints instead of more static
reading or a full TCB-struct reverse-engineering effort (see
notes/kernel-rtos.md's task catalog row for this task and
notes/kernel-rtos-history.md's "chased pvPortMalloc" section for why a
from-scratch TCB layout derivation would be expensive and uncertain --
a targeted execution trace answers the actual question directly):

  BP_WAIT_RETURN (0x200b9d34): sdcard_file_rpc_dispatch_task's outer
  wait call (FUN_20186de4, infinite timeout) returned with a value the
  task's own dispatcher recognizes as a real command ID (r4 at this PC
  == the command/event value). Hitting this with r4==0xf means the
  RTOS's wait/notify primitive genuinely woke this task in direct
  response to our post -- the real answer to "did it get scheduled at
  all".

  BP_HANDLER_DISPATCH (0x200b9da0, the `blx r12` at the end of the
  inner byte-popping loop): about to invoke the real per-command
  handler function (r12) for command r4. Hitting this with r4==0xf
  means the popped-byte record genuinely matched our posted command
  too, and the real "open" handler is about to run -- if this fires but
  MMCIF is still never touched (per the existing -d unimp finding), the
  mystery is relocated to inside that handler's own execution, not the
  RPC dispatch mechanism.

Runs two trials back to back on fresh boots -- a baseline (no forced
call at all, to see whether this task's own bookkeeping naturally trips
either breakpoint from ordinary boot-time SD-menu/settings activity)
and the real forced-call scenario -- so a hit in the second trial can be
read against what "no forced call" alone already produces.

Usage: test_fup_scheduling.py [watch_seconds]
"""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu as _launch_qemu

# -- breakpoint targets, see module docstring -------------------------
BP_WAIT_RETURN = 0x200B9D34
BP_HANDLER_DISPATCH = 0x200B9DA0
# Coarser, added after the first pass came back silent on both of the
# above: fires the instant FUN_20186de4 (the outer, infinite-timeout
# wait call) returns at ALL, matched command or not, spurious wake or
# not -- r0 is the raw low return word (0x10 == the "real value in r1"
# gate the big command switch requires), r1 the raw high word (the
# candidate command/event value before that gate is checked). Answers
# "does this task get rescheduled out of its blocking wait at all after
# the forced call" even if nothing downstream ever recognizes what it
# got -- a strictly weaker, more diagnostic condition than either
# breakpoint above, see qemu-machine/README.md's follow-up section.
BP_WAIT_UNBLOCKED = 0x200B9E30
# The poster side, inside file_rpc_post_command (FUN_200bc048) itself --
# added after a first pass came back silent on all three of the above,
# to directly re-confirm (in this exact automated run, not just by
# citing the earlier manually-single-stepped session) that the forced
# call genuinely reaches the post at all, and specifically reaches the
# one call inside it that signals the consumer's wait object
# (FUN_20186e68 -- the same handle, iVar1+0x28, that FUN_20186de4 blocks
# on in sdcard_file_rpc_dispatch_task -- read as the real wait/signal
# pair for this RPC channel).
BP_POST_ENTRY = 0x200BC048
BP_POST_SIGNAL = 0x200BC0D4

# -- force_call_fup.py's own constants, reused verbatim ---------------
FUP_MAIN = 0x20025AE4
PATH_BUF = 0x203D86C4
TRAMPOLINE = 0x209F0000
FORCED_PATH = b"IC-7300.DAT\x00"

# The idle/WFE spin (test_irq.py's own WFE_LOOP_LO/HI) is the one place
# earlier manual sessions specifically parked at before hijacking --
# the "idle task's own registers", not an arbitrary mid-kernel-function
# snapshot. Wait for PC to actually land there (retrying, same pattern
# as test_irq.py) rather than hijacking from wherever a fixed real-time
# delay happens to land, which a first pass of this script found can
# easily be mid-way through real kernel-internal code instead.
WFE_LOOP_LO = 0x200B939C
WFE_LOOP_HI = 0x200B93AC
BOOT_SETTLE_S = 1.5
POLL_SLICE_S = 1.0


def wait_for_idle_loop(g: GdbRsp, max_retries: int = 5) -> dict:
    g.cont()
    time.sleep(BOOT_SETTLE_S)
    g.interrupt()
    g.wait_stop()
    regs = g.read_registers()
    for _ in range(max_retries):
        if WFE_LOOP_LO <= regs["r15"] <= WFE_LOOP_HI:
            return regs
        g.cont()
        time.sleep(0.3)
        g.interrupt()
        g.wait_stop()
        regs = g.read_registers()
    return regs  # caller checks and reports if this never landed


def launch_qemu() -> subprocess.Popen:
    return _launch_qemu()


def run_trial(label: str, forced_call: bool, watch_seconds: float) -> dict:
    print(f"\n=== trial: {label} (forced_call={forced_call}) ===")
    proc = launch_qemu()
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        regs = g.read_registers()
        assert regs["r15"] == 0x18000000, f"not a fresh boot, PC={regs['r15']:#x}"

        regs = wait_for_idle_loop(g)
        in_idle = WFE_LOOP_LO <= regs["r15"] <= WFE_LOOP_HI
        print(f"  post-boot-settle: PC={regs['r15']:08x} "
              f"({'idle loop' if in_idle else 'NOT idle loop -- weaker baseline'})")

        if forced_call:
            g.write_memory(PATH_BUF, FORCED_PATH)
            g.write_u32(TRAMPOLINE, 0xEAFFFFFE)  # b . (branch to self)
            new_regs = dict(regs)
            new_regs["r14"] = TRAMPOLINE
            new_regs["r15"] = FUP_MAIN
            new_regs["cpsr"] = regs["cpsr"] & ~(1 << 5)  # ARM mode
            g.write_registers(new_regs)
            print(f"  forced PC={FUP_MAIN:#x} LR={TRAMPOLINE:#x}, "
                  f"path={FORCED_PATH!r}")

        g.set_breakpoint(BP_WAIT_RETURN)
        g.set_breakpoint(BP_HANDLER_DISPATCH)
        g.set_breakpoint(BP_WAIT_UNBLOCKED)
        g.set_breakpoint(BP_POST_ENTRY)
        g.set_breakpoint(BP_POST_SIGNAL)

        hits = {"wait_return": [], "handler_dispatch": [], "wait_unblocked": [],
                "post_entry": [], "post_signal": [], "unexpected": []}
        deadline = time.time() + watch_seconds
        g.cont()
        while time.time() < deadline:
            remaining = deadline - time.time()
            try:
                g.wait_stop(timeout=min(remaining, POLL_SLICE_S))
            except (socket.timeout, TimeoutError, OSError):
                continue  # nothing hit in this slice, keep waiting

            r = g.read_registers()
            pc = r["r15"]
            if pc == BP_WAIT_RETURN:
                hits["wait_return"].append(r["r4"])
                print(f"  HIT wait_return  r4(cmd)={r['r4']:#x}  t={watch_seconds - remaining:.1f}s")
            elif pc == BP_HANDLER_DISPATCH:
                hits["handler_dispatch"].append((r["r4"], r["r12"]))
                print(f"  HIT handler_dispatch  r4(cmd)={r['r4']:#x} r12(fn)={r['r12']:#x}"
                      f"  t={watch_seconds - remaining:.1f}s")
            elif pc == BP_WAIT_UNBLOCKED:
                hits["wait_unblocked"].append((r["r0"], r["r1"]))
                print(f"  HIT wait_unblocked  r0(status)={r['r0']:#x} r1(event)={r['r1']:#x}"
                      f"  t={watch_seconds - remaining:.1f}s")
            elif pc == BP_POST_ENTRY:
                hits["post_entry"].append(r["r0"])
                print(f"  HIT post_entry  r0(cmd)={r['r0']:#x}  t={watch_seconds - remaining:.1f}s")
            elif pc == BP_POST_SIGNAL:
                hits["post_signal"].append((r["r0"], r["r1"], r["r6"]))
                print(f"  HIT post_signal  r0(handle)={r['r0']:#x} r1(cmd)={r['r1']:#x} "
                      f"r6(struct)={r['r6']:#x}  t={watch_seconds - remaining:.1f}s")
            else:
                hits["unexpected"].append(pc)
                print(f"  unexpected stop, PC={pc:08x} (not a breakpoint we set -- "
                      "an async signal or our own late interrupt racing a hit)")

            g.remove_breakpoint(pc)
            g.step()
            g.set_breakpoint(pc)
            g.cont()

        g.interrupt()
        g.wait_stop()
        r = g.read_registers()
        print(f"  end-of-trial: PC={r['r15']:08x} CPSR={r['cpsr']:08x} "
              f"({'still in idle loop' if WFE_LOOP_LO <= r['r15'] <= WFE_LOOP_HI else 'NOT idle loop'})")
        # DAT_200ba174 is a global holding the address of this task's own
        # control struct (pcVar1 in the decompile) -- read live rather than
        # trust the static image, since it may be runtime-populated.
        try:
            consumer_struct = g.read_u32(0x200BA174)
            poster_struct = g.read_u32(0x200BBC50)
            same = "SAME struct" if consumer_struct == poster_struct else "DIFFERENT structs!"
            print(f"  consumer's struct (DAT_200ba174) @ {consumer_struct:#x}, "
                  f"poster's struct (DAT_200bbc50) @ {poster_struct:#x} -- {same}")
            for name, base in (("consumer", consumer_struct), ("poster", poster_struct)):
                if base:
                    blob = g.read_memory(base, 0x30)
                    off28 = int.from_bytes(blob[0x28:0x2c], "little")
                    off20 = int.from_bytes(blob[0x20:0x24], "little")
                    print(f"    {name}: [+0]={blob[0]:#x} [+0x20]={off20:#x} [+0x28]={off28:#x}")
        except Exception as e:  # noqa: BLE001 -- diagnostic only, never fail the trial on this
            print(f"  (could not read task struct diagnostics: {e})")
        g.remove_breakpoint(BP_WAIT_RETURN)
        g.remove_breakpoint(BP_HANDLER_DISPATCH)
        g.remove_breakpoint(BP_WAIT_UNBLOCKED)
        g.remove_breakpoint(BP_POST_ENTRY)
        g.remove_breakpoint(BP_POST_SIGNAL)
        g.close()
        return hits
    finally:
        proc.kill()
        proc.wait()


def main():
    watch_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0

    baseline = run_trial("baseline (no forced call)", False, watch_seconds)
    forced = run_trial("forced firmware_update_main", True, watch_seconds)

    print("\n=== summary ===")
    for label, hits in (("baseline", baseline), ("forced", forced)):
        print(f"{label}: wait_return={hits['wait_return']} "
              f"handler_dispatch={hits['handler_dispatch']} "
              f"wait_unblocked={hits['wait_unblocked']} "
              f"post_entry={hits['post_entry']} "
              f"post_signal={hits['post_signal']}")

    forced_cmd_f_wait = 0xF in forced["wait_return"]
    forced_cmd_f_dispatch = any(cmd == 0xF for cmd, _fn in forced["handler_dispatch"])
    baseline_cmd_f_wait = 0xF in baseline["wait_return"]

    print()
    if forced_cmd_f_wait and not baseline_cmd_f_wait:
        print("RESULT: the outer wait genuinely returned with our posted command "
              "(0xf) ONLY in the forced-call trial -- the RTOS wait/notify "
              "primitive really did wake this task in response to our post. "
              "Hypothesis (1) [never scheduled] is REFUTED.")
        if forced_cmd_f_dispatch:
            print("         The real per-command handler was also reached (blx r12 "
                  "hit with r4=0xf) -- the RPC dispatch mechanism works end to end "
                  "against a hijacked context. The mystery is INSIDE that handler's "
                  "own execution (or something it calls), not the scheduling layer.")
        else:
            print("         But BP_HANDLER_DISPATCH never fired with r4=0xf -- the "
                  "wait woke up recognizing the command, yet the inner popped-byte "
                  "loop never matched it well enough to reach dispatch. Points at "
                  "hypothesis (3) [hijacked-context state mismatch] in the byte-"
                  "popping/queue-matching logic specifically, not general scheduling.")
    elif forced_cmd_f_wait and baseline_cmd_f_wait:
        print("RESULT: command 0xf's wait_return fired in BOTH trials -- this task "
              "processes an 'open' RPC during ordinary boot too (plausibly a "
              "config/settings file), so this signal alone doesn't distinguish our "
              "forced post from baseline activity. Re-run with a path/marker unique "
              "enough to tell them apart (e.g. read memory at the popped record for "
              "a filename match) before drawing a conclusion.")
    else:
        print("RESULT: BP_WAIT_RETURN never fired with r4=0xf in the forced trial "
              "at all -- the post was made (per README's earlier confirmed trace "
              "into file_rpc_post_command) but the consumer's outer wait never woke "
              "recognizing it. Points at hypothesis (1) [never scheduled] or (3) "
              "[hijack breaks the wake mechanism itself] -- both still open, but "
              "narrowed to this exact wait primitive (FUN_20186de4 / whatever posts "
              "to it) as the next thing to inspect, not the dispatch/handler layer.")


if __name__ == "__main__":
    main()
