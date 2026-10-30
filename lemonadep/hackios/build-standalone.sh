#!/bin/bash
# Build the standalone HackiOS boot for lemonadep: power on the phone and iOS starts, no computer.
#
#   lemonadep-standalone.img  UEFI + loader + kernel + root.apfs + boot-args, flashed to boot_<slot>
#   system-rd.bin             4 KB header ("XNURDISK", u64 size) + system.apfs, written raw into
#                             userdata at DISKRD_OFFSET (past the 11.3 GB ios-sweet root image)
#
# The loader reads system-rd.bin from userdata to the resident address ("diskrd" payload item,
# patches/kernel-0002-wlan-hooks.patch) where "oem ramdisk" would have put it over USB, then runs the
# kernel as run.ps1 would. No loader_return_after: the session does not end on a timer. When the
# kernel comes back the loader powers the phone off if iOS asked to power off (or five fingers were
# held on the screen), and otherwise (restart, panic) starts iOS again.
#
# usage (from WSL): bash build-standalone.sh [OUT_DIR]      env: ARGS, DROP, EXTRA as build-boot.sh
set -e
FK=${FK:-/root/fkernel}
FZ=${FZ:-/root/hakios/fz/Mu-Silicium}
OUT=${1:-"/mnt/d/ios port/ios-sweet/lemonadep/out/hackios"}
DISKRD_PART=userdata
DISKRD_OFFSET=0x400000000      # 16 GiB into userdata
cd "$FK"
[ -e Mu-Silicium ] || ln -sfn "$FZ" Mu-Silicium
grep -q diskrd xnuloader/loader.c || { echo "loader without diskrd: apply patches/kernel-0002-wlan-hooks.patch"; exit 1; }

DEVICE=lemonadep
. common/devices/lemonadep.conf
FD=$FZ/Build/lemonadepPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
DTB=$FZ/Resources/DTBs/lemonadep.dtb
[ -f "$FD" ] && [ -f "$DTB" ] || { echo "missing $FD or $DTB"; exit 1; }
FD_BASE=0x9FD00000

# /private/var kept in userdata from 20 GiB on (patches/kernel-0004-userdata-store.patch + 0002): restored at boot,
# saved every 30 min and when iOS powers off or five fingers are held on the screen (which powers off)
STORE=${STORE-"ufs=2 ufs_write=1 var_store=1 var_store_every=1800"}
[ -n "$ARGS" ] && BOOTARGS=$ARGS
for w in $DROP; do BOOTARGS=$(echo " $BOOTARGS " | sed "s/ $w / /g; s/^ //; s/ $//"); done
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
printf '%s %s\0' $DISKRD_PART ${DISKRD_OFFSET#0x} > out/diskrd.txt
python3 xnuloader/mkpayload.py -o out/standalone-payload.bin loader.efi=xnuloader/loader.efi kernel=$K \
	ramdisk=userland/root.apfs diskrd=out/diskrd.txt --cmdline "sb_delay=8 $BOOTARGS $STORE $EXTRA"
FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE UEFI_FD=$FD DTB=$DTB UEFI_TARGET=lemonadep \
	BOOT_HEADER_VERSION=3 BOOT_ARGS="$BOOT_ARGS" BOOTSHIM_DEFS=$BOOTSHIM_DEFS \
	python3 mkxnuimg.py -p out/standalone-payload.bin -o out/lemonadep-standalone.img

mkdir -p "$OUT"
cp out/lemonadep-standalone.img "$OUT/"
if [ ! "$OUT/system-rd.bin" -nt userland/system.apfs ]; then
	python3 - userland/system.apfs "$OUT/system-rd.bin" <<'EOF'
import os, struct, sys, shutil
src, dst = sys.argv[1], sys.argv[2]
with open(dst, 'wb') as o:
    o.write((b'XNURDISK' + struct.pack('<Q', os.path.getsize(src))).ljust(4096, b'\0'))
    with open(src, 'rb') as i:
        shutil.copyfileobj(i, o, 16 << 20)
EOF
fi
ls -la "$OUT/lemonadep-standalone.img" "$OUT/system-rd.bin"
echo "write system-rd.bin at $DISKRD_PART + $DISKRD_OFFSET, flash lemonadep-standalone.img to boot_<slot>: see install-standalone.ps1"
