#!/bin/bash
# Build the lemonadep M2 iOS root image on a real Linux box (EndeavourOS/Arch)
# instead of WSL2, whose kernel has no HFS+ support at all.
#
# Run this on the MacBook (or any Arch-based machine with a normal kernel):
#   bash m2-build-macbook.sh
#
# Needs ~25 GB free disk, a few minutes of CPU, and a fast connection (downloads
# ~7.5 GB from Apple's CDN). Produces ios16-root.simg + its SHA-256 in ~/m2-build/.
# Nothing here touches a phone -- this only builds the image.
set -euo pipefail

WORK=${WORK:-$HOME/m2-build}
mkdir -p "$WORK"
cd "$WORK"

echo "== 1/6: installing dependencies =="
sudo pacman -Sy --needed --noconfirm base-devel cmake fuse3 rsync android-tools git curl

# hfsprogs is AUR-only on Arch. Try pacman first (some repos carry it), else
# build from the AUR snapshot (no AUR helper required).
if ! command -v mkfs.hfsplus >/dev/null 2>&1; then
	echo "-- hfsprogs not found, building from AUR --"
	rm -rf hfsprogs-aur
	git clone -q https://aur.archlinux.org/hfsprogs.git hfsprogs-aur
	(cd hfsprogs-aur && makepkg -si --noconfirm)
fi

# apfs-fuse has no Arch package at all: build from source (same as everywhere else).
if ! command -v apfs-fuse >/dev/null 2>&1; then
	echo "-- apfs-fuse not found, building from source --"
	rm -rf apfs-fuse
	git clone -q --recursive https://github.com/sgan81/apfs-fuse.git
	# apfs-fuse's CMakeLists.txt predates CMake 3.5; recent CMake (4.x on
	# Arch) dropped compatibility with that old cmake_minimum_required.
	# Its headers also rely on <cstdint> being pulled in transitively by
	# other includes, which recent libstdc++ (GCC 16) no longer does;
	# force-include it instead of patching 48 source files.
	cmake -S apfs-fuse -B apfs-fuse/build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"
	make -C apfs-fuse/build -j"$(nproc)"
	sudo install -m 755 apfs-fuse/build/apfs-fuse /usr/local/bin/apfs-fuse
fi

# blacktop/ipsw: only needed to extract the D22AP device tree (for mkadt.py's
# /product node). Download the release binary directly, no AUR/go needed.
if ! command -v ipsw >/dev/null 2>&1; then
	echo "-- ipsw CLI not found, downloading release binary --"
	curl -sfL -o ipsw.tar.gz \
		https://github.com/blacktop/ipsw/releases/download/v3.1.739/ipsw_3.1.739_linux_x86_64.tar.gz
	mkdir -p ipsw_bin && tar xzf ipsw.tar.gz -C ipsw_bin
	sudo install -m 755 ipsw_bin/ipsw /usr/local/bin/ipsw
fi

echo "== 2/6: getting the repo (for mkiosroot.sh, sweet-root-tweaks.py, remotezip.py) =="
if [ ! -d ios-sweet ]; then
	git clone -q -b lemonadep https://github.com/popovluka/ios-lemonadep.git ios-sweet
fi
cd ios-sweet && git fetch -q origin lemonadep && git checkout -q lemonadep && git reset -q --hard origin/lemonadep
cd "$WORK"
TOOLS="$WORK/ios-sweet/tools"

echo "== 3/6: downloading iPhone X 16.7.16 (20H392) IPSW parts (~7.5 GB total) =="
IPSW='https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw'
mkdir -p dt
[ -f OS.dmg ] || { python3 "$TOOLS/remotezip.py" "$IPSW" "087-86698-044.dmg" .; mv 087-86698-044.dmg OS.dmg; }
[ -f SystemOS.dmg ] || { python3 "$TOOLS/remotezip.py" "$IPSW" "087-86593-047.dmg" .; mv 087-86593-047.dmg SystemOS.dmg; }
[ -f AppOS.dmg ] || { python3 "$TOOLS/remotezip.py" "$IPSW" "087-86715-047.dmg" .; mv 087-86715-047.dmg AppOS.dmg; }
[ -f dt/Firmware/all_flash/DeviceTree.d22ap.im4p ] || python3 "$TOOLS/remotezip.py" "$IPSW" "DeviceTree.d22ap.im4p" dt

echo "sha256sums (compare against the ones in the session transcript):"
sha256sum OS.dmg SystemOS.dmg AppOS.dmg
# expected:
# 80f426d5b8fca8552f95be427434f1911dbb4c2c6d28aad94f2426ddd0c92483  OS.dmg
# 9cd738f3b0169055549df84ad6d8d13e50a2da39af618ce0522ca4e06df86849  SystemOS.dmg
# 391f2df322c3172b021b2874f6da5c3e78b70b3df8f502e1a4af2b1d715c4608  AppOS.dmg

echo "== 4/6: extracting the D22AP device tree (for mkadt.py's /product node) =="
mkdir -p "$WORK/ios-sweet/research/ipsw-x"
ipsw img4 im4p extract dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o "$WORK/ios-sweet/research/ipsw-x/d22ap.adt"

echo "== 5/6: building the HFS+ root image (this is the slow step: APFS decompress + rsync) =="
[ -f ios16-root.img ] || bash "$TOOLS/mkiosroot.sh" OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G

echo "== 6/6: applying SWEET bring-up tweaks and converting to a sparse image =="
sudo fsck.hfsplus -fy ios16-root.img || true
sudo mkdir -p /mnt/iosroot
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 "$TOOLS/sweet-root-tweaks.py" /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg

echo "=== DONE ==="
ls -la ios16-root.img ios16-root.simg
sha256sum ios16-root.simg
echo
echo "Copy ios16-root.simg (and this sha256sum line) back to the Windows machine."
