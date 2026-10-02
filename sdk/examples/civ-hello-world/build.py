#!/usr/bin/env python3
"""Build a modified IC-7300 firmware container with the civ-hello-world hook installed.

Live-tested procedure (2026-09-25) -- see README.md in this directory for the full test log.
Requires the arm-none-eabi toolchain (as/ld/objcopy) on PATH.

Usage:
    python3 sdk/examples/civ-hello-world/build.py [container.dat] [output.dat]

Defaults to $ICOM_FW_DIR/7300_142.dat (default firmware/7300_142.dat) -> scratch/civ_hello_world_142.dat (repo-relative).
To boot it in qemu-machine:
    python3 qemu-machine/tools/build_flash.py scratch/civ_hello_world_142.dat qemu-machine/flash.bin
    python3 qemu-machine/tools/run_gui.py --no-pwrk --icount off --civ /tmp/civ.sock --display none
Then trigger with (needs the fp control socket run_gui.py prints):
    RZA1H_FPCTL=<sock> python3 qemu-machine/tools/fp.py combo XFC "SPEECH/LOCK" --hold 0.5
and read the HOMEBREW frame off /tmp/civ.sock (tools/civ.py, or a raw socket read --
civ.py's own frame filter only keeps frames addressed to its own controller address).
"""

from __future__ import annotations

import os
import struct
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
HOOK_ADDR = 0x20600000              # see app.s's own header comment for why not right after
                                     # the static image ends -- that region gets progressively
                                     # overwritten by a runtime allocator during boot, confirmed
                                     # live; 0x20600000 was empirically confirmed to survive.


def run(*args: str) -> None:
    subprocess.run(args, check=True, cwd=HERE)


def assemble_at(src: Path, address: int, out_bin: Path) -> bytes:
    obj = out_bin.with_suffix(".o")
    elf = out_bin.with_suffix(".elf")
    run("arm-none-eabi-as", "-mcpu=cortex-a9", "-march=armv7-a", str(src), "-o", str(obj))
    run("arm-none-eabi-ld", f"-Ttext={address:#x}", str(obj), "-o", str(elf))
    run("arm-none-eabi-objcopy", "-O", "binary", str(elf), str(out_bin))
    return out_bin.read_bytes()


def build(container_in: Path, container_out: Path) -> None:
    scratch = HERE / "build"
    scratch.mkdir(exist_ok=True)

    # 1. Assemble the hook itself at its fixed final address.
    app_bin = assemble_at(HERE / "app.s", HOOK_ADDR, scratch / "app.bin")
    print(f"app.s -> {len(app_bin)} bytes at {HOOK_ADDR:#x}")

    # 2. Assemble the one-instruction call-site patch (`bl HOOK_ADDR`), placed at the exact
    #    address it will overwrite so the branch encoding comes out right.
    patch_src = scratch / "patch.s"
    patch_src.write_text(
        ".syntax unified\n.arm\n.section .text\n"
        f".equ HOOK_ADDR, {HOOK_ADDR:#x}\nbl HOOK_ADDR\n"
    )
    patch_bytes = assemble_at(patch_src, PATCH_SITE, scratch / "patch.bin")
    assert len(patch_bytes) == 4, patch_bytes

    # 3. Unpack a real release's body, apply the one-instruction patch, and splice the hook
    #    in at HOOK_ADDR (padding with zeros in between -- LZSS compresses long zero runs
    #    cheaply, see the compressed-size check below).
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
    new_body = bytes(body) + bytes(pad_len) + app_bin

    # 4. Repack into a valid, checksum-correct container.
    new_container = pack(container_in.read_bytes(), new_body)
    container_out.write_bytes(new_container)
    print(f"wrote {container_out} ({len(new_container)} bytes, "
          f"body {len(body)} -> {len(new_body)} decompressed)")


if __name__ == "__main__":
    default_in = FW_DIR / "7300_142.dat"
    default_out = REPO / "scratch" / "civ_hello_world_142.dat"
    container_in = Path(sys.argv[1]) if len(sys.argv) > 1 else default_in
    container_out = Path(sys.argv[2]) if len(sys.argv) > 2 else default_out
    container_out.parent.mkdir(parents=True, exist_ok=True)
    build(container_in, container_out)
