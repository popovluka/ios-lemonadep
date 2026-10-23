#!/bin/bash
# Regenerate lemonadep/xnu/patches/1009-lemonadep-pixelios-ui-fixes.patch.
#
# Four small fixes from PixeliOS (github.com/PixOS-Project/pixelios-kernel,
# xnu/patches, commit 41198ee, APSL 2.0 like all XNU patches) whose base
# files are identical to ours, applied to a tree that already has every
# earlier overlay (check-overlay.sh WORKDIR leaves one at WORKDIR/xnu):
#   0084 no IOKit-daemon busy hold (IOKit can go quiet) + empty IOAcceleratorES
#        (QuartzCore stops polling 30 s per backboardd start; sweetaccel=0 off)
#   0085 sandbox op 4 container path for app extensions (PlugInKit aborts
#        every .appex without one)
#   0099 IONetworkStack stand-in: configd's InterfaceNamer waited 300 s for it,
#        which is the Settings -> General -> About freeze; Wi-Fi needs it too
#   0101 vsync callback reports the refresh period (was 0: CA saw every frame
#        deadline as already missed)
#   PIXOS=dir-with-their-patches lemonadep/xnu/gen-1009.sh /root/work/xnu-ref/xnu
set -euo pipefail
XNU=${1:?usage: PIXOS=DIR gen-1009.sh PATCHED_XNU_TREE}
PIXOS=${PIXOS:?PIXOS=directory holding their 0084/0085/0099/0101 patches}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/patches/1009-lemonadep-pixelios-ui-fixes.patch"
FILES="iokit/Kernel/IOStartIOKit.cpp iokit/Kernel/IOService.cpp iokit/Kernel/arm/SweetFramebuffer.cpp bsd/kern/sweet_amfi.c"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
for f in $FILES; do
	mkdir -p "$W/a/$(dirname "$f")" "$W/b/$(dirname "$f")"
	cp "$XNU/$f" "$W/a/$f"
	cp "$XNU/$f" "$W/b/$f"
done
for p in 0084-sweet-iokit-quiet-accelerator 0085-sweet-sandbox-container-path-appex \
	0099-sweet-ionetworkstack 0101-sweet-fb-vsync-period; do
	sed 's/\r$//' "$PIXOS/$p.patch" | patch -s -p1 -d "$W/b" --no-backup-if-mismatch
done
(cd "$W" && for f in $FILES; do diff -u "a/$f" "b/$f" || true; done) |
	sed -e 's/^\(--- a\/[^\t]*\)\t.*/\1/' -e 's/^\(+++ b\/[^\t]*\)\t.*/\1/' > "$OUT"
echo "$OUT: $(grep -c '^+++ ' "$OUT") files, $(wc -l < "$OUT") lines"
