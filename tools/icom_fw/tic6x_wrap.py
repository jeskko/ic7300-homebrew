"""Wrap a raw TMS320C674x binary dump into a proper TI-tagged ELF object.

Two independent open-source disassemblers (GNU binutils' ``tic6x-objdump -EL`` and Capstone's
``CS_ARCH_TMS320C64X``) will happily disassemble a raw flash dump directly with no wrapping needed.
TI's own official ``dis6x`` (from the C6000 Code Generation Tools) will *not*: it requires a real ELF
object with section headers, and even then it silently refuses to decode C674x-specific floating-point
opcodes (prints "Unknown C6x Si version" per fetch packet) unless the object carries a proper
``.c6xabi.attributes`` build-attributes section declaring ``Tag_ISA = C674X``. It ignores the
``--silicon_version``/``-mv`` command-line flag entirely for this purpose.

GNU ``objcopy`` (built from stock binutils with ``--target=tic6x-elf``, see notes/multi-cpu-images.md)
gets a raw binary into an ELF shell, but has no flag to emit the attributes section or set a section's
``sh_type``. This module builds that section by hand from the format documented in binutils'
``bfd/elf-attrs.c`` / ``include/elf/tic6x{,-attrs}.h``, and patches the ELF section header's ``sh_type``
field directly (offset arithmetic, no external tool needed for that last step).

Usage (see notes/multi-cpu-images.md's "Third independent confirmation" section for the full story;
requires a tic6x-target `objcopy` on PATH or passed explicitly):

    python3 -m icom_fw.tic6x_wrap dsp_program.bin dsp_program.elf --objcopy /path/to/tic6x-objcopy
    dis6x dsp_program.elf

Only Tag_ISA is set; add more (tag, value) pairs to ATTR_PAIRS below if a future need arises (e.g.
Tag_ABI_wchar_t) -- see include/elf/tic6x-attrs.h in the binutils source for the full tag list.
"""

import argparse
import shutil
import struct
import subprocess
import sys

SHT_C6000_ATTRIBUTES = 0x70000003
TAG_ISA = 4
ISA_C674X = 8

# (tag, value) pairs, both required to fit in one ULEB128 byte (i.e. < 0x80) by this simple encoder.
ATTR_PAIRS = [(TAG_ISA, ISA_C674X)]


def build_attributes_section(pairs=ATTR_PAIRS) -> bytes:
    """Build a minimal '.c6xabi.attributes' section body (the 'A'-format build-attributes blob)."""
    vendor_name = b"c6xabi\x00"
    payload = bytes(b for tag, val in pairs for b in (tag, val))  # 1-byte ULEB128 per field only
    tag_file_len = 1 + 4 + len(payload)
    tag_file = struct.pack("<B", 1) + struct.pack("<I", tag_file_len) + payload  # tag=1 is Tag_File
    vendor_size = 4 + len(vendor_name) + len(tag_file)
    subsection = struct.pack("<I", vendor_size) + vendor_name + tag_file
    return b"A" + subsection


def patch_section_type(elf_path: str, section_name: str, new_type: int) -> None:
    """Rewrite one section's sh_type field in place (objcopy has no --set-section-type)."""
    with open(elf_path, "rb") as f:
        data = bytearray(f.read())

    e_shoff = struct.unpack_from("<I", data, 0x20)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x2E)[0]
    e_shnum = struct.unpack_from("<H", data, 0x30)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x32)[0]

    shstrtab_off = struct.unpack_from("<I", data, e_shoff + e_shstrndx * e_shentsize + 0x10)[0]

    for i in range(e_shnum):
        entry_off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from("<I", data, entry_off)[0]
        end = data.index(b"\x00", shstrtab_off + name_off)
        name = data[shstrtab_off + name_off : end].decode()
        if name == section_name:
            struct.pack_into("<I", data, entry_off + 4, new_type)
            with open(elf_path, "wb") as f:
                f.write(data)
            return

    raise ValueError(f"section {section_name!r} not found in {elf_path}")


def wrap(raw_path: str, elf_path: str, objcopy: str = "objcopy") -> None:
    """Wrap raw_path's bytes as a tic6x ELF .text section at address 0, tagged Tag_ISA=C674X."""
    subprocess.run(
        [
            objcopy,
            "-I",
            "binary",
            "-O",
            "elf32-tic6x-le",
            "-B",
            "tic6x",
            "--set-section-flags",
            ".data=alloc,contents,code",
            "--rename-section",
            ".data=.text",
            raw_path,
            elf_path,
        ],
        check=True,
    )

    attrs_bytes = build_attributes_section()
    with open(elf_path + ".attrs.tmp", "wb") as f:
        f.write(attrs_bytes)
    subprocess.run(
        [objcopy, "--add-section", f".c6xabi.attributes={elf_path}.attrs.tmp", elf_path, elf_path],
        check=True,
    )

    patch_section_type(elf_path, ".c6xabi.attributes", SHT_C6000_ATTRIBUTES)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw_bin", help="raw TMS320C674x binary dump")
    parser.add_argument("out_elf", help="output ELF path, suitable for TI's dis6x or readelf -A")
    parser.add_argument(
        "--objcopy", default=shutil.which("objcopy") or "objcopy", help="tic6x-target objcopy to use"
    )
    args = parser.parse_args()
    wrap(args.raw_bin, args.out_elf, args.objcopy)
    print(f"wrote {args.out_elf} (Tag_ISA=C674X) -- verify with: readelf -A {args.out_elf}")


if __name__ == "__main__":
    main()
