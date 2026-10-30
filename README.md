# ios-lemonadep

**Apple's iOS 16 running on a OnePlus 9 Pro -- standalone, with GPU, touch, Wi-Fi and Safari.**

Power the phone on and iOS 16.5 starts by itself: no computer, no `fastboot boot`. This repository holds the
OnePlus 9 Pro (`lemonadep`, Qualcomm SM8350 / Snapdragon 888) port in two generations:

- **Current: HackiOS-based** (`lemonadep/hackios/`). furangelz's [hackios-xnu-kernel / hackios-xnu-userland](https://github.com/furangelz)
  (XNU 8796 + the unmodified iOS 16.5 user space of the iPhone X), Mu-Silicium UEFI and an XNU loader, plus this
  repository's patches: the phone's Wi-Fi driver, the standalone boot, persistence and the fixes that make iOS's own
  Wi-Fi stack, Safari and location work. Latest release:
  [lemonadep-2026-10-31](https://github.com/popovluka/ios-lemonadep/releases/tag/lemonadep-2026-10-31).
- **Earlier: the ios-sweet shim path** (everything else under `lemonadep/`): the bare-metal shim, a CPU-rendered
  home screen from iOS 16.7 on HFS+, and the GPU bring-up research. Kept for reference.

> Experimental, not a daily-driver OS. Apple's security model is mostly switched off, there is no Bluetooth,
> cellular or camera, and installing it **overwrites the `userdata` partition and the boot partition** (Android
> stops booting until you restore your backups).

## Status (2026-10-31, HackiOS path)

| Works | |
| --- | --- |
| Standalone boot | UEFI + XNU loader + kernel in `boot_b`; the 2.4 GB iOS system image is read from `userdata` at boot |
| Display, GPU | Adreno 660 through Mesa (GL): blur, transparency, smooth animation |
| Touch, buttons, battery, backlight | |
| Wi-Fi | QCA6490 with Linux's ath11k over PCIe; join at boot or from Settings, status bar, DHCP by iOS itself |
| Safari | pages and video |
| Location | Wi-Fi positioning (Weather knows where you are) |
| Your data | `/private/var` saved to flash and restored at boot (icons, settings, dark mode survive) |

| Not working yet | Why (as far as known) |
| --- | --- |
| Sound (speaker, Safari, system sounds) | the audio DSP starts, but nothing is heard: speaker path not brought up on this phone |
| Weather data, Maps tiles | Weather requests are FairPlay-signed (no Secure Enclave); Maps not investigated |
| Bluetooth, cellular, camera | not started |
| Power off with the Power button | use the five-finger hold (1.5 s) or iOS's own power off, which save your data |

## How to reproduce

The full recipe, the root cause of every fix and the debugging tools are in
**[`lemonadep/HANDOFF.md`](lemonadep/HANDOFF.md)** (start there) and
**[`lemonadep/README.md`](lemonadep/README.md#standalone-hackios-boot-no-computer)**. In short:

1. **Back up** every partition first (TWRP / `dd`), at least `boot_a`, `boot_b`, `vendor_boot_*`, `dtbo_*`.
2. **Lab**: a Linux (WSL works) checkout of furangelz's two trees at `~/fkernel`, branch `lemonadep`; apply
   `lemonadep/hackios/patches/*.patch` with `git apply`; build Mu-Silicium's lemonadep UEFI; let
   `ios165/fetch.sh` extract iOS 16.5 (20F66, iPhone10,3) from Apple's IPSW.
3. **Phone files** (never in git): Wi-Fi firmware and board data pulled from your own phone into
   `~/fkernel/fw-wlan/` (`amss.bin`, `m3.bin`, `bdwlan.bin`, `regdb.bin`), and `fw-wlan/network` with your
   network's name and passphrase.
4. **Build**: `common/build.sh lemonadep`, then `bash lemonadep/hackios/build-standalone.sh`.
5. **Install**: phone in TWRP: `lemonadep/hackios/install-standalone.ps1 -SystemImage`; phone in fastboot:
   `install-standalone.ps1 -Boot`. Undo: `fastboot flash boot_b <your boot_b backup>`.

No prebuilt images are published: they would contain Apple's iOS, Qualcomm firmware and Wi-Fi credentials.

## The earlier shim path

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the OnePlus 9 Pro (`lemonadep`, Qualcomm SM8350 / Snapdragon 888). On top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and the stock apps — the same bare-metal shim and patched kernel as [ios-sweet](https://github.com/dipa-studio/ios-sweet), whose original target (the Xiaomi Redmi Note 10 Pro) this repo still fully supports too (see [below](#also-runs-on-the-redmi-note-10-pro-sweet)).

As of **2026-10-23** the phone boots to an **interactive iOS 16 home screen**: touch (Synaptics S3908) and the hardware buttons (PMK8350/PM8350) work, and you can unlock, open Settings, browse the app drawer and pull down Control Center. Rendering still runs entirely on the CPU, so it's slow. A native Adreno 660 GPU driver is being brought up step by step on the real phone; its GMU power-management controller now gets its firmware loaded and every cold-start step short of the final reset release.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**.

### Shim path status

| Milestone | State |
| --- | --- |
| Boot + framebuffer | **confirmed on-device** |
| UFS mounts the iOS root | **confirmed on-device** |
| Lock screen | **confirmed on-device**: time, status bar, slide bar, home indicator all rendered |
| Touch (Synaptics S3908 over TCM, bit-banged I²C) | **confirmed on-device** |
| Buttons (PMK8350/PM8350) + interactive home screen | **confirmed on-device**: Settings, app drawer, Control Center |
| GPU (Adreno 660) | **in progress**: power/clocks, GMU firmware load and the GMU cold start up to its reset release confirmed on-device; GMU memory and SMMU context next ([details](lemonadep/README.md#gpu-bring-up)) |

Full detail, every sourced hardware fact with its citation, and the complete replication guide live in **[`lemonadep/README.md`](lemonadep/README.md)**. This top-level page is the overview; that one is the manual.

## What's different from sweet

Same shim, same kernel patch series, same iPhone X userland — only the hardware bring-up layer is device-specific, and it's expressed as **ADT-flag-driven runtime overrides** read via `SecureDTLookupEntry`/`SecureDTGetProperty`, never compile-time board macros. When lemonadep's ADT node is absent, sweet's original code runs exactly as it did before any of this existed; the two boards share one kernel source tree.

| Area | sweet (Redmi Note 10 Pro, SM7150) | lemonadep (OnePlus 9 Pro, SM8350) |
| --- | --- | --- |
| Touch | Goodix GT9896 over I²C, GENI SE7 | Synaptics S3908, TCM v1/v2 protocol, GENI SE4 |
| Buttons | PM6150 PON / PM6150L GPIO2 | PMK8350 PON_HLOS (gen-3) / PM8350 GPIO6 |
| Panel | 1080×2400 DSC video mode | Samsung AMB670YF01, 1080×2412 command mode |
| CPU | 6× Cortex-A55 (homogeneous) | 4×A55 + 3×A78 + 1×X1 (boots on 1 core so far) |
| GICv3/timer | direct EL1 access | under a hypervisor: `CNTP_CTL_EL0` and `GICR_IPRIORITYR` writes need working around |

## How it boots (shim path)

```
Bootloader (unlocked)
  -> fastboot boot shim-boot.img              (temporary; nothing is flashed to boot)
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU -> UFS driver -> HFS+ root in the userdata partition
              -> dyld + shared cache -> launchd -> daemons -> backboardd + SpringBoard
```

## How to do it yourself (shim path)

Full step-by-step instructions (backup, kernel build via CI, iOS root image, boot image, flash, log pull) are in **[`lemonadep/README.md`](lemonadep/README.md)**. Short version:

```sh
gh workflow run lemonadep.yml -R YOU/ios-lemonadep -f build_kernel=true
gh run download RUN_ID -R YOU/ios-lemonadep -n xnu-kernel-lemonadep -D lemonadep/m2-artifacts/kernel
bash lemonadep/m2-build-macbook.sh            # builds the HFS+ iOS root (needs real Linux, not WSL2)
python3 tools/mkadt.py lemonadep/m2-artifacts/l.adt --board lemonadep --dram 0x100000000 0xfcc00000
bash lemonadep/m2-boot-build.sh lemonadep/m2-artifacts/kernel/xnu-kernel-lemonadep/kernel.development.sweet 1
fastboot -S 512M flash userdata lemonadep/m2-artifacts/ios16-root.simg   # erases Android's data
fastboot boot shim/out/lemonadep/m2-boot.img
```

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm), shared by both boards (`BOARD=lemonadep` or the sweet default) |
| `xnu/patches/` | The base XNU patch series (applied in order by CI), plus staging scripts for libpthread, HFS and BearSSL |
| `lemonadep/` | Everything lemonadep-specific: overlay patches, hardware-fact sourcing, build scripts, CI workflow, and its own [`README.md`](lemonadep/README.md) |
| `tools/mkadt.py` | Generates the Apple Device Tree; `--board lemonadep` selects the OnePlus 9 Pro profile, otherwise sweet's own (byte-identical to before this existed) |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `sweet-root-tweaks.py` | Build the HFS+ iOS root from the IPSW images (shared by both boards) |
| `tools/sweet-log-pull.sh`, `sweet-log.py`, `sweet-crash.py` | Read and symbolicate the persistent kernel log from recovery |
| `.github/workflows/xnu.yml`, `lemonadep.yml` | Kernel CI builds, one per board |
| `NOTES.md` | Detailed sweet engineering log: every boot, crash and decision, plus hardware facts |

Not in git: partition backups, live device trees, recovery images, Apple firmware and root images, and downloaded or generated build trees. They contain device serials and calibration data, or Apple's copyrighted files.

## Kernel patches

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0055 | Diagnostics: persistent log, /dev/null tee, IOKit trace, heap strings and user stack sampler on crash; permissive Sandbox policy; HFS rename-swap; AppleKeyStore selectors |
| 0056–0057 | Throttled panel copy; no external identity resolver |
| 0058 | HID event services. Apple's IOHIDFamily-1915.140.3 user client and event queue, matching the iOS 16.7 userland |
| 0059 | iPhone X identity (`hw.product` / `hw.target` from the device tree) |
| 0060 | Power and volume keys over SPMI; touch over GENI I²C (sweet's own ICs: PM6150/PM6150L, Goodix GT9896) |
| 0061 | Present the display as 1125×2436 |
| 0062 | RPMh regulator vote for the touch rail; battery power source |
| 0063 | GENI FIFO packing fix and the Goodix GT9896 protocol |
| 0064 | Grant PRIV_WORK_INTERVAL (UIKit's CoreAnimation work interval) |
| 0065 | SMP: secondary A55 cores via PSCI CPU_ON |
| 0066–0067 | Fault-region crash info; console mute |
| 0068 | IOSurface lookup semantics for already-mapped surfaces |
| 0069 | Batched persistent-log flushes |
| 0070 | PID hibernate support |
| 0071 | Touch edge-case fixes |
| 0072–0077, 0081 | USB device-mode networking: DWC3 probe, EP0, VBUS override, ECM, RX mbuf handling, live log, link-state reporting |
| 0078 | SMP: all 8 cores (6×A55 + 2×A76) as one homogeneous cluster |
| 0079 | Display power management |
| 0080 | Network packet tracing |
| `lemonadep/xnu/patches/1001` | Early framebuffer address read from `boot_args->Video` instead of hardcoded |
| `lemonadep/xnu/patches/1002` | Hypervisor quirks: skip `CNTP_CTL_EL0`/`GICR_IPRIORITYR` writes when the ADT says to |
| `lemonadep/xnu/patches/1004` | Keys: PMK8350 PON (gen-3) + PM8350 GPIO6, as ADT overrides of sweet's own PM6150/PM6150L driver |
| `lemonadep/xnu/patches/1005` | Touch: Synaptics S3908 over TCM v1/v2, GENI SE4, as an ADT-selected protocol path alongside sweet's Goodix/Focaltech one |

## Key hardware facts (lemonadep)

- SoC: Qualcomm SM8350 (Snapdragon 888), codename `lahaina`.
- Touch: Synaptics S3908, I²C address `0x4B`, GENI SE4 (`i2c@990000`), standard TCM v1/v2 protocol.
- Buttons: PMK8350 PON_HLOS (power, vol-down; gen-3 PON), PM8350 GPIO6 (vol-up).
- Panel: Samsung AMB670YF01, command mode, native 1080×2412 — renders correctly as-is.
- Hypervisor quirks measured on-device, not assumed: EL1 `CNTP_CTL_EL0` writes are undefined, and the hypervisor's virtual GIC faults `GICR_IPRIORITYR` writes.

Every fact above has a full citation (device-tree file:line or kernel driver source) in [`lemonadep/HWMAP_lemonadep.md`](lemonadep/HWMAP_lemonadep.md) — nothing was guessed.

## Also runs on: the Redmi Note 10 Pro (sweet)

This is the device the project started on, and it's further along: as of 2026-10-18 it reaches the **iOS home screen** with working touch, buttons and navigation (not just the lock screen). It shares every piece of the kernel/shim above; only the hardware bring-up differs.

<details>
<summary>sweet status, build steps and hardware facts</summary>

### What works on sweet

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi bootloader → `fastboot boot` of the shim → XNU. Nothing is flashed to the boot partitions |
| CPU | 6 of the 8 cores (the six Cortex-A55s) via PSCI, 16K pages, GICv3 interrupts, timers |
| Memory | About 3.95 GiB of the upper DDR bank |
| Storage | Own UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`) services; CoreAnimation renders in software; frames are copied into the bootloader's framebuffer. Presented to iOS as an iPhone X panel (1125×2436) |
| GUI | backboardd, SpringBoard, lock screen, home screen, Control Center, notification center, Settings |
| Touch | Goodix GT9896 over I²C (GENI), rail enabled through RPMh, delivered as HID digitizer events |
| Buttons | Power, volume up/down (PMIC through SPMI) as HID consumer keys; lock/wake works |
| Battery | IOPMPowerSource service (static 100 %, charging icon; real fuel-gauge readout prepared) |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |
| Activation | Native "Hactivation" path of mobileactivationd, chosen through device-tree security status. No Apple servers involved |

### What does not work on sweet (yet)

- **Navigation:** you cannot leave an app (the home gesture and AssistiveTouch Home do nothing yet).
- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS or SEP / Face ID.** Daemons for them start and fail or keep relaunching.
- **No GPU.** Everything is drawn by the CPU at roughly 20–30 fps. The two Cortex-A76 cores are not used yet.
- **Few apps.** The image has only what is on the system volume. There is no App Store, no Apple ID, and some Settings pages are empty.
- **Slow boot:** about 4 minutes on the Apple logo.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection. Do not put anything personal on it.

### How to do it yourself (sweet)

You need a Redmi Note 10 Pro (`sweet`, global or Indian variant, SM7150) with an **unlocked bootloader**, a Linux PC, and a GitHub account for the kernel build. This is the exact path used for the original screenshots; other variants or firmware may need changes. **Everything on `userdata` is lost.** Bootloaders, recovery, modem and persist partitions are never touched.

#### 0. Back up the phone

Boot OrangeFox (or any recovery with adb root) temporarily and pull at least `persist`, `modemst1`, `modemst2`, `fsg`, `fsc` and `boot`, `dtbo`, `vbmeta`. They contain per-device calibration data. Keep them off git; `.gitignore` already excludes `backup/`, `dtb/` and `recovery/`.

The shim needs the stock DTB. Unpack your phone's stock `boot.img`:

```sh
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked   # -> dtb/boot_unpacked/dtb
```

#### 1. Build the kernel

The kernel builds on GitHub's macOS runners with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). The workflow fetches xnu-8796, applies `xnu/patches/` in order, and also stages the pinned open-source libpthread, HFS and BearSSL sources it needs. Fork the repo (public repos get free macOS minutes) and run:

```sh
gh workflow run xnu.yml -R YOU/ios-lemonadep
gh run download RUN_ID -R YOU/ios-lemonadep -n xnu-kernel -D xnu/out
```

To check a patch locally before spending a CI run, `tools/xnu-cc.py` replays the compile commands of a previous CI log for single files.

#### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple. Then, on Linux, with `apfs-fuse`, `hfsprogs` and sudo:

```sh
# OS, SystemOS and AppOS disk images from the IPSW (see BuildManifest.plist for their names)
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo fsck.hfsplus -fy ios16-root.img
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

`mkiosroot.sh` copies the OS volume and both cryptexes into one case-sensitive journaled HFS+ volume. It adds the dyld cache links, the plain system keybag and the log directories the daemons expect. `sweet-root-tweaks.py` applies the bring-up changes: it skips Setup, seeds the activation state, removes the baseband daemon, disables the watchdogs that kill slow apps and the crash-looping media paths, and so on. The script's header explains each change.

The device tree builder copies `/product` from the real iPhone X device tree, which MobileGestalt needs. Extract it once, without downloading the whole IPSW:

```sh
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

#### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and `mkbootimg`.

```sh
python3 tools/mkadt.py shim/out/sweet.adt --highmem
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/sweet.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=6 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30'
shim/build.sh ../shim/out/payload.bin      # -> shim/out/shim-boot.img
```

What the less obvious boot-args do:

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on the `userdata` partition, writable |
| `cpus=6` | Start the six A55 cores; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetfb_noconsole=1` | Stop drawing kernel text over the GUI. It still goes to the persistent log; panics are drawn |
| `sweetfb_hz=30` | Panel refresh rate of the CPU copy |
| `sweettouch=0`, `sweetkeys=0` | Disable the touch or button driver |

#### 4. Flash the root and boot

```sh
fastboot -S 512M flash userdata ios16-root.simg   # ~7 minutes; do not unplug
# Xiaomi's fastboot often hangs after a large flash: re-enter fastboot with Power + Volume down
fastboot boot shim/out/shim-boot.img
```

The first boot takes about 4–5 minutes on the Apple logo, then the lock screen appears. Swipe up to unlock. To get back to fastboot, hold Power + Volume down.

#### 5. Read the kernel log

```sh
fastboot boot recovery/recovery.img        # OrangeFox, temporary
tools/sweet-log-pull.sh logs/boot.txt      # 4 MiB console ring from the end of userdata
adb reboot bootloader
```

The log contains crash reports with backtraces (`CRASH name ...`), IOKit traces of backboardd and SpringBoard, and the drivers' `SWEET ...` lines. `tools/sweet-crash.py` symbolicates them against the shared cache.

#### QEMU

`tools/qemu-sweet.py` boots the same kernel on QEMU `virt` with a test RAM-disk root. It is useful for kernel-only changes; see `NOTES.md`.

### Key hardware facts (sweet)

- RAM: payloads use the upper bank, `0x100000000` + `0xfcc00000` (about 3.95 GiB), above Qualcomm's protected carve-outs.
- GICv3 with security extensions: the timer goes through Group 1 IRQ (virtual timer INTID 19), not FIQ as on Apple silicon.
- PSCI through `smc`, 19.2 MHz timer, ARMv8.2 without pointer authentication. MPIDR uses Aff1 for the core number.
- Panel: 1080×2400 DSC video mode, continuous splash from ABL, no refresh kick needed.
- Touch: Goodix GT9896 at I²C `0x5d` on QUP SE7 (`0xa84000`), powered by PM6150L LDO11 through RPMh.
- Buttons: power and volume down on the PM6150 PON block, volume up on PM6150L GPIO2 (SPMI SID 4).

More in [`NOTES.md`](NOTES.md).

</details>

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi/OnePlus's published Linux kernel sources for the hardware details, and [OrangeFox Recovery](https://orangefox.download). Forked from [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the original Redmi Note 10 Pro bring-up. iOS itself is Apple's and is not distributed here.
