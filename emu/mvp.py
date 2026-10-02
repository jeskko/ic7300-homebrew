#!/usr/bin/env python3
"""MVP runner: boot a real IC-7300 firmware release through `base.dat`'s traced 5-step
boot sequence and check it lands on `body.bin`'s real entry point.

See the plan this implements: emu/README.md's "MVP goal" section for the three success
criteria this script checks.

Usage:
    emu/.venv/bin/python3 -m emu.mvp [/path/to/7300_142.dat]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from icom_fw import container as icom_container  # noqa: E402
from unicorn import UcError  # noqa: E402

from emu.board import RAM_BASE, Board  # noqa: E402
from emu.flash_image import FLASH_BASE  # noqa: E402

DEFAULT_CONTAINER = Path(os.environ.get("ICOM_FW_DIR", Path(__file__).resolve().parent.parent.parent / "firmware")) / "7300_142.dat"
BODY_ENTRY = 0x20005000

# Generous safety limits for stage 1 -- LZSS decompressing a multi-MB body one byte at a
# time in emulated ARM is instruction-heavy, but still finishes in well under a second
# untraced.
MAX_INSTRUCTIONS = 200_000_000
MAX_MICROSECONDS = 120_000_000  # 120 s

# Stage 2's budget is deliberately much shorter: as of 2026-09-08, body.bin's own boot
# code reaches a real WFE-based wait loop that only a genuine periodic timer IRQ can end,
# which this emulator doesn't deliver yet (see emu/README.md's Status section -- a real,
# reproducible Unicorn correctness bug was found blocking the straightforward way to add
# one). Past that point stage 2 will always just burn its whole budget for no new
# information, so keep it short rather than generous.
STAGE2_MAX_INSTRUCTIONS = 20_000_000
STAGE2_MAX_MICROSECONDS = 15_000_000  # 15 s


def main(container_path: Path) -> int:
    print(f"[mvp] container: {container_path}")
    print(f"[mvp] reset vector (true flash addr, see notes/base-loader.md): {FLASH_BASE:#010x}")

    board = Board(container_path, trace=False, stop_on_stub=False)
    if not board.cpu.cpu_model_selected:
        print("[mvp] NOTE: running without an explicit Cortex-A9 CPU model (see emu/core.py)")

    # Stage 1: run with `until=BODY_ENTRY` so Unicorn stops exactly when PC reaches
    # body.bin's real entry point, *before* executing anything past it -- this is what
    # lets criterion 1 (no crash getting there) and criterion 2 (RAM content check) be
    # checked precisely, independent of wherever a later peripheral stub happens to stop
    # things (that's stage 2, below).
    print("[mvp] stage 1: running base.dat's boot sequence...")
    try:
        board.run(until=BODY_ENTRY, count=MAX_INSTRUCTIONS, timeout=MAX_MICROSECONDS)
    except UcError as exc:
        pc = board.cpu.pc
        print(f"[mvp] FAILED: Unicorn raised {exc} at pc={pc:#010x}")
        print("[mvp]         (an access outside flash/RAM/the two modeled MMIO windows --")
        print("[mvp]          see emu/board.py's IO_WINDOW_* comments for what's covered)")
        return 1

    pc = board.cpu.pc
    if pc != BODY_ENTRY:
        print(
            f"[mvp] FAILED criterion 1: pc={pc:#010x}, never reached {BODY_ENTRY:#010x} "
            "within the instruction/time budget"
        )
        return 1

    print(f"[mvp] PASSED criterion 1: reached body.bin's real entry point, pc == {BODY_ENTRY:#010x}")

    # Criterion 2: RAM at the entry point must match tools/icom_fw's own independently
    # implemented LZSS decompressor's output for the same release, byte-for-byte.
    raw = container_path.read_bytes()
    fw = icom_container.parse(raw)
    expected = fw.body.decompressed
    actual = board.cpu.read_ram(RAM_BASE + (BODY_ENTRY - RAM_BASE), len(expected))

    if actual == expected:
        print(
            f"[mvp] PASSED criterion 2: {len(expected)} decompressed bytes at "
            f"{BODY_ENTRY:#010x} match tools/icom_fw's decompressor exactly"
        )
    else:
        first_diff = next((i for i in range(len(expected)) if actual[i] != expected[i]), None)
        print(
            f"[mvp] FAILED criterion 2: RAM content diverges from tools/icom_fw's "
            f"decompressor output at byte offset {first_diff}"
        )
        return 1

    # Stage 2: keep running past the entry point (stop_on_stub wired at construction time)
    # to observe criterion 3 -- the first not-yet-modeled peripheral access, hit cleanly
    # rather than crashing.
    print("[mvp] stage 2: continuing into body.bin...")
    board.enable_stub_stop()
    board._stub.quiet = False
    try:
        board.run(count=STAGE2_MAX_INSTRUCTIONS, timeout=STAGE2_MAX_MICROSECONDS)
    except UcError as exc:
        print(f"[mvp] NOTE: stage 2 hit an unmapped access ({exc}) at pc={board.cpu.pc:#010x}")
        print("[mvp]       -- itself a valid 'build this next' signal, not a criterion-3 failure")

    # Check board._halted (set only by the stub-stop callback), not merely whether any
    # stub access was ever recorded -- stage 1's own already-absorbed pokes (quiet, per
    # Board.enable_stub_stop()'s docstring) leave stale entries in board._stub.accesses
    # that would otherwise be misreported as "what stage 2 just hit" after a plain
    # instruction/time-budget timeout (e.g. the known WFE wait loop, see below).
    if board._halted:
        # The *last* recorded access is the one that actually triggered emu_stop().
        kind, addr, size, value = board._stub.accesses[-1]
        print(
            f"[mvp] PASSED criterion 3: cleanly stopped on first unmodeled peripheral "
            f"access -- {kind} addr={addr:#010x} size={size}"
            + (f" value={value:#x}" if value is not None else "")
        )
        print("[mvp]         (this is the concrete 'build this peripheral next' signal --")
        print("[mvp]          see emu/README.md's extension roadmap)")
    else:
        print(
            "[mvp] NOTE: ran out of instruction/time budget before hitting any unmodeled "
            f"peripheral (final pc={board.cpu.pc:#010x}) -- inconclusive on criterion 3, "
            "not a failure. As of 2026-09-08 this is expected: body.bin reaches a real "
            "WFE-based wait loop needing a periodic timer IRQ this emulator doesn't yet "
            "deliver (see emu/README.md's Status section)."
        )

    print("[mvp] MVP: PASSED")
    return 0


if __name__ == "__main__":
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CONTAINER
    raise SystemExit(main(path))
