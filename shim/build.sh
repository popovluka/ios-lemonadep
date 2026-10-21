#!/bin/bash
# Build the shim and wrap it in an Android boot image.
#   ./build.sh [PAYLOAD]                    sweet: boot.img v2 mirroring the stock PixelOS header
#   BOARD=lemonadep OS_VERSION=x.y.z OS_PATCH_LEVEL=YYYY-MM ./build.sh [PAYLOAD]
#                                           lemonadep: boot.img v3 (DTB comes from vendor_boot)
#   PACK=0 ...                              build Image only, no mkbootimg
set -euo pipefail
cd "$(dirname "$0")"
BOARD=${BOARD:-sweet}
# AOSP system/tools/mkbootimg (Ubuntu 24.04's mkbootimg package lacks its gki module):
#   MKBOOTIMG="python3 /path/to/mkbootimg/mkbootimg.py" ./build.sh
MKBOOTIMG=${MKBOOTIMG:-mkbootimg}
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
case "$BOARD" in
sweet) OUT=out ;;
lemonadep) OUT=out/lemonadep; CFLAGS="$CFLAGS -DBOARD_LEMONADEP" ;;
*) echo "error: unknown BOARD=$BOARD (sweet, lemonadep)" >&2; exit 1 ;;
esac
SRCS="start.S shim.c console.c mmu.c fdt.c loader.c ufs.c payload_scan.c adt_patch.c display.c"
mkdir -p "$OUT"
objs=""
for f in $SRCS; do
	$CC $CFLAGS -c $f -o "$OUT/${f%.*}.o"
	objs="$objs $OUT/${f%.*}.o"
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r ${objs/$OUT\/start.o/} | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$objs -o "$OUT/shim.elf"
$OBJCOPY -O binary "$OUT/shim.elf" "$OUT/Image"
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm "$OUT/shim.elf" | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" "$OUT/Image"
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" "$OUT/ramdisk.bin"
else
	(cd "$OUT" && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
if [ "${PACK:-1}" = 0 ]; then
	# Image only (CI without stock os_version/patch level): no boot.img.
	IMG=$OUT/Image
	BOARD=none
fi
case "$BOARD" in
none) ;;
sweet)
	$MKBOOTIMG --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
		--dtb ../dtb/boot_unpacked/dtb --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
		--os_version 17.0.0 --os_patch_level 2035-06 \
		--cmdline "console=null" -o out/shim-boot.img
	IMG=out/shim-boot.img
	;;
lemonadep)
	# Header v3 (sm8350-common BoardConfigCommon.mk): kernel + ramdisk + cmdline only;
	# page size 4096 is implied, DTB/dtbo/vendor ramdisk come from the flashed slot.
	# os_version/os_patch_level must not be older than the device's: copy them from
	# the stock boot.img (AOSP unpack_bootimg.py). TODO(lemonadep): no default on purpose.
	: "${OS_VERSION:?set OS_VERSION from the stock boot.img (AOSP unpack_bootimg.py)}"
	: "${OS_PATCH_LEVEL:?set OS_PATCH_LEVEL (YYYY-MM) from the stock boot.img}"
	$MKBOOTIMG --header_version 3 --kernel "$OUT/Image" --ramdisk "$OUT/ramdisk.bin" \
		--os_version "$OS_VERSION" --os_patch_level "$OS_PATCH_LEVEL" \
		--cmdline "console=null" -o "$OUT/shim-boot-lemonadep.img"
	IMG=$OUT/shim-boot-lemonadep.img
	;;
esac
# Host render of the console layout.
gcc -O1 -Wall -o "$OUT/host_test" console.c host_test.c && "$OUT/host_test"
ls -l "$OUT/Image" "$IMG"
