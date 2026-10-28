# QEMU Emulation & Testing Guide for ios-kebab

Two ways to test on QEMU without touching hardware:

1. **The sample image** (`scripts/mk-qemu-image.sh`) — boots the real phone
   chain (shim → SWEETXNU payload) with a small demo kernel in place of XNU.
   This is what "make a sample qemu vm image" means: it verifies the boot
   plumbing, the payload format, the ADT and the tools.
2. **A real XNU kernel** (`tools/qemu-kebab.py --payload-only`) — the
   kernel-only test rig the sweet port used; needs a CI-built kernel.

---

## 1. The sample image (no phone, no XNU build needed)

```sh
scripts/mk-qemu-image.sh      # builds everything into qemu-img/
qemu-img/run.sh               # boots for 15 s (run.sh SECONDS to change)
```

What `mk-qemu-image.sh` builds, from source only:

| File | What |
| --- | --- |
| `qemu-img/Image` | The boot shim (`shim/build.sh --qemu`) |
| `qemu-img/demo.macho` | Demo kernel ELF (linked at 0x40000188) wrapped as a Mach-O (`tools/mkmacho.py`) |
| `qemu-img/demo.adt` | Apple Device Tree for QEMU virt (`tools/mkadt.py --qemu`) |
| `qemu-img/payload.bin` | SWEETXNU payload = demo + ADT + cmdline (`tools/mkpayload.py`) |
| `qemu-img/demo.dtb` | QEMU FDT with `/chosen/linux,initrd-*` pointing at the payload |
| `qemu-img/run.sh` | Boots it all and dumps the screen |
| `qemu-img/kebab-demo.qcow2` | The same files in a portable FAT32 qcow2 (guestfish) |

The chain is byte-for-byte the phone's: QEMU starts the shim as a raw arm64
Image with `x0 = DTB` (exactly what the OnePlus ABL does), the shim finds
the payload through `/chosen/linux,initrd-start/end` and jumps into it with
the MMU off. The demo kernel then logs to the PL011 UART, walks the Apple
Device Tree, and draws the same font the shim uses into the 1080×2400
framebuffer.

`run.sh` output (in `qemu-img/qemu-run/`):

- `serial.txt` — boot_args, cmdline, ADT node list (`/device-tree`,
  `/chosen`, `/cpus/cpu0`, `/arm-io/gic`, …), heartbeat ticks
- `fb.png` — the framebuffer as the phone would show it
- `fb.txt` — decoded text of that screen
- `result.json` — status summary

Rebuild any time — everything compiles on a Linux PC with
`aarch64-linux-gnu-gcc`, `qemu-system-aarch64`, `fdtput` and `guestfish`.

To reuse the files from the qcow2 alone:

```sh
mkdir vm && cd vm
guestfish --ro -a ../qemu-img/kebab-demo.qcow2 -m /dev/sda1 copy-out / .
./run.sh
```

## 2. Kernel-only XNU testing (after a CI build)

```sh
# QEMU-compatible ADT, payload with a test RAM disk:
python3 tools/mkadt.py /tmp/qemu.adt --qemu
python3 tools/mkpayload.py xnu/out/kernel.development.sweet /tmp/qemu.adt /tmp/payload-qemu.bin \
    --ramdisk /path/to/root-fork.img \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam rd=md0'

# 40 s run, fail on panic (needs the unstripped kernel for font decoding):
python3 tools/qemu-kebab.py --payload-only /tmp/payload-qemu.bin /tmp/qemu_run \
    --kernel xnu/out/kernel.development.sweet --secs 40 --fail-on-panic
```

Other options: `--gdb` (halt at start, GDB stub on :1234), `--cpu` (default
`cortex-a76`, matching Kryo 585 Gold), `--check-env`.

## 3. Prerequisites

- **Arch:** `sudo pacman -S qemu-system-aarch64 aarch64-linux-gnu-gcc dtc libguestfs`
- **Ubuntu/Debian:** `sudo apt install qemu-system-arm gcc-aarch64-linux-gnu device-tree-compiler libguestfs-tools`
- **Fedora:** `sudo dnf install qemu-system-aarch64 gcc-aarch64-linux-gnu dtc libguestfs-tools`

Check with `python3 tools/qemu-kebab.py --check-env`.

## 4. Known QEMU-vs-phone differences

- QEMU virt has no UFS, no GPU, no DSI — the demo kernel exists so the chain
  can be exercised without any of them; the kebab drivers log "no Adreno
  650 (QEMU run?)" and stay out of the way.
- The QEMU GIC/timer addresses differ from SM8250; `mkadt --qemu` switches
  them (same trick as sweet's `--qemu`).
- The PL011 is QEMU-only; on the phone the console is the framebuffer (plus
  USB-ECM live log).
