#!/usr/bin/env python3
"""Follow-up to trace_main_idle_wait_flags.py (icom-main-idle-loop-not-reached thread,
2026-09-21 session): that script live-confirmed the SCIF5/DSP job-ring's active flag
(0x203906ed) flips to 1 around t=8s during DSP-link bring-up and never clears again for 32+
real seconds, which is what parks main_idle_loop's own blocking wait forever on the RTOS idle
task. This script finds out *why* the ring never drains.

Static trace of shared_job_ring_dispatch (0x200b0f68, ring base 0x20414da0, confirmed via its
own literal pool at 0x200b1ca8) found its case 1 (the "DSP command pending" job type) only ever
actually checks the real reply (via scif5_classify_reply) when called with param_1 != 0 -- when
called with param_1 == 0, it *always* treats the command as still-pending and re-arms the retry
timer (scif5_arm_retry_timer(0)) without checking anything. param_1 != 0 only ever happens from
scif5_rx_isr, SCIF5's real RX-side ISR -- i.e. an actual received reply byte. If this project's
own virtual DSP responder in scif.c doesn't produce a reply for whatever specific exchange is
stuck, shared_job_ring_dispatch(1) (the only call that could resolve it) would simply never
fire for that job, while shared_job_ring_dispatch(0) (the retry-timer-driven variant, which
can *never* resolve anything on its own) keeps firing forever. This script distinguishes those
two cases live: which param_1 value each hit uses, the current job type in the ring slot being
serviced, and whether scif5_arm_retry_timer keeps getting invoked.

Uses this project's own established "poll to detect, breakpoint only once armed" technique
(see trace_job_ring_producer.py) -- breakpoints only go live once flag_ed is already observed
stuck, not for the whole boot, to avoid the class of timing-perturbation bug this project has
hit before.

Usage: trace_scif5_job_ring_stuck.py [seconds] [poll_interval]
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom_pwrk_test.img"

FLAG_ED = 0x203906ed
RING_BASE = 0x20414da0
RING_WRITE_IDX = RING_BASE + 0x570
RING_READ_IDX = RING_BASE + 0x571
JOB_RING_DISPATCH = 0x200b0f68
SEND_AND_WAIT_REPLY = 0x200b237c  # scif5_send_and_wait_reply -- log LR (its caller) + r0
ARM_RETRY_TIMER = 0x200b0cd4
CLASSIFY_REPLY = 0x200b0dc4
PWRK_WFI_PC = 0x20029B18
GPIO_PATH = "/machine/unattached/device[14]"


# QEMU can interleave async event objects ({"event": ...}) with command replies on the same
# QMP socket -- e.g. a RESUME event right after a GDB-side `cont()` changes the VM run state.
# Skip events, return the first object that's an actual command reply.
_qmp_buf = {}


def _qmp_next_obj(s: socket.socket):
    buf = _qmp_buf.setdefault(s, b"")
    while b"\n" not in buf:
        buf += s.recv(65536)
    line, _, buf = buf.partition(b"\n")
    _qmp_buf[s] = buf
    return json.loads(line.decode())


def qmp_open(sock_path: str) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    _qmp_buf[s] = b""
    _qmp_next_obj(s)  # greeting
    s.send(b'{"execute":"qmp_capabilities"}\n')
    _qmp_next_obj(s)  # capabilities reply
    return s


def qmp_cmd(s: socket.socket, execute: str, **arguments):
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode() + b"\n")
    while True:
        obj = _qmp_next_obj(s)
        if "event" not in obj:
            return obj


def hmp(s: socket.socket, cmd: str) -> str:
    s.send(json.dumps({"execute": "human-monitor-command",
                        "arguments": {"command-line": cmd}}).encode() + b"\n")
    while True:
        obj = _qmp_next_obj(s)
        if "event" not in obj:
            return obj["return"]


def qmp_pc(s: socket.socket) -> int:
    text = hmp(s, "info registers")
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("R15 not found")


def job_type_at(g: GdbRsp, read_idx: int) -> int:
    addr = RING_BASE + read_idx * 0x10 + 0xc
    return g.read_memory(addr, 1)[0]


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5

    if not RIIC_IMAGE.exists():
        print(f"missing {RIIC_IMAGE}", file=sys.stderr)
        sys.exit(1)

    sock_path = "/tmp/qemu_scif5_job_ring.sock"
    Path(sock_path).unlink(missing_ok=True)

    proc = launch_qemu([
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-qmp", f"unix:{sock_path},server,nowait",
    ])
    try:
        time.sleep(0.4)
        g = GdbRsp(port=1234)
        g.read_registers()

        qmp = qmp_open(sock_path)

        print("Running to the PWRK-wait wfi landing point (QMP poll, GDB kept running --"
              " PWRK_WFI_PC may be Thumb-mode, avoid patching a 4-byte GDB breakpoint over it)...")
        g.cont()
        t_wait = time.time()
        while time.time() - t_wait < 15:
            time.sleep(0.1)
            if qmp_pc(qmp) == PWRK_WFI_PC:
                break
        else:
            raise RuntimeError("never reached PWRK_WFI_PC within 15s")

        qmp_cmd(qmp, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print("PWRK pressed and held.")

        # Arm immediately rather than reactively on flag_ed==1: the first
        # session's trial showed flag_ed goes 0->1 and gets zero further
        # dispatch/retry/classify hits afterward -- the whole enqueue+dispatch+
        # retry-arm sequence that SETS the flag apparently completes within one
        # guest instant, faster than reactive QMP-poll-then-arm can catch. This
        # path isn't the hot/high-frequency ring (unlike the already-fixed RIIC2
        # one), so breakpointing it for the whole run is an acceptable tradeoff
        # to actually see the one-time event.
        g.interrupt()
        g.wait_stop()
        g.set_breakpoint(JOB_RING_DISPATCH)
        g.set_breakpoint(ARM_RETRY_TIMER)
        g.set_breakpoint(CLASSIFY_REPLY)
        g.set_breakpoint(SEND_AND_WAIT_REPLY)
        armed = True
        print("*** armed from the start ***")
        g.cont()
        dispatch_hits = []
        retry_hits = []
        classify_hits = []
        send_wait_hits = []
        start = time.time()
        last_flag = None

        while time.time() - start < total_seconds:
            time.sleep(poll_interval)
            g.interrupt()
            g.wait_stop()
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start

            if armed and pc in (JOB_RING_DISPATCH, ARM_RETRY_TIMER, CLASSIFY_REPLY,
                                 SEND_AND_WAIT_REPLY):
                if pc == JOB_RING_DISPATCH:
                    param1 = regs["r0"]
                    read_idx = g.read_memory(RING_READ_IDX, 1)[0]
                    write_idx = g.read_memory(RING_WRITE_IDX, 1)[0]
                    jtype = job_type_at(g, read_idx)
                    lr = regs["r14"]
                    dispatch_hits.append((elapsed, param1, read_idx, write_idx, jtype, lr))
                    print(f"t={elapsed:7.3f}s  DISPATCH  param1={param1}  "
                          f"read_idx={read_idx}  write_idx={write_idx}  job_type={jtype}  "
                          f"lr={lr:#010x}")
                elif pc == ARM_RETRY_TIMER:
                    param1 = regs["r0"]
                    lr = regs["r14"]
                    retry_hits.append((elapsed, param1, lr))
                    print(f"t={elapsed:7.3f}s  ARM_RETRY_TIMER  param1={param1}  "
                          f"lr={lr:#010x}")
                elif pc == CLASSIFY_REPLY:
                    lr = regs["r14"]
                    classify_hits.append((elapsed, lr))
                    print(f"t={elapsed:7.3f}s  CLASSIFY_REPLY  lr={lr:#010x}")
                else:
                    r0 = regs["r0"]
                    lr = regs["r14"]
                    send_wait_hits.append((elapsed, r0, lr))
                    print(f"t={elapsed:7.3f}s  SEND_AND_WAIT_REPLY  passthrough_r0={r0:#x}  "
                          f"caller_lr={lr:#010x}")
                g.remove_breakpoint(pc)
                g.step()
                g.set_breakpoint(pc)
                g.cont()
                continue

            flag = g.read_memory(FLAG_ED, 1)[0]
            if flag != last_flag:
                print(f"t={elapsed:7.3f}s  poll  flag_ed={flag}  pc={pc:#010x}")
                last_flag = flag

            g.cont()

        print(f"\n{len(dispatch_hits)} shared_job_ring_dispatch hits, "
              f"{len(retry_hits)} scif5_arm_retry_timer hits, "
              f"{len(classify_hits)} scif5_classify_reply hits, "
              f"{len(send_wait_hits)} scif5_send_and_wait_reply hits.")
        if dispatch_hits:
            print("dispatch param1 values seen:", sorted({h[1] for h in dispatch_hits}))
            print("job types seen:", sorted({h[4] for h in dispatch_hits}))
        if send_wait_hits:
            print("send_and_wait_reply caller LRs seen:",
                  sorted({h[2] for h in send_wait_hits}))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
