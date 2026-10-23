#!/bin/bash
# Pull the QCA6490 boot firmware from your own phone (in TWRP) into
# lemonadep/fw-wlan/ (gitignored: proprietary Qualcomm binaries, never
# committed or redistributed). cnss2 loads it from the modem partition's
# image/ directory (NON-HLOS FAT): amss.bin for chip major version 1,
# amss20.bin for 2 (cnss2/pci.c cnss_pci_update_fw_name), m3.bin later (QMI).
#   PATH=/c/platform-tools:$PATH lemonadep/tools/pull-wlan-fw.sh
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$REPO/lemonadep/fw-wlan"
mkdir -p "$OUT"
SLOT=$(adb shell getprop ro.boot.slot_suffix | tr -d '\r')
SLOT=${SLOT:-_b}
adb shell "mkdir -p /tmp/fwm && (mountpoint -q /tmp/fwm || mount -t vfat -o ro /dev/block/by-name/modem$SLOT /tmp/fwm)"
for f in amss.bin amss20.bin m3.bin; do
	# Git Bash: a Windows adb.exe needs a Windows path for the local side.
	dst="$OUT/$f"
	command -v cygpath >/dev/null && dst=$(cygpath -w "$dst")
	adb pull "/tmp/fwm/image/$f" "$dst" >/dev/null
	echo "$f $(wc -c < "$OUT/$f") bytes"
done
adb shell umount /tmp/fwm || true
(cd "$OUT" && sha256sum amss.bin amss20.bin m3.bin)
