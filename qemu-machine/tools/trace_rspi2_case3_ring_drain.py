#!/usr/bin/env python3
"""Follow-up to trace_scif5_job_ring_stuck.py (icom-main-idle-loop-not-reached thread,
2026-09-21 fresh session): that trace found that right after the 6th DSP identity-query
command resolves (~t=61s), a new job_type=3 ("rspi2_transmit") appears on the shared ring for
the first time, and a coarse QMP poll separately caught PC sitting at 0x2001dc38 -- inside
FUN_2001dbcc, which this session's static re-analysis has now positively identified as the
*already fully documented and already-modeled* RIIC2 "RI" (receive-data-full) interrupt
handler (registered via riic2_driver_init, GIC ID 206/0xce -- see riic.c's own header comment
and README-history.md's "RI (receive-data-full)" section). This is a *different peripheral*
(I2C, not SPI) reached only via a real hardware interrupt, structurally unreachable from
rspi2_transmit's own call chain -- so the two events are very plausibly an unrelated timing
coincidence, not a causal chain, matching this exact investigation's own prior "aggregate
effect of many brief legitimate interrupts, not one culprit function" lesson from the
ring-overflow thread.

This script settles it live: breakpoints (armed from the very start, per this project's own
"fast rings desync reactive arming" lesson) on:
  - shared_job_ring_dispatch (0x200b0f68) -- log job_type/read_idx/write_idx every hit
  - rspi2_transmit entry (0x200b6c50) -- confirm it's reached and how long it takes to return
  - FUN_2001dbcc entry (0x2001dbcc, the RIIC2 RI ISR) -- log driver-state fields on each hit,
    to see whether hits keep recurring (real ongoing RIIC2 activity) or are a one-off tail of
    the already-known early-boot diode-matrix EEPROM read
  - FUN_200b8308 entry (0x200b8308) -- log r0 to catch the 0xa2 (RSPI2 completion IRQ enable)
    call rspi2_transmit's own tail makes

Also samples flag_ed (0x203906ed) and read_idx/write_idx on every polling tick regardless of
breakpoint hits, so the ring's overall drain state is visible even between hits.

Usage: trace_rspi2_case3_ring_drain.py [seconds] [poll_interval]
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
RSPI2_TRANSMIT = 0x200b6c50
RIIC2_RI_ISR = 0x2001dbcc
GIC_ENABLE_HELPER = 0x200b8308
RIIC2_DRIVER_STATE = 0x203945a4  # DAT_2001e6c0's own held pointer -- state[0]/count[4]/idx[5]
PWRK_WFI_PC = 0x20029B18
GPIO_PATH = "/machine/gpio"

_qmp_buf: dict = {}


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
    _qmp_next_obj(s)
    s.send(b'{"execute":"qmp_capabilities"}\n')
    _qmp_next_obj(s)
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
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
    poll_interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5

    if not RIIC_IMAGE.exists():
        print(f"missing {RIIC_IMAGE}", file=sys.stderr)
        sys.exit(1)

    sock_path = "/tmp/qemu_rspi2_case3.sock"
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

        print("Running to the PWRK-wait wfi landing point...")
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

        g.interrupt()
        g.wait_stop()
        # NOTE: FUN_2001dbcc (RIIC2 RI ISR) breakpoint deliberately dropped -- a first run of
        # this script with it included showed continuous hits every ~0.75s for 50+ seconds, always
        # with driver state all-zero, looking like a spurious interrupt storm. A fully GDB-free
        # RZA1H_DEBUG=riic capture (tools/trace_riic1_long_capture.py) over the same real-time
        # window proved this was a GDB-perturbation artifact (icom-gdb-perturbation-resolved
        # mechanisms A/D: a breakpoint degrades its containing page to one-instruction-per-TB,
        # and -icount shift=auto turns that into a real CPU/peripheral speed skew) -- RIIC2's own
        # debug-log line count is genuinely frozen (13088 lines, unchanged from t=10s to t=90s)
        # with zero GDB involved. FUN_2001dbcc is not re-invoked at all after early boot; leaving
        # its breakpoint armed only slows this trace down for no signal.
        for addr in (JOB_RING_DISPATCH, RSPI2_TRANSMIT, GIC_ENABLE_HELPER):
            g.set_breakpoint(addr)
        print("*** armed from the start ***")
        g.cont()

        dispatch_hits = []
        rspi2_hits = []
        riic2_ri_hits = []
        gic_enable_hits = []
        start = time.time()
        last_flag = None
        last_read_idx = None
        last_write_idx = None

        while time.time() - start < total_seconds:
            time.sleep(poll_interval)
            g.interrupt()
            g.wait_stop()
            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start

            if pc in (JOB_RING_DISPATCH, RSPI2_TRANSMIT, GIC_ENABLE_HELPER):
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
                elif pc == RSPI2_TRANSMIT:
                    lr = regs["r14"]
                    rspi2_hits.append((elapsed, lr))
                    print(f"t={elapsed:7.3f}s  RSPI2_TRANSMIT entry  lr={lr:#010x}")
                elif pc == RIIC2_RI_ISR:
                    state = g.read_memory(RIIC2_DRIVER_STATE, 8)
                    riic2_ri_hits.append((elapsed, list(state)))
                    print(f"t={elapsed:7.3f}s  RIIC2_RI_ISR  "
                          f"state0={state[0]} count[4]={state[4]} idx[5]={state[5]}")
                else:
                    r0 = regs["r0"]
                    lr = regs["r14"]
                    gic_enable_hits.append((elapsed, r0, lr))
                    print(f"t={elapsed:7.3f}s  GIC_ENABLE_HELPER  id={r0:#x}  lr={lr:#010x}")
                g.remove_breakpoint(pc)
                g.step()
                g.set_breakpoint(pc)
                g.cont()
                continue

            flag = g.read_memory(FLAG_ED, 1)[0]
            read_idx = g.read_memory(RING_READ_IDX, 1)[0]
            write_idx = g.read_memory(RING_WRITE_IDX, 1)[0]
            if flag != last_flag or read_idx != last_read_idx or write_idx != last_write_idx:
                print(f"t={elapsed:7.3f}s  poll  flag_ed={flag}  read_idx={read_idx}  "
                      f"write_idx={write_idx}  pc={pc:#010x}")
                last_flag, last_read_idx, last_write_idx = flag, read_idx, write_idx

            g.cont()

        print(f"\n{len(dispatch_hits)} shared_job_ring_dispatch hits, "
              f"{len(rspi2_hits)} rspi2_transmit hits, "
              f"{len(riic2_ri_hits)} RIIC2 RI-ISR hits, "
              f"{len(gic_enable_hits)} GIC-enable-helper hits.")
        if dispatch_hits:
            print("job types seen:", sorted({h[4] for h in dispatch_hits}))
        if gic_enable_hits:
            print("GIC IDs enabled:", sorted({f"{h[1]:#x}" for h in gic_enable_hits}))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
