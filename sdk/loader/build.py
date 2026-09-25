#!/usr/bin/env python3
"""Build the homebrew loader firmware: a stock 1.42 container with sdk/loader/loader.c appended
and wired in. Flash the result once (SET > SD Card > Firmware Update); after that, apps are just
.BIN files in \\homebrew\\ on the SD card (sdk/tools/build_app.py), picked from SD Card > Homebrew Apps.

Usage:
    python3 sdk/loader/build.py [container.dat] [output.dat]

Defaults to /data/misc/icom/7300/7300_142.dat -> scratch/hb_loader_142.dat (repo-relative).

Patches, all checked against the expected stock bytes first:
  - main_idle_loop's `bl civ_tx_pump` (0x20052f64) -> `bl hb_idle_hook` (per-pass app tick)
  - SD CARD menu (g_settings_category_registry[0x18]): 8 -> 9 items, list pointer -> a copy
    with a "Homebrew Apps" row appended, whose catalog record's action is hb_menu_action.
    The list, label and record live in a confirmed-unused padding gap inside the image, so
    only the action pointer reaches into appended RAM. The 14 catalog slots after that record
    are left zero for the app picker's rows, which loader.c fills in at runtime (see
    sdk/examples/homebrew-apps-menu/README.md for why, and notes/ui-menu.md).
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent
REPO = SDK.parent
sys.path.insert(0, str(REPO / "tools"))
from icom_fw.container import pack, parse  # noqa: E402

BODY_BASE = 0x20005000
LOADER_ADDR = 0x20600000

IDLE_CALL_SITE = 0x20052f64
IDLE_CALL_ORIG = bytes.fromhex("06f9feeb")          # bl civ_tx_pump (0x20011384)

REGISTRY_SD_CARD = 0x20199500
REGISTRY_ORIG_COUNT = 8
REGISTRY_ORIG_LIST = 0x201990bc

CATALOG_BASE = 0x2018ed48
NEW_CATALOG_INDEX = 0x881                          # the Homebrew Apps row
APP_ROW_SLOTS = 14                                 # 0x882.., filled at runtime (loader.c MAX_APPS)
NEW_CATALOG_RECORD = CATALOG_BASE + NEW_CATALOG_INDEX * 20
NEW_LIST_ADDR = NEW_CATALOG_RECORD + 20 * (1 + APP_ROW_SLOTS)
NEW_LABEL_ADDR = NEW_LIST_ADDR + 36
GAP_END = 0x201998cc
FORMAT_FLAGS = 0x00010700
LABEL = b"Homebrew Apps\0"

CFLAGS = [
    "-mcpu=cortex-a9", "-marm", "-mfloat-abi=soft", "-Os", "-std=c11", "-Wall", "-Wextra",
    "-ffreestanding", "-fno-builtin", "-nostdlib", "-fno-pic", f"-I{SDK / 'include'}",
]


def compile_loader(work: Path) -> tuple[bytes, dict[str, int]]:
    elf = work / "loader.elf"
    subprocess.run(["arm-none-eabi-gcc", *CFLAGS, "-T", str(HERE / "loader.ld"),
                    str(HERE / "loader.c"), "-lgcc", "-o", str(elf)], check=True)
    blob = work / "loader.bin"
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", str(elf), str(blob)], check=True)
    nm = subprocess.run(["arm-none-eabi-nm", str(elf)], check=True, capture_output=True,
                        text=True).stdout
    syms = {m.group(2): int(m.group(1), 16)
            for m in (re.match(r"^([0-9a-fA-F]+)\s+\S+\s+(\S+)$", l) for l in nm.splitlines()) if m}
    return blob.read_bytes(), syms


def arm_bl(site: int, target: int) -> bytes:
    off = (target - (site + 8)) >> 2
    assert -(1 << 23) <= off < (1 << 23)
    return struct.pack("<I", 0xeb000000 | (off & 0xffffff))


def build(container_in: Path, container_out: Path) -> None:
    work = HERE / "build"
    work.mkdir(exist_ok=True)
    loader, syms = compile_loader(work)
    action, idle = syms["hb_menu_action"], syms["hb_idle_hook"]
    print(f"loader.c -> {len(loader)} bytes at {LOADER_ADDR:#x} "
          f"(hb_menu_action {action:#x}, hb_idle_hook {idle:#x})")

    body = bytearray(parse(container_in.read_bytes()).body.decompressed)

    def at(addr: int, n: int) -> bytes:
        return bytes(body[addr - BODY_BASE:addr - BODY_BASE + n])

    def put(addr: int, data: bytes) -> None:
        body[addr - BODY_BASE:addr - BODY_BASE + len(data)] = data

    def word(addr: int) -> int:
        return struct.unpack("<I", at(addr, 4))[0]

    # Check everything we're about to overwrite is what we expect (i.e. this is 1.42).
    assert at(IDLE_CALL_SITE, 4) == IDLE_CALL_ORIG, "idle call site mismatch -- wrong version?"
    assert word(REGISTRY_SD_CARD) == REGISTRY_ORIG_COUNT, "SD CARD registry count mismatch"
    assert word(REGISTRY_SD_CARD + 4) == REGISTRY_ORIG_LIST, "SD CARD registry list mismatch"
    assert NEW_LABEL_ADDR + len(LABEL) <= GAP_END
    assert at(NEW_CATALOG_RECORD, GAP_END - NEW_CATALOG_RECORD).count(0) == GAP_END - NEW_CATALOG_RECORD, \
        "padding gap not empty"

    put(IDLE_CALL_SITE, arm_bl(IDLE_CALL_SITE, idle))

    new_list = at(REGISTRY_ORIG_LIST, 4 * REGISTRY_ORIG_COUNT) + struct.pack("<I", 3 | (NEW_CATALOG_INDEX << 16))
    put(NEW_LIST_ADDR, new_list)
    put(NEW_LABEL_ADDR, LABEL)
    put(NEW_CATALOG_RECORD, struct.pack("<5I", action, 0, FORMAT_FLAGS, NEW_LABEL_ADDR, NEW_LABEL_ADDR))
    put(REGISTRY_SD_CARD, struct.pack("<2I", REGISTRY_ORIG_COUNT + 1, NEW_LIST_ADDR))

    pad = LOADER_ADDR - BODY_BASE - len(body)
    assert pad > 0
    new_body = bytes(body) + bytes(pad) + loader
    out = pack(container_in.read_bytes(), new_body)
    container_out.write_bytes(out)
    print(f"wrote {container_out} ({len(out)} bytes)")


if __name__ == "__main__":
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/data/misc/icom/7300/7300_142.dat")
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else REPO / "scratch" / "hb_loader_142.dat"
    dst.parent.mkdir(parents=True, exist_ok=True)
    build(src, dst)
