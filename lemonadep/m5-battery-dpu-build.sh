#!/bin/bash
# Build the M5 boot image: battery (BQ28Z610 gauge via I2C) + DPU page-flip
# + DSI backlight, on top of the M3 GPU kernel (overlays 1001+1002+1007+1011).
#   lemonadep/m5-battery-dpu-build.sh KERNEL [CPUS]
# KERNEL: kernel.development.sweet from xnu-kernel-lemonadep (overlays 1–1011).
#
# New boot-args vs m3:
#   board=lemonadep   gates redwood_dpu_init() and redwood_battery_init() --
#                     without it both drivers return immediately
#   battery=1         enables redwood_battery_init() -> redwood_gauge_start()
#                     (the BQ28Z610 gauge on I2C gpio12/13, 100 kHz)
#   backlight=1       enables redwood_dpu_backlight() via DCS 0x51 on DSI0
#
# GPU (sweetgpu=1) is kept on: the GPU kernel is the same as m3, patch 1011
# only adds battery/DPU alongside it.
set -euo pipefail
KERNEL=${1:?usage: m5-battery-dpu-build.sh KERNEL [CPUS]}
CPUS=${2:-1}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/shim/out/lemonadep"
GMU_FW="$REPO/lemonadep/fw-gpu/a660_gmu.bin"
mkdir -p "$OUT"
[ -f "$GMU_FW" ] || { echo "error: $GMU_FW missing (run lemonadep/fw-gpu/pull.sh first)" >&2; exit 1; }
export OS_VERSION=${OS_VERSION:-16.0.0} OS_PATCH_LEVEL=${OS_PATCH_LEVEL:-2026-08}
export MKBOOTIMG=${MKBOOTIMG:-python3 /opt/aosp-mkbootimg/mkbootimg.py}
BOOTARGS="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0"
BOOTARGS="$BOOTARGS unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1"
BOOTARGS="$BOOTARGS ufsrw=1"
BOOTARGS="$BOOTARGS sweetfb_noconsole=1"
BOOTARGS="$BOOTARGS sweettouch=1 sweetkeys=1 sweetusb=0 sweetgpu=${SWEETGPU:-1} wdt=-1 wdt_test=no_user_checkin"
# Patch 1011 drivers:
BOOTARGS="$BOOTARGS board=lemonadep battery=1 backlight=1"

python3 "$REPO/tools/mkadt.py" "$OUT/m5.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m5.adt" "$OUT/m5-payload.bin" --cmdline "$BOOTARGS" \
	--extra-blob gmufw:"$GMU_FW" --extra-blob gpumem:zero:0x1414000
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m5-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m5-battery-dpu-boot.img"
sha256sum "$KERNEL" "$GMU_FW" "$OUT/m5-battery-dpu-boot.img"
echo "boot-args: $BOOTARGS"
