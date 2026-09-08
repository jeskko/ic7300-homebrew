#!/usr/bin/env python3
"""Standalone validation of the rza1h-mmc device: drive a real SD card
identification sequence (CMD0/CMD8/CMD55+ACMD41/CMD2/CMD3/CMD9/CMD7/
CMD16/CMD17) directly over GDB, exactly the way this session validated
the GIC/OSTM0 IRQ-delivery work -- independent of whether body.bin's own
driver has been found/triggered yet (see qemu-machine/README.md's
SD-card section).

Usage: point -global rza1h-mmc.image=<path> at a raw image with known
content at block 0/1 (see the block above `main()`), then run this
against an already-running, GDB-stopped `rz-a1h` instance.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdbrsp import GdbRsp

MMC_BASE = 0xE804C800
REG_CMD_SETH = MMC_BASE + 0x00
REG_CMD_SETL = MMC_BASE + 0x02
REG_ARG = MMC_BASE + 0x08
REG_BLOCK_SET = MMC_BASE + 0x14
REG_RESP0 = MMC_BASE + 0x2C
REG_DATA = MMC_BASE + 0x34
REG_INT = MMC_BASE + 0x40
REG_HOST_STS1 = MMC_BASE + 0x48
REG_DETECT = MMC_BASE + 0x70

INT_CRSPE = 1 << 16
INT_BUFREN = 1 << 20


def send_cmd(g: GdbRsp, cmd: int, arg: int, rtyp: int = 1, wdat: bool = False,
            dwen: bool = False, cmlte: bool = False, cmd12en: bool = False) -> int:
    """Issue one command, wait for CRSPE, return CE_RESP0. Mirrors the
    manual's own documented sequence: CE_ARG then CE_CMD_SETL (setup, no
    trigger) then CE_CMD_SETH (the real trigger)."""
    g.write_u32(REG_INT, 0)  # clear all flags first
    g.write_u32(REG_ARG, arg)
    g.write_memory(REG_CMD_SETL, (0).to_bytes(2, "little"))
    seth = ((cmd << 8) | (rtyp << 6)
           | ((1 << 3) if wdat else 0) | ((1 << 2) if dwen else 0)
           | ((1 << 1) if cmlte else 0) | (1 if cmd12en else 0))
    g.write_memory(REG_CMD_SETH, seth.to_bytes(2, "little"))
    ce_int = g.read_u32(REG_INT)
    assert ce_int & INT_CRSPE, f"CMD{cmd}: CRSPE not set, CE_INT={ce_int:#x}"
    return g.read_u32(REG_RESP0)


def main():
    g = GdbRsp(port=1234)
    g.status()

    detect = g.read_u32(REG_DETECT)
    print(f"CE_DETECT={detect:#x} (bit14 CDSIG should be set: card 'inserted')")
    assert detect & (1 << 14)

    resp = send_cmd(g, 0, 0, rtyp=0)
    print(f"CMD0 (GO_IDLE_STATE): ok")

    resp = send_cmd(g, 8, 0x1AA)
    print(f"CMD8 (SEND_IF_COND): resp={resp:#x} (expect echo 0x1aa)")
    assert resp == 0x1AA

    for attempt in range(3):
        resp = send_cmd(g, 55, 0)
        assert resp & 0x20, f"CMD55: APP_CMD bit not set, resp={resp:#x}"
        resp = send_cmd(g, 41, 0x40FF8000, rtyp=3)
        print(f"ACMD41 (SD_SEND_OP_COND) attempt {attempt}: OCR={resp:#x}")
        if resp & 0x80000000:
            break
    assert resp & 0x80000000, "ACMD41 never reported power-up complete"
    assert resp & 0x40000000, "ACMD41 OCR missing CCS (expected SDHC)"

    resp = send_cmd(g, 2, 0, rtyp=2)
    print(f"CMD2 (ALL_SEND_CID): resp0={resp:#x}")

    resp = send_cmd(g, 3, 0)
    rca = resp >> 16
    print(f"CMD3 (SEND_RELATIVE_ADDR): RCA={rca:#x}")
    assert rca != 0

    resp = send_cmd(g, 9, rca << 16, rtyp=2)
    print(f"CMD9 (SEND_CSD): resp0={resp:#x}")

    resp = send_cmd(g, 7, rca << 16)
    print(f"CMD7 (SELECT_CARD): resp={resp:#x}")

    resp = send_cmd(g, 16, 512)
    print(f"CMD16 (SET_BLOCKLEN=512): resp={resp:#x}")

    # CMD17: read block 0, expect our known test pattern.
    resp = send_cmd(g, 17, 0, wdat=True, dwen=False)
    ce_int = g.read_u32(REG_INT)
    assert ce_int & INT_BUFREN, f"CMD17: BUFREN not set, CE_INT={ce_int:#x}"
    words = [g.read_u32(REG_DATA) for _ in range(4)]
    block0_start = b"".join(w.to_bytes(4, "little") for w in words)
    print(f"CMD17 (READ_SINGLE_BLOCK 0): first 16 bytes = {block0_start!r}")
    assert block0_start == b"HELLO_SD_BLOCK0!", block0_start

    # Read block 1 too, to confirm LBA addressing/CE_ARG handling.
    resp = send_cmd(g, 17, 1, wdat=True, dwen=False)
    words = [g.read_u32(REG_DATA) for _ in range(4)]
    block1_start = b"".join(w.to_bytes(4, "little") for w in words)
    print(f"CMD17 (READ_SINGLE_BLOCK 1): first 16 bytes = {block1_start!r}")
    assert block1_start == b"HELLO_SD_BLOCK1!", block1_start

    # Multi-block read: CMD18 (CMLTE set), 2 blocks starting at LBA 0.
    g.write_u32(REG_BLOCK_SET, (2 << 16) | 512)
    send_cmd(g, 18, 0, wdat=True, cmlte=True)
    multi = b"".join(
        g.read_u32(REG_DATA).to_bytes(4, "little") for _ in range(256)
    )
    print(f"CMD18 (READ_MULTIPLE_BLOCK, 2 blocks): "
         f"block0={multi[:16]!r} block1={multi[512:528]!r}")
    assert multi[:16] == b"HELLO_SD_BLOCK0!"
    assert multi[512:528] == b"HELLO_SD_BLOCK1!"
    ce_int = g.read_u32(REG_INT)
    assert ce_int & (1 << 22), f"BUFRE not set after final block, CE_INT={ce_int:#x}"

    # Write: CMD24 to LBA 5, then read it back with CMD17.
    send_cmd(g, 24, 5, wdat=True, dwen=True)
    payload = b"WRITE_TEST_BLK5!" + b"\x00" * (512 - 16)
    for i in range(0, 512, 4):
        g.write_u32(REG_DATA, int.from_bytes(payload[i:i + 4], "little"))
    ce_int = g.read_u32(REG_INT)
    assert ce_int & (1 << 23), f"DTRANE not set after write, CE_INT={ce_int:#x}"
    send_cmd(g, 17, 5, wdat=True)
    readback = b"".join(g.read_u32(REG_DATA).to_bytes(4, "little") for _ in range(4))
    print(f"CMD24 write + CMD17 readback of LBA 5: {readback!r}")
    assert readback == b"WRITE_TEST_BLK5!"

    print("\nALL CHECKS PASSED -- rza1h-mmc's standalone command/response/"
         "data-read/write protocol (including multi-block) is self-"
         "consistent and correctly round-trips real backing-image content.")
    g.close()


if __name__ == "__main__":
    main()
