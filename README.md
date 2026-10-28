# ios-kebab

**Apple's iOS 16 running on a OnePlus 8T.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the OnePlus 8T (`kebab`, Qualcomm SM8250 / Snapdragon 865). On top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and the stock apps.

This repo is a fork of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which boots the same kernel on the Redmi Note 10 Pro (`sweet`, SM7150) — the sweet port reached the **iOS home screen on hardware**, and everything in this repo that is not `kebab`-specific is that proven work. The difference here:

| | ios-sweet (origin) | ios-kebab (this repo) |
| --- | --- | --- |
| Target | Redmi Note 10 Pro, SM7150 | **OnePlus 8T, SM8250** |
| iOS root | HFS+ in `userdata` (wipes Android data) | **RAM disk — userdata is never touched** |
| CPU | 6× A55 + 2× A76 | 4× A55 + 3× A77 + 1× A77 prime (Kryo 585) |
| GPU | none (CPU renders everything) | **Adreno 650 driver in bring-up** (GMU + CP ring + blit) |
| Panel | 1080×2400 @ 60, CPU copy | 1080×2400 @ 120, CPU copy + GPU-blit path |
| Other drivers | touch/keys/battery/USB on SM7150 | touch/keys/battery/USB on SM8250 (`drivers/`, `VERIFY`-marked) |
| QEMU | kernel test rig | **bootable sample image** (`scripts/mk-qemu-image.sh`) |

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's security model is mostly switched off. **Your Android installation and all of your data stay intact** — iOS runs from RAM, booted with a temporary `fastboot boot` image.

## How to do it yourself

**[BUILD_8T.md](BUILD_8T.md)** is the complete guide: backup the phone, build the kernel on GitHub's macOS runners, build a RAM-sized iOS root image, pack the boot image, and `fastboot boot` it. Nothing is flashed, nothing is wiped.

## What works / what does not

- **Boot:** stock OnePlus ABL (unlocked) → `fastboot boot` of the shim → XNU → HFS+ root in RAM → launchd → SpringBoard. Nothing is written to the phone's flash.
- **Display:** kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`) services; CoreAnimation renders in software. Presented to iOS as an iPhone X panel (1125×2436). **Note:** the 8T's Samsung AMB655X is a DSI **command-mode** panel (DSC) — unlike sweet's video-mode panel, frame updates need the DSI+DSC stream path in `drivers/kebab_panel.c`; until it works iOS runs on the frozen ABL splash.
- **GPU:** the Adreno 650 driver probes the GPU (0x3d00000), boots the GMU firmware (0x3d6a000, from your own vendor partition embedded in the device tree) and sets up a CP ring for kernel-side blits. No Metal/IOAccelerator yet — CoreAnimation still renders on the CPU.
- **Touch, buttons, battery:** Synaptics S3908 touch over GENI I²C (SE13 @ 0xa94000, addr 0x4b), PM8150/PM8150B PON keys over SPMI, IOPMPowerSource battery (the sweet port's HID chain, retargeted to SM8250 — addresses in `drivers/kebab_sm8250.h`, `VERIFY`-marked until a phone boot confirms them).
- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS or SEP/Face ID** — daemons for them start and fail or keep relaunching, exactly as on sweet.
- **Few apps, no App Store, no Apple ID**, ~4 minute boot, security off (permissive AMFI/sandbox, stub AppleKeyStore). Do not put anything personal on it.

## How it boots

```
ABL (stock OnePlus bootloader, unlocked)
  -> fastboot boot shim-boot.img              (temporary; nothing is flashed)
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args [+ RAM root image]
           -> XNU -> HFS+ root from the RAM disk (rd=md0)
              -> dyld + shared cache -> launchd -> daemons -> backboardd + SpringBoard
```

There is no serial port on the 8T either; the kernel draws its console into the framebuffer ABL leaves at `0x9c000000`, and with `kebakusb=1` the whole console streams live over USB-C as Ethernet + UDP (`tools/sweet-usbnet.sh`).

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) packed as an Android boot.img v2 |
| `xnu/patches/` | The XNU patch series (applied in order by CI). 0001–0081 are the sweet foundation; **0082+ is the kebab device work** (staged from `drivers/`) |
| `drivers/` | OnePlus 8T device drivers: `kebab_adreno.c` (GPU), `kebab_panel.c` (DPU/DSI), `kebab_touch.c`, `kebab_pmic.c` (keys/battery), `kebab_ufs.c`, `kebab_usb.c`, `kebab_audio.c`, and `kebab_sm8250.h` (every MMIO address in one place) |
| `.github/workflows/xnu.yml` | Kernel CI build (macOS runners, xnu-8796 + patches) |
| `tools/mkadt.py` | Generates the Apple Device Tree from kebab's SM8250 layout + iPhone X identity (`--gmu-fw` embeds the Adreno firmware) |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args (+ RAM root) into the shim payload |
| `tools/mkmacho.py` | Wraps the QEMU demo kernel in a Mach-O for mkpayload |
| `tools/mkiosroot.sh` | Builds the HFS+ iOS root from the IPSW images |
| `tools/sweet-root-tweaks.py` | Bring-up changes to the root (Setup skip, activation, daemons) |
| `tools/qemu-kebab.py` | QEMU runs: the shim+payload demo chain, or a real XNU kernel |
| `scripts/mk-qemu-image.sh` | Builds the sample QEMU image (shim + demo + payload + qcow2) |
| `qemu/demo/` | The demo kernel that stands in for XNU in QEMU |
| `tools/sweet-log-pull.sh`, `sweet-usbnet.sh`, `sweet-crash.py`, `xnu-cc.py`, … | Bring-up helpers (names keep the `sweet` prefix: the kernel-side features they drive are in the shared patch series) |
| `NOTES.md` | Detailed engineering log: every boot, crash and decision, plus hardware facts |
| `BUILD_8T.md` | **The complete compile + run guide, no userdata wipe** |
| `docs/QEMU_TESTING.md` | QEMU testing guide |

Not in git: partition backups, live device trees, recovery images, Apple firmware and root images, and downloaded or generated build trees. They contain device serials and calibration data, or Apple's copyrighted files.

## Kernel patches

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0057 | Diagnostics, permissive Sandbox policy, HFS rename-swap, HID event services (0058), iPhone X identity (0059) |
| 0058–0071 | HID services, identity, keys + touch drivers, display size, battery, SMP via PSCI |
| 0072–0081 | USB device mode (DWC3), CDC-ECM live console, 8 cores, display power |
| **0082** | **kebab device bring-up: Adreno 650, panel, touch, keys/PMIC, UFS, USB, audio (`drivers/`)** |

## Key hardware facts

- SoC: SM8250 (Snapdragon 865), Kryo 585: 1× A77 @ 2.84 GHz + 3× A77 @ 2.42 GHz + 4× A55 @ 1.80 GHz, 16K pages, ARMv8.2 without PAC, PSCI through `smc`, 19.2 MHz timer, GICv3 with the timer on Group-1 IRQ INTID 19.
- RAM: 8 or 12 GiB LPDDR; the kernel uses the upper bank above 4 GiB (12 GiB → 10 GiB usable, 8 GiB → ~5.9 GiB, ends 0x27B800000 per the U-Boot kebab tree). Check your phone's live FDT (BUILD_8T.md §3.1).
- GPU: Adreno 650 at `0x3d00000`, GMU block at `0x3d6a000`, GPUCC at `0x3d90000`, SMMU at `0x3da0000`; GMU firmware from the vendor partition.
- Display: Samsung AMB655X 1080×2400 @ 120 Hz, DSI **command mode** with DSC; splash at `0x9c000000` (VERIFY); presented as iPhone X 1125×2436. Real frame updates need the DSI+DSC path (`drivers/kebab_panel.c`).
- UFS: JEDEC UFSHC at `0x1d84000` (same IP the sweet driver runs).
- Touch/keys/battery: Synaptics S3908 touch (SE13 @ 0xa94000), SPMI PON keys on PM8150/PM8150B, PMIC fuel gauge — SM8250 addresses in `drivers/kebab_sm8250.h`, all `VERIFY`-marked until a phone boot.

More in [`NOTES.md`](NOTES.md).

## Credits

Built on the [ios-sweet](https://github.com/dipa-studio/ios-sweet) bring-up (itself on Apple's open-source releases — xnu, IOHIDFamily, libpthread, hfs — [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/OnePlus's published Linux kernel sources for the hardware details, and [OrangeFox Recovery](https://orangefox.download)). iOS itself is Apple's and is not distributed here.
