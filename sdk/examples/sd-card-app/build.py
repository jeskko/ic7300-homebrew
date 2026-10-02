#!/usr/bin/env python3
"""Build the sd-card-app example: a modified firmware container (the loader hook, flashed
once) plus a standalone APP.BIN (dropped on the SD card, no reflash needed to iterate on it).

Live-tested procedure (2026-09-25) -- see README.md in this directory for the full test log.
Requires the arm-none-eabi toolchain (as/ld/objcopy) on PATH.

Usage:
    python3 sdk/examples/sd-card-app/build.py [container.dat] [output.dat] [app_output.bin]

Defaults to $ICOM_FW_DIR/7300_142.dat (default firmware/7300_142.dat) -> scratch/sd_card_app_142.dat + scratch/APP.BIN
(repo-relative). To test in qemu-machine:
    python3 qemu-machine/tools/build_flash.py scratch/sd_card_app_142.dat qemu-machine/flash.bin
    python3 qemu-machine/tools/build_sdcard.py -o scratch/sdcard.img --size-mb 64
    mmd -i scratch/sdcard.img@@1M "::IC-7300"
    mcopy -i scratch/sdcard.img@@1M scratch/APP.BIN "::IC-7300/APP.BIN"
    python3 qemu-machine/tools/run_gui.py --no-pwrk --icount off --civ /tmp/civ.sock \
        --sd scratch/sdcard.img --display none
Then trigger and read the frame exactly as in sdk/examples/civ-hello-world/README.md.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
FW_DIR = Path(os.environ.get("ICOM_FW_DIR", REPO / "firmware"))
sys.path.insert(0, str(REPO / "tools"))
from icom_fw.container import pack  # noqa: E402

BODY_BASE = 0x20005000
PATCH_SITE = 0x20052f64            # main_idle_loop's own `bl civ_tx_pump` call site
ORIGINAL_PATCH_BYTES = bytes.fromhex("06f9feeb")  # `bl 0x20011384`, verified before patching
HOOK_ADDR = 0x20600000              # same confirmed-safe address as civ-hello-world; see
                                     # sdk/app-loader-design.md's "Where appended code actually
                                     # has to live" for why not right after the static image.
APP_LOAD_ADDR = 0x20610000          # must match loader_hook.s's own APP_LOAD_ADDR .equ


def run(*args: str) -> None:
    subprocess.run(args, check=True, cwd=HERE)


def assemble_at(src: Path, address: int, out_bin: Path) -> bytes:
    obj = out_bin.with_suffix(".o")
    elf = out_bin.with_suffix(".elf")
    run("arm-none-eabi-as", "-mcpu=cortex-a9", "-march=armv7-a", str(src), "-o", str(obj))
    run("arm-none-eabi-ld", f"-Ttext={address:#x}", str(obj), "-o", str(elf))
    run("arm-none-eabi-objcopy", "-O", "binary", str(elf), str(out_bin))
    return out_bin.read_bytes()


def build(container_in: Path, container_out: Path, app_out: Path) -> None:
    scratch = HERE / "build"
    scratch.mkdir(exist_ok=True)

    # 1. Assemble the loader hook (goes into the firmware) and the app (goes on the SD card)
    #    at their respective fixed addresses.
    loader_bin = assemble_at(HERE / "loader_hook.s", HOOK_ADDR, scratch / "loader_hook.bin")
    print(f"loader_hook.s -> {len(loader_bin)} bytes at {HOOK_ADDR:#x}")
    app_bin = assemble_at(HERE / "app_main.s", APP_LOAD_ADDR, scratch / "app_main.bin")
    app_out.write_bytes(app_bin)
    print(f"app_main.s -> {len(app_bin)} bytes at {APP_LOAD_ADDR:#x}, wrote {app_out}")

    # 2. Assemble the one-instruction call-site patch (`bl HOOK_ADDR`).
    patch_src = scratch / "patch.s"
    patch_src.write_text(
        ".syntax unified\n.arm\n.section .text\n"
        f".equ HOOK_ADDR, {HOOK_ADDR:#x}\nbl HOOK_ADDR\n"
    )
    patch_bytes = assemble_at(patch_src, PATCH_SITE, scratch / "patch.bin")
    assert len(patch_bytes) == 4, patch_bytes

    # 3. Unpack a real release's body, apply the patch, and splice the loader in at HOOK_ADDR.
    unpack_dir = scratch / "unpacked"
    if not (unpack_dir / "body.bin").exists():
        from icom_fw.container import parse
        unpack_dir.mkdir(exist_ok=True)
        (unpack_dir / "body.bin").write_bytes(parse(container_in.read_bytes()).body.decompressed)
    body = bytearray((unpack_dir / "body.bin").read_bytes())

    offset = PATCH_SITE - BODY_BASE
    assert bytes(body[offset:offset + 4]) == ORIGINAL_PATCH_BYTES, (
        "call site doesn't match the expected original bytes -- wrong container version?"
    )
    body[offset:offset + 4] = patch_bytes

    pad_len = (HOOK_ADDR - BODY_BASE) - len(body)
    assert pad_len > 0, f"HOOK_ADDR is inside the existing image (need pad_len > 0, got {pad_len})"
    new_body = bytes(body) + bytes(pad_len) + loader_bin

    # 4. Repack into a valid, checksum-correct container. (APP.BIN is NOT part of this --
    #    it's a separate file for the SD card, the whole point of this example.)
    new_container = pack(container_in.read_bytes(), new_body)
    container_out.write_bytes(new_container)
    print(f"wrote {container_out} ({len(new_container)} bytes, "
          f"body {len(body)} -> {len(new_body)} decompressed)")


if __name__ == "__main__":
    default_in = FW_DIR / "7300_142.dat"
    default_out = REPO / "scratch" / "sd_card_app_142.dat"
    default_app = REPO / "scratch" / "APP.BIN"
    container_in = Path(sys.argv[1]) if len(sys.argv) > 1 else default_in
    container_out = Path(sys.argv[2]) if len(sys.argv) > 2 else default_out
    app_out = Path(sys.argv[3]) if len(sys.argv) > 3 else default_app
    container_out.parent.mkdir(parents=True, exist_ok=True)
    app_out.parent.mkdir(parents=True, exist_ok=True)
    build(container_in, container_out, app_out)
