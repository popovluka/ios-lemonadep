#!/bin/bash
# Build the HackiOS UEFI boot image and the run payload for lemonadep, in the WSL lab
# (furangelz/hackios-xnu-kernel + hackios-xnu-userland, branch lemonadep, checked out at ~/fkernel).
# This is the exact recipe of the first GPU-accelerated iOS boot (2026-10-29); see ../HANDOFF.md.
#
# usage (from WSL): bash build-boot.sh [OUT_DIR]
set -e
FK=${FK:-/root/fkernel}
FZ=${FZ:-/root/hakios/fz/Mu-Silicium}   # the Mu-Silicium tree whose lemonadep FD reached the Silicium screen
OUT=${1:-"/mnt/d/ios port/ios-sweet/lemonadep/out/hackios"}
cd "$FK"

# mkxnuimg.py silently falls back to prebuilt/uefi/ (redwood's UEFI, another SoC) when ./Mu-Silicium
# is missing. That fallback crashed the phone into XBL ramdump on every boot: never let it happen.
[ -e Mu-Silicium ] || ln -sfn "$FZ" Mu-Silicium

DEVICE=lemonadep
. common/devices/lemonadep.conf          # first: it sets DTB=none, which the next lines override
FD=$FZ/Build/lemonadepPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
DTB=$FZ/Resources/DTBs/lemonadep.dtb
[ -f "$FD" ] && [ -f "$DTB" ] || { echo "missing $FD or $DTB"; exit 1; }
# The working values, whatever the conf says: FD at 0x9FD00000 (what the FD is linked for) and the
# OnePlus 9 DTB appended (DTB=none in the conf put the phone in 05C6:900E crash-dump mode).
FD_BASE=0x9FD00000

python3 xnuloader/mkpayload.py -o out/usb-payload.bin loader.efi=xnuloader/loader.efi --usb > /dev/null
FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE UEFI_FD=$FD DTB=$DTB UEFI_TARGET=lemonadep \
	BOOT_HEADER_VERSION=3 BOOT_ARGS="$BOOT_ARGS" BOOTSHIM_DEFS=$BOOTSHIM_DEFS \
	python3 mkxnuimg.py -p out/usb-payload.bin -o out/lemonadep-boot.img

[ -n "$ARGS" ] && BOOTARGS=$ARGS     # ARGS="...": the whole list instead of the profile's
for w in $DROP; do BOOTARGS=$(echo " $BOOTARGS " | sed "s/ $w / /g; s/^ //; s/ $//"); done
# wifi_mac: en0 made at kernel start, before configd (see build-standalone.sh); kept in fw-wlan/mac, not in git
MACF=$FK/fw-wlan/mac
[ -s "$MACF" ] || od -An -N5 -tx1 /dev/urandom | awk '{printf "02:%s:%s:%s:%s:%s\n", $1, $2, $3, $4, $5}' > "$MACF"
case " $BOOTARGS " in *" wlan_ath=1 "*) BOOTARGS="$BOOTARGS wifi_mac=$(cat "$MACF")" ;; esac   # DROP="adsp=1 ...": leave words out
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
python3 xnuloader/mkpayload.py -o run-payload.bin kernel=$K ramdisk=userland/root.apfs \
	--cmdline "loader_return_after=${SECS:-$DEFAULT_SECS} epoch=$(date +%s) sb_delay=8 $BOOTARGS $EXTRA"

mkdir -p "$OUT"
cp out/lemonadep-boot.img run-payload.bin "$OUT/"
[ "$OUT/system.apfs" -nt userland/system.apfs ] || cp userland/system.apfs "$OUT/system.apfs"
ls -la "$OUT"
