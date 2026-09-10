#!/usr/bin/env python3
"""Differential test (2026-09-10). HISTORICAL/regression-reference now, not a live comparison:
the write-drop and wraparound bugs this test found below led directly to riic.c being rewired
onto a real I2CBus + hw/nvram/eeprom_at24c.c slave device (same session, see README.md's Status
section) -- so `OurRiicModel` below no longer reflects riic.c's actual current code at all (it
intentionally models the OLD, now-replaced hand-rolled array behavior). Kept, unmodified, as the
record of what the bug was and why the fix was needed; run it if you want to see that record
reproduce, not to check today's riic.c.

Original header: our own riic.c virtual-EEPROM data-plane semantics vs. QEMU's
real hw/nvram/eeprom_at24c.c reference implementation (both ported faithfully from their real C,
not re-derived from scratch -- see the comments citing exact source lines/behavior).

This is NOT a live-QEMU test -- it's a pure-Python behavioral model comparison of the EEPROM
"backing store" logic only (address counter, auto-increment/wrap, read, write). riic.c's own
RIIC *controller* protocol/IRQ timing (CR2/SR2/STI/TI/TEI/RI/SPI sequencing) is unrelated to this
comparison and untouched -- see qemu-machine/README.md's Status section for why the two are
separable, what this test found, and what a full live A/B (wiring riic.c to a real I2CBus +
at24c-eeprom device instead of its own hand-rolled array) would additionally need.

Usage: python3 tools/eeprom_ab_diff_test.py
"""

ROM_SIZE = 16384  # GT24C128B, matches build_riic_eeprom_image.py


class Reference24C:
    """Faithful port of hw/nvram/eeprom_at24c.c's at24c_eeprom_send/recv/event (2-byte address,
    since rsize > 256 per its own init_rom() -- asize = 1 if rsize<=256 else 2, confirmed by
    reading the real init function; not shown here since only send/recv/event matter)."""

    def __init__(self, rom, rsize=ROM_SIZE, asize=2, writable=True):
        self.mem = bytearray(rom) if rom else bytearray(rsize)
        assert len(self.mem) == rsize
        self.rsize = rsize
        self.asize = asize
        self.writable = writable
        self.cur = 0
        self.haveaddr = 0

    def event_start_send(self):
        self.haveaddr = 0

    def event_start_recv(self):
        pass  # only affects blk flush in the real code, no logic-visible effect here

    def event_finish(self):
        self.haveaddr = 0

    def send(self, data):
        """I2C_START_SEND / mid-transaction data byte from master to slave."""
        if self.haveaddr < self.asize:
            self.cur = ((self.cur << 8) | data) & 0xFFFFFFFF
            self.haveaddr += 1
            if self.haveaddr == self.asize:
                self.cur %= self.rsize
        else:
            if self.writable:
                self.mem[self.cur] = data
            self.cur = (self.cur + 1) % self.rsize

    def recv(self):
        """I2C_START_RECV / mid-transaction data byte from slave to master."""
        if 0 < self.haveaddr < self.asize:
            return 0xFF
        ret = self.mem[self.cur]
        self.cur = (self.cur + 1) % self.rsize
        return ret


class OurRiicModel:
    """Faithful port of src/riic.c's actual behavior:
    - riic_eeprom_read(): pread(image_fd, 1, addr) -- image_fd is O_RDONLY (confirmed by
      grepping riic.c: only `open(s->image_path, O_RDONLY)` appears) -- returns 0 past EOF.
    - mem_addr: uint16_t, set from two DRT writes (WAIT_MEM_HI << 8 | WAIT_MEM_LO), incremented
      with plain C uint16_t wraparound (mod 65536), NOT modulo the real EEPROM size (0x4000) --
      there is no `% ROM_SIZE` anywhere in riic.c's mem_addr handling.
    - Write-data path: after the 2 address bytes, the only phases the DRT-write switch handles
      are RIIC_WAIT_RESTART (expects CR2=RS) and (via CR2) SP/STOP -- there is no phase that
      accepts further DRT writes as data. A DRT write in RIIC_WAIT_RESTART phase falls through to
      the `default:` case, which only logs "unexpected DRT write" and does nothing -- so it is a
      structural no-op: every value ever offered as write DATA (i.e. every byte after the 2
      address bytes, before a restart/stop) is discarded, not merely "not the model's write
      path" -- there is no write path, and image_fd is opened read-only regardless.
    """

    def __init__(self, rom):
        self.rom = bytes(rom) if rom else b""
        self.mem_addr = 0
        self._addr_bytes_seen = 0

    def set_addr_hi(self, byte):
        self.mem_addr = (byte << 8) & 0xFFFF
        self._addr_bytes_seen = 1

    def set_addr_lo(self, byte):
        self.mem_addr = (self.mem_addr | byte) & 0xFFFF
        self._addr_bytes_seen = 2

    def send_data_after_address(self, byte):
        """Real hardware/reference: this would be a write-data byte. Our model: dropped."""
        pass  # no-op, matches "unexpected DRT write" default case doing nothing

    def recv(self):
        addr = self.mem_addr
        byte = self.rom[addr] if addr < len(self.rom) else 0
        self.mem_addr = (self.mem_addr + 1) & 0xFFFF  # uint16_t wraparound, NOT % ROM_SIZE
        return byte


def run_case(name, rom, ops, verbose=False):
    """ops: list of ('addr_hi', b) / ('addr_lo', b) / ('send', b) / ('recv',) / ('restart',)"""
    ref = Reference24C(rom)
    ours = OurRiicModel(rom)
    ref.event_start_send()
    mismatches = []
    addr_bytes = 0
    for i, op in enumerate(ops):
        kind = op[0]
        if kind == 'addr_hi':
            ref.send(op[1])
            ours.set_addr_hi(op[1])
            addr_bytes = 1
        elif kind == 'addr_lo':
            ref.send(op[1])
            ours.set_addr_lo(op[1])
            addr_bytes = 2
        elif kind == 'send':  # write-data byte, post-address
            ref.send(op[1])
            ours.send_data_after_address(op[1])
        elif kind == 'restart':
            ref.event_start_recv()
        elif kind == 'recv':
            r = ref.recv()
            o = ours.recv()
            if r != o:
                mismatches.append((i, op, r, o))
            if verbose:
                print(f"  [{i}] recv: ref={r:#04x} ours={o:#04x} {'OK' if r==o else 'MISMATCH'}")
    status = "PASS" if not mismatches else f"FAIL ({len(mismatches)} mismatches)"
    print(f"{name}: {status}")
    if mismatches:
        for i, op, r, o in mismatches[:5]:
            print(f"    op[{i}]={op}  ref={r:#04x}  ours={o:#04x}")
    return not mismatches


def addr_ops(hi, lo):
    return [('addr_hi', hi), ('addr_lo', lo)]


def main():
    import random
    rng = random.Random(42)
    rom = bytes(rng.randrange(256) for _ in range(ROM_SIZE))

    print(f"Differential test: our riic.c EEPROM model vs. QEMU's real at24c_eeprom.c "
          f"(rsize={ROM_SIZE})\n")

    results = []

    # 1. Plain sequential read, matches the real traced 32-byte-chunk scan pattern.
    ops = addr_ops(0x04, 0x20) + [('restart',)] + [('recv',)] * 32
    results.append(run_case("sequential 32-byte read @0x0420 (matches traced FUN_2006cb84 scan)",
                             rom, ops))

    # 2. The documented "dummy read after switching to receive mode" quirk -- one extra recv()
    #    before the real data, per riic.c's own file comment (0x2001dbf4 dead-store finding).
    ops = addr_ops(0x3d, 0xf0) + [('restart',)] + [('recv',)] * 17  # 1 dummy + 16 real
    results.append(run_case("dummy-read-then-16-real-bytes quirk @0x3df0", rom, ops, verbose=True))

    # 3. Read straddling the real EEPROM's own size boundary (0x4000) -- checks wraparound.
    ops = addr_ops(0x3f, 0xf8) + [('restart',)] + [('recv',)] * 16  # crosses 0x4000
    results.append(run_case("read crossing the real 0x4000 EEPROM-size boundary", rom, ops))

    # 4. Read starting past the real EEPROM's size entirely (still < 0x10000, a real 2-byte
    #    address can express up to 0xffff even though the device is only 0x4000 bytes).
    ops = addr_ops(0x50, 0x00) + [('restart',)] + [('recv',)] * 8
    results.append(run_case("read at 0x5000 (past real EEPROM size, within 16-bit address range)",
                             rom, ops))

    # 5. A genuine WRITE transaction: address, then real data bytes, then (in the real protocol)
    #    a STOP with no restart -- no read ever happens. Checked by writing then reading back
    #    (via a fresh transaction) with each model's own semantics.
    write_ops = addr_ops(0x3d, 0xf0) + [('send', b) for b in b"SX3765 V0.9H-000"]
    ref = Reference24C(rom)
    ours = OurRiicModel(rom)
    for op in write_ops:
        if op[0] == 'addr_hi':
            ref.send(op[1]); ours.set_addr_hi(op[1])
        elif op[0] == 'addr_lo':
            ref.send(op[1]); ours.set_addr_lo(op[1])
        elif op[0] == 'send':
            ref.send(op[1]); ours.send_data_after_address(op[1])
    # Now read back offset 0x3df0 in a FRESH transaction on each model (a real re-addressing,
    # same as the real firmware would do for a later, separate read of the same offset).
    ref.event_start_send()
    ref.send(0x3d); ref.send(0xf0)  # re-send the 2 address bytes, same as a real fresh transaction
    ref.event_start_recv()
    read_back_ref = bytes(ref.recv() for _ in range(16))
    ours2 = OurRiicModel(rom)  # fresh "transaction" -- re-point at the same address
    ours2.set_addr_hi(0x3d); ours2.set_addr_lo(0xf0)
    read_back_ours = bytes(ours2.recv() for _ in range(16))
    write_persisted_ref = (read_back_ref == b"SX3765 V0.9H-000")
    write_persisted_ours = (read_back_ours == b"SX3765 V0.9H-000")
    print(f"\nwrite-then-read-back @0x3df0 (\"SX3765 V0.9H-000\", the real traced cold-boot write):")
    print(f"  reference model persisted the write:  {write_persisted_ref}")
    print(f"  our riic.c model persisted the write: {write_persisted_ours}  "
          f"({'matches real hardware behavior' if write_persisted_ours else 'BUG -- silently drops all EEPROM writes'})")
    results.append(write_persisted_ours == write_persisted_ref)

    print(f"\n{'='*70}")
    print(f"{sum(results)}/{len(results)} scenarios match between our model and the reference.")


if __name__ == "__main__":
    main()
