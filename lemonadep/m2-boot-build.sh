#!/bin/bash
# Build the M2 boot image: first real XNU boot with the actual iOS root
# (now flashed to userdata) mounted read-write.
#   lemonadep/m2-boot-build.sh KERNEL [CPUS]
# KERNEL: kernel.development.sweet (xnu-kernel-lemonadep artifact, overlay 1001+1002).
#
# ufsrw=1 rootrw=1: userdata now holds a real HFS+ iOS root (flashed from
# lemonadep/m2-artifacts/ios16-root.simg), so the UFS driver and HFS+ mount can
# write (journal replay needs this). launchd_unsecure_cache=1 matches the sweet
# README's documented boot-args: launchd accepts its job cache without AMFI.
set -euo pipefail
KERNEL=${1:?usage: m2-boot-build.sh KERNEL [CPUS]}
CPUS=${2:-1}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/shim/out/lemonadep"
mkdir -p "$OUT"
export OS_VERSION=${OS_VERSION:-16.0.0} OS_PATCH_LEVEL=${OS_PATCH_LEVEL:-2026-08}
export MKBOOTIMG=${MKBOOTIMG:-python3 /opt/aosp-mkbootimg/mkbootimg.py}
BOOTARGS="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress"
BOOTARGS="$BOOTARGS rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1"
BOOTARGS="$BOOTARGS sweettouch=0 sweetkeys=0 sweetusb=0 wdt=-1 wdt_test=no_user_checkin"

python3 "$REPO/tools/mkadt.py" "$OUT/m2.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m2.adt" "$OUT/m2-payload.bin" --cmdline "$BOOTARGS"
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m2-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m2-boot.img"
sha256sum "$KERNEL" "$OUT/m2-boot.img"
echo "boot-args: $BOOTARGS"
