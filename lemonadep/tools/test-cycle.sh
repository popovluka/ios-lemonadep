#!/bin/bash
# One hands-off test cycle for a boot image built with sweetreboot=<s>
# (overlay 1010): boot it, wait until the phone comes back to fastboot on its
# own (timer or panic), boot TWRP, pull the persistent log. The phone is left
# in TWRP, ready for the next cycle ("adb reboot bootloader" is the first step).
#   PATH=/c/platform-tools:$PATH lemonadep/tools/test-cycle.sh IMAGE OUT.txt [WAIT_S]
# Start with the phone in fastboot or TWRP. WAIT_S (default 420) should be a
# bit longer than the image's sweetreboot.
set -euo pipefail
IMG=${1:?usage: test-cycle.sh IMAGE OUT.txt [WAIT_S]}
OUT=${2:?usage: test-cycle.sh IMAGE OUT.txt [WAIT_S]}
WAIT=${3:-420}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TWRP=${TWRP:-"$REPO/../twrp-3.7.1_12-0-lemonadep.img"}
export MSYS_NO_PATHCONV=1
win() { command -v cygpath >/dev/null 2>&1 && cygpath -w "$1" || echo "$1"; }

in_fastboot() { fastboot devices 2>/dev/null | grep -q fastboot; }
wait_fastboot() {
	local t=0
	until in_fastboot; do
		sleep 5; t=$((t + 5))
		[ "$t" -lt "$1" ] || return 1
	done
}

if ! in_fastboot; then
	adb reboot bootloader
	wait_fastboot 60 || { echo "phone did not reach fastboot" >&2; exit 1; }
fi
fastboot boot "$(win "$IMG")"
echo "booted $(basename "$IMG") at $(date +%T); waiting up to ${WAIT}s for it to return to fastboot"
sleep 30   # leave fastboot before polling for it again
if ! wait_fastboot "$WAIT"; then
	echo "phone did not come back to fastboot within ${WAIT}s (hang or hard crash): needs Power+Vol Down" >&2
	exit 2
fi
echo "back in fastboot at $(date +%T)"
fastboot boot "$(win "$TWRP")"
t=0
until adb devices 2>/dev/null | grep -q recovery; do
	sleep 3; t=$((t + 3))
	[ "$t" -lt 120 ] || { echo "TWRP did not come up" >&2; exit 3; }
done
sleep 5
# relative path: under MSYS_NO_PATHCONV a /d/... script path reaches the
# Windows python3 unconverted and the decode step fails
(cd "$REPO" && bash tools/sweet-log-pull.sh "$OUT")
