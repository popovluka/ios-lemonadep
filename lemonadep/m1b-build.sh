#!/bin/bash
# Build the M1b boot image (first XNU boot on lemonadep): ADT + kernel payload
# inside the lemonadep shim, packed as a boot.img v3 for `fastboot boot`.
#   lemonadep/m1b-build.sh KERNEL [CPUS]
# KERNEL: kernel.development.sweet from the lemonadep.yml artifact
# (xnu-kernel-lemonadep, built with overlay 1001 + 1002).
# Read-only on purpose: no ufsrw=1, because userdata still holds Android and
# the persistent log ring would overwrite its last 4 MiB.
set -euo pipefail
KERNEL=${1:?usage: m1b-build.sh KERNEL [CPUS]}
CPUS=${2:-1}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/shim/out/lemonadep"
mkdir -p "$OUT"
# Values from the phone's boot_b header (BOOT_lemonadep.md, measured 2026-10-22).
export OS_VERSION=${OS_VERSION:-16.0.0} OS_PATCH_LEVEL=${OS_PATCH_LEVEL:-2026-08}
export MKBOOTIMG=${MKBOOTIMG:-python3 /opt/aosp-mkbootimg/mkbootimg.py}
BOOTARGS="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0"
BOOTARGS="$BOOTARGS sweettouch=0 sweetkeys=0 sweetusb=0 wdt=-1 wdt_test=no_user_checkin"

python3 "$REPO/tools/mkadt.py" "$OUT/m1b.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m1b.adt" "$OUT/m1b-payload.bin" --cmdline "$BOOTARGS"
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m1b-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m1b-boot.img"
sha256sum "$KERNEL" "$OUT/m1b-boot.img"
echo "boot-args: $BOOTARGS"
