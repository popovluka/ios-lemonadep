#!/bin/bash
# Boot image for Wi-Fi bring-up (overlay 1008): the usual interactive build
# plus sweetwlan=N, which stops QCA6490/PCIe0 bring-up cleanly after step N
# (see bsd/dev/lemonadep_wlan.c; 11+ = through reading the chip's PCI ID).
#   SWEETWLAN=N lemonadep/m4-wlan-build.sh KERNEL [CPUS]
# No Wi-Fi firmware is needed yet: stage 1 only powers the chip and brings up
# the PCIe link. Logs: TWRP + tools/sweet-log-pull.sh (needs ufsrw=1, set below).
set -euo pipefail
KERNEL=${1:?usage: SWEETWLAN=N m4-wlan-build.sh KERNEL [CPUS]}
CPUS=${2:-4}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/shim/out/lemonadep"
mkdir -p "$OUT"
export OS_VERSION=${OS_VERSION:-16.0.0} OS_PATCH_LEVEL=${OS_PATCH_LEVEL:-2026-08}
export MKBOOTIMG=${MKBOOTIMG:-python3 /opt/aosp-mkbootimg/mkbootimg.py}
BOOTARGS="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0"
# launchd needs these on a full boot (NOTES.md:171, m2-boot-build.sh).
BOOTARGS="$BOOTARGS unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1"
# ufsrw=1: the persistent log ring is only written by a read-write boot.
BOOTARGS="$BOOTARGS ufsrw=1 sweetfb_noconsole=1"
BOOTARGS="$BOOTARGS sweettouch=1 sweetkeys=1 sweetusb=0 sweetwlan=${SWEETWLAN:-1} wdt=-1 wdt_test=no_user_checkin"

python3 "$REPO/tools/mkadt.py" "$OUT/m4.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m4.adt" "$OUT/m4-payload.bin" --cmdline "$BOOTARGS"
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m4-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m4-wlan-boot.img"
sha256sum "$KERNEL" "$OUT/m4-wlan-boot.img"
echo "boot-args: $BOOTARGS"
