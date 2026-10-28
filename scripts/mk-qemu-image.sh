#!/bin/bash
# Build the ios-kebab QEMU sample image: the real phone boot chain in a VM.
#
#   mk-qemu-image.sh [OUTDIR]        (default: qemu-img/)
#
# Produces, from source only:
#   Image            the boot shim (shim/build.sh --qemu)
#   demo.macho       the demo kernel (qemu/demo/), wrapped as a Mach-O
#   demo.adt         Apple Device Tree for QEMU virt (tools/mkadt.py --qemu)
#   payload.bin      SWEETXNU payload = demo Mach-O + ADT + cmdline
#   demo.dtb         QEMU FDT with /chosen/linux,initrd-* = the payload
#   run.sh           boots everything with qemu-system-aarch64 + fb.png dump
#   kebab-demo.qcow2 the same files in a portable FAT32 qcow2 (guestfish)
#
# The chain is identical to the phone: QEMU starts the shim as a raw arm64
# Image with x0 = DTB, the shim finds the payload through
# /chosen/linux,initrd-* and jumps into it (shim/loader.c). When a real XNU
# kernel is built (see BUILD_8T.md), tools/mkpayload.py packs it the same
# way and this script is not involved.
#
# Rebuild any time: everything here compiles on a Linux PC with
# aarch64-linux-gnu-gcc, qemu-system-aarch64, fdtput and guestfish.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/qemu-img}"
CC=aarch64-linux-gnu-gcc
mkdir -p "$OUT"

# 1. The shim (raw Image only; the boot.img path needs the phone's DTB).
"$ROOT/shim/build.sh" --qemu
cp "$ROOT/shim/out/Image" "$OUT/Image"

# 2. The demo kernel: freestanding aarch64, linked at 0x40000188 (demo.ld),
#    wrapped into a minimal Mach-O (tools/mkmacho.py).
(cd "$ROOT/qemu/demo" && \
	$CC -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -fno-stack-protector \
	    -Wall -Wextra -I"$ROOT/shim" -static -T demo.ld -o demo.elf demo.S demo.c \
	    -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N)
python3 "$ROOT/tools/mkmacho.py" "$ROOT/qemu/demo/demo.elf" "$OUT/demo.macho"

# 3. Apple Device Tree for QEMU virt (the shim jumps with the ADT it carries).
python3 "$ROOT/tools/mkadt.py" "$OUT/demo.adt" --qemu

# 4. The SWEETXNU payload: what the phone's ramdisk would hold.
python3 "$ROOT/tools/mkpayload.py" "$OUT/demo.macho" "$OUT/demo.adt" "$OUT/payload.bin" \
	--cmdline "ios-kebab demo cpus=1 keepsyms=1"

# 5. A QEMU FDT whose /chosen points at the payload in guest RAM. QEMU places
#    its own DTB at 0x48000000; the payload goes at 0x50000000, below the
#    ADT's phys_base (0xb0000000) so the shim's overlap check passes.
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 \
	-machine dumpdtb="$OUT/demo.dtb" 2>/dev/null
PAYLOAD_SIZE=$(stat -c %s "$OUT/payload.bin")
PAYLOAD_END=$((0x50000000 + ((PAYLOAD_SIZE + 0xfff) & ~0xfff)))
fdtput -t x "$OUT/demo.dtb" /chosen linux,initrd-start 0x50000000
fdtput -t x "$OUT/demo.dtb" /chosen linux,initrd-end "$PAYLOAD_END"

# 6. The runner: serial on the console, framebuffer + decoded text in out/.
cat > "$OUT/run.sh" <<'RUN'
#!/bin/bash
# Boot the ios-kebab QEMU sample. Usage: run.sh [SECONDS]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
python3 "$HERE/../tools/qemu-kebab.py" \
	--shim Image --dtb demo.dtb --payload payload.bin \
	--secs "${1:-15}" qemu-run
echo; echo "serial:     qemu-run/serial.txt"
echo "screen:     qemu-run/fb.png"
echo "screen txt: qemu-run/fb.txt"
RUN
chmod +x "$OUT/run.sh"

# 7. A portable qcow2: FAT32 with the same files, so the image can be copied
#    or attached to a VM. Extract with:
#      guestfish --ro -a kebab-demo.qcow2 -m /dev/sda1 copy-out / .
if command -v guestfish >/dev/null && command -v qemu-img >/dev/null; then
	rm -f "$OUT/kebab-demo.qcow2"
	qemu-img create -f qcow2 "$OUT/kebab-demo.qcow2" 64M >/dev/null
	guestfish -a "$OUT/kebab-demo.qcow2" >/dev/null 2>&1 <<EOF || true
run
part-disk /dev/sda gpt
mkfs vfat /dev/sda1
mount /dev/sda1 /
upload $OUT/Image /Image
upload $OUT/payload.bin /payload.bin
upload $OUT/demo.dtb /demo.dtb
upload $OUT/demo.macho /demo.macho
upload $OUT/demo.adt /demo.adt
upload $OUT/run.sh /run.sh
umount-all
EOF
fi

ls -l "$OUT" | tail -10
echo "boot it:  $OUT/run.sh"
