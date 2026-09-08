"""Default "unimplemented peripheral" handler.

Modeled directly on QEMU's own `unimplemented-device`: logs the access and returns a
configurable benign value, rather than crashing or silently corrupting state. This is
what makes hitting an un-modeled peripheral during the MVP boot a useful signal ("build
this next") instead of a dead end -- see emu/README.md's MVP success criteria.
"""

from __future__ import annotations

from typing import Callable


class StubPeripheral:
    def __init__(
        self,
        default_read: int = 0,
        log=print,
        quiet: bool = False,
        on_access: Callable[[], None] | None = None,
    ) -> None:
        self.default_read = default_read
        self._log = log
        self.quiet = quiet
        # Injected hook, e.g. wired by a caller to `uc.emu_stop()` -- lets "stop cleanly
        # on the first un-modeled peripheral access" live outside this class, which has
        # no business knowing about Unicorn. See emu/board.py's `stop_on_stub` option.
        self._on_access = on_access
        self.accesses: list[tuple[str, int, int, int | None]] = []

    def read(self, addr: int, size: int) -> int:
        self.accesses.append(("read", addr, size, None))
        if not self.quiet:
            self._log(f"[stub] unimplemented READ  addr={addr:#010x} size={size}")
        if self._on_access is not None:
            self._on_access()
        return self.default_read

    def write(self, addr: int, size: int, value: int) -> None:
        self.accesses.append(("write", addr, size, value))
        if not self.quiet:
            self._log(f"[stub] unimplemented WRITE addr={addr:#010x} size={size} value={value:#x}")
        if self._on_access is not None:
            self._on_access()
