#!/bin/bash
# Build the ios-kebab iOS root image from live ipsw FUSE mounts.
#
# apfs-fuse cannot parse the iOS 16.7 images on this box ("invalid
# superblock"), so mkiosroot.sh's apfs-fuse path is replaced by blacktop's
# `ipsw mount fs/sys/app` (go-apfs) which mounts them correctly.
#
#   mkiosroot-ipsw.sh IPSW OUT.img SIZE       (SIZE e.g. 16G)
#
# Pre: the mounts are already live (this script only copies):
#   ipsw mount fs  IPSW --mount-point /tmp/osmount
#   ipsw mount sys IPSW --mount-point /tmp/sysmount
#   ipsw mount app IPSW --mount-point /tmp/appmount
#
# Same output as tools/mkiosroot.sh: one case-sensitive journaled HFS+
# volume with the OS root, the SystemOS + AppOS cryptexes hard-linked in,
# logd directories and the plain-plist system keybag.
set -euo pipefail
[[ $# -ge 3 ]] || { sed -n '2,16p' "$0"; exit 2; }
ipsw=$(realpath "$1") out=$(realpath "$2") size=$3
OSM=/tmp/osmount/root SYSM=/tmp/sysmount/root APPM=/tmp/appmount/root
if ! sudo -n du -sb "$OSM" >/dev/null 2>&1 || ! sudo -n du -sb "$SYSM" >/dev/null 2>&1; then
	echo "run 'ipsw mount fs/sys IPSW --mount-point ...' first (as root)" >&2
	exit 1
fi
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

work=$(mktemp -d)
cleanup() { sudo -n umount "$work/root" 2>/dev/null || true; }
trap cleanup EXIT

truncate -s "$size" "$out"
sudo -n mkfs.hfsplus -s -J -v iOSRoot "$out" >/dev/null
mkdir -p "$work/root"
sudo -n mount -t hfsplus -o loop,force "$out" "$work/root"

copy() { echo "copying ${1#/tmp/}mount/root -> ${2#$work/root}"; sudo -n mkdir -p "$2"; sudo -n rsync -aH --numeric-ids "$1/" "$2/"; }
copy "$OSM" "$work/root"
copy "$SYSM" "$work/root/private/preboot/Cryptexes/OS"
[ -d "$APPM" ] && copy "$APPM" "$work/root/private/preboot/Cryptexes/App"

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
sudo -n mkdir -p "$work/root/System/Library/Caches/com.apple.dyld"
for f in "$work/root/$dsc"/*; do
	sudo -n ln -f "$f" "$work/root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file that has no counterpart in the OS root. Existence checks use
# only whitelisted sudo commands (find/du), not test/ls.
exists() { sudo -n du -sb "$1" >/dev/null 2>&1; }
islink() { [ -n "$(sudo -n find "$1" -maxdepth 0 -type l -print -quit 2>/dev/null)" ]; }
for c in OS App; do
	sudo -n du -sb "$work/root/private/preboot/Cryptexes/$c" >/dev/null 2>&1 || continue
	(cd "$work/root/private/preboot/Cryptexes/$c" && sudo -n find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if ! exists "$work/root/$f" && ! islink "$work/root/$f"; then
			sudo -n mkdir -p "$work/root/$(dirname "$f")"
			src="$work/root/private/preboot/Cryptexes/$c/$f"
			if islink "$src"; then sudo -n cp -P "$src" "$work/root/$f"; else sudo -n ln "$src" "$work/root/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores (owner _logd 272), as in mkiosroot.sh.
db="$work/root/private/var/db"
sudo -n install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
sudo -n install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	sudo -n install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
sudo -n install -d -m 0700 -o 0 -g 0 "$work/root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	sudo -n install -m 0600 -o 0 -g 0 /dev/stdin "$work/root/private/var/keybags/systembag.kb"

sudo -n umount "$work/root"
sudo -n fsck.hfsplus -f -y "$out" 2>&1 | grep -v 'Illegal name' || true
sudo -n fsck.hfsplus -f -n "$out" 2>&1 | tail -1
echo "done: $out"
