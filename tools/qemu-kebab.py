#!/usr/bin/env python3
"""Boot ios-kebab payloads in QEMU's virt board, two ways:

  1. Demo mode (the default, built by scripts/mk-qemu-image.sh):
       qemu-kebab.py --shim shim/out/Image --dtb demo.dtb --payload payload.bin OUT
     runs the real phone boot path: QEMU starts the shim as a raw arm64 Image
     (x0 = DTB), the shim reads /chosen/linux,initrd-* and jumps into the
     SWEETXNU payload, exactly as ABL + loader.c do on the device.

  2. Kernel mode (a real XNU kernel, as tools/qemu-sweet.py did):
       qemu-kebab.py --payload-only payload.bin OUT [--kernel KERNEL]
     places the payload's segments with a loader device and starts the CPU at
     a 6-instruction stub that jumps to the entry with x0 = boot_args.

After N seconds the guest is stopped and OUT gets:
  serial.txt  everything the guest wrote to the PL011 (the demo logs here)
  fb.png      the 1080x2400 framebuffer at 0x9c000000
  fb.txt      decoded text, using the shim font for the demo (2x glyphs) or
              the kernel's _iso_font with --kernel
  result.json status summary
"""
import argparse, json, os, re, socket, struct, subprocess, sys, time, zlib

FB_BASE, FB_W, FB_H = 0x9c000000, 1080, 2400
FB_STRIDE = FB_W * 4
PAGE = 0x4000
BOOT_LINE_LENGTH = 608
PAYLOAD_ADDR = 0x50000000	# virt RAM: below the ADT's phys_base (0xb0000000)
STUB_PA = 0x40200000		# above the dtb QEMU places at RAM base


def check_qemu():
    qemu = subprocess.run(['qemu-system-aarch64', '--version'], capture_output=True, text=True)
    if qemu.returncode:
        print('[!] qemu-system-aarch64 not found. Install: sudo pacman -S qemu-system-aarch64 '
              '(Arch) / sudo apt install qemu-system-arm (Debian/Ubuntu)', file=sys.stderr)
        return False
    print(f'[*] {qemu.stdout.splitlines()[0]}', file=sys.stderr)
    return True


def shim_font():
    """font8x16 from shim/font.h -> {glyph bytes: char}, same table the demo draws."""
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../shim/font.h')).read()
    vals = [int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]{2}', src)]
    return {tuple(vals[g * 16:(g + 1) * 16]): chr(32 + g) for g in range(95)}


def write_png(path, raw):
    rows = []
    for y in range(FB_H):
        line = raw[y * FB_STRIDE:(y + 1) * FB_STRIDE]
        rgb = bytearray(FB_W * 3)	# a8r8g8b8 little endian = B G R A in memory
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(b'\0' + bytes(rgb))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', FB_W, FB_H, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def decode_demo(raw, glyphs):
    """The demo draws font8x16 at 2x scale, 16x32 px cells from the origin."""
    bg = raw[:4]	# the demo fills the whole screen first
    rows, unknown = [], 0
    for row in range(FB_H // 32):
        chars = []
        for col in range(FB_W // 16):
            g = [0] * 16
            for y in range(16):
                for x in range(8):
                    off = (row * 32 + y * 2) * FB_STRIDE + (col * 16 + x * 2) * 4
                    if raw[off:off + 4] != bg:
                        g[y] |= 1 << (7 - x)
            key = tuple(g)
            if any(g):
                chars.append(glyphs.get(key, '?'))
                unknown += key not in glyphs
            else:
                chars.append(' ')
        rows.append(''.join(chars).rstrip())
    return '\n'.join(rows).rstrip() + '\n', unknown


def decode_xnu(raw, font):
    """Exact glyph matching for pe_serial.c's 2x white-on-black fbcon (as qemu-sweet.py)."""
    glyphs = {font[c * 16:(c + 1) * 16]: chr(c) for c in range(32, 127)}
    rows, unknown = [], 0
    for row in range(FB_H // 32):
        chars = []
        for col in range(FB_W // 16):
            glyph = bytearray(16)
            for y in range(16):
                off = (row * 32 + y * 2) * FB_STRIDE + col * 16 * 4
                for x in range(8):
                    if raw[off + x * 8:off + x * 8 + 4] == b'\xff' * 4:
                        glyph[y] |= 1 << x
            key = bytes(glyph)
            char = glyphs.get(key, '?')
            unknown += key not in glyphs
            chars.append(char)
        rows.append(''.join(chars).rstrip())
    return '\n'.join(rows).rstrip() + '\n', unknown


def payload_font(payload, kernel):
    """The kernel's _iso_font bytes, by VA, from the unstripped Mach-O."""
    syms = subprocess.check_output(['llvm-nm', '--defined-only', kernel], text=True)
    fonts = [int(line.split()[0], 16) for line in syms.splitlines()
             if line.split()[-1:] == ['_iso_font']]
    if len(fonts) != 1:
        raise SystemExit('kernel must contain exactly one _iso_font symbol (llvm-nm needed)')
    nsegs = struct.unpack_from('<I', payload, 12)[0]
    for i in range(nsegs):
        vmaddr, _, foff, fsize = struct.unpack_from('<4Q', payload, 0x2a8 + 32 * i)
        if vmaddr <= fonts[0] and fonts[0] + 256 * 16 <= vmaddr + fsize:
            return payload[foff + fonts[0] - vmaddr:foff + fonts[0] - vmaddr + 256 * 16]
    raise SystemExit('_iso_font VA not in initialized payload segments')


def payload_info(payload):
    magic, ver, nsegs = struct.unpack_from('<8sII', payload, 0)
    assert magic == b'SWEETXNU' and ver == 1, 'not a SWEETXNU v1 payload'
    virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size = \
        struct.unpack_from('<7Q', payload, 0x10)
    return virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size


def build_ram_image(payload):
    """(phys_base, img, boot_args_pa, entry_pa): segment layout, as the phone shim does."""
    virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size = payload_info(payload)
    cmdline = payload[0x48:0x48 + BOOT_LINE_LENGTH]
    delta = phys_base - virt_base
    adt_pa = kernel_end + delta
    ba_pa = (adt_pa + adt_size + PAGE - 1) & ~(PAGE - 1)
    top = ba_pa + PAGE
    img = bytearray(top - phys_base)
    nsegs = struct.unpack_from('<I', payload, 12)[0]
    for i in range(nsegs):
        vmaddr, vmsize, foff, fsize = struct.unpack_from('<4Q', payload, 0x2a8 + 32 * i)
        o = vmaddr + delta - phys_base
        img[o:o + fsize] = payload[foff:foff + fsize]
    img[adt_pa - phys_base:adt_pa - phys_base + adt_size] = payload[adt_off:adt_off + adt_size]
    ba = struct.pack('<HH4x4Q6QI4xQI', 2, 2, virt_base, phys_base, mem_size, top,
                     FB_BASE, 1, FB_STRIDE, FB_W, FB_H, 32, 0, adt_pa - delta, adt_size)
    ba += cmdline + b'\0' * 4 + struct.pack('<QQ', 0, mem_size)
    assert len(ba) == 736
    img[ba_pa - phys_base:ba_pa - phys_base + len(ba)] = ba
    return phys_base, bytes(img), ba_pa, entry_va + delta


def stub(ba_pa, entry_pa):
    # ldr x0, =ba ; ldr x1, =entry ; br x1 ; nop ; .quad ba ; .quad entry
    return struct.pack('<4I2Q', 0x58000080, 0x580000a1, 0xd61f0020, 0xd503201f, ba_pa, entry_pa)


def qmp(sock_path, *cmds, pmemsave=None):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    f = s.makefile('rw')
    f.readline()

    def call(execute, **args):
        f.write(json.dumps({'execute': execute, 'arguments': args}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                return r

    call('qmp_capabilities')
    for c in cmds:
        call('human-monitor-command', **{'command-line': c})
    if pmemsave:
        call('pmemsave', val=pmemsave[0], size=pmemsave[1], filename=pmemsave[2])
    s.close()


def main():
    ap = argparse.ArgumentParser(description='Boot ios-kebab payloads in QEMU (demo or XNU).')
    ap.add_argument('outdir', help='output directory (serial.txt, fb.png, fb.txt, result.json)')
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--payload-only', help='a SWEETXNU payload placed by loader devices (XNU mode)')
    g.add_argument('--payload', help='payload the shim boots from /chosen/linux,initrd-* (demo mode)')
    ap.add_argument('--shim', help='shim/out/Image for demo mode')
    ap.add_argument('--dtb', help='FDT with linux,initrd-start/end in /chosen (demo mode)')
    ap.add_argument('--kernel', help='unstripped XNU Mach-O for _iso_font text decoding')
    ap.add_argument('--secs', type=float, default=15, help='run duration (default 15)')
    ap.add_argument('--cpu', default='cortex-a76', help='QEMU CPU model')
    ap.add_argument('--gdb', action='store_true', help='halt at start, gdb stub on :1234')
    ap.add_argument('--fail-on-panic', action='store_true',
                    help='exit 2 for a visible panic, 3 for undecodable/blank (needs --kernel)')
    ap.add_argument('--check-env', action='store_true', help='only check QEMU is installed')
    a = ap.parse_args()

    if a.check_env:
        return 0 if check_qemu() else 1
    if not check_qemu():
        return 1
    if a.fail_on_panic and not a.kernel:
        ap.error('--fail-on-panic requires --kernel')
    if a.payload and (not a.shim or not a.dtb):
        ap.error('--payload needs --shim and --dtb (see scripts/mk-qemu-image.sh)')
    os.makedirs(a.outdir, exist_ok=True)
    out = os.path.abspath(a.outdir)
    mon = f'{out}/mon.sock'
    if os.path.exists(mon):
        os.unlink(mon)

    cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', a.cpu, '-smp', '1',
           '-m', '4G', '-nographic', '-serial', f'file:{out}/serial.txt',
           '-qmp', f'unix:{mon},server,nowait', '-no-reboot',
           '-d', 'int,guest_errors,unimp', '-D', f'{out}/qemu.log']
    if a.payload_only:
        phys_base, img, ba_pa, entry_pa = build_ram_image(open(a.payload_only, 'rb').read())
        open(f'{out}/ram.bin', 'wb').write(img)
        open(f'{out}/stub.bin', 'wb').write(stub(ba_pa, entry_pa))
        print(f'phys_base={phys_base:#x} boot_args={ba_pa:#x} entry={entry_pa:#x}', file=sys.stderr)
        cmd += ['-device', f'loader,file={out}/ram.bin,addr={phys_base:#x},force-raw=on',
                '-device', f'loader,file={out}/stub.bin,addr={STUB_PA:#x},force-raw=on',
                '-device', f'loader,addr={STUB_PA:#x},cpu-num=0']
    else:
        cmd += ['-kernel', os.path.abspath(a.shim), '-dtb', os.path.abspath(a.dtb),
                '-device', f'loader,file={os.path.abspath(a.payload)},addr={PAYLOAD_ADDR:#x},force-raw=on']
    if a.gdb:
        cmd += ['-S', '-gdb', 'tcp::1234']

    payload = open(a.payload or a.payload_only, 'rb').read()
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
    if a.gdb:
        print(f'gdb stub on :1234; QMP socket: {mon}', file=sys.stderr)
        p.wait()
        return 0
    time.sleep(a.secs)

    result = {'status': 'qemu_exited', 'qemu_returncode': p.returncode}
    if p.poll() is None:
        qmp(mon, 'stop', pmemsave=(FB_BASE, FB_STRIDE * FB_H, f'{out}/fb.raw'))
        p.kill()
        p.wait()
        raw = open(f'{out}/fb.raw', 'rb').read()
        write_png(f'{out}/fb.png', raw)
        if a.kernel:
            screen, unknown = decode_xnu(raw, payload_font(payload, a.kernel))
            open(f'{out}/fb.txt', 'w').write(screen)
            if 'panic(cpu ' in screen or 'SWEET EARLY PANIC:' in screen:
                result = {'status': 'panic'}
            elif unknown:
                result = {'status': 'incomplete_decode', 'unknown_glyphs': unknown}
            elif screen.strip():
                result = {'status': 'no_panic_observed'}
            else:
                result = {'status': 'blank_screen'}
        else:
            screen, unknown = decode_demo(raw, shim_font())
            open(f'{out}/fb.txt', 'w').write(screen)
            result = {'status': 'no_panic_observed' if screen.strip() else 'blank_screen',
                      'unknown_glyphs': unknown}
        os.unlink(f'{out}/fb.raw')
    open(f'{out}/result.json', 'w').write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), file=sys.stderr)
    print(f'{out}/serial.txt  {out}/fb.png  {out}/fb.txt', file=sys.stderr)
    if a.fail_on_panic and result['status'] == 'panic':
        return 2
    if a.fail_on_panic and result['status'] in ('incomplete_decode', 'blank_screen'):
        return 3
    return 0 if p.returncode is None else 1


if __name__ == '__main__':
    sys.exit(main())
