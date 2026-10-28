#!/bin/bash
# Stage the ios-kebab device drivers (drivers/) into the xnu tree.
# dxb runs patches/*.sh from the XNU source root before applying *.patch,
# so this copies the sources that the 0082+ patches then wire into bsd/conf/files.
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
# The scripts run BEFORE the patches: guard on a vanilla-tree file, not on
# sweet_ufs.c (which patch 0028 adds later).
[[ -f bsd/kern/bsd_init.c ]] || exit 1

here=$(dirname "$0")/../../drivers
[[ -d "$here" ]] || { echo "drivers/ missing (checked out?)" >&2; exit 1; }

mkdir -p bsd/dev/kebab
cp "$here"/kebab_adreno.c bsd/dev/kebab/
cp "$here"/kebab_panel.c bsd/dev/kebab/
cp "$here"/kebab_touch.c bsd/dev/kebab/
cp "$here"/kebab_keys.c bsd/dev/kebab/
cp "$here"/kebab_pmic.c bsd/dev/kebab/
cp "$here"/kebab_ufs.c bsd/dev/kebab/
cp "$here"/kebab_usb.c bsd/dev/kebab/
cp "$here"/kebab_audio.c bsd/dev/kebab/
cp "$here"/kebab_sm8250.h bsd/dev/kebab/
echo "Staged ios-kebab drivers for SWEET (OnePlus 8T, SM8250)"
