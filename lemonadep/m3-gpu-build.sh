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
# unentitled_ios_sim_launch=1 + ignition_level=1: the documented fix for the
# namespace-9/code-0xe launchd panic every full boot hits without it (a
# macOS-vs-iOS-platform mismatch, unrelated to GPU work -- NOTES.md:171,
# lemonadep/m2-boot-build.sh). Without this, boot panics at launchd before
# the console shows enough to read live.
BOOTARGS="$BOOTARGS unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1"
# ufsrw=1: enables the persistent console log ring (patch 0047, last 4 MiB of
# userdata) so a crash can be read back with TWRP + tools/sweet-log-pull.sh
# instead of filming the screen -- read-only boots never write it. Same
# setting the proven m2-boot-build.sh uses (Android is gone from userdata).
# SWEETGPU: stage, see bsd/dev/lemonadep_gpu.c (1 = PDC handshake only).
BOOTARGS="$BOOTARGS ufsrw=1"
# Stop drawing console text over the GUI once it presents; it still goes to
# the persistent log, and a panic still draws (patch 0067).
BOOTARGS="$BOOTARGS sweetfb_noconsole=1"
# touch/keys on: logs are read back with TWRP + grep now, so their log
# lines don't get in the way, and the phone stays usable during a test.
BOOTARGS="$BOOTARGS sweettouch=1 sweetkeys=1 sweetusb=0 sweetgpu=${SWEETGPU:-1} wdt=-1 wdt_test=no_user_checkin"

python3 "$REPO/tools/mkadt.py" "$OUT/m3.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m3.adt" "$OUT/m3-payload.bin" --cmdline "$BOOTARGS" \
	--extra-blob gmufw:"$GMU_FW"
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m3-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m3-gpu-boot.img"
sha256sum "$KERNEL" "$GMU_FW" "$OUT/m3-gpu-boot.img"
echo "boot-args: $BOOTARGS"
