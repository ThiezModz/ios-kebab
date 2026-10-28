# ios-kebab: compile and run iOS 16 on the OnePlus 8T **without wiping userdata**

This is the complete path from a stock OnePlus 8T (`kebab`, SM8250) to the iOS
home screen, with one hard rule:

> **Your Android installation and every byte of userdata survive.** Nothing is
> ever flashed to `userdata`, no partition table changes, no repartitioning.
> The iOS root lives in RAM (or later on a USB-C drive); the phone boots it
> with a temporary `fastboot boot` image. Reboot and you are back in Android.

The catch: a RAM root means the iOS root image must fit in the phone's RAM
next to the kernel. Details and the fallback options are in step 3.

```
┌───────────── OnePlus 8T (SM8250) ─────────────┐      ┌──── Linux PC ────┐
│ stock ABL (unlocked)                         │      │ 0. backup        │
│   └─ fastboot boot shim-boot.img  (temporary)│◄─────│ 1. kernel (CI)    │
│        └─ shim: MMU off, reads live FDT      │      │ 2. root image     │
│           └─ SWEETXNU payload (kernel+ADT    │      │ 3. boot image     │
│              + RAM root image)               │      │ 4. fastboot boot  │
│              └─ XNU ─► HFS+ root in RAM      │      └───────────────────┘
│                 └─ launchd ─► SpringBoard    │
└──────────────────────────────────────────────┘
```

Nothing here needs root on the PC except mounting the HFS+ image for tweaks,
and nothing here writes to the phone's flash at all.

---

## 0. Prerequisites and one-time preparation

- A OnePlus 8T, any OxygenOS version, **bootloader already unlocked**
  (`fastboot oem unlock` — note: unlocking wipes the phone ONCE, do it before
  you put data on it; everything below never wipes anything again).
- A Linux PC (Arch/Ubuntu/Fedora all fine). Root on the PC only for the
  `mount -o loop` steps.
- A GitHub account (free macOS CI minutes build the kernel; see step 1).

### 0.1 Install the tools

```sh
# Arch
sudo pacman -S qemu-system-aarch64 aarch64-linux-gnu-gcc android-tools dtc \
    apfs-fuse hfsprogs img2simg cpio dosfstools python
# Ubuntu/Debian
sudo apt install qemu-system-arm gcc-aarch64-linux-gnu android-tools-android \
    device-tree-compiler apfs-fuse hfsprogs android-sdk-libsparse-utils python3
```

### 0.2 Back up the phone (per-device calibration data)

Boot the stock recovery or a temporary OrangeFox and pull at least `persist`,
`modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo`, `vbmeta`, and the
**vendor partition** (the GPU firmware for step 2.5 lives there). These hold
your IMEI and RF calibration; keep them off git (`.gitignore` covers
`backup/`).

```sh
mkdir -p backup
adb pull /dev/block/by-name/persist backup/       # in recovery, root shell
# ... same for modemst1 modemst2 fsg fsc boot dtbo vbmeta
adb pull /dev/block/by-name/vendor_b backup/vendor_b.img   # ~1.5 GB, for the GPU fw
```

### 0.3 Extract the stock DTB (the shim needs it for the boot.img)

```sh
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked   # -> dtb/boot_unpacked/dtb
fdtdump dtb/boot_unpacked/dtb | grep -A4 memory@      # note the RAM banks, step 3
```

`dtb/` is gitignored (it contains the device serial).

---

## 1. Build the XNU kernel (GitHub Actions, macOS runners)

The kernel is Apple's open-source xnu-8796 (iOS 16 generation) plus this
repo's patch series. The macOS CI does it because the build needs Xcode's
headers. Fork this repo (public repos get free macOS runner minutes) and:

```sh
gh workflow run xnu.yml -R YOU/ios-sweet        # ios-sweet = this repo's name
gh run watch -R YOU/ios-sweet
gh run download -R YOU/ios-sweet -n xnu-kernel -D xnu/out
ls xnu/out      # kernel.development.sweet (the SWEET machine config: the port codename)
```

The workflow builds the **same kernel** the Redmi Note 10 Pro port boots;
the 8T differs only in the Apple Device Tree (step 2.4) and the drivers in
patch `0082-*` (`drivers/`, staged into `bsd/dev/kebab/`).

---

## 2. Build the iOS root image (RAM-sized)

The root is one journaled HFS+ image holding the iOS OS volume plus both
cryptexes — exactly what `tools/mkiosroot.sh` makes. For a RAM root the image
must fit the upper DDR bank next to the kernel (see the size table in step
3.1), so the default changes:

```sh
# OS, SystemOS and AppOS images from the iPhone X (iPhone10,3) 16.7.16
# (20H392) IPSW - see BuildManifest.plist for the file names. Only Apple
# hosts the IPSW; use your own download.
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 8G
```

- **8G** is the sweet spot for a 12 GB phone (see the RAM map below).
- If the result is too big for your RAM bank, drop the AppOS cryptex (it
  holds preinstalled apps only — the home screen boots without it):
  `tools/mkiosroot.sh OS.dmg SystemOS.dmg /dev/null ios16-root.img 5G`
- A journaled HFS+ volume is required (XNU replays the journal after a
  panic; Linux mounts it read-write with `-o force`).

Then the bring-up tweaks (same script the sweet port ships):

```sh
sudo fsck.hfsplus -fy ios16-root.img
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot   # Setup skip, activation, watchdogs...
sudo umount /mnt/iosroot
```

### 2.1 The iPhone X identity (one-time)

`tools/mkadt.py` copies `/product` from the real D22AP device tree. Extract
it once, without downloading the whole IPSW:

```sh
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

### 2.2 The GPU firmware (one-time)

The Adreno 650 GMU firmware ships in your phone's vendor partition; extract
it from the backup you took:

```sh
# vendor_b.img is Android's ext4/erofs image; mount it read-only:
mkdir -p /tmp/vendor && sudo mount -o loop,ro backup/vendor_b.img /tmp/vendor
cp /tmp/vendor/firmware/a650_gmu.bin backup/    # exact path may differ; find it:
find /tmp/vendor -name 'a650*' -o -name '*gmu*'
sudo umount /tmp/vendor
```

### 2.3 Build the Apple Device Tree (this is the device-specific part)

```sh
python3 tools/mkadt.py shim/out/kebab.adt --highmem \
    [--highmem-base 0x100000000] [--highmem-size 0x...] \
    [--gmu-fw backup/a650_gmu.bin]
```

`mkadt.py` embeds the kebab MMIO layout (SM8250 GIC, UFS, GENI QUP, DPU,
framebuffer — the same numbers `drivers/kebab_sm8250.h` uses) plus the
iPhone X identity and the `/chosen` security status that activates iOS
without Apple's servers. `--gmu-fw` embeds the Adreno firmware as a
`/chosen` property the GPU driver reads at boot.

### 2.4 Build the payload and boot image

```sh
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/kebab.adt shim/out/payload.bin \
    --ramdisk ios16-root.img \
    --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress \
        rd=md0 rootdev=md0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin \
        rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 kebabgputest=1'
shim/build.sh shim/out/payload.bin        # -> shim/out/shim-boot.img
```

What the new parts do:

| Boot-arg | Effect |
| --- | --- |
| `rd=md0 rootdev=md0` | Root = the ramdisk payload (no UFS, no userdata touch) |
| `cpus=8` | All 8 Kryo 585 cores (1+3 A77 + 4 A55); `cpus=1` is the safe fallback |
| `kebabgputest=1` | Adreno self-test: GPU blit roundtrip, PASS/FAIL in the log |
| `kebabnoprobe=1` | Skip every kebab driver (bisect helper) |
| `kebakusb=1`, `kebabdpu=1`, `kebakaudio=1` | Opt-in USB/DPU/audio probes |

The kernel log: with a RAM root there is no persistent userdata ring, so the
console goes to the framebuffer and (with `kebakusb=1`) streams live over
USB-C Ethernet — see step 5.

---

## 3. Boot it — nothing is flashed, nothing is wiped

```sh
fastboot boot shim/out/shim-boot.img
```

That is the whole step. The image is transferred to RAM and executed; the
phone's storage is never written. The first boot takes 4-5 minutes on the
Apple logo, then the lock screen. **To go back to Android: reboot.** To get
back to fastboot: hold Power + Volume down.

### 3.1 RAM map and why the 12 GB variant is the comfortable one

| Variant | DDR | Upper bank above 4 GiB | iOS root budget |
| --- | --- | --- | --- |
| 12 GB | 0x80000000 + 12 GiB | 0x1_00000000 … 0x3_80000000 (**10 GiB**) | 8G image fits |
| 8 GB | 0x80000000 + ~7.93 GiB | 0x1_00000000 … 0x2_7B800000 (**~5.9 GiB**, U-Boot kebab: total ends 0x27B800000) | ≤ 4G image (drop AppOS) |

Check the real numbers on your phone first — the live FDT is authoritative:

```sh
fdtdump dtb/boot_unpacked/dtb | grep -B1 -A4 'memory@' | grep -E 'reg|memory'
```

and pass them to mkadt (`--highmem-base/--highmem-size`). The shim's ADT and
the payload keep the same numbers, and the shim refuses to boot a payload
whose RAM window overlaps the payload itself.

### 3.2 Why not USB root yet (and how it will work)

The USB-C port could hold a small SSD with a 16G HFS+ root and keep the
phone's RAM free. That needs the DWC3 in **host** mode; the current USB
patches (0072-0077) bring up **device** mode (ECM networking, which is what
streams the log). `drivers/kebab_usb.c` is the place where host mode lands;
until then, RAM root is the no-wipe answer.

---

## 4. Verify and iterate

First boot checklist:

1. `KEBAK GPU: RBBM_STATUS ...` — the Adreno answers at 0x3d00000.
2. `KEBAK GPU: blit self-test PASS` — the CP ring and memcpy blit work.
   FAIL here means the GPU SMMU is not in bypass for the kernel's buffers
   (the next bring-up step; the display falls back to CPU copies).
3. Touch + power/volume work (Goodix over GENI SE7, PMIC PON).
4. Home screen renders (CPU path, as on sweet).

Panics are drawn on the screen; with `kebakusb=1` the whole console also
streams live over USB-C as Ethernet + UDP (see `tools/sweet-usbnet.sh`).

---

## 5. Reading the kernel log

RAM root = no persistent log ring on disk. Two options:

- **Live over USB (preferred):** boot with `kebakusb=1 sweetusb=2` and on
  the PC run `tools/sweet-usbnet.sh logs/boot.txt` — the phone enumerates as
  CDC-ECM, the PC answers 192.168.7.1/24 and the kernel streams its console
  as UDP to 192.168.7.1:5140 (starts with the last 64 KiB).
- **On screen:** drop `sweetfb_noconsole` and film or photograph the panel.
- Once USB host mode exists, the log ring will live on the USB root drive
  like the userdata ring did on sweet (`tools/sweet-log-pull.sh`).

---

## 6. The QEMU sample (test the whole chain on your PC)

No phone needed to verify the boot chain, the payload format or the tools:

```sh
scripts/mk-qemu-image.sh        # builds shim + demo kernel + ADT + payload + qcow2
qemu-img/run.sh                 # boots it: serial log + fb.png screen dump
```

It runs the exact phone path (shim → SWEETXNU payload) with a tiny demo
kernel in place of XNU, and is rebuilt from source at any time. See
`docs/QEMU_TESTING.md`.

---

## 7. What can still go wrong (honest list)

- **RAM root size:** an 8G HFS+ image in a ~5.9 GiB bank will not boot —
  check the FDT numbers first, and drop the AppOS cryptex if needed.
- **The panel is DSI command mode (Samsung AMB655X, DSC).** Unlike sweet's
  video-mode panel, writes into the splash buffer do not reach the glass
  once ABL stops streaming: real frame updates need the DSI+DSC command
  path in `drivers/kebab_panel.c` (the known big bring-up step; until then
  iOS boots on the frozen splash and the kernel console).
- **Touch is a Synaptics S3908** (not sweet's Goodix): the probe dump under
  `kebabtouch` confirms power/rail and the register map before the event
  thread starts.
- **GPU SMMU:** the Adreno may not see the kernel's buffers until the SMMU
  (0x3da0000) is configured or bypassed; the self-test says exactly that.
- **GMU firmware path** inside vendor differs per OxygenOS version; the
  driver logs what it found.
- **Panel/touch addresses** are `VERIFY`-marked in `drivers/kebab_sm8250.h`;
  the live FDT on your phone is the ground truth and the first boot logs
  every value the drivers program.
- **Crashes on the GUI path** are usually root-image issues, not drivers;
  the sweet notes (`NOTES.md`) document every one of them and their fixes.
