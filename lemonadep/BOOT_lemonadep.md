# lemonadep boot path (STEP 2)

Sources are the same as in `HWMAP_lemonadep.md`. **LOS-C** = `LineageOS/android_device_oneplus_sm8350-common` @ `8cf6c3d`, file `BoardConfigCommon.mk`.

## Facts from source

| Item | Value | Source | Conf |
| --- | --- | --- | --- |
| Boot image header | **version 3** (`BOARD_BOOT_HEADER_VERSION := 3`, passed as `--header_version`) | LOS-C `BoardConfigCommon.mk:68,86` | M (LineageOS build; stock OxygenOS is expected to match because it launched on the same Android 11 base, but check by unpacking a stock `boot.img`) |
| Page size | 4096 | LOS-C `:84` | M |
| Kernel base | `0x00000000` | LOS-C `:69` | M |
| Kernel image | `Image` (uncompressed arm64 Image) | LOS-C `BOARD_KERNEL_IMAGE_NAME` | M |
| Ramdisk compression | LZ4 (Android's own ramdisk; ABL does not care about our payload's format) | LOS-C `:87` | M |
| DTB | not in `boot.img`. With header ≥ 3, `BOARD_INCLUDE_DTB_IN_BOOTIMG` puts it in **`vendor_boot`** | LOS-C `:131` + AOSP v3 layout | M |
| DTBO | separate `dtbo` partition (`BOARD_KERNEL_SEPARATED_DTBO`), 24 MiB | LOS-C `:85,110` | M |
| Recovery | no recovery partition; recovery resources live in `vendor_boot` (`BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT`) | LOS-C `:132` | M |
| A/B | yes (`AB_OTA_UPDATER`); A/B partitions include boot, dtbo, vendor_boot, vbmeta* | LOS-C `:10-24` | M |
| Partition sizes | boot 192 MiB, vendor_boot 192 MiB | LOS-C `:109,112` | M |
| Stock cmdline (LineageOS) | `androidboot.hardware=qcom … androidboot.usbcontroller=a600000.dwc3 … swiotlb=0 …` | LOS-C `:70-83` | M |
| Fastboot key combo | Volume Up + Volume Down + Power (device off) | LineageOS wiki, lemonadep install page (2026-10-21) | M |
| LineageOS install method | flashes `dtbo`, `vbmeta`, `vendor_boot`, then **`fastboot flash boot boot.img`**. The wiki never uses `fastboot boot` | same wiki page | M |

## Measured on the target phone (`fastboot getvar all`, 2026-10-22)

The full output is in `backup/getvar-all.txt` (git-ignored: it contains the serial).

| Variable | Value | Meaning |
| --- | --- | --- |
| `product` / `variant` | `lahaina` / `SM_ UFS` | SM8350, UFS storage |
| `unlocked` / `secure` | `yes` / `yes` | bootloader unlocked: `fastboot boot` of unsigned images should be allowed (D1 still to test) |
| `is-userspace` | `no` | this is ABL's fastboot, not fastbootd |
| `current-slot` | `b` | both slots `successful:yes`, `unbootable:no` |
| `partition-size:boot_a/_b`, `vendor_boot_a/_b` | `0xC000000` (192 MiB) | matches LOS-C `BOARD_BOOTIMAGE_PARTITION_SIZE` |
| `partition-size:dtbo_a/_b` | `0x1800000` (24 MiB) | matches LOS-C |
| `partition-type:userdata` / size | `f2fs` / `0x1986FB3000` (102.1 GiB) | the 16 GiB iOS root image fits; the log ring sits at the end |
| `logical-block-size` / `erase-block-size` | `0x1000` | 4 KiB UFS blocks, as the UFS driver assumes |
| `max-download-size` | `0x30000000` (768 MiB) | `fastboot -S 512M` for the root image stays below it |
| `kernel` | `uefi` | ABL is the UEFI-based Qualcomm bootloader |
| `version-bootloader`, `version-baseband` | empty | not reported by this ABL |
| calibration partitions present | `modemst1`, `modemst2`, `fsg`, `fsc`, `persist_bkp`, `oplusreserve1-5` | back these up (checklist 0.5) |

## Header v3 layout vs what `shim/build.sh` makes today

sweet makes a v2 image:
- `--dtb` embedded, `--base 0` with explicit kernel/ramdisk/tags/dtb offsets.

Header v3 (AOSP `mkbootimg`):
- `boot.img` holds **only kernel + ramdisk + cmdline + os_version/patch level**.
- Page size is fixed at 4096.
- There are no load addresses: those, the DTB and the vendor ramdisk come from `vendor_boot`.
- So `--dtb`, `--base` and `--*_offset` are dropped or ignored for v3.

lemonadep packaging therefore becomes:

```
mkbootimg --header_version 3 --kernel out/Image --ramdisk out/ramdisk.bin \
          --os_version <match stock> --os_patch_level <match stock> \
          --cmdline "console=null" -o out/shim-boot-lemonadep.img
```

`os_version`/`os_patch_level` must not be *older* than the device's rollback state, or ABL may refuse the image. Copy them from the stock boot.img (TODO(lemonadep): unpack the user's stock boot.img once; `unpack_bootv2.py` needs v3 support).

## How the shim is delivered

1. **Preferred: `fastboot boot shim-boot-lemonadep.img`** (temporary, nothing flashed). ABL loads our kernel + ramdisk and combines them with the **current slot's flashed `vendor_boot`** (DTB + vendor ramdisk) and `dtbo`.
   - Whether the OnePlus ABL accepts `fastboot boot` on this device/firmware is **TODO(lemonadep)**. There is no source evidence either way, and LineageOS avoids it. It is test #1 on the device.
2. **Fallback: flash to the *inactive* slot's `boot`** (`fastboot flash boot_b …`), then `fastboot --set-active=b` and reboot.
   - Slot A keeps a working Android/recovery. Switch back with `--set-active=a`.
   - This is destructive to slot B's boot only, and needs explicit user approval per AGENTS.md.
   - vbmeta: boot is verified through vbmeta. An unlocked bootloader only warns (orange state), so no vbmeta change is expected, but **TODO(lemonadep)** confirm.

## Consequences for the shim (all go into STEP 3)

1. **The ramdisk is concatenated.** With v3, ABL hands Linux one initrd = **vendor_boot ramdisk(s) followed by the boot ramdisk**. `/chosen linux,initrd-start` therefore points at the vendor ramdisk, not at our `SWEETXNU` payload. `loader.c` must scan `[initrd-start, initrd-end)` for the 8-byte magic. `mkpayload.py` already 16-byte-aligns the payload, so the scan can step 16 bytes and must check the header version and sizes after a hit.
   - Expected size cost: the vendor ramdisk is a few MiB plus a recovery ramdisk under `BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT`.
   - This must stay below the kernel RAM window. The loader's overlap check stays.
2. **DTB comes from vendor_boot + dtbo**, not from our image. Good: the live FDT in `x0` is the real lemonadep tree with the overlay applied. It is the source for `/memory`, for confirming the splash `reg`, and for the panel name in `/chosen/bootargs`.
3. **Load address.** ABL picks it. The shim is position-independent and declares "place anywhere" (`start.S` flags `0xa`), so nothing changes. Record the actual address (`adr x0, _head`) on the first boot.
4. **Kernel cmdline.** ABL appends its own arguments (`androidboot.*`, `msm_drm.dsi_display0=…`). The shim ignores the cmdline; our boot-args travel in the payload.

## Getting the kernel log back (persistent log, patch 0047)

`tools/sweet-log-pull.sh` needs a recovery with **adb root** and `/dev/block/by-name/userdata`.
- On lemonadep, recovery lives in `vendor_boot` + `boot`, so it is reached by booting a recovery `boot.img`. Example: Lineage Recovery's `boot.img` via `fastboot boot`, or flashed to the inactive slot, with the matching `vendor_boot` already on the device.
- Whether Lineage Recovery gives adb root shell access with `dd` on userdata is **TODO(lemonadep)**.
- Note: once the iOS image is in `userdata`, Android on slot A will not boot (userdata is no longer f2fs/FBE). Recovery still works.

## Open items (all on-device, none from source)

| # | Question | How |
| --- | --- | --- |
| B1 | Does ABL accept `fastboot boot` of a v3 image? | `fastboot boot` the unmodified shim (diagnostic mode) |
| B2 | Stock header version / os_version / patch level | `fastboot`-free: unpack the stock `boot.img` from the OTA payload |
| B3 | Entry EL, load address, `x0` DTB magic | shim prints them |
| B4 | Is the vendor ramdisk prepended (initrd layout)? | shim prints initrd range + offset of `SWEETXNU` |
| B5 | Live `/memory` and splash `reg` | shim prints them (needs `fdt.c` support) |
| B6 | Recovery with adb root for log pull | boot Lineage Recovery, `adb shell id` |
