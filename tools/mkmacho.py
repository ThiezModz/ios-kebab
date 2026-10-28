#!/usr/bin/env python3
"""Build the smallest Mach-O tools/mkpayload.py accepts, from the demo kernel.

  mkmacho.py DEMO.ELF OUT

The input is the qemu/demo ELF, linked by demo.ld at 0x40000188+ with
FILEHDR-in-LOAD (the shim's trick). mkpayload.py needs a 64-bit little-endian
Mach-O with a __TEXT LC_SEGMENT_64 at file offset 0 and an LC_UNIXTHREAD
entry point; the demo has no fixups, so one segment and two load commands
are enough. A real XNU kernel (LC_DYLD_CHAINED_FIXUPS and all) goes through
mkpayload.py directly instead.

Placement invariant: the shim copies __TEXT to physBase + (vmaddr - virtBase)
and jumps to entry + delta. delta is page-aligned, so PC-relative references
(adrp etc.) stay valid only if every link-time VA differs from its runtime VA
by a page multiple. The code is dropped at Mach-O file offset 0x188 (right
after the Mach-O header), so with vmaddr = e_entry - 0x188 the code's link
VA (e_entry + n) equals its Mach-O VA exactly, and it runs at
physBase + (vmaddr - virtBase) + 0x188 + n: base congruence holds per page.
"""
import argparse, struct, sys

MH_MAGIC_64 = 0xfeedfacf
CPU_TYPE_ARM64 = 0x0100000c
MH_EXECUTE = 2
LC_SEGMENT_64, LC_UNIXTHREAD = 0x19, 0x5
ARM_THREAD_STATE64 = 6
PAGE = 0x1000
HDR = 0x188			# mach_header_64 + LC_SEGMENT_64 + LC_UNIXTHREAD


def elf_loads(f):
    """Return [(vaddr, bytes)] for every PT_LOAD, in ascending vaddr order."""
    assert f[:4] == b'\x7fELF' and f[4] == 2 and f[5] == 1, 'not a 64-bit LE ELF'
    e_entry, = struct.unpack_from('<Q', f, 24)
    e_phoff, = struct.unpack_from('<Q', f, 32)
    e_phentsize, e_phnum = struct.unpack_from('<HH', f, 54)
    loads = []
    for i in range(e_phnum):
        ph = struct.unpack_from('<IIQQQQQQ', f, e_phoff + i * e_phentsize)
        p_type, p_offset, p_vaddr, p_filesz = ph[0], ph[2], ph[3], ph[5]
        if p_type == 1:		# PT_LOAD
            loads.append((p_vaddr, f[p_offset:p_offset + p_filesz]))
    loads.sort()
    return e_entry, loads


def build(code, vmaddr, entry):
    vmsize = (HDR + len(code) + PAGE - 1) & ~(PAGE - 1)
    out = bytearray(vmsize)
    # mach_header_64
    struct.pack_into('<IIIIIII', out, 0, MH_MAGIC_64, CPU_TYPE_ARM64, 0, MH_EXECUTE,
                     2, 72 + 288, 0x00200085)	# MH_PIE|MH_NOUNDEFS|MH_DYLDLINK|MH_TWOLEVEL
    # LC_SEGMENT_64: __TEXT at file offset 0, matching mkpayload's assert
    struct.pack_into('<II', out, 32, LC_SEGMENT_64, 72)
    out[40:56] = b'__TEXT\0'.ljust(16, b'\0')
    struct.pack_into('<4QIIII', out, 56, vmaddr, vmsize, 0, vmsize,
                     7, 5, 0, 0)	# r-x, no sections
    # LC_UNIXTHREAD: pc = entry, everything else zero (mkpayload reads pc)
    o = 32 + 72
    struct.pack_into('<IIII', out, o, LC_UNIXTHREAD, 288, ARM_THREAD_STATE64, 68)
    out[o + 16 + 32 * 8:o + 16 + 32 * 8 + 8] = struct.pack('<Q', entry)
    out[HDR:HDR + len(code)] = code
    return bytes(out)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('out')
    a = ap.parse_args()
    f = open(a.input, 'rb').read()
    e_entry, loads = elf_loads(f)
    assert len(loads) == 1, f'expected one PT_LOAD, got {len(loads)}'
    base, data = loads[0]
    # The ELF headers live inside the LOAD right before the code: drop them.
    skip = e_entry - base
    assert 0 < skip <= len(data), f'bogus entry offset {skip}'
    code = data[skip:]
    vmaddr = e_entry - HDR	# code's Mach-O VA = e_entry, entry = e_entry
    open(a.out, 'wb').write(build(code, vmaddr, e_entry))
    print(f'{a.out}: code {len(code)} bytes, vmaddr {vmaddr:#x}, entry {e_entry:#x}',
          file=sys.stderr)
