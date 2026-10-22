# OnePlus 9 Pro (lemonadep) port — build notes

Work in progress. Sweet stays the default everywhere. Read in order:

1. `DEVICE_SURFACE.md`: what in ios-sweet is sweet-specific
2. `HWMAP_lemonadep.md`: lemonadep hardware facts with sources
3. `BOOT_lemonadep.md`: boot image v3 / A/B delivery
4. `OVERLAY_PATCHES.md`: kernel changes still to write
5. `GAPS.md`: status, risks, on-device checklist

## Pieces

| Piece | lemonadep form |
| --- | --- |
| ADT | `tools/mkadt.py OUT --board lemonadep --dram BASE SIZE` (the sweet output is byte-identical to before) |
| Shim | `BOARD=lemonadep shim/build.sh` → `shim/out/lemonadep/` (`shim/board.h`, `shim/display.c`) |
| Boot image | header v3; `OS_VERSION`/`OS_PATCH_LEVEL` must be copied from the stock boot.img |
| Kernel | sweet series + `lemonadep/xnu/patches/` overlay (1001 so far; check with `lemonadep/xnu/check-overlay.sh`) |
| CI | `.github/workflows/lemonadep.yml` (shim + host tests on push; kernel on dispatch) |
| Host tests | `shim/host_tests.sh` (payload scan, FDT walk, ADT patch; ASan/UBSan) |

## Build (Linux / WSL)

Needs `aarch64-linux-gnu-gcc`, `dtc`, `cpio` and AOSP mkbootimg. Ubuntu 24.04's `mkbootimg` package is broken: use the AOSP source.

```sh
git clone -b android-14.0.0_r1 https://android.googlesource.com/platform/system/tools/mkbootimg /opt/aosp-mkbootimg
export MKBOOTIMG="python3 /opt/aosp-mkbootimg/mkbootimg.py"

# 0. stock header values (once): unpack the stock boot.img from the OTA
python3 /opt/aosp-mkbootimg/unpack_bootimg.py --boot_img boot.img --out stock_boot   # prints os version / patch level

# 1. diagnostic shim (no payload): milestone 1
BOARD=lemonadep OS_VERSION=<stock> OS_PATCH_LEVEL=<stock> shim/build.sh

# 2. with a kernel (after overlay 1001 exists and the shim reported /memory)
python3 tools/mkadt.py shim/out/lemonadep/l.adt --board lemonadep --dram <BASE> <SIZE>
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/lemonadep/l.adt shim/out/lemonadep/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 sweettouch=0 sweetkeys=0 sweetusb=0 wdt=-1 wdt_test=no_user_checkin'
BOARD=lemonadep OS_VERSION=<stock> OS_PATCH_LEVEL=<stock> shim/build.sh shim/out/lemonadep/payload.bin
```

Pick `<BASE> <SIZE>` inside a `MEM` bank the diagnostic shim printed, above 4 GiB, and start small (1 GiB). The shim refuses to start the kernel when the range is outside the live `/memory` or overlaps a `no-map` carve-out.
