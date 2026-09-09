#!/usr/bin/env python3
"""One-off diagnostic for the DMAC/icount stall (README-history.md's newest sections,
2026-09-09): confirmed via DMAC_DEBUG_LOG that the completion ptimer callback fires correctly
and promptly, and `qemu_irq_pulse()` executes -- yet `FUN_200b5ea4`'s busy-wait on `[r4+2]`
never clears. This checks the next link in the chain: does the real ISR (`FUN_200b5b90`,
already confirmed by an earlier session per `dmac.c`'s own file comment) ever actually get
entered at all under `-icount`?

One low-frequency breakpoint (the ISR only needs to fire once for this specific transfer), plus
the overflow trap as a safety net. Logs a timestamp on each hit.

Usage: trace_dmac_isr.py [seconds]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import GdbRsp
from qemu_launch import launch_qemu, HERE

RIIC_IMAGE = HERE / "riic2_eeprom.img"

DMAC_ISR = 0x200b5b90          # FUN_200b5b90, already confirmed the real DMAINT0 ISR
DMAC_WAIT_LOOP = 0x200b5f28    # FUN_200b5ea4's own busy-wait check (the stall point)
OVERFLOW_TRAP = 0x200b93fc

BREAKPOINTS = {
    DMAC_ISR: "DMAC ISR entry",
    DMAC_WAIT_LOOP: "DMAC wait-loop check",
    OVERFLOW_TRAP: "OVERFLOW TRAP",
}


def main():
    total_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

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

        for addr in BREAKPOINTS:
            g.set_breakpoint(addr)
        g.cont()

        start = time.time()
        isr_hits = 0
        wait_hits = 0
        while time.time() - start < total_seconds:
            try:
                g.wait_stop(timeout=min(2.0, total_seconds - (time.time() - start)))
            except (TimeoutError, OSError):
                continue

            regs = g.read_registers()
            pc = regs["r15"]
            elapsed = time.time() - start
            label = BREAKPOINTS.get(pc)

            if label is None:
                print(f"t={elapsed:7.3f}s  unexpected stop pc={pc:#010x}")
                g.cont()
                continue

            if pc == DMAC_WAIT_LOOP:
                wait_hits += 1
                if wait_hits == 1:
                    # Genuinely hot once the wait starts spinning (a tight 3-instruction
                    # loop) -- one hit is all we need (confirms we reached the stall), then
                    # permanently disarm it so it doesn't reintroduce per-push-frequency
                    # masking on top of whatever's already being investigated.
                    print(f"t={elapsed:7.3f}s  {label}  pc={pc:#010x}  "
                          f"(isr_hits={isr_hits}) -- disarming, won't log again")
                    g.remove_breakpoint(pc)
                g.cont()
                continue

            if pc == DMAC_ISR:
                isr_hits += 1

            print(f"t={elapsed:7.3f}s  {label}  pc={pc:#010x}  "
                  f"(isr_hits={isr_hits}, wait_hits={wait_hits})")

            if pc == OVERFLOW_TRAP:
                break

            g.remove_breakpoint(pc)
            g.step()
            g.set_breakpoint(pc)
            g.cont()

        print(f"\nfinal: DMAC ISR entered {isr_hits} time(s); wait-loop check hit "
              f"{wait_hits} time(s) before disarming")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    main()
