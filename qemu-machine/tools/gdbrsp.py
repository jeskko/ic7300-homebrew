"""Minimal GDB remote-serial-protocol (RSP) client, raw sockets only.

Built 2026-09-08 specifically because `gdb`'s own Python API against QEMU's
gdbstub proved unreliable in this project's earlier sessions (`continue`/
`interrupt` not blocking synchronously as expected, stale-looking register
snapshots across reconnects) -- see qemu-machine/README.md's Status section
and its Extension-roadmap item 1. This talks the wire protocol directly
(https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html)
so there is no `gdb` process, no Python-API threading, nothing to be
unreliable except this ~150-line client itself.

Deliberately minimal: only what qemu-machine/tools/ needs. Register layout
(`g`/`G` packet order) is QEMU's own ARM gdbstub order -- r0-r15, then cpsr
-- confirmed against target/arm/gdbstub.c (`arm_cpu_gdb_read_register`) in
the vendored checkout, not guessed.
"""

from __future__ import annotations

import socket
import time


class GdbRspError(RuntimeError):
    pass


class GdbRsp:
    def __init__(self, host: str = "127.0.0.1", port: int = 1234, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._buf = b""

    def close(self):
        self.sock.close()

    # -- wire-level packet framing --------------------------------------

    @staticmethod
    def _checksum(data: bytes) -> int:
        return sum(data) & 0xFF

    def _send_raw(self, data: bytes):
        self.sock.sendall(data)

    def send_packet(self, body: str):
        data = body.encode()
        pkt = b"$" + data + b"#" + f"{self._checksum(data):02x}".encode()
        self._send_raw(pkt)
        ack = self._read_byte()
        if ack != b"+":
            raise GdbRspError(f"no ack for packet {body!r}, got {ack!r}")

    def _read_byte(self) -> bytes:
        if not self._buf:
            self._buf = self.sock.recv(4096)
            if not self._buf:
                raise GdbRspError("connection closed")
        b, self._buf = self._buf[:1], self._buf[1:]
        return b

    def read_packet(self) -> str:
        """Read one '$...#cc'-framed packet body, ACKing it. Skips any
        stray '+'/'-' bytes (e.g. a leftover ack from a prior exchange)."""
        while True:
            b = self._read_byte()
            if b == b"$":
                break
        data = b""
        while True:
            b = self._read_byte()
            if b == b"#":
                break
            data += b
        # two checksum bytes, not validated (trusted local link)
        self._read_byte()
        self._read_byte()
        self._send_raw(b"+")
        return data.decode(errors="replace")

    # -- higher-level operations ------------------------------------

    def handshake(self):
        """QEMU's gdbstub starts in ack mode; nothing to negotiate for our
        minimal needs beyond just being ready to send/receive packets."""
        pass

    def read_registers(self) -> dict:
        self.send_packet("g")
        raw = self.read_packet()
        # 16 general regs (r0-r15) + cpsr, each 8 hex chars, little-endian
        # bytes within each 4-byte register (per QEMU's arm-core.xml).
        regs = {}
        names = [f"r{i}" for i in range(16)] + ["cpsr"]
        for i, name in enumerate(names):
            chunk = raw[i * 8:(i + 1) * 8]
            if len(chunk) < 8:
                break
            le_bytes = bytes.fromhex(chunk)
            regs[name] = int.from_bytes(le_bytes, "little")
        return regs

    def write_registers(self, regs: dict):
        """Write r0-r15 + cpsr in one 'G' packet. `regs` must be a dict
        as returned by read_registers() (same key names) -- typically
        read_registers() first, mutate a few keys, then pass it back
        here, so every register keeps its current value except the ones
        deliberately changed."""
        names = [f"r{i}" for i in range(16)] + ["cpsr"]
        data = "".join(
            regs[name].to_bytes(4, "little").hex() for name in names
        )
        self.send_packet(f"G{data}")
        reply = self.read_packet()
        if reply != "OK":
            raise GdbRspError(f"write_registers error: {reply}")

    def read_memory(self, addr: int, length: int) -> bytes:
        self.send_packet(f"m{addr:x},{length:x}")
        raw = self.read_packet()
        if raw.startswith("E"):
            raise GdbRspError(f"read_memory error: {raw}")
        return bytes.fromhex(raw)

    def write_memory(self, addr: int, data: bytes):
        hexdata = data.hex()
        self.send_packet(f"M{addr:x},{len(data):x}:{hexdata}")
        reply = self.read_packet()
        if reply != "OK":
            raise GdbRspError(f"write_memory error: {reply}")

    def write_u32(self, addr: int, value: int):
        self.write_memory(addr, value.to_bytes(4, "little"))

    def read_u32(self, addr: int) -> int:
        return int.from_bytes(self.read_memory(addr, 4), "little")

    def cont(self):
        """Resume execution. Non-blocking at the wire level -- reply comes
        later as a stop-reply packet, read it with wait_stop()."""
        data = b"$c#63"
        self._send_raw(data)
        ack = self._read_byte()
        if ack != b"+":
            raise GdbRspError(f"no ack for continue, got {ack!r}")

    def interrupt(self):
        """Send the RSP out-of-band interrupt (raw 0x03), no packet
        framing -- this is what actually stops a running target."""
        self._send_raw(b"\x03")

    def wait_stop(self, timeout: float | None = None) -> str:
        old_timeout = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            return self.read_packet()
        finally:
            self.sock.settimeout(old_timeout)

    def status(self) -> str:
        """Query current stop status ('?') -- unlike interrupt(), this
        always gets a reply whether the target is already stopped or
        running, so it's the safe way to establish a known state."""
        self.send_packet("?")
        return self.read_packet()

    def step(self) -> str:
        self.send_packet("s")
        return self.wait_stop()

    # -- breakpoints ---------------------------------------------------
    # 'Z0'/'z0' = software breakpoint (type 0), handled generically by
    # QEMU's TCG accel (gdb_breakpoint_insert -> cpu_breakpoint_insert),
    # confirmed against qemu-src/gdbstub/{gdbstub,system}.c -- not
    # ARM-specific. `kind` is the instruction length in bytes; this
    # project's breakpoints are all ARM-mode (4-byte) so it's hardcoded
    # here rather than threaded through as a parameter.

    def set_breakpoint(self, addr: int, kind: int = 4):
        self.send_packet(f"Z0,{addr:x},{kind:x}")
        reply = self.read_packet()
        if reply != "OK":
            raise GdbRspError(f"set_breakpoint({addr:#x}) error: {reply!r}")

    def remove_breakpoint(self, addr: int, kind: int = 4):
        self.send_packet(f"z0,{addr:x},{kind:x}")
        reply = self.read_packet()
        if reply != "OK":
            raise GdbRspError(f"remove_breakpoint({addr:#x}) error: {reply!r}")

    # Watchpoints (`Z2`=write, `Z3`=read, `Z4`=access) -- QEMU's TCG-based
    # gdbstub implements these generically (cpu_watchpoint_insert), not
    # ARM-specific, added 2026-09-09 to find a write to a RAM address with
    # no known static writer instead of guessing which function to trace.

    def set_watchpoint(self, addr: int, length: int = 1, kind: int = 2):
        self.send_packet(f"Z{kind},{addr:x},{length:x}")
        reply = self.read_packet()
        if reply != "OK":
            raise GdbRspError(f"set_watchpoint({addr:#x}) error: {reply!r}")

    def remove_watchpoint(self, addr: int, length: int = 1, kind: int = 2):
        self.send_packet(f"z{kind},{addr:x},{length:x}")
        reply = self.read_packet()
        if reply != "OK":
            raise GdbRspError(f"remove_watchpoint({addr:#x}) error: {reply!r}")
