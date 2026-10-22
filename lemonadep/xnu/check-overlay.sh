#!/bin/bash
# Check the lemonadep overlay without a macOS build:
#  1. clone xnu-8796.141.3 (the tag darwin-xnu-build uses for MACOS_VERSION=13.5)
#  2. replay xnu/patches/*.patch then lemonadep/xnu/patches/*.patch with CI's
#     rule (git apply --check, else SKIP); fail if any overlay patch is skipped,
#     since CI would silently build without it
#  3. run lemonadep/xnu/test against the patched tree (1001 code, verbatim):
#     start.s blocks assembled for arm64 Mach-O and run on aarch64 (qemu-user),
#     sweet_early.c + sweet_early_map_final() on aarch64 and under x86 ASan
# Needs: git, python3, clang, aarch64-linux-gnu-gcc, qemu-user.
#   lemonadep/xnu/check-overlay.sh [WORKDIR]      (default /tmp/lemonadep-xnu)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
T="$REPO/lemonadep/xnu/test"
WD=${1:-/tmp/lemonadep-xnu}
XNU_TAG=xnu-8796.141.3
mkdir -p "$WD"
[ -d "$WD/xnu-base" ] || git clone -q --depth 1 -b "$XNU_TAG" https://github.com/apple-oss-distributions/xnu.git "$WD/xnu-base"
rm -rf "$WD/xnu" && cp -a "$WD/xnu-base" "$WD/xnu"
cd "$WD/xnu"
skipped_overlay=0
lf=$(mktemp)
for p in "$REPO"/xnu/patches/*.patch "$REPO"/lemonadep/xnu/patches/*.patch; do
	# a Windows checkout (core.autocrlf) has CRLF patch files; CI's has LF
	sed 's/\r$//' "$p" > "$lf"
	if git apply --check "$lf" 2>/dev/null; then
		git apply "$lf" 2>/dev/null
	else
		echo "SKIP $(basename "$p")"
		case "$p" in "$REPO"/lemonadep/*) skipped_overlay=1 ;; esac
	fi
done
# 0031/0041 edit HFS sources that CI's stage-hfs.sh adds; they skip here only.
rm -f "$lf"
[ "$skipped_overlay" = 0 ] || { echo "error: an overlay patch does not apply on top of the sweet series" >&2; exit 1; }
if grep -n '0x9c000000' osfmk/arm64/start.s osfmk/arm64/arm_vm_init.c osfmk/arm64/sweet_early.c; then
	echo "error: sweet framebuffer address still hard-coded" >&2
	exit 1
fi

X="$WD/xnu"
W="$WD/t1001"
rm -rf "$W" && mkdir -p "$W/stub/pexpert/arm" "$W/stub/pexpert/arm64" "$W/stub/kern" "$W/x86"
python3 "$T/gen.py" "$X" "$W"

echo "== start.s blocks: arm64 Mach-O (integrated assembler, as the kernel build)"
clang -target arm64-apple-macos13 -x assembler-with-cpp -c "$W/frag.S" -o "$W/frag-macho.o"
echo "== start.s blocks: run on aarch64 (qemu-user)"
clang -target aarch64-linux-gnu -x assembler-with-cpp -c "$W/frag-elf.S" -o "$W/frag.o"
aarch64-linux-gnu-gcc -O1 -static -o "$W/test_frag" "$T/test_frag.c" "$W/frag.o"
qemu-aarch64 "$W/test_frag"

: > "$W/stub/pexpert/arm/consistent_debug.h"
: > "$W/stub/pexpert/arm/protos.h"
echo '#define SWEET 1' > "$W/stub/pexpert/arm64/board_config.h"
cat > "$W/stub/kern/misc_protos.h" <<'EOF'
#include <stdarg.h>
void _doprnt(const char *fmt, va_list *argp, void (*putc)(char), int radix);
EOF
INC="-I$W/stub -I$X/pexpert -I$X/osfmk -I$W"
CASES="map sweet lemonadep depth24 nofb relocate0"
echo "== C on aarch64 (verbatim; UBSan trap mode)"
aarch64-linux-gnu-gcc -O1 -g -Wall -Wextra -fsanitize=undefined -fsanitize-undefined-trap-on-error $INC \
	-static -o "$W/test_c_arm64" "$T/test_c.c" "$X/osfmk/arm64/sweet_early.c"
for c in $CASES; do qemu-aarch64 "$W/test_c_arm64" "$c"; done
echo "== C on x86 (ASan/UBSan; only the dsb barriers become compiler barriers)"
sed 's/__asm__ volatile("dsb sy" ::: "memory");/__asm__ volatile("" ::: "memory");/' \
	"$X/osfmk/arm64/sweet_early.c" > "$W/x86/sweet_early.c"
sed 's/__asm__ volatile("dsb ishst" ::: "memory");/__asm__ volatile("" ::: "memory");/' \
	"$W/map_final.inc" > "$W/x86/map_final.inc"
clang -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I"$W/x86" $INC \
	-o "$W/test_c" "$T/test_c.c" "$W/x86/sweet_early.c"
for c in $CASES; do "$W/test_c" "$c"; done
echo "overlay check: ok"
