#!/usr/bin/env python3
"""Build the homebrew-apps-menu example: a modified firmware container with a real, visible
"Homebrew Apps" row added to the SD CARD menu -- plus APP.BIN, reused unmodified from
sdk/examples/sd-card-app/.

Live-tested procedure (2026-09-25) -- see README.md in this directory for the full test log
(including screenshots of the real menu tree with the new row). Requires the arm-none-eabi
toolchain (as/ld/objcopy/nm) on PATH.

Usage:
    python3 sdk/examples/homebrew-apps-menu/build.py [container.dat] [output.dat] [app_output.bin]

Defaults to /data/misc/icom/7300/7300_142.dat -> scratch/homebrew_apps_menu_142.dat +
scratch/APP.BIN (repo-relative). To test in qemu-machine, same steps as
sdk/examples/sd-card-app/README.md, but there's no key combo to press -- MENU > SET > (page 2)
SD Card > (page 3) Homebrew Apps.

2026-09-25, adversarial review pass: `sd_menu_list_v2` and `menu_label`'s own byte content is
now copied into the firmware image's own read-only padding gap (next to the new catalog
record) instead of pointing the registry/catalog at their appended-RAM addresses -- see
menu_hook.s's own header comment for why. Symbol addresses are read from `nm` rather than
hardcoded, so an edit to menu_hook.s can't silently desync the patches from the real layout.
"""

from __future__ import annotations

import re
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
# (0x20199758-0x201998cc is confirmed-unused -- see notes/ui-menu.md). The list (36 bytes) and
# label (<=16 bytes) live right after the record itself, still inside the same confirmed-unused
# gap -- see the bounds assertion in build() below.
CATALOG_BASE = 0x2018ed48
NEW_CATALOG_INDEX = 0x881
NEW_CATALOG_RECORD = CATALOG_BASE + NEW_CATALOG_INDEX * 20
NEW_LIST_ROM_ADDR = NEW_CATALOG_RECORD + 20     # 0x20199770
NEW_LABEL_ROM_ADDR = NEW_LIST_ROM_ADDR + 36     # 0x20199794
GAP_END = 0x201998cc
FORMAT_FLAGS = 0x00010700           # copied from the real "Format" row's own flags field


def run(*args: str) -> None:
    subprocess.run(args, check=True, cwd=HERE)


def assemble_at(src: Path, address: int, out_bin: Path) -> tuple[bytes, dict[str, int]]:
    obj = out_bin.with_suffix(".o")
    elf = out_bin.with_suffix(".elf")
    run("arm-none-eabi-as", "-mcpu=cortex-a9", "-march=armv7-a", str(src), "-o", str(obj))
    run("arm-none-eabi-ld", f"-Ttext={address:#x}", str(obj), "-o", str(elf))
    run("arm-none-eabi-objcopy", "-O", "binary", str(elf), str(out_bin))
    nm_out = subprocess.run(["arm-none-eabi-nm", str(elf)], check=True, cwd=HERE,
                             capture_output=True, text=True).stdout
    symbols: dict[str, int] = {}
    for line in nm_out.splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+\S+\s+(\S+)$", line.strip())
        if m:
            symbols[m.group(2)] = int(m.group(1), 16)
    return out_bin.read_bytes(), symbols


def poke_word(body: bytearray, addr: int, val: int) -> None:
    off = addr - BODY_BASE
    body[off:off + 4] = struct.pack("<I", val)


def poke_bytes(body: bytearray, addr: int, data: bytes) -> None:
    off = addr - BODY_BASE
    body[off:off + len(data)] = data


def check_word(body: bytes, addr: int, expected: int) -> None:
    off = addr - BODY_BASE
    got = struct.unpack_from("<I", body, off)[0]
    assert got == expected, f"at {addr:#x}: expected {expected:#x}, got {got:#x} -- wrong container version?"


def check_zero(body: bytes, addr: int, length: int) -> None:
    off = addr - BODY_BASE
    region = body[off:off + length]
    assert region == bytes(length), f"at {addr:#x}: expected {length} unused bytes, found real data"


def build(container_in: Path, container_out: Path, app_out: Path) -> None:
    scratch = HERE / "build"
    scratch.mkdir(exist_ok=True)

    # 1. Assemble the menu hook (goes into the firmware, appended -- no call-site patch this
    #    time, this example only ever touches data) and the app (goes on the SD card, byte-
    #    identical to sd-card-app's own APP.BIN -- same source file, same link address).
    menu_hook_bin, hook_syms = assemble_at(HERE / "menu_hook.s", HOOK_ADDR, scratch / "menu_hook.bin")
    print(f"menu_hook.s -> {len(menu_hook_bin)} bytes at {HOOK_ADDR:#x}")
    action_addr = hook_syms["homebrew_menu_action"]
    assert action_addr == HOOK_ADDR, f"homebrew_menu_action moved to {action_addr:#x}, expected {HOOK_ADDR:#x}"

    app_bin, _ = assemble_at(SD_CARD_APP / "app_main.s", APP_LOAD_ADDR, scratch / "app_main.bin")
    app_out.write_bytes(app_bin)
    print(f"app_main.s (from sd-card-app) -> {len(app_bin)} bytes, wrote {app_out}")

    # 2. Pull sd_menu_list_v2 (36 bytes) and menu_label's own content out of the assembled
    #    blob, by symbol address (not a hardcoded offset -- see the module docstring).
    list_off = hook_syms["sd_menu_list_v2"] - HOOK_ADDR
    list_bytes = menu_hook_bin[list_off:list_off + 36]
    assert len(list_bytes) == 36, f"sd_menu_list_v2 read short ({len(list_bytes)} bytes)"

    label_start = hook_syms["menu_label"] - HOOK_ADDR
    label_bytes = menu_hook_bin[label_start:label_start + 36]
    label_end = label_bytes.index(b"\x00") + 1  # keep the terminating NUL, drop alignment padding
    label_bytes = label_bytes[:label_end]
    assert len(label_bytes) <= 16, f"menu_label is {len(label_bytes)} bytes, only 16 reserved"

    # 3. Unpack a real release's body.
    unpack_dir = scratch / "unpacked"
    if not (unpack_dir / "body.bin").exists():
        from icom_fw.container import parse
        unpack_dir.mkdir(exist_ok=True)
        (unpack_dir / "body.bin").write_bytes(parse(container_in.read_bytes()).body.decompressed)
    body = bytearray((unpack_dir / "body.bin").read_bytes())

    # 4. Sanity-check the existing bytes before patching.
    check_word(body, REGISTRY_SD_CARD, REGISTRY_ORIG_COUNT)
    check_word(body, REGISTRY_SD_CARD + 4, REGISTRY_ORIG_LIST)
    check_zero(body, NEW_CATALOG_RECORD, 20)
    assert NEW_LABEL_ROM_ADDR + 16 <= GAP_END, "label placement overruns the confirmed-unused gap"
    check_zero(body, NEW_LIST_ROM_ADDR, 36)
    check_zero(body, NEW_LABEL_ROM_ADDR, 16)

    # 5. Apply the patches. Registry -> new list (in ROM, not appended RAM). Catalog record's
    #    action -> appended code; everything else the record needs (query/flags/en/jp) is
    #    either a constant or a ROM pointer too.
    poke_word(body, REGISTRY_SD_CARD, REGISTRY_ORIG_COUNT + 1)
    poke_word(body, REGISTRY_SD_CARD + 4, NEW_LIST_ROM_ADDR)
    poke_bytes(body, NEW_LIST_ROM_ADDR, list_bytes)
    poke_bytes(body, NEW_LABEL_ROM_ADDR, label_bytes)

    poke_word(body, NEW_CATALOG_RECORD + 0x00, action_addr)       # action = homebrew_menu_action
    poke_word(body, NEW_CATALOG_RECORD + 0x04, 0)                 # query = NULL, always selectable
    poke_word(body, NEW_CATALOG_RECORD + 0x08, FORMAT_FLAGS)
    poke_word(body, NEW_CATALOG_RECORD + 0x0c, NEW_LABEL_ROM_ADDR)  # en
    poke_word(body, NEW_CATALOG_RECORD + 0x10, NEW_LABEL_ROM_ADDR)  # jp (same, like several real rows)

    # 6. Append the menu hook (the action code + its own file-I/O working data; the list/label
    #    bytes are also appended here as inert leftovers of the assembled blob -- harmless, not
    #    pointed at by anything, kept only because splitting them out needs a custom linker
    #    script for negligible benefit).
    pad_len = HOOK_ADDR - BODY_BASE - len(body)
    assert pad_len > 0, f"HOOK_ADDR is inside the existing image (need pad_len > 0, got {pad_len})"
    new_body = bytes(body) + bytes(pad_len) + menu_hook_bin

    # 7. Repack into a valid, checksum-correct container.
    new_container = pack(container_in.read_bytes(), new_body)
    container_out.write_bytes(new_container)
    print(f"wrote {container_out} ({len(new_container)} bytes, "
          f"body {len(body)} -> {len(new_body)} decompressed)")
    print(f"list/label now live in ROM at {NEW_LIST_ROM_ADDR:#x}/{NEW_LABEL_ROM_ADDR:#x} "
          f"(only the action pointer, {action_addr:#x}, still points into appended RAM)")


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
