#!/usr/bin/env python3
"""Walk cold_boot_hw_init's own call sites with GDB breakpoints to find the LAST one reached.

Context (icom-main-idle-loop-not-reached thread, 2026-09-21): a GDB-free QMP probe
(trace_boot_path_state.py) showed the boot DOES take the cold-boot branch
(*0x2039030f == 1, written only by cold_boot_mode_dispatch at 0x2002b1d4), yet
main_idle_loop's own entry marker never appears (FUN_20062c28 would set *0x20390316 = 1
and that byte reads 0 for an entire 120s capture, while *0x20390315 free-runs as a real
tick counter).  So main_idle_loop is never ENTERED at all -- the stall is upstream, inside
cold_boot_hw_init (0x2002afc0-0x2002b1c7, ARM mode) or its callees.

This walks every BL site in cold_boot_hw_init: set a breakpoint on each, then on every hit
remove it and continue.  Whatever the last reached site is, the stall is inside that call.

Usage: trace_cold_boot_hw_init_walk.py [stall_timeout_s]
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp  # noqa: E402

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"
RIIC_IMAGE = HERE / "riic2_eeprom_pwrk_test.img"
GPIO_PATH = "/machine/gpio"

# every BL site in cold_boot_hw_init, in address order, plus its epilogue and the
# two follow-on landmarks in cold_boot_mode_dispatch / main_idle_loop.
SITES = [
    0x2002AFD0, 0x2002AFE4, 0x2002AFE8, 0x2002AFF0, 0x2002AFF4, 0x2002AFFC,
    0x2002B000, 0x2002B004, 0x2002B008, 0x2002B00C, 0x2002B010, 0x2002B02C,
    0x2002B038, 0x2002B03C, 0x2002B040, 0x2002B058, 0x2002B05C, 0x2002B060,
    0x2002B064, 0x2002B080, 0x2002B084, 0x2002B088, 0x2002B08C, 0x2002B090,
    0x2002B094, 0x2002B098, 0x2002B09C, 0x2002B0A0, 0x2002B0A4, 0x2002B0A8,
    0x2002B0AC, 0x2002B0B0, 0x2002B0B4, 0x2002B0B8, 0x2002B0BC, 0x2002B0E0,
    0x2002B0E4, 0x2002B0EC, 0x2002B0F0, 0x2002B0F4, 0x2002B0F8, 0x2002B0FC,
    0x2002B100, 0x2002B104, 0x2002B110, 0x2002B114, 0x2002B17C, 0x2002B180,
    0x2002B1C4,            # cold_boot_hw_init epilogue (ldmia sp!,{r4,r5,r6,pc})
    0x20052E30,            # main_idle_loop entry
]

NAMES = {
    0x2002AFD0: "ext_irq6_config_init",
    0x2002AFE4: "FUN_20029c60",
    0x2002AFE8: "FUN_200b4494",
    0x2002AFF0: "ostm1_busywait_delay_us(10ms)",
    0x2002AFF4: "frontpanel_mcu_release_reset",
    0x2002AFFC: "ostm1_busywait_delay_us(30ms)",
    0x2002B000: "scif3_frontpanel_init_and_latch_version",
    0x2002B004: "scif3_dynqueue_post_and_flush",
    0x2002B008: "FUN_200b4800",
    0x2002B00C: "FUN_2007ed9c",
    0x2002B010: "FUN_2007ede0",
    0x2002B02C: "itron_act_tsk(ui_graphics_lifecycle_task)",
    0x2002B038: "bmp_capture_task_bootstrap",
    0x2002B03C: "spectrum_scope_fft_task_bootstrap",
    0x2002B040: "ext_irq1_config_init",
    0x2002B058: "FUN_200b5b64 (DMAC)",
    0x2002B05C: "FUN_200b5be0 (DMAC)",
    0x2002B060: "FUN_200b5ea4 (DMAC)",
    0x2002B064: "FUN_200b5f38 (DMAC wait)",
    0x2002B080: "ostm1_busywait_delay_us(100us)",
    0x2002B084: "scif5_dsp_link_driver_init",
    0x2002B088: "FUN_200b48e4",
    0x2002B08C: "scif5_wait_hsk1_ready",
    0x2002B090: "dsp_boot_handshake",
    0x2002B094: "dsp_cmd_table_init",
    0x2002B098: "dsp_identity_query_record0",
    0x2002B09C: "dsp_identity_query_record1",
    0x2002B0A0: "dsp_identity_query_record2 / rspi2_driver_init",
    0x2002B0A4: "FUN_200b7020",
    0x2002B0A8: "FUN_2005f8ac",
    0x2002B0AC: "FUN_2005fcb4",
    0x2002B0B0: "FUN_2005f9b0",
    0x2002B0B4: "FUN_2005fac4",
    0x2002B0B8: "FUN_200605fc (SSIF)",
    0x2002B0BC: "nvram_block_3fc0_verify (FUN_200291d8)",
    0x2002B0E0: "nvram_settings_180b_load",
    0x2002B0E4: "(after 0x20006c74)",
    0x2002B0EC: "FUN_2000a0a8",
    0x2002B0F0: "FUN_2000a264",
    0x2002B0F4: "FUN_200609c0",
    0x2002B0F8: "FUN_2006756c",
    0x2002B0FC: "FUN_20035af4",
    0x2002B100: "nvram_multirecord_load_and_verify",
    0x2002B104: "nvram_wearleveled_ring_load",
    0x2002B110: "FUN_20029308",
    0x2002B114: "diode_matrix_cold_boot_init",
    0x2002B17C: "tuner_jack_signal_precheck",
    0x2002B180: "FUN_2002ae6c",
    0x2002B1C4: "*** cold_boot_hw_init RETURNS ***",
    0x20052E30: "*** main_idle_loop ENTERED ***",
}


def qmp_open(sock_path: str) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.recv(65536)
    s.send(b'{"execute":"qmp_capabilities"}')
    s.recv(65536)
    return s


def hmp(s, cmd):
    s.send(json.dumps({"execute": "human-monitor-command",
                       "arguments": {"command-line": cmd}}).encode())
    return json.loads(s.recv(1 << 20).decode())["return"]


def qmp_cmd(s, execute, **arguments):
    s.send(json.dumps({"execute": execute, "arguments": arguments}).encode())
    return json.loads(s.recv(65536).decode())


def pc_qmp(s):
    for line in hmp(s, "info registers").splitlines():
        for tok in line.split():
            if tok.startswith("R15="):
                return int(tok[4:], 16)
    raise RuntimeError("no R15")


def main():
    stall_timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0
    sock_path = "/tmp/qemu_cbhi_walk.sock"
    Path(sock_path).unlink(missing_ok=True)

    qemu_args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none",
        "-global", f"rza1h-riic.image={RIIC_IMAGE}",
        "-icount", "shift=1",
        "-qmp", f"unix:{sock_path},server,nowait",
        "-gdb", "tcp::1234",
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = qmp_open(sock_path)
        t0 = time.time()
        while time.time() - t0 < 15:
            time.sleep(0.1)
            if pc_qmp(s) == 0x20029B18:
                break
        qmp_cmd(s, "qom-set", path=GPIO_PATH, property="pwrk-pressed", value=True)
        print("PWRK pressed and held; attaching gdbstub...")

        g = GdbRsp(port=1234)
        g.handshake()
        try:
            g.interrupt()
            g.wait_stop(timeout=5)
        except Exception:
            pass
        pending = set(SITES)
        for a in SITES:
            g.set_breakpoint(a, kind=4)
        print(f"{len(SITES)} breakpoints armed. Walking...\n")

        last = None
        hits = []
        while pending:
            g.cont()
            try:
                g.wait_stop(timeout=stall_timeout)
            except Exception:
                print(f"\n*** STALLED: no further breakpoint in {stall_timeout:.0f}s ***")
                break
            regs = g.read_registers()
            cur = regs["r15"]
            if cur in pending:
                pending.discard(cur)
                g.remove_breakpoint(cur, kind=4)
                hits.append(cur)
                last = cur
                print(f"  hit 0x{cur:08x}  {NAMES.get(cur, '')}")
            else:
                print(f"  (unexpected stop at 0x{cur:08x})")
        print("\n--- summary ---")
        if last is not None:
            print(f"last site reached: 0x{last:08x}  {NAMES.get(last, '')}")
        nxt = [a for a in SITES if a not in [h for h in hits]]
        print(f"never reached ({len(nxt)}): " + ", ".join(
            f"0x{a:08x}({NAMES.get(a,'')})" for a in nxt[:8]))
        try:
            g.interrupt()
        except Exception:
            pass
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
