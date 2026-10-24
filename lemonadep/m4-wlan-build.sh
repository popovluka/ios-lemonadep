#!/bin/bash
# Boot image for Wi-Fi bring-up (overlay 1008): the usual interactive build
# plus sweetwlan=N, which stops QCA6490/PCIe0 bring-up cleanly after step N
# (see bsd/dev/lemonadep_wlan.c; 11+ = through reading the chip's PCI ID).
#   SWEETWLAN=N lemonadep/m4-wlan-build.sh KERNEL [CPUS]
# Stages 1-13 need no firmware; 14+ embed amss.bin/amss20.bin (below).
# Logs: TWRP + tools/sweet-log-pull.sh (needs ufsrw=1, set below).
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
# Flush the log ring every second (default 10 s): stage 2 logs a checkpoint
# before each risky access and waits for it to reach the disk.
BOOTARGS="$BOOTARGS ufslogsecs=1"
BOOTARGS="$BOOTARGS sweettouch=1 sweetkeys=1 sweetusb=0 sweetwlan=${SWEETWLAN:-1} wdt=-1 wdt_test=no_user_checkin"

# Stage 2 (sweetwlan >= 14) hands the chip its firmware straight from where
# these blobs land; pull them first with lemonadep/tools/pull-wlan-fw.sh.
FW="$REPO/lemonadep/fw-wlan"
BLOBS=()
if [ "${SWEETWLAN:-1}" -ge 14 ]; then
	for f in amss.bin amss20.bin; do
		[ -f "$FW/$f" ] || { echo "error: $FW/$f missing (run lemonadep/tools/pull-wlan-fw.sh)" >&2; exit 1; }
	done
	# Only the SBL part (first 512 KiB, cnss2 sbl_size): the full 5 MB images
	# made the payload (ABL's initrd) big enough that the boot never reached
	# the kernel log (2026-10-24, logs/lemonadep-wlan3/4); the shim refuses a
	# payload that overlaps kernel memory. BHIe will need the rest from disk.
	head -c 524288 "$FW/amss.bin" > "$OUT/wlan-sbl1.bin"
	head -c 524288 "$FW/amss20.bin" > "$OUT/wlan-sbl2.bin"
	BLOBS=(--extra-blob "wlanfw1:$OUT/wlan-sbl1.bin" --extra-blob "wlanfw2:$OUT/wlan-sbl2.bin")
fi

python3 "$REPO/tools/mkadt.py" "$OUT/m4.adt" --board lemonadep
python3 "$REPO/tools/mkpayload.py" "$KERNEL" "$OUT/m4.adt" "$OUT/m4-payload.bin" --cmdline "$BOOTARGS" "${BLOBS[@]}"
BOARD=lemonadep bash "$REPO/shim/build.sh" "$OUT/m4-payload.bin"
cp "$OUT/shim-boot-lemonadep.img" "$OUT/m4-wlan-boot.img"
sha256sum "$KERNEL" "$OUT/m4-wlan-boot.img"
echo "boot-args: $BOOTARGS"
