#!/usr/bin/env python3
"""Live validation (2026-09-10) for riic.c's new real EEPROM write-data-loop path (the
TEI-then-conditional-TI chaining added after this project's own differential test found the old
model silently dropped every write -- see README.md's Status section).

Boot never naturally reaches the one traced real EEPROM write (cold_boot_hw_init's own "SX3765
V0.9H-000" stamp) in the current build -- it happens later in cold_boot_hw_init's own sequence
than the still-unresolved ring-overflow trap boot currently halts at. So this drives RIIC2's
real registers directly over GDB instead (same "bypass firmware, inject a call/sequence
directly" spirit as tools/force_call_fup.py), timed to run once the CPU is confirmed stuck
spinning at that trap (0x200b93fc, a real `b .`) -- the safest possible moment: real time keeps
advancing (the CPU is genuinely still retiring instructions, not halted, so icount-driven
ptimers fire normally) and firmware is provably never going to touch RIIC2 again, so there is no
risk of racing a real transaction.

Protocol: real START -> address(0x1234, 2 bytes) -> 8 real write-data bytes ("TESTDATA") -> real
STOP -- then a fresh transaction re-reads the same 8 bytes back and checks they match. Each
register write is followed by a brief real-time run (the target keeps executing its own trap
loop throughout -- cont()/interrupt() around each poll, same technique as every other GDB-based
tool in this project) so this device's own ptimer-scheduled IRQs actually fire before the next
step; nothing here depends on any code the guest CPU itself is running.
"""
import json
import socket
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp  # noqa: E402

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom.img"
TRAP_PC = 0x200b93fc
QMP_SOCK = "/tmp/qemu_riic_eeprom_test.sock"

RIIC2_BASE = 0xFCFEE800
REG_CR2 = RIIC2_BASE + 0x04
REG_SR2 = RIIC2_BASE + 0x24
REG_DRT = RIIC2_BASE + 0x3c
REG_DRR = RIIC2_BASE + 0x40

CR2_ST = 0x02
CR2_RS = 0x04
CR2_SP = 0x08

EEPROM_ADDR = 0x1234
EEPROM_I2C_ADDR7 = 0x50
PAYLOAD = b"TESTDATA"

STEP_SLEEP = 0.05  # real seconds to free-run between register pokes -- comfortably more than
                    # the ~26us real byte time this device schedules internally.


def w8(gdb, addr, val):
    gdb.write_memory(addr, bytes([val & 0xff]))


def r8(gdb, addr):
    return gdb.read_memory(addr, 1)[0]


def run_step(gdb):
    gdb.cont()
    time.sleep(STEP_SLEEP)
    gdb.interrupt()
    gdb.wait_stop(timeout=5)


def qmp_pc(sock_path: str):
    """QMP `info registers` read-only spot-check -- same technique as check_overflow_r0.py.
    Deliberately GDB-free: this project has repeatedly found that stopping the vCPU via GDB
    (which an earlier version of this function did, via interrupt()) can leave it stopped if the
    caller forgets to resume it -- confirmed the hard way this session (the first version of this
    wait loop hung forever after its own first iteration silently halted the CPU for good).  QMP
    reads live state without ever stopping the vCPU, so there's nothing to forget to resume. """
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.recv(65536)
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    s.send(b'{"execute":"human-monitor-command","arguments":{"command-line":"info registers"}}')
    reply = json.loads(s.recv(65536).decode())
    s.close()
    text = reply["return"]
    for line in text.splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    return None


def wait_for_trap(sock_path: str, max_seconds: float = 90.0):
    deadline = time.time() + max_seconds
    while time.time() < deadline:
        try:
            pc = qmp_pc(sock_path)
            if pc == TRAP_PC:
                return True
        except Exception:
            pass
        time.sleep(1.0)
    return False


def main():
    if not QEMU.exists():
        sys.exit(f"{QEMU} not found -- run setup.sh first")

    Path(QMP_SOCK).unlink(missing_ok=True)
    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1", "-s",
        "-qmp", f"unix:{QMP_SOCK},server,nowait",
    ]
    debug_log = open("/tmp/riic_eeprom_write_test_debug.log", "w")
    env = dict(os.environ, RZA1H_DEBUG="riic")
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=debug_log, env=env)
    try:
        print("Waiting for the known ring-overflow trap (confirms real time is still advancing "
              "and firmware will never touch RIIC2 again)...", flush=True)
        time.sleep(1.0)  # let the QMP socket come up
        if not wait_for_trap(QMP_SOCK):
            print("FAIL: never reached the trap within 90s -- boot behavior may have changed.",
                  flush=True)
            return 1
        print(f"At trap (PC={TRAP_PC:#x}). Connecting for the real test.", flush=True)

        gdb = GdbRsp(port=1234, timeout=5.0)
        gdb.handshake()
        # A fresh connection's interrupt() only gets a reply if *this* connection has an
        # outstanding cont() -- matching tools/test_irq.py's own established cont()+sleep+
        # interrupt()+wait_stop() convention (confirmed live, 2026-09-10: a bare interrupt()
        # right after connecting timed out with no reply).
        gdb.cont()
        time.sleep(0.1)
        gdb.interrupt()
        gdb.wait_stop(timeout=5)

        # Force the channel back to idle first (2026-09-10, found live): the trap is reached
        # mid-boot, not at a clean transaction boundary -- whatever real RIIC2 transaction
        # firmware was last driving may still be in RIIC_WAIT_RESTART (its own ptimer keeps
        # ticking forever regardless of the CPU being stuck in the trap's own busy loop). A
        # CR2=ST sent while phase != IDLE is silently ignored (see rza1h_riic_write()'s own
        # CR2_ST condition) -- confirmed the hard way: without this, every byte below got fed
        # into whatever address that leftover transaction had, not EEPROM_ADDR, and read-back
        # correctly showed the untouched original image instead of PAYLOAD.
        w8(gdb, REG_CR2, CR2_SP)
        run_step(gdb)

        # --- WRITE: START, address (write dir), 2 addr bytes, 8 data bytes, STOP ---
        print(f"Writing {PAYLOAD!r} to EEPROM offset {EEPROM_ADDR:#06x}...", flush=True)
        w8(gdb, REG_CR2, CR2_ST)
        run_step(gdb)
        w8(gdb, REG_DRT, (EEPROM_I2C_ADDR7 << 1) | 0)  # address+write
        run_step(gdb)
        w8(gdb, REG_DRT, (EEPROM_ADDR >> 8) & 0xff)
        run_step(gdb)
        w8(gdb, REG_DRT, EEPROM_ADDR & 0xff)
        run_step(gdb)
        for b in PAYLOAD:
            w8(gdb, REG_DRT, b)
            run_step(gdb)
        cr2 = r8(gdb, REG_CR2)
        w8(gdb, REG_CR2, cr2 | CR2_SP)
        run_step(gdb)
        sr2 = r8(gdb, REG_SR2)
        print(f"  SR2 after STOP: {sr2:#04x} (STOP bit expected set)", flush=True)

        # --- READ BACK: fresh transaction, same address, same length ---
        print(f"Reading back {len(PAYLOAD)} bytes from {EEPROM_ADDR:#06x}...", flush=True)
        w8(gdb, REG_CR2, CR2_ST)
        run_step(gdb)
        w8(gdb, REG_DRT, (EEPROM_I2C_ADDR7 << 1) | 0)  # address+write (to set pointer)
        run_step(gdb)
        w8(gdb, REG_DRT, (EEPROM_ADDR >> 8) & 0xff)
        run_step(gdb)
        w8(gdb, REG_DRT, EEPROM_ADDR & 0xff)
        run_step(gdb)
        cr2 = r8(gdb, REG_CR2)
        w8(gdb, REG_CR2, cr2 | CR2_RS)  # restart -> read direction
        run_step(gdb)
        w8(gdb, REG_DRT, (EEPROM_I2C_ADDR7 << 1) | 1)  # address+read
        run_step(gdb)

        readback = bytearray()
        for i in range(len(PAYLOAD)):
            byte = r8(gdb, REG_DRR)
            readback.append(byte)
            if i == len(PAYLOAD) - 1:
                cr2 = r8(gdb, REG_CR2)
                w8(gdb, REG_CR2, cr2 | CR2_SP)
            run_step(gdb)

        gdb.close()

        print(f"\nWrote:      {PAYLOAD!r}", flush=True)
        print(f"Read back:  {bytes(readback)!r}", flush=True)
        if bytes(readback) == PAYLOAD:
            print("PASS -- the write was genuinely persisted and read back correctly.", flush=True)
            return 0
        else:
            print("FAIL -- read-back does not match what was written.", flush=True)
            return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(main())
