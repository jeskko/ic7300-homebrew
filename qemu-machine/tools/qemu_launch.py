"""Shared QEMU launch helper for qemu-machine/'s tools.

Built 2026-09-09 alongside the job-ring-overflow fix (README-history.md's newest sections):
a real emulation-timing-realism fix paired with `ostm.c`'s real, schematic-confirmed
OSTM_FREQ_HZ. Before this, every tool script had its own copy-pasted `subprocess.Popen([...])`
launch line -- one shared place means a future machine-wide default only needs updating once.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
QEMU = HERE / "qemu-src" / "build" / "qemu-system-arm"
FLASH = HERE / "flash.bin"

# 2026-09-23: switched from shift=auto -- under shift=auto main_idle_loop starves the renderer after the boot splash (see README.md 2026-09-23 Status); shift=1 = 2ns/insn ~ the real 400MHz Cortex-A9.
# See README.md's "Running it" section for why this is the recommended default now.
DEFAULT_ICOUNT = "shift=1"


def launch_qemu(
    extra_args: list[str] | None = None,
    *,
    icount: str | None = DEFAULT_ICOUNT,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
) -> subprocess.Popen:
    """Launch a fresh `rz-a1h` QEMU instance stopped at reset (`-S -gdb tcp::1234`), with this
    machine's own recommended defaults plus whatever `extra_args` the caller needs (e.g.
    `-global rza1h-riic.image=...`).

    Pass `icount=None` to opt out (e.g. for an A/B comparison against the old, unthrottled
    timing model, or for a quick trial that never runs long enough to need it and would
    otherwise just run slower for no benefit).
    """
    args = [
        str(QEMU), "-M", "rz-a1h", "-nographic", "-kernel", str(FLASH),
        "-serial", "none", "-monitor", "none", "-S", "-gdb", "tcp::1234",
    ]
    if icount:
        args += ["-icount", icount]
    args += extra_args or []
    return subprocess.Popen(
        args, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
    )
