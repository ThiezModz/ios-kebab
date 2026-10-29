#!/usr/bin/env python3
"""Resolve kernel VAs to symbol names using LC_SYMTAB (no llvm-nm needed).

  xnu-sym.py KERNEL 0xADDR [0xADDR...]     - nearest symbol + offset per addr
"""
import os, struct, sys

LC_SYMTAB = 0x2

def syms(kernel):
    data = open(kernel, 'rb').read()
    magic, = struct.unpack_from('<I', data, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    ncmds, = struct.unpack_from('<I', data, 16)
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, off + 8)
            strtab = data[stroff:stroff + strsize]
            out = []
            for i in range(nsyms):
                strx, n_type, n_desc, n_value = struct.unpack_from('<IBxHQ', data, symoff + 16 * i)
                if strx < strsize and n_type & 0x0e:  # N_SECT symbols only
                    end = strtab.find(b'\0', strx)
                    if end >= 0 and end > strx:
                        out.append((n_value, strtab[strx:end].decode('latin1')))
            return sorted(out)
        off += size
    raise SystemExit('no LC_SYMTAB')

if __name__ == '__main__':
    table = syms(sys.argv[1])
    vals = [x[0] for x in table]
    import bisect
    for a in sys.argv[2:]:
        addr = int(a, 0)
        i = bisect.bisect_right(vals, addr) - 1
        if i >= 0:
            print(f'{a}: {table[i][1]} + 0x{addr - table[i][0]:x}')
        else:
            print(f'{a}: (no symbol)')
