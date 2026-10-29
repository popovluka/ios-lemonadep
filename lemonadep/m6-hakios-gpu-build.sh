#!/bin/bash
# Build the M6 boot image: HakiOS's Adreno driver (patch 1012), on top of the
# M5 kernel (battery + DPU + keys + touch).
#   lemonadep/m6-hakios-gpu-build.sh KERNEL [CPUS] [STEPS]
# KERNEL: kernel.development.sweet from a build with overlays 1-1012.
#
# The driver runs steps 1..STEPS of its own 22 and then puts everything back,
# so STEPS is the whole experiment: 1 reads the clocks and RPMh names, 10 has
# written two words into the GMU's memory, 15 has the GMU's firmware running,
# 17 has the 3D core powered, 21 has it out of secure mode, 22 draws a picture.
# Start at 1. Steps that only read are 1-3, 6-10; the rest write.
#
# Unlike m5 this leaves our own GPU driver out of the way (sweetgpu=0): both
# would otherwise reach for the same GMU, SMMU and clocks in one boot.
#
# Firmware: all four files the driver asks for travel as mkpayload blobs, which
# arrive as <name>_pa/<name>_bytes boot-args -- the mechanism a660_gmu.bin
# already used in m5, now also for the 3D core's microcode and the zap shader.
# They are the phone's own, from its /vendor/firmware, and are gitignored.
set -euo pipefail
KERNEL=${1:?usage: m6-hakios-gpu-build.sh KERNEL [CPUS] [STEPS]}
CPUS=${2:-8}
STEPS=${3:-1}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/shim/out/lemonadep"
FW="$REPO/lemonadep/fw-gpu"
mkdir -p "$OUT"

for f in a660_gmu.bin a660_sqe.fw a660_zap.mdt a660_zap.b02; do
	[ -f "$FW/$f" ] || { echo "error: $FW/$f missing (run lemonadep/fw-gpu/pull.sh)" >&2; exit 1; }
done

export OS_VERSION=${OS_VERSION:-16.0.0} OS_PATCH_LEVEL=${OS_PATCH_LEVEL:-2026-08}
export MKBOOTIMG=${MKBOOTIMG:-python3 /opt/aosp-mkbootimg/mkbootimg.py}

BOOTARGS="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0"
BOOTARGS="$BOOTARGS unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1"
BOOTARGS="$BOOTARGS ufsrw=1 sweetfb_noconsole=1"
BOOTARGS="$BOOTARGS sweettouch=1 sweetkeys=1 sweetusb=0 sweetgpu=0 wdt=-1 wdt_test=no_user_checkin"
BOOTARGS="$BOOTARGS board=lemonadep battery=1 backlight=1"
# The driver's own: how far to go, and how long it waits after announcing a
# group of accesses so the log reaches storage before a silent reset.
BOOTARGS="$BOOTARGS adreno=$STEPS adreno_pause=${ADRENO_PAUSE:-2200}"

python3 "$REPO/tools/mkadt.py" "$OUT/m6.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m6.adt" "$OUT/m6-payload.bin" --cmdline "$BOOTARGS" \
	--extra-blob gmufw:"$FW/a660_gmu.bin" \
	--extra-blob sqefw:"$FW/a660_sqe.fw" \
	--extra-blob zapmdt:"$FW/a660_zap.mdt" \
	--extra-blob zapb02:"$FW/a660_zap.b02"
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m6-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m6-hakios-gpu-boot.img"
sha256sum "$KERNEL" "$OUT/m6-hakios-gpu-boot.img"
echo "boot-args: $BOOTARGS"
