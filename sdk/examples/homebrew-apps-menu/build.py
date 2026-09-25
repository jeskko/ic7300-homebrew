#!/usr/bin/env python3
"""Build the homebrew-apps-menu example: a modified firmware container with a real, visible
"Homebrew Apps" row added to the SD CARD menu -- plus APP.BIN, reused unmodified from
sdk/examples/sd-card-app/.

Live-tested procedure (2026-09-25) -- see README.md in this directory for the full test log
(including screenshots of the real menu tree with the new row). Requires the arm-none-eabi
toolchain (as/ld/objcopy) on PATH.

Usage:
    python3 sdk/examples/homebrew-apps-menu/build.py [container.dat] [output.dat] [app_output.bin]

Defaults to /data/misc/icom/7300/7300_142.dat -> scratch/homebrew_apps_menu_142.dat +
scratch/APP.BIN (repo-relative). To test in qemu-machine, same steps as
sdk/examples/sd-card-app/README.md, but there's no key combo to press -- MENU > SET > (page 2)
SD Card > (page 3) Homebrew Apps.
"""

from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SD_CARD_APP = HERE.parent / "sd-card-app"
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools"))
from icom_fw.container import pack  # noqa: E402

BODY_BASE = 0x20005000
HOOK_ADDR = 0x20600000              # same confirmed-safe address the other two examples use
APP_LOAD_ADDR = 0x20610000          # must match menu_hook.s's own APP_LOAD_ADDR .equ, and
                                     # sd-card-app/app_main.s's own link address

# g_settings_category_registry[0x18] -- the SD CARD menu's own entry (notes/ui-menu.md).
REGISTRY_SD_CARD = 0x20199500
REGISTRY_ORIG_COUNT = 8
REGISTRY_ORIG_LIST = 0x201990bc

# g_settings_item_catalog's own base, and the padding slot the new record goes in
# (0x20199758-0x201998cc is confirmed-unused -- see notes/ui-menu.md).
CATALOG_BASE = 0x2018ed48
NEW_CATALOG_INDEX = 0x881
NEW_CATALOG_RECORD = CATALOG_BASE + NEW_CATALOG_INDEX * 20
FORMAT_FLAGS = 0x00010700           # copied from the real "Format" row's own flags field


def run(*args: str) -> None:
    subprocess.run(args, check=True, cwd=HERE)


def assemble_at(src: Path, address: int, out_bin: Path) -> bytes:
    obj = out_bin.with_suffix(".o")
    elf = out_bin.with_suffix(".elf")
    run("arm-none-eabi-as", "-mcpu=cortex-a9", "-march=armv7-a", str(src), "-o", str(obj))
    run("arm-none-eabi-ld", f"-Ttext={address:#x}", str(obj), "-o", str(elf))
    run("arm-none-eabi-objcopy", "-O", "binary", str(elf), str(out_bin))
    return out_bin.read_bytes()


def poke_word(body: bytearray, addr: int, val: int) -> None:
    off = addr - BODY_BASE
    body[off:off + 4] = struct.pack("<I", val)


def check_word(body: bytes, addr: int, expected: int) -> None:
    off = addr - BODY_BASE
    got = struct.unpack_from("<I", body, off)[0]
    assert got == expected, f"at {addr:#x}: expected {expected:#x}, got {got:#x} -- wrong container version?"


def build(container_in: Path, container_out: Path, app_out: Path) -> None:
    scratch = HERE / "build"
    scratch.mkdir(exist_ok=True)

    # 1. Assemble the menu hook (goes into the firmware, appended -- no call-site patch this
    #    time, this example only ever touches data) and the app (goes on the SD card, byte-
    #    identical to sd-card-app's own APP.BIN -- same source file, same link address).
    menu_hook_bin = assemble_at(HERE / "menu_hook.s", HOOK_ADDR, scratch / "menu_hook.bin")
    print(f"menu_hook.s -> {len(menu_hook_bin)} bytes at {HOOK_ADDR:#x}")
    app_bin = assemble_at(SD_CARD_APP / "app_main.s", APP_LOAD_ADDR, scratch / "app_main.bin")
    app_out.write_bytes(app_bin)
    print(f"app_main.s (from sd-card-app) -> {len(app_bin)} bytes, wrote {app_out}")

    # 2. Unpack a real release's body.
    unpack_dir = scratch / "unpacked"
    if not (unpack_dir / "body.bin").exists():
        from icom_fw.container import parse
        unpack_dir.mkdir(exist_ok=True)
        (unpack_dir / "body.bin").write_bytes(parse(container_in.read_bytes()).body.decompressed)
    body = bytearray((unpack_dir / "body.bin").read_bytes())

    # 3. Sanity-check the existing bytes before patching, then apply the two data patches.
    check_word(body, REGISTRY_SD_CARD, REGISTRY_ORIG_COUNT)
    check_word(body, REGISTRY_SD_CARD + 4, REGISTRY_ORIG_LIST)
    for off in (0x00, 0x04, 0x08, 0x0c, 0x10):
        check_word(body, NEW_CATALOG_RECORD + off, 0)  # confirmed-blank padding

    new_list_addr = HOOK_ADDR + 0x120  # sd_menu_list_v2's own link address -- see nm output
                                        # in README.md if this ever needs re-deriving
    poke_word(body, REGISTRY_SD_CARD, REGISTRY_ORIG_COUNT + 1)
    poke_word(body, REGISTRY_SD_CARD + 4, new_list_addr)

    menu_label_addr = HOOK_ADDR + 0x110  # menu_label's own link address
    poke_word(body, NEW_CATALOG_RECORD + 0x00, HOOK_ADDR)         # action = homebrew_menu_action
    poke_word(body, NEW_CATALOG_RECORD + 0x04, 0)                 # query = NULL, always selectable
    poke_word(body, NEW_CATALOG_RECORD + 0x08, FORMAT_FLAGS)
    poke_word(body, NEW_CATALOG_RECORD + 0x0c, menu_label_addr)   # en
    poke_word(body, NEW_CATALOG_RECORD + 0x10, menu_label_addr)   # jp (same, like several real rows)

    # 4. Append the menu hook (code + data + the new 9-entry list) after the static image.
    pad_len = HOOK_ADDR - BODY_BASE - len(body)
    assert pad_len > 0, f"HOOK_ADDR is inside the existing image (need pad_len > 0, got {pad_len})"
    new_body = bytes(body) + bytes(pad_len) + menu_hook_bin

    # 5. Repack into a valid, checksum-correct container.
    new_container = pack(container_in.read_bytes(), new_body)
    container_out.write_bytes(new_container)
    print(f"wrote {container_out} ({len(new_container)} bytes, "
          f"body {len(body)} -> {len(new_body)} decompressed)")


if __name__ == "__main__":
    default_in = Path("/data/misc/icom/7300/7300_142.dat")
    default_out = REPO / "scratch" / "homebrew_apps_menu_142.dat"
    default_app = REPO / "scratch" / "APP.BIN"
    container_in = Path(sys.argv[1]) if len(sys.argv) > 1 else default_in
    container_out = Path(sys.argv[2]) if len(sys.argv) > 2 else default_out
    app_out = Path(sys.argv[3]) if len(sys.argv) > 3 else default_app
    container_out.parent.mkdir(parents=True, exist_ok=True)
    app_out.parent.mkdir(parents=True, exist_ok=True)
    build(container_in, container_out, app_out)
