#!/bin/bash
# Fetch reference sources from the LineageOS kernel for this phone, pinned at
# the commit HWMAP_lemonadep.md cites (LOS-K), into /root/work/los-k. Read-only
# references for sourcing register facts; nothing here is copied into the tree.
#   lemonadep/tools/fetch-los-k.sh path/in/kernel ...
set -euo pipefail
COMMIT=187d13c
B=https://raw.githubusercontent.com/LineageOS/android_kernel_oneplus_sm8350/$COMMIT
D=/root/work/los-k
for f in "$@"; do
	mkdir -p "$D/$(dirname "$f")"
	if curl -sfL -o "$D/$f" "$B/$f"; then
		echo "ok $f $(wc -l < "$D/$f")"
	else
		echo "MISSING $f"
	fi
done
