#!/usr/bin/env python3
"""Parse a TI C674x AIS boot script and emit an ELF32 (EM_TI_C6000, LE) with one
section per Section Load command at its real load address, plus a hand-built
.c6xabi.attributes (Tag_ISA=C674x) so TI's dis6x decodes FP/compact opcodes.

usage: ais2elf.py <ais.bin> <out.elf>      (prints the command list)
"""
import struct, sys

OP_SECTION_LOAD, OP_FUNC_EXEC, OP_JUMP_CLOSE = 0x58535901, 0x5853590D, 0x58535906

def parse(d):
    w = lambda o: struct.unpack_from('<I', d, o)[0]
    assert w(0) == 0x41504954, 'no AIS magic'
    o, sections, entry, log = 4, [], None, []
    while o < len(d):
        c = w(o)
        if c == OP_SECTION_LOAD:
            a, n = w(o + 4), w(o + 8)
            sections.append((a, d[o + 12:o + 12 + n], o + 12))
            log.append(f'{o:#07x} SectionLoad  addr={a:#010x} size={n:#07x} file={o+12:#07x}')
            o += 12 + ((n + 3) & ~3)
        elif c == OP_FUNC_EXEC:
            a = w(o + 4); n = a >> 16
            args = [w(o + 8 + 4 * k) for k in range(n)]
            log.append(f'{o:#07x} FunctionExec idx={a & 0xffff} args={[hex(x) for x in args]}')
            o += 8 + 4 * n
        elif c == OP_JUMP_CLOSE:
            entry = w(o + 4)
            log.append(f'{o:#07x} Jump&Close   entry={entry:#010x}')
            o += 8
            break
        elif (c >> 8) == 0x585359:
            # single-word opcode not in the list above (0x58535963 seen at +4); log and skip
            log.append(f'{o:#07x} op {c:#010x} (no args assumed)')
            o += 4
        else:
            raise SystemExit(f'unknown AIS word {c:#x} at {o:#x}')
    return sections, entry, log, o

def attrs():
    payload = bytes([4, 8])                      # Tag_ISA = C674x
    sub = b'c6xabi\0' + bytes([1]) + struct.pack('<I', 1 + 4 + len(payload)) + payload
    return b'A' + struct.pack('<I', 4 + len(sub)) + sub

def write_elf(path, sections, entry):
    shstr = b'\0'; names = []
    def addname(s):
        nonlocal shstr
        names.append(len(shstr)); shstr += s.encode() + b'\0'
    blobs = []
    for i, (a, data, _) in enumerate(sections):
        addname(f'.sec{i}_{a:08x}'); blobs.append(data)
    addname('.c6xabi.attributes'); blobs.append(attrs())
    addname('.shstrtab'); blobs.append(shstr)
    off = 52; offs = []
    body = b''
    for b in blobs:
        pad = (-off) % 4; body += b'\0' * pad; off += pad
        offs.append(off); body += b; off += len(b)
    pad = (-off) % 4; body += b'\0' * pad; off += pad
    shoff = off
    sh = b'\0' * 40
    for i, b in enumerate(blobs):
        if i < len(sections):
            sh += struct.pack('<10I', names[i], 1, 6, sections[i][0], offs[i], len(b), 0, 0, 32, 0)
        elif i == len(sections):
            sh += struct.pack('<10I', names[i], 0x70000003, 0, 0, offs[i], len(b), 0, 0, 1, 0)
        else:
            sh += struct.pack('<10I', names[i], 3, 0, 0, offs[i], len(b), 0, 0, 1, 0)
    hdr = b'\x7fELF\x01\x01\x01' + b'\0' * 9 + struct.pack('<HHIIIIIHHHHHH',
          1, 140, 1, entry or 0, 0, shoff, 0, 52, 0, 0, 40, len(blobs) + 1, len(blobs))
    open(path, 'wb').write(hdr + body + sh)

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    secs, entry, log, end = parse(d)
    print('\n'.join(log)); print(f'script ends at {end:#x} of {len(d):#x}')
    write_elf(sys.argv[2], secs, entry)
