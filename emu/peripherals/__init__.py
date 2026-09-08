"""Peripheral handlers for the IC-7300 emulator.

Every handler duck-types the same tiny interface: `read(addr, size) -> int` and
`write(addr, size, value) -> None`, where `addr` is the absolute CPU address (not an
offset from some window base) -- see `registry.py`. This shape is deliberately the same
as a QEMU MMIO device's `read`/`write` callbacks; see emu/README.md for why.
"""
