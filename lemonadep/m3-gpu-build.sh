#!/bin/bash
# Build the M3 boot image (first on-device GPU bring-up attempt, overlay
# 1007): ADT + kernel payload + the real GMU firmware blob, packed as a
# boot.img v3 for `fastboot boot`.
#   lemonadep/m3-gpu-build.sh KERNEL [CPUS]
# KERNEL: kernel.development.sweet from the lemonadep.yml artifact
# (xnu-kernel-lemonadep, built with overlay 1001+1002+1007).
#
# sweetgpu=1: opt-in (unlike touch/keys/usb's opt-out default) -- every
# register write lemonadep_gpu_init() makes is unverified on real
# hardware (lemonadep/HANDOFF.md "GPU, Phase 8"). touch/keys/usb are
# disabled for this run to keep the console log focused on the GPU path.
# Read-only on purpose: no ufsrw=1, same reasoning as m1b-build.sh.
set -euo pipefail
KERNEL=${1:?usage: m3-gpu-build.sh KERNEL [CPUS]}
CPUS=${2:-1}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/shim/out/lemonadep"
GMU_FW="$REPO/lemonadep/fw-gpu/a660_gmu.bin"
mkdir -p "$OUT"
[ -f "$GMU_FW" ] || { echo "error: $GMU_FW missing (run lemonadep/fw-gpu/pull.sh from the phone first)" >&2; exit 1; }
export OS_VERSION=${OS_VERSION:-16.0.0} OS_PATCH_LEVEL=${OS_PATCH_LEVEL:-2026-08}
export MKBOOTIMG=${MKBOOTIMG:-python3 /opt/aosp-mkbootimg/mkbootimg.py}
BOOTARGS="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0"
BOOTARGS="$BOOTARGS sweettouch=0 sweetkeys=0 sweetusb=0 sweetgpu=1 wdt=-1 wdt_test=no_user_checkin"

python3 "$REPO/tools/mkadt.py" "$OUT/m3.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m3.adt" "$OUT/m3-payload.bin" --cmdline "$BOOTARGS" \
	--extra-blob gmufw:"$GMU_FW"
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m3-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m3-gpu-boot.img"
sha256sum "$KERNEL" "$GMU_FW" "$OUT/m3-gpu-boot.img"
echo "boot-args: $BOOTARGS"
