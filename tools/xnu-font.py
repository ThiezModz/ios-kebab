#!/usr/bin/env python3
"""Extract the kernel's iso_font (256*16 bytes) from a Mach-O kernel.

llvm-nm replacement for the QEMU framebuffer decode; walks LC_SYMTAB.

  xnu-font.py KERNEL OUT.bin   - write the font bytes
  xnu-font.py KERNEL           - print the _iso_font VA
"""
import struct, sys

LC_SYMTAB = 0x2

def symtab(kernel):
    data = open(kernel, 'rb').read()
    magic, = struct.unpack_from('<I', data, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    ncmds, = struct.unpack_from('<I', data, 16)
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, off + 8)
            return data, symoff, nsyms, stroff, strsize
        off += size
    raise SystemExit('no LC_SYMTAB')

def find(data, symoff, nsyms, stroff, strsize, name):
    strtab = data[stroff:stroff + strsize]
    for i in range(nsyms):
        n = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
        strx, value = n[0], n[4]
        if strx < strsize:
            end = strtab.find(b'\0', strx)
            if end >= 0 and strtab[strx:end] == name:
                return value
    raise SystemExit(f'symbol {name} not found')

if __name__ == '__main__':
    data, symoff, nsyms, stroff, strsize = symtab(sys.argv[1])
    va = find(data, symoff, nsyms, stroff, strsize, b'_iso_font')
    if len(sys.argv) > 2:
        # find the segment containing the VA to get the file offset
        off = 32
        ncmds, = struct.unpack_from('<I', data, 16)
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, off)
            if cmd == 0x19:  # LC_SEGMENT_64
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, off + 24)
                if vmaddr <= va < vmaddr + vmsize:
                    open(sys.argv[2], 'wb').write(data[fileoff + va - vmaddr:fileoff + va - vmaddr + 256 * 16])
                    print(f'{sys.argv[2]}: 4096 bytes at VA {va:#x}')
                    break
            off += size
    else:
        print(f'_iso_font VA: {va:#x}')
