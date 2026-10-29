#!/bin/bash
# Rebuild the QEMU XNU payload with the freshly CI-built kernel and re-run the boot.
# Usage: scripts/rebuild-qemu-payload.sh [outdir]
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-qemu-run-big15}"

# 1. Fresh ADT for QEMU (mkpayload adds /chosen/memory-map/RAMDisk itself)
python3 tools/mkadt.py /tmp/qemu-kebab.adt --qemu \
    --qemu-dram-base 0x100000000 --qemu-dram-size 0x160000000

# 2. Pack kernel + ADT + iOS root image (md0) into a SWEETXNU payload
python3 tools/mkpayload.py xnu/out/kernel.development.sweet /tmp/qemu-kebab.adt \
    ipsw-raw/qemu-big-payload2.bin \
    --ramdisk ipsw-raw/test-root.img \
    --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin rootrw=1 launchd_unsecure_cache=1 wdt=-1"

# 3. Boot it (--mem 10G is REQUIRED; 8G crashes in early console init)
python3 tools/qemu-kebab.py --payload-only ipsw-raw/qemu-big-payload2.bin \
    --font /tmp/iso-font.bin --mem 10G --secs 60 "$OUT"
