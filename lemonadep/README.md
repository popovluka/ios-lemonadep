# OnePlus 9 Pro (lemonadep) port

**Porting ios-sweet's iOS 16 bring-up from the Redmi Note 10 Pro to the OnePlus 9 Pro** (codename `lemonadep`, Qualcomm SM8350 / Snapdragon 888). Same shim, same patched XNU kernel, same unmodified iPhone X (`iPhone10,3`/`D22AP`) userland as the main [`README.md`](../README.md) — only the hardware bring-up layer (ADT values, MMIO addresses, drivers) is device-specific, expressed as runtime overrides so the original sweet code path stays byte-for-byte unchanged when they're absent.

> Same caveats as sweet: experimental, not a usable phone OS, no GPU acceleration yet (bring-up in progress, see below), Apple's security model is mostly off, and it installs into `userdata` (**erases Android's user data**). This document reflects the state as of **2026-10-23**; read it with a critical eye against the actual code and the sourced facts in `HWMAP_lemonadep.md` before relying on anything here.

## Status

| Milestone | State |
| --- | --- |
| M1: boot + framebuffer | **confirmed on-device** — shim and kernel console visible, boots to the BSD root-mount retry loop |
| M2: UFS mounts the iOS root | **confirmed on-device** — iOS root built and flashed to `userdata`, mounted read-write |
| M3: lock screen | **confirmed on-device** — boots through to the rendered iOS 16 lock screen (time, status bar, slide-to-unlock, home indicator) |
| M4: touch (Synaptics S3908 over TCM) | **confirmed on-device** — bit-banged I²C on gpio20/21 (SE4 is GSI-only), unlock, scroll, swipe gestures |
| M5: buttons (PMK8350/PM8350 keys) + interactive home screen | **confirmed on-device** — power/volume, home screen, Settings, app drawer, Control Center (slow: software rendering) |
| M6: GPU (Adreno 660) | **in progress** — power/clocks, GMU firmware load and every GMU cold-start step up to the reset release confirmed on-device (see [GPU bring-up](#gpu-bring-up)) |

Known not to work: the Settings → General → About page hangs (it asks for hardware details the ADT doesn't provide yet). Everything is CPU-rendered, so the UI is slow.

**Prebuilt kernels** are on the [releases page](https://github.com/popovluka/ios-lemonadep/releases). Boot images that enable the GPU are **not** published: they embed Qualcomm's GMU firmware, which you pull from your own phone (`lemonadep/fw-gpu/pull.sh`) and must not redistribute.

## What's different from sweet

| Area | sweet (Redmi Note 10 Pro, SM7150) | lemonadep (OnePlus 9 Pro, SM8350) |
| --- | --- | --- |
| Touch | Goodix GT9896 over I²C, GENI SE7 | Synaptics S3908, **TCM v1/v2 protocol**, GENI SE4 |
| Buttons | PM6150 PON (power/vol-down), PM6150L GPIO2 (vol-up) | PMK8350 PON_HLOS (gen-3, different RT_STS bit positions), PM8350 GPIO6 |
| Panel | 1080×2400 DSC video mode | Samsung AMB670YF01, 1080×2412 command mode (native, no shim geometry fix needed) |
| CPU | 6 Cortex-A55 (homogeneous) | 4×Cortex-A55 + 3×Cortex-A78 + 1×Cortex-X1 (`cpus=4` confirmed; more cores untested) |
| GPU | Adreno 618 (not attempted) | Adreno 660 + GMU; native bring-up driver in `lemonadep/xnu/gpu/` (overlay 1007) |
| GICv3/timer | straightforward EL1 access | **under a hypervisor**: `CNTP_CTL_EL0` writes are undefined and the virtual GIC faults `GICR_IPRIORITYR` writes — both skipped via ADT flags |

The mechanism is the same throughout: sweet's existing drivers gained ADT-flag-driven runtime overrides (read via `SecureDTLookupEntry`/`SecureDTGetProperty`), never compile-time board macros. When lemonadep's ADT node is absent, sweet's code runs exactly as it did before any of this existed.

## Repository layout (lemonadep-specific)

| Path | What |
| --- | --- |
| `lemonadep/HWMAP_lemonadep.md` | Every hardware fact used here, with its exact source (device tree file:line, driver source, or "measured on the target phone") and a confidence rating. Read this before trusting any address in the code |
| `lemonadep/GAPS.md` | Milestone tracker, known risks, on-device test checklist |
| `lemonadep/HANDOFF.md` | Running engineering log: every boot, build, and CI run with commit hashes and checksums |
| `lemonadep/BOOT_lemonadep.md` | Boot image header / A-B partition layout facts |
| `lemonadep/DEVICE_SURFACE.md` | What in ios-sweet's shared code is sweet-specific vs. generic |
| `lemonadep/OVERLAY_PATCHES.md` | Design notes for each overlay patch |
| `lemonadep/xnu/patches/100*.patch` | The overlay patch series, applied after `xnu/patches/` (sweet's own series) |
| `lemonadep/xnu/check-overlay.sh` | Local gate: applies the overlay series to a fresh xnu clone and runs every host test, no macOS needed |
| `lemonadep/xnu/test/` | Host-side ASan/UBSan (+ aarch64/qemu) tests that extract real overlay code verbatim and run it against mocks |
| `lemonadep/m1b-build.sh`, `m2-boot-build.sh` | Build the shim+kernel boot image for each milestone |
| `lemonadep/m3-gpu-build.sh` | Boot image for GPU bring-up: packs your GMU firmware and a reserved GPU memory region, `SWEETGPU=N` picks the stage |
| `lemonadep/xnu/gpu/` | The GPU driver sources (host-tested here, injected into xnu by overlay 1007), their tests, and `gen-1007.py` which generates the patch |
| `lemonadep/fw-gpu/pull.sh` | Pulls the Adreno 660 firmware from your phone (gitignored, never redistributed) |
| `lemonadep/m2-build-macbook.sh` | Builds the HFS+ iOS root image on real Linux (not WSL2 — it has no HFS+ kernel support at all) |
| `tools/mkadt.py --board lemonadep` | Generates lemonadep's Apple Device Tree (sweet's own output is unaffected — see the function-level comments) |
| `.github/workflows/lemonadep.yml` | CI: shim + host tests on every push; the macOS kernel build on a `[kernel]`-tagged commit or manual dispatch |

## Overlay patches

| # | What |
| --- | --- |
| 1001 | Early framebuffer address read from `boot_args->Video` instead of hardcoded |
| 1002 | Hypervisor quirks: skip the `CNTP_CTL_EL0` write and the `GICR_IPRIORITYR` writes when the ADT says to |
| 1004 | Keys: PMK8350 PON (gen-3 RT_STS bits) + PM8350 GPIO6, as ADT overrides of sweet's PM6150/PM6150L driver |
| 1005 | Touch: Synaptics S3908 over TCM v1/v2, bit-banged I²C on gpio20/21 (SE4 is GSI-only), as an ADT-selected protocol path alongside sweet's existing Goodix/Focaltech one |
| 1006 | Touch: scale the S3908's own 1440×3216 coordinate range onto the panel (`sweet_hid_touch_extents`) |
| 1007 | GPU: Adreno 660 power/clocks, GMU cold start, GMU memory + SMMU context — generated by `lemonadep/xnu/gpu/gen-1007.py`, off unless `sweetgpu=N` |

## How to do it yourself

You need an **unlocked** OnePlus 9 Pro (`lemonadep`, SM8350; this was done against the LE2123 global variant — other regional variants may differ), a Linux machine (real Linux — WSL2 has no HFS+ support), and a GitHub account (fork this repo; public repos get free CI minutes on every runner type, macOS included).

**Everything on `userdata` is erased.** Bootloaders, recovery, modem, and the calibration partitions (`persist`, `modemst1/2`, `fsg`, `fsc`) are never touched by anything here — only `fastboot boot` (temporary, RAM-only) until the one deliberate `fastboot flash userdata` step below.

### 0. Back up the phone

Before anything else, back up every partition you care about, especially `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot_a/b`, `dtbo_a/b`, `vbmeta*`, `vendor_boot_a/b`. These hold per-device calibration data (IMEI, Wi-Fi/BT MAC, etc.) that cannot be regenerated. Use an adb-root-capable recovery (flashed temporarily to `boot` via `fastboot boot`, never installed) — see `GAPS.md` section 4 for the exact checklist.

### 1. Build the kernel

Same CI as sweet, different workflow:

```sh
gh workflow run lemonadep.yml -R YOU/ios-sweet -f build_kernel=true
gh run list -R YOU/ios-sweet --workflow=lemonadep.yml -L1          # get the run id
gh run download RUN_ID -R YOU/ios-sweet -n xnu-kernel-lemonadep -D lemonadep/m2-artifacts/kernel
```

Before spending a CI run on a kernel-touching change, run `lemonadep/xnu/check-overlay.sh` locally (needs `git`, `python3`, `clang`, `aarch64-linux-gnu-gcc`, `qemu-user`) — it applies the whole patch series to a fresh xnu clone and runs every host test, including a standalone test of the touch protocol framing, without ever needing a macOS compiler.

### 2. Build the iOS root image

Same process as sweet's top-level README section 2 (`mkiosroot.sh` + `sweet-root-tweaks.py`), but it needs a kernel with **HFS+ filesystem support**, which rules out WSL2 entirely (its kernel ships with none, not even as a module). `lemonadep/m2-build-macbook.sh` automates the whole thing — despite the name, any real Linux box works, not just a Mac:

```sh
bash lemonadep/m2-build-macbook.sh
```

It installs `apfs-fuse` (built from source — no distro package exists), `hfsprogs`, `img2simg`, and `ipsw`, downloads the iPhone X 16.7.16 (20H392) IPSW, extracts the D22AP device tree, and produces `lemonadep/m2-artifacts/ios16-root.simg`.

### 3. Build the boot image

```sh
python3 tools/mkadt.py lemonadep/m2-artifacts/l.adt --board lemonadep --dram 0x100000000 0xfcc00000
bash lemonadep/m2-boot-build.sh lemonadep/m2-artifacts/kernel/xnu-kernel-lemonadep/kernel.development.sweet 1
```

The second argument to `m2-boot-build.sh` is the CPU count; start at `1` (only single-core boot has been confirmed on lemonadep so far — SMP across the A55/A78/X1 cluster is untested here, unlike sweet's homogeneous 6×A55). It produces `shim/out/lemonadep/m2-boot.img` with `sweettouch=1 sweetkeys=1` (edit the script's `BOOTARGS` to `=0` to isolate a driver while debugging).

### 4. Flash the root and boot

```sh
fastboot -S 512M flash userdata lemonadep/m2-artifacts/ios16-root.simg   # erases Android's data
fastboot boot shim/out/lemonadep/m2-boot.img
```

Expect roughly 4 minutes on the Apple logo (matches sweet's own documented boot time), then the lock screen. To get back to fastboot, hold Power + Volume down.

### 5. Read the kernel log

No filming the screen: the kernel keeps a 4 MiB console ring in the last blocks of `userdata` (sweet patch 0047), and you read it back through TWRP (temporarily `fastboot boot`ed, never flashed). This works with TWRP 3.7.1 for lemonadep (`twrp-3.7.1_12-0-lemonadep.img`):

```sh
fastboot boot twrp-3.7.1_12-0-lemonadep.img      # wait for `adb shell true` to succeed
tools/sweet-log-pull.sh logs/lemonadep-boot.txt
adb reboot bootloader                             # back to fastboot, no buttons needed
```

Two things that bite:

- The ring is only written when the boot has **`ufsrw=1`**. A read-only boot leaves the previous boot's log in place, and you'll be reading a stale one.
- On Windows Git Bash, set `MSYS_NO_PATHCONV=1` first, or `/dev/block/...` paths get rewritten and the pull reads 0 bytes.

Look for `SWEET KEYS:`, `SWEET TOUCH:` and `SWEET GPU:` lines. After a test, holding Power + Volume down gets back to fastboot.

## GPU bring-up

The Adreno 660 has a GMU, a small Cortex-M3 that owns GPU power and has to be cold-started with Qualcomm's firmware before the GPU does anything. There is no existing XNU driver for any Adreno to build on. We checked UniXNU, UniXNU-sweet, ios-guacamole, ios-topaz and ios-sailfish: the working ones are a GMU-less generation (Adreno 5xx) or a different chip. So `lemonadep/xnu/gpu/` is a native driver: register sequences sourced from Linux's `drivers/gpu/drm/msm/adreno/a6xx_gmu.c` (pinned, sha256-verified) and mainline `sm8350.dtsi`, every piece host-tested against mock hardware before it runs on the phone.

It's built in stages selected by the `sweetgpu=N` boot-arg, so each step can be tried on real hardware without a new kernel build:

| `sweetgpu` | Adds | On-device |
| --- | --- | --- |
| 1 | CX GDSC + GMU clocks, RSC/PDC handshake | **ok** |
| 2 | GMU firmware into ITCM/DTCM | **ok** |
| 3 | `fw_start`: HFI table pointer, fence range, chip ID, log buffer | **ok** |
| 4 | GBIF QoS (first writes into the GPU register window) | **ok** |
| 5–6 | `power_config`, GMU start probe | **ok** |
| 7 | GMU memory (HFI queue table, firmware icache block) + read-only SMMU probe | next |
| 8 | GMU SMMU context: program, attach SIDs 5/0x405, map icache/dummy/debug/log/hfi | next |
| 9+ | Release the GMU from reset, wait for it to report ready | next |

The boot reaches a working home screen with touch and keys at every stage that's been run.

```sh
lemonadep/fw-gpu/pull.sh                                       # once, with the phone in TWRP: firmware from its vendor partition
SWEETGPU=7 bash lemonadep/m3-gpu-build.sh KERNEL 4             # then fastboot boot shim/out/lemonadep/m3-gpu-boot.img
```

Findings so far, each one written up with sources in `lemonadep/HANDOFF.md`:

- **The RSC wake-poll must not run on a first boot.** Linux's `a6xx_rpmh_start()` only wakes the RSC if a suspend put it to sleep (`GMU_STATUS_PDC_SLEEP`). Our first version always ran it, and on real silicon it timed out.
- **Releasing the GMU from reset without its memory faults the whole SoC** into an XBL ramdump. The firmware's main 54 KiB block lives at GMU address 0x4000, which the GMU reaches through the Adreno SMMU. Stages 7–8 put it there and map it, in a physically contiguous region the shim reserves (`mkpayload --extra-blob gpumem:zero:SIZE`: no boot-image growth).
- **The Adreno SMMU is `dma-coherent`, and the GMU's stream is `<5 0x400>`** (SIDs 5 and 0x405), from mainline `sm8350.dtsi`. The SMMU kit only installs exact stream matches, so both SIDs are attached.

What comes after the GMU starts: the HFI handshake, then the GPU's own SMMU context and CP ring, then the first real command (`CP_ME_INIT`) completing on a fence. That code (`lemonadep_ring.c`, `lemonadep_cp_init.c`, `lemonadep_gpu_boot.c`) is written and passes its host tests, but hasn't run on the phone yet. Metal is further out still: it needs a userspace driver (Mesa Turnip) and a Metal-to-Vulkan layer on top of this kernel driver.

## Key hardware facts

- SoC: Qualcomm SM8350 (Snapdragon 888), codename `lahaina`.
- Touch: Synaptics S3908, I²C address `0x4B`, GENI SE4 (`i2c@990000`), standard TCM v1/v2 protocol — not Goodix.
- Buttons: PMK8350 PON_HLOS (power, vol-down; gen-3 PON, different RT_STS bit positions than sweet's gen-2 PM6150), PM8350 GPIO6 (vol-up).
- Panel: Samsung AMB670YF01, command mode, native 1080×2412 — renders correctly as-is, no geometry fix needed (unlike what was originally expected).
- Hypervisor quirks: EL1 `CNTP_CTL_EL0` writes are undefined, and the hypervisor's virtual GIC faults `GICR_IPRIORITYR` writes — both measured on-device via the M1a shim runs, not assumed.
- RAM: same upper-DDR-bank window as sweet proved for `--highmem` (`0x100000000` + `0xfcc00000`), confirmed against the live FDT.
- GPU: Adreno 660 (A6XX gen 4, not A7XX). GMU windows at `0x03d6a000` (core), `0x03de0000` (RSCC), `0x0b290000` (PDC); GPU registers at `0x03d00000`; Adreno SMMU-500 at `0x03da0000` (`dma-coherent`); GMU stream `<5 0x400>`, GPU streams `<0 0x400>, <1 0x400>`. Sources in `HWMAP_lemonadep.md` section 8 and `lemonadep/xnu/gpu/LemonadepGmuSource.h`.

Every one of these has a full citation (device-tree file:line or kernel driver source) in `HWMAP_lemonadep.md` — nothing here was guessed.

## Credits

Builds directly on [ios-sweet](../README.md)'s shim, kernel patch series, and iOS root tooling. See the top-level README's credits for the upstream pieces (darwin-xnu-build, Apple's open-source releases, etc.) this still depends on.
