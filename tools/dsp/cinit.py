#!/usr/bin/env python3
"""Walk the TI C6000 (COFF-ABI style) .cinit table: records {u32 len, u32 dest, data[len]} padded to 8.
usage: cinit.py <ais.bin> <cinit_load_addr>   (the DSP Program image: .cinit is at 0x1182f2f8)
Importable: records(path, addr) -> [(dest, bytes, src_addr)]"""
import struct, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from ais2elf import parse

def memimage(path):
    secs, *_ = parse(open(path, 'rb').read())
    return {a: data for a, data, _ in secs}

def read(mem, addr, n):
    for a, data in mem.items():
        if a <= addr < a + len(data): return data[addr - a:addr - a + n]
    raise KeyError(hex(addr))

def records(path, addr):
    mem = memimage(path); out = []
    while True:
        hdr = read(mem, addr, 8)
        if len(hdr) < 8: break               # table runs to the end of its section
        n, dest = struct.unpack('<II', hdr)
        if n == 0: break
        out.append((dest, read(mem, addr + 8, n), addr + 8))
        addr += 8 + ((n + 7) & ~7)
    return out

if __name__ == '__main__':
    for dest, data, src in records(sys.argv[1], int(sys.argv[2], 16)):
        print(f'dest={dest:#010x} len={len(data):#06x} src={src:#010x}')
