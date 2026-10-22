# lemonadep hardware map (STEP 2)

OnePlus 9 Pro (`lemonadep`, project 19815), Qualcomm SM8350 "lahaina".
Every value is from a source file. Values without a source are `TODO(lemonadep)`,
never guessed.

## Sources (read-only, pinned)

| Tag | Repository @ commit | Notes |
| --- | --- | --- |
| **OP** | `OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8350` @ `d95833d` (branch `oneplus/sm8350_u_14.0.0_oneplus9`) | Vendor DT. Paths below are relative to `vendor/qcom/proprietary/` |
| **ML** | `torvalds/linux` tag `v6.17` (`e5f0a69`) | `arch/arm64/boot/dts/qcom/sm8350.dtsi`, `pm8350.dtsi`, `pmk8350.dtsi` |
| **LOS-C** | `LineageOS/android_device_oneplus_sm8350-common` @ `8cf6c3d` (lineage-23.2) | Build/boot config |
| **LOS-D** | `LineageOS/android_device_oneplus_lemonadep` @ `07728a7` (lineage-23.2) | |
| LOS-K | `LineageOS/android_kernel_oneplus_sm8350` @ `187d13c` (lineage-23.2) | Same vendor DT tree under `arch/arm64/boot/dts/vendor/`; not cited separately |
| OP-K | `OnePlusOSS/android_kernel_oneplus_sm8350` @ `9e38a9f` | No DT in this repo |
| **ML-GPU** | `torvalds/linux`, default branch, fetched 2026-10-23 (not pinned to the `ML` tag's `v6.17` -- `drivers/gpu/drm/msm/adreno/` moves fast; re-fetch and diff before relying on an exact line number) | `a6xx_catalog.c`, `a6xx_gpu.c` |
| **UniXNU** | `github.com/UniXNU/{UniXNU,NativeAdreno,NativeMetal}`, default branch, cloned 2026-10-23 | `NativeAdrenoABI.h`, `patches/mesa/turnip-darwin-kmd.patch`, `scripts/data/adreno-compiler-inputs.json` |

**How the DT is put together.** ABL merges a base DTB with the dtbo overlay for this board:
- base = `devicetree/oplus/lemonadev/lahaina{,-v2,-v2.1}.dts` → `devicetree/qcom/lahaina.dtsi`
- overlay = `devicetree/oplus/lemonadev/lemonadep-19815-overlay.dts`, which includes `qcom/lahaina-mtp-overlay.dts` (→ `lahaina-mtp.dtsi`), `lahaina_overlay_oplus_common.dtsi`, `lemonade_common.dtsi` and `lemonadep-19815.dtsi`

The overlay overrides the base. The table cites the node that wins.

Confidence:
- **H**: vendor DT and mainline agree.
- **M**: one DT source only, or derived from DT by a documented rule.
- **L**: indirect evidence.
- **TODO**: must be measured on the device.

---

## 1. Memory

### DRAM banks

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| `/memory` reg | **placeholder `<0 0 0 0>`; ABL fills it in at boot** | OP `devicetree/qcom/lahaina.dtsi:40`; ML `sm8350.dtsi:300` (`memory@80000000 reg = <0x0 0x80000000 0x0 0x0>`) | H |
| DRAM banks, 8 GB LE2123 (EU) | `<0x80000000 0x39700000>`, `<0xc0000000 0x1c0000000>` (top `0x2_8000_0000`) | **measured**: live `/sys/firmware/fdt` `/memory` via adb root, 2026-10-22; matches `/proc/iomem` System RAM | H (device) |
| Kernel RAM window chosen | `0x1_0000_0000 + 0xfcc00000` (sweet's proven size) | inside bank 2; no live reserved-memory node above `0xeaf00000` | H (device) |
| DRAM banks, 12 GB SKU | **TODO(lemonadep)**, but the window above fits any larger bank 2 | none | TODO |
| Hot-pluggable ranges (hint only) | `<0x1 0x40000000 0x0 0x40000000>`, `<0x1 0xc0000000 0x0 0x80000000>`, `<0x2 0xc0000000 0x1 0x40000000>` | OP `qcom/lahaina.dtsi:43-50` `mem-offline/offline-sizes` | L (these are offline-able sizes, not the bank map) |

### Fixed carve-outs (no-map unless noted), final values after the lemonadep overlay

| Region | Base | Size | Source | Conf |
| --- | --- | --- | --- | --- |
| hyp_mem | `0x80000000` | `0x600000` | OP `qcom/lahaina.dtsi:307`; ML `:422` | H |
| xbl_aop_mem | `0x80700000` | `0x160000` | OP `lahaina.dtsi:312`; ML `:427` | H |
| cmd_db | `0x80860000` | `0x20000` | OP `lahaina.dtsi:317`; ML `:432` | H |
| reserved_xbl_uefi_log | `0x80880000` | `0x14000` | OP `lahaina.dtsi:323`; ML `:438` | H |
| smem | `0x80900000` | `0x200000` | OP `lahaina.dtsi:328`; ML `:445` | H |
| cpucp_fw | `0x80b00000` | `0x100000` | OP `lahaina.dtsi:333`; ML `:450` | H |
| cdsp_secure_heap | `0x80c00000` | `0x4600000` | OP `lahaina.dtsi:338`; ML `:455` | H |
| pil_camera / video / cvp | `0x85200000` / `0x85700000` / `0x85c00000` | `0x500000` each | OP `lahaina.dtsi:343-355`; ML `:460-471` | H |
| pil_adsp | `0x86100000` | `0x2100000` | OP `lahaina.dtsi:358`; ML `:475` | H |
| pil_slpi | `0x88200000` | `0x1500000` | OP `lahaina.dtsi:363`; ML `:480` | H |
| pil_cdsp | `0x89700000` | `0x1e00000` | OP `lahaina.dtsi:368`; ML `:485` | H |
| pil_ipa_fw / ipa_gsi / gpu | `0x8b500000` / `0x8b510000` / `0x8b51a000` | `0x10000` / `0xa000` / `0x2000` | OP `lahaina.dtsi:373-385`; ML `:490-501` | H |
| pil_spss | `0x8b600000` | `0x100000` | OP `lahaina.dtsi:388`; ML `:505` | H |
| pil_modem | `0x8b800000` | `0x10000000` | OP `lahaina.dtsi:393`; ML `:510` | H |
| rmtfs | vendor: dynamic (`qcom,rmtfs_sharedmem reg <0 0x280000>`); mainline: `0x9b800000` | `0x280000` | OP `lahaina.dtsi:3051`; ML `:515` | M (they disagree: the vendor driver allocates it) |
| kboot_uboot_logmem (OPLUS) | `0x9fe00000` | `0x200000` | OP `oplus/lemonadev/lemonade_common.dtsi:241` | M |
| hyp_reserved / pil_trustedvm | `0xd0000000` / `0xd0800000` | vendor: **`reg` commented out**; ML `0x800000` / `0x76f7000` | OP `lahaina.dtsi:398-410`; ML `:524-530` | M (treat as reserved anyway) |
| qrtr_shbuf, neuron chan0/1 | `0xd7ef7000`, `0xd7f00000`, `0xd7f80000` | `0x9000`, `0x80000`, `0x80000` | OP `lahaina.dtsi:414-426`; ML `:534-545` | H |
| removed_mem (**lemonadep override**) | `0xd8800000` | **`0x8e00000`** (base: `0x6800000`) | OP `oplus/lemonadev/lemonade_common.dtsi:213-217` | M |
| **cont_splash** (**lemonadep override**) | **`0xe4d00000`** (base: `0xe1000000`) | `0x2300000` | OP `lemonade_common.dtsi:218-222` (base `qcom/lahaina.dtsi:535-538`) | M |
| disp_rdump (override) | `0xe4d00000` (aliases splash) | `0x800000` | OP `lemonade_common.dtsi:224-228` (base `display-devicetree/display/lahaina-sde.dtsi:5`) | M |
| dfps_data (override) | `0xe7000000` | `0x100000` | OP `lemonade_common.dtsi:230-234` | M |
| ramoops (OPLUS) | `0xe9700000` | `0x5b8000` | OP `oplus/lemonadev/lahaina_overlay_oplus_common.dtsi:19-21` | M |
| param_mem (OPLUS) | `0xea700000` | `0x800000` | OP `lemonade_common.dtsi:249-252` | M |

Dynamic pools: adsp, sdsp, cdsp, user_contig, qseecom, qseecom_ta, secure_display, cnss_wlan, linux,cma, dump, sp, audio_cma, memshare and non_secure_display. Every one has `alloc-ranges` ending at or below `0xffffffff` (`linux,cma` and memshare at or below `0xdfffffff`). Source: OP `qcom/lahaina.dtsi:434-581`.

**Consequence for the kernel RAM window (mkadt `phys_base`/`mem_size`):**
- The highest fixed carve-out ends at `0xeaf00000` (param_mem).
- All dynamic pools are allocated below 4 GiB.
- So, as on sweet, **DRAM at or above `0x1_0000_0000` has no DT-declared carve-out.**

The bank start and size above 4 GiB are **TODO(lemonadep)**, read from the live `/memory` node. Do not reuse sweet's `0xfcc00000`.

Risk: SM8350 runs Qualcomm's hypervisor (hyp_mem at `0x80000000`). It may protect memory that no DT lists. Mitigation: the first bring-up uses a small window (e.g. 1 GiB), grown later.

## 2. Framebuffer / continuous splash

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| Splash region | `0xe4d00000`, size `0x2300000` | OP `oplus/lemonadev/lemonade_common.dtsi:218-222`; **confirmed** in the live FDT (`splash_region`, label `cont_splash_region`) | H (device) |
| Panel candidates (ABL picks one at runtime and puts it on the kernel cmdline) | `samsung amb670yf01 dsc cmd`, `..._o_dsc_cmd`, `samsung sofef00_m video` | OP `devicetree/oplus/lemonadev/lemonadep-19815.dtsi:52-57` (touch `platform_support_project_commandline` / `panel`) | M |
| Retail panel | **`qcom,mdss_dsi_samsung_amb670yf01_dsc_cmd`** (command mode); touch `synaptics-s3908` | **measured**: `/proc/cmdline` (`msm_drm.dsi_display0=`, `oplus_bsp_tp_custom.dsi_display0=`) on the LE2123, 2026-10-22 | H (device) |
| amb670yf01 mode | **`dsi_cmd_mode`**, DSC, `bpp 30` (10 bpc), 4 lanes, TE pin 1, `wr-mem-start 0x2c` | OP `display-devicetree/display/dsi-panel-samsung-amb670yf01-dsc-cmd.dtsi:3-31` | M |
| amb670yf01 default timing | `timing@fhd_sdc_60` (`qcom,mdss-dsi-timing-default`): **1080×2412 @ 60 Hz**, DSC slice 540×36 | same file `:957-1171` | M |
| amb670yf01 other timings | FHD 1080×2412 at 60/72/90/120; WQHD **1440×3216** at 60/72/90/120; `wqhd_memc_60` | same file (`timing@*`) | M |
| Physical size | 70 × 156 mm | same file `:24-25` | M |
| `_o` variant | also cmd mode, 30 bpp, default `fhd_sdc_60` 1080×2412 | OP `display-devicetree/display/dsi-panel-samsung-amb670yf01-o-dsc-cmd.dtsi:3-54` | M |
| sofef00_m variant | **video mode**, 24 bpp | OP `display-devicetree/display/dsi-panel-samsung_sofef00_m_video.dtsi:21,28` | M |
| Splash plane ABL leaves running | CTL 0 → INTF_1/DSI0 (`CTL_INTF_ACTIVE=0x2`); **two pipes VIG0+VIG1** (`FETCH_PIPE_ACTIVE=0x30000`), each 540×2412 (`SRC_SIZE=0x096C021C`) from one buffer at `0xE4D00000`; `YSTRIDE0=0x10E0`; `SRC_FORMAT=0x000237FF` (4 bytes/pixel), `UNPACK=0x03020001` | **measured**: shim M1a page 1 (MDSS registers), 2026-10-22 | H (device) |
| Framebuffer | **1080×2412, stride 4320, 32 bpp**; our a8r8g8b8 colours display correctly | same | H (device) |
| Command-mode refresh | **ABL already enables INTF_1 autorefresh** (`INTF_TEAR_AUTOREFRESH_CONFIG=0x80000001`, tear check on): CPU writes reach the panel, no kick needed | same | H (device) |
| MDSS base | `0xae00000` (vendor `reg <0x0ae00000 0x84000>`; mainline MDSS `0xae00000` + MDP `0xae01000`) | OP `display-devicetree/display/lahaina-sde-common.dtsi:4-6`; ML `sm8350.dtsi:2748-2784` | H |
| DSI0 ctrl / PHY | `0xae94000` / `0xae94400` | OP `lahaina-sde-common.dtsi:281-286,361-366`; ML `:2946,3026` | H |

**⚠ The panel is command mode.** sweet's panel was video mode: the panel scans RAM continuously, so CPU writes to the splash buffer simply appear. In command mode the panel keeps its own frame memory, and MDP sends it a frame only when software kicks a transfer (or when MDP autorefresh is on). So after ABL hands over, writes to `0xe4d00000` may never reach the glass. Step 3 must add a "kick" path; that register sequence is still TODO and comes from the Linux SDE driver in OP-K. This is the #1 risk to milestone 1.

## 3. UFS

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| UFS host (`ufs_mem`) | **`0x01d84000`**, size `0x3000` (same as sweet) | OP `devicetree/qcom/lahaina.dtsi:3083-3088`; ML `sm8350.dtsi:1735-1738` | H |
| ICE / ICE-HWKM | `0x1d88000` + `0x8000` / `0x1d90000` + `0x9000` | OP `lahaina.dtsi:3086-3088` | M |
| UFS PHY | `0x01d87000` (vendor size `0xe10`, mainline `0x1000`), 2 lanes per direction | OP `lahaina.dtsi:3069-3073`; ML `:1787-1789` (`qcom,sm8350-qmp-ufs-phy`) | H |
| Host IRQ | GIC_SPI 265 | OP `lahaina.dtsi:3089` | M |
| Host compatible | vendor `qcom,ufshc`; mainline `qcom,sm8350-ufshc`, `qcom,ufshc`, `jedec,ufs-2.0` | OP `:3084`; ML `:1736-1737` | H |
| Clocks | `core_clk`, `core_clk_unipro`, `core_clk_ice` at 75–300 MHz; `bus_aggr`, `iface`, `ref_clk` (CXO 19.2 MHz, `dev-ref-clk-freq = <0>`), tx/rx symbol clocks | OP `lahaina.dtsi:3094-3125` | M |
| Rails | VCC = `pm8350_l7` (2.504–2.95 V); VCCQ = `pm8350_l9` (parent `pm8350_s12`); vdd-hba = `gcc_ufs_phy_gdsc` | OP `devicetree/qcom/lahaina-mtp.dtsi:144-161` | M |
| Linux device path for userdata | `sysfs_path=/sys/devices/platform/soc/1d84000.ufshc`; `/dev/block/bootdevice/by-name/userdata`, f2fs + inlinecrypt (wrapped keys) | LOS-C `init/fstab.default:46` | M |
| UFSHCI / host state after ABL | **UFSHCI 3.0** (`VER=0x300`), `CAP=0x1587031F` (64-bit DMA, 32 slots); **ABL leaves it running** (`HCE=1`, `HCS=0x10F`); our list is taken; TUR/READ CAPACITY/READ all OK; LUN 0 4 KiB blocks, userdata at LBA `0x2BE848` (102.1 GiB, metadata-encrypted) | measured: M1a run 4 page 3 | H (device) |
| userdata LUN | **LUN 0** (`/dev/block/sda20`, 102.1 GiB); `persist` is also on LUN 0 (`sda3`) | **measured**: by-name links via adb root, 2026-10-22 | H (device) |

The kernel driver adopts the host ABL left running, so the base address is the only value it needs. ICE is bypassed as long as the UTRD crypto-enable bit stays 0, which `sweet_ufs.c` already does. Android's FBE ciphertext in userdata is irrelevant: the iOS image overwrites the partition.

## 4. Interrupts, timer, CPUs, PSCI

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| GICv3 distributor | `0x17a00000`, size `0x10000` | OP `qcom/lahaina.dtsi:649-656`; ML `sm8350.dtsi:3545-3552` | H |
| GICv3 redistributors | `0x17a60000`, size `0x100000`, stride `0x20000` (8 PEs), 1 region | same | H |
| GIC maintenance IRQ | PPI 9 | same | H |
| Arch timer frequency | 19.2 MHz | OP `lahaina.dtsi:666` | M (mainline has no `clock-frequency`) |
| Timer PPIs, DT order (secure-phys, phys, virt, hyp) | vendor `13, 14, 11, 12`; mainline `13, 14, 11, 10` | OP `lahaina.dtsi:660-667`; ML `:4519-4525` | H for the first three |
| **Virtual timer INTID** | **PPI 11 → INTID 27** (sweet was 19!) | from the two rows above (INTID = 16 + PPI) | H from DT; **confirm on device** with the shim's `irq_window` |
| CPU MPIDRs | `0x000, 0x100, …, 0x700` (Aff1 = core, Aff0 = 0) | OP `lahaina.dtsi:72-215` (`reg = <0x0 0xN00>`); ML `:56-197` | H |
| Core types | cpu0-3 **Cortex-A55**, cpu4-6 **Cortex-A78**, cpu7 **Cortex-X1** | ML `sm8350.dtsi:58,82,101,120,139,158,177,196` (vendor says only `qcom,kryo`) | H (mainline) |
| Capacity (dmips/MHz) | 1024 ×4, 1946 ×3, 2048 ×1 | OP `lahaina.dtsi:79-217` | M |
| Cache topology | per-core L2 (`L2_0`…`L2_7`), shared `L3_0` | OP `lahaina.dtsi:83-224` | M |
| CPU frequency domains | `cpufreq_hw` 0 (cpu0-3), 1 (cpu4-6), 2 (cpu7) | OP `lahaina.dtsi` `qcom,freq-domain` | M |
| Max clock per core | **TODO(lemonadep)**: comes from the CPUFREQ-HW LUT at runtime; mkadt's clock-frequency is descriptive only | none | TODO |
| PSCI | `arm,psci-1.0`, **`method = "smc"`** | OP `lahaina.dtsi:587-590`; ML `:321-323` | H |
| Entry EL from ABL | **EL1**; shim image loaded at `0xA0080000`, DTB at `0xAE755000`; initrd `0xAE956000-0xAFFFE263` (vendor + boot ramdisk) | **measured**: shim M1a page 1 | H (device) |
| PSCI version | 1.1 (`0x10001`) | measured: shim M1a page 1 | H (device) |
| GIC seen by EL1 | the hypervisor's **virtual GIC** (`GICR_IIDR=0x47000070`, not GIC-600). A redistributor write that arrives right after `IGROUPR0`/`ICENABLER0`/`ICACTIVER0` takes a synchronous external abort (ESR `0x96000010`); the same writes a few ms later work (all 8 priority words). The run-1 "byte write" abort was this, not the width | measured: M1a runs 1-4 (`PRIO A fault=0`, `PRIO B fault=FF`) | H (device) |
| vtimer / SGI / SMP | **INTID 27 delivers** (30 IRQs/300 ms, `HPPIR=0x1B`); SGI 1 10/10; **PSCI CPU_ON starts all 7 secondaries** (MPIDR `0x81000100`-`0x81000700`) | measured: M1a run 4 page 2 | H (device) |
| EL1 physical timer | **`msr cntp_ctl_el0` is undefined at EL1** (EC 0x00); CNTV_* and CNTPCT work | measured: M1a run 3 | H (device) |
| Watchdog | none bit during a full 4.5 min shim run; Linux pets a hypervisor `qcom,hh-watchdog` (SMC `0x86000005-8`, `drivers/virt/haven/hh_virt_wdt.c`) | measured run 4 + LOS kernel source | M |
| PS_HOLD / reset | **not found** in OP lahaina*.dtsi or ML sm8350.dtsi. **TODO(lemonadep)**; use PSCI SYSTEM_RESET only | none | TODO |
| IMEM restart_reason | `0x146bf000 + 0x65c` = `0x146bf65c` | OP `lahaina.dtsi:2130-2144` | M |

## 5. PMIC / SPMI / buttons

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| SPMI PMIC arbiter | core `0xc440000` + `0x1100`, chnls `0xc600000` + `0x2000000`, obsrvr `0xe600000` + `0x100000`, intr `0xe700000` + `0xa0000`, cnfg `0xc40a000` + `0x26000`; EE 0, channel 0 | OP `qcom/lahaina.dtsi:3379-3395`; ML `sm8350.dtsi:3218-3225` | H (core/obsrvr/cnfg **equal sweet's patch 0060 constants**) |
| PMK8350 | SPMI SID 0 | OP `qcom/pmk8350.dtsi:17-19`; ML `pmk8350.dtsi` (`pmic@0`) | H |
| PON block | **PMK8350 `pon_hlos` @ `0x1300`** (+ `pon_pbs` @ `0x800`); gen-3 PON, unlike sweet's PM6150 PON @ `0x800` | OP `pmk8350.dtsi:23-36`; ML `pmk8350.dtsi:33-36` | H |
| Power key | PON `KPDPWR` → `KEY_POWER`; IRQ spec `<0x0 0x13 0x7>` = SID 0, periph `0x13`, bit 7 | OP `pmk8350.dtsi:34-42` | H |
| **Volume up** | PON `RESIN` → **`KEY_VOLUMEUP` on lemonadep** (`pon_2` overridden); IRQ spec `<0x0 0x13 0x6>` = bit 6 | OP `oplus/lemonadev/lemonade_common.dtsi:689-697` (override of `pmk8350.dtsi:44-47`) | M |
| **Volume down** | **PM8350 GPIO 6**, active low, pull-up (`gpio_keys/vol_up` relabelled `KEY_VOLUMEDOWN` on lemonadep) | OP `lemonade_common.dtsi:701-706` overriding `qcom/lahaina-mtp.dtsi:18-34`; pin config `qcom/lahaina-pmic-overlay.dtsi:13-19` | M |
| PM8350 | SPMI SID 1; GPIO block base `0x8800` | OP `qcom/pm8350.dtsi:10,23-25`; ML `pm8350.dtsi:34,47-49` | H |
| PM8350 GPIO6 register block | `0x8800 + (6-1)*0x100` = **`0x8d00`** (SID 1) | derived (Qualcomm SPMI GPIO layout: one 0x100 block per pin, GPIO1 at the base) | M (derived) |
| PON RT status register | **TODO(lemonadep)**: gen-3 PON HLOS register layout. Sweet used `PON + 0x10`; confirm against `qcom-pon`/`pm8941-pwrkey` in OP-K before reusing | none | TODO |
| Alert slider | Hall sensors: ist8801 @ I²C `0x19`/`0x18` and mxm1120 @ `0x0D`/`0x0C` on `qupv3_se15_i2c`; TLMM GPIO 50/51 | OP `lemonade_common.dtsi:342-396` | M (not needed for iOS) |

## 6. Touch

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| Controller | **Synaptics S3908** (`synaptics-s3908`), I²C address **`0x4B`** | OP `devicetree/oplus/lemonadev/lemonadep-19815.dtsi:3-6` | M |
| Bus | `qupv3_se4_i2c` = **`i2c@990000`** (size `0x4000`), GENI, IRQ SPI 605, QUP wrapper 0 (`qupv3_0` @ `0x9c0000`) | OP `devicetree/qcom/lahaina-qupv3.dtsi:29-31,339-358`; ML `sm8350.dtsi:1097-1099` (`i2c4`) | H |
| SE clock | `GCC_QUPV3_WRAP0_S4_CLK` (+ wrap0 m/s AHB) | OP `lahaina-qupv3.dtsi:345-348` | M |
| GCC | `0x100000`, size `0x1f0000` | OP `qcom/lahaina.dtsi:2400-2402` | M |
| SE4 clock bring-up (full register map, by analogy with sweet's SE7/wrap1 `touch_clocks()`) | **Vote register** `GCC+0x52008` (same reg for all of wrap0's bits, unlike wrap1 which votes at `0x5200c`): `BIT(6)`=m_ahb, `BIT(7)`=s_ahb, `BIT(8)`=`gcc_qupv3_wrap0_core_clk`, `BIT(9)`=`gcc_qupv3_wrap0_core_2x_clk`, `BIT(14)`=`gcc_qupv3_wrap0_s4_clk` → **vote mask `0x43C0`**. **Halt registers** (all `BRANCH_HALT_VOTED`, poll bit31 clear exactly as sweet's `touch_wait_clear` already does): m_ahb `GCC+0x17004`, s_ahb `GCC+0x17008`, core `GCC+0x23000`, core_2x `GCC+0x23008`, s4 `GCC+0x174cc` — **5 branches**, matching sweet's `branches[]` array length exactly. **RCG** (`gcc_qupv3_wrap0_s4_clk_src`) `cmd_rcgr = GCC+0x174d0` (CMD at +0, CFG at +4, same `clk_rcg2` layout sweet's code assumes); SE4 **is DFS-enabled** (`DEFINE_RCG_DFS(gcc_qupv3_wrap0_s4_clk_src)`, same as every other wrap0/1/2 QUP SE on this SoC) so sweet's DFS-mode branch (skip RCG CFG write, verify level0==XO/1) applies unchanged | `los-kernel` (lineage-23.2) `drivers/clk/qcom/gcc-lahaina.c:916-932,2696-2803,3116-3144,4310` (S4 RCG/branch defs, core/AHB branches, DFS list), 2026-10-22 | H |
| IRQ / reset GPIO | TLMM **23** (IRQ, flags `0x2008`), TLMM **22** (reset) | OP `lemonadep-19815.dtsi:10-11,20-21` | M |
| Power | `enable2v8_gpio` TLMM 31, `enable1v8_gpio` TLMM 197, `vdd_2v8-supply = <&L3C>` = **pm8350c_l3** (RPMh resource `ldoc3`), 3.008 V | OP `lemonadep-19815.dtsi:14-18`; `qcom/lahaina-regulators.dtsi:571-580` | M |
| Coordinates | panel-coords **1440 × 3216**, display-coords 1440 × 3216, 10 fingers, TX/RX 17/38 | OP `lemonadep-19815.dtsi:26-31` | M |
| TLMM base | **RESOLVED (not a real disagreement)**: IP base `0x0F000000` (OP `lahaina.dtsi:2184`, `reg = <0x0F000000 0x1000000>`) + driver's internal `REG_BASE 0x100000` pin-array offset (`pinctrl-lahaina.c:21`) = **`0x0F100000`** — exactly mainline's pre-folded `0x0f100000`. Per-GPIO: `ctl_reg = 0x0F100000 + 0x1000*pin` (`io_reg=+4`, `intr_cfg_reg=+8`, `intr_status_reg=+0xc`); bit layout `mux_bit=2 pull_bit=0 drv_bit=6 oe_bit=9` (`pinctrl-lahaina.c:38-56`) — **identical** to the bit-packing sweet's `touch_gpio()` already does (`mux<<2 \| pull \| drive<<6 \| out?0x200:0`); only the base address changes | `los-kernel` (lineage-23.2) `drivers/pinctrl/qcom/pinctrl-lahaina.c:21,38-56`, 2026-10-22 | H |
| S3908 protocol | **Synaptics TCM v1/v2** (standard protocol, not OnePlus-custom). Exact driver identified by matching DT `compatible = "synaptics-s3908"` against each variant's own `of_device_id` table: **not** `S3706/synaptics_drivers_s3706.c` or `Syna_tcm_S3910/synaptics_tcm_S3910.c` (those register `"synaptics-s3910"`) — the match is `Syna_tcm_oncell/synaptics_tcm_oncell.c:6562` (`.compatible = "synaptics-s3908"`). Message framing: 4-byte header `{marker=0xA5, code, length_lo, length_hi}` + 1 trailing byte, `total_length = 4 + payload_length + 1` (`synaptics_tcm_oncell.c:1531`); core commands `CMD_RESET=0x04`, `CMD_IDENTIFY=0x02`, `CMD_ENABLE_REPORT=0x05`/`CMD_DISABLE_REPORT=0x06`, `CMD_GET_APPLICATION_INFO=0x20`, `CMD_RUN_APPLICATION_FIRMWARE=0x14`; report IDs `REPORT_IDENTIFY=0x10`, `REPORT_TOUCH=0x11`; status `STATUS_OK=0x01`, `STATUS_CONTINUED_READ=0x03` (all `Syna_tcm_oncell/synaptics_tcm_oncell.h:248-301`) | `los-kernel` (lineage-23.2) `drivers/input/touchscreen/oplus_touchscreen_v2/Synaptics/Syna_tcm_oncell/{synaptics_tcm_oncell.c,synaptics_tcm_oncell.h}`, 2026-10-22 | H |
| SE4 transfer mode | **GSI-only**: `dmas = <&gpi_dma0 ...>` in the vendor dtsi. On-device, `GENI_IF_FIFO_DISABLE_RO` (SE+0x64) bit0 reads **1**. FIFO transfers time out, and driving the SE-DMA engine crashed the SoC (XBL ramdump). Overlay 1005 therefore **bit-bangs I2C on the TLMM pins**: SDA **gpio20**, SCL **gpio21**, as mux 0 GPIOs with pull-up. This is the same approach as zoe-vb/ios-sargo patch 0087 on the Pixel 3a | OP `lahaina-qupv3.dtsi` (se4 i2c node); on-device log 2026-10-22; ios-sargo | H |
| wrap0 m/s AHB branches | **HW clock gated** (`.hwcg_reg`/`.hwcg_bit = 1`). An idle `CLK_OFF` with HWCG (`0x80000002`, seen on-device) is not a failure, which is why Linux skips the halt check in that mode | `gcc-lahaina.c`; `clk-branch.c` `clk_branch_in_hwcg_mode`; on-device 2026-10-22 | H |

## 7. RPMh (touch rail, later)

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| apps_rsc | drv-0 `0x18200000`, drv-1 `0x18210000`, **drv-2 `0x18220000`** (each `0x10000`); `qcom,tcs-offset = 0xd00`, `drv-id = 2`; TCS ACTIVE 2 / SLEEP 3 / WAKE 3 / CONTROL 0 | OP `qcom/lahaina.dtsi:592-607`; ML `sm8350.dtsi:3615-3628` | H (**the same values sweet's patch 0062 hard-codes**) |
| cmd-db | reserved memory `0x80860000` + `0x20000` (direct; sweet read it through an AOP msgram pointer `0x0c3f000c`) | OP `lahaina.dtsi:317`; ML `:432-434` | H |

## 8. GPU (Adreno 660)

Scoping pass (2026-10-23) for the Metal-on-Adreno bring-up; see
`HANDOFF.md`'s "GPU, Phase 0/1" sections for the toolchain side (metal2vulkan,
Turnip, the UniXNU reference) and the real firmware already pulled from this
phone (`lemonadep/fw-gpu/`).

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| Chip ID (exact entry for `revn=660`, not a neighboring a6xx chip) | `0x06060001`, `ADRENO_6XX_GEN4` | `linux` (mainline) `drivers/gpu/drm/msm/adreno/a6xx_catalog.c:1020-1038` | H |
| GMEM | `SZ_1M + SZ_512K` = **1.5 MiB** (not the 1 MiB some other a6xx chips use -- do not reuse a 640/740 example value) | same, `:1027` | H |
| Quirks | `ADRENO_QUIRK_HAS_CACHED_COHERENT \| ADRENO_QUIRK_HAS_HW_APRIV` | same, `:1030-1031` | H |
| Firmware names (match the files already pulled from this phone byte for byte) | SQE `a660_sqe.fw`, GMU `a660_gmu.bin`, zap `a660_zap.mdt` | same, `:1024-1026,1033`; confirmed present at `/vendor/firmware/` on-device, 2026-10-23 | H |
| Clock-gating (`a660_hwcg`) and protected-register (`a660_protect`, 48 entries) tables | full reglist, not yet transcribed here (long; pull from source when writing the driver) | same, `:400-586` (hwcg), `:587-631` (protect) | H |
| GBIF CX config | shared with a640 (`a640_gbif`), not a660-unique | same, `:1036` | H |
| `gmu_cgc_mode` / `prim_fifo_threshold` | `0x00020000` / `0x00300200` | same, `:1036-1037` | H |
| Kernel ABI the Turnip darwin backend expects | `NativeAdrenoABI.h` (UniXNU/NativeAdreno) -- versioned, `static_assert`-pinned wire structs for Query/BoAlloc/BoMap/BoImport/Submit/WaitFence/GetDeviceInfo/queues | github.com/UniXNU/NativeAdreno, 2026-10-23 | H |
| IOKit discovery the darwin KMD patch uses | `IOServiceGetMatchingService` for `$TU_DARWIN_SERVICE` (default `"IOAcceleratorES"` -- the patch's own comment confirms this is the real Apple class name "the original Metal discovery finds", not a UniXNU invention), open type `$TU_DARWIN_OPEN_TYPE` (default `0x4147`); service name `"NativeAdrenoGpuPublished"` selects the unsandboxed direct form. **Sweet itself has no existing accelerator driver to extend** (`grep` of `iokit/Kernel/arm/` confirms no `SweetAccelerator.cpp`; matches the project README's "no GPU driver") -- the GPU user client is new code, published under `IOAcceleratorES` so real Metal discovery finds it | `patches/mesa/turnip-darwin-kmd.patch` (UniXNU/UniXNU), 2026-10-23 | H |
| GMU/CP bring-up sequence for A660 specifically | **not yet sourced**: UniXNU's own `NativeAdrenoA740HwInit.h` is for the 740 (a different a6xx generation's exact register sequence) and must not be reused verbatim; the real source is `a6xx_gpu.c`/`a6xx_gmu.c`/`a6xx_hfi.c` in the same Linux tree, not yet read in full | TODO | - |

## 9. USB (deferred; recorded for later)

Scoping pass (2026-10-22), re-checking against the real retail dtsi rather than
relying on the earlier deferred note: sweet's existing USB overlay (xnu/patches
0072-0077, 0081; ~1266 lines total) is almost entirely DWC3-generic, not
SE7/sweet-specific. Only `QUSB2`/PHY references appear in 0072/0073 (8 lines
total across the whole series, `grep -c QUSB2`); 0074-0077 and 0081
(VBUS override, ECM networking, RX mbuf, live log, link-state -- the bulk of
the code) have zero PHY-specific lines and should apply to lemonadep
unchanged. The real, bounded new work for a future overlay 1006 is just the
PHY init sequence: sweet's is QUSB2-specific, lemonadep's chip is a different
PHY family entirely (SNPS femto, below) and needs its own init.

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| DWC3 | `ssusb@a600000`, `reg = <0xa600000 0x100000>`, `compatible = "qcom,dwc-usb3-msm"` -- **identical address and compatible string to sweet's own DWC3** (`androidboot.usbcontroller=a600000.dwc3`) | OP `qcom/lahaina-usb.dtsi:6-9`; LOS-C `BoardConfigCommon.mk` cmdline | H |
| HS PHY (primary port) | **SNPS femto** (`compatible = "qcom,usb-hsphy-snps-femto"`), `hsphy@88e3000`, `reg = <0x88e3000 0x114>` -- a different PHY family from sweet's QUSB2, needs its own init sequence, not just a new address | OP `lahaina-usb.dtsi:118-121` | H |
| ⚠ trap: `0x088e2000` | This is sweet's own `QUSB2_PA` (its PHY base). On lemonadep the *same* address is a completely unrelated register: the primary HS PHY node's **second** `reg` entry, labelled `"eud_enable_reg"` (Embedded USB Debugger enable) -- not a PHY register at all. Do not reuse sweet's `QUSB2_PA` value for anything on lemonadep; it means something else here | OP `lahaina-usb.dtsi:119-123` | H |
| SS/DP PHY | `ssphy@88e8000` | OP `lahaina-usb.dtsi:142-144` | M |
| Secondary HS PHY (unused here) | `hsphy@88e4000`, same `qcom,usb-hsphy-snps-femto` compatible, no `eud_enable_reg` entry | OP `lahaina-usb.dtsi:415-418` | M |

## 10. What this means for the sweet code (feeds STEP 3)

| sweet constant | lemonadep | Change? |
| --- | --- | --- |
| GICD `0x17a00000`, GICR `0x17a60000` | same | no |
| UFS `0x01d84000` | same | no |
| SPMI core/obs/cnfg | same | no (but PON/PMIC registers differ) |
| RPMh RSC/TCS layout | same | no (regulator name differs) |
| VTIMER INTID 19 | **27** | **yes** (ADT `virtual-timer-intid`, shim) |
| FB `0x9c000000`, 1080×2400, a8r8g8b8 | `0xe4d00000`, geometry/format TODO, cmd-mode panel | **yes** + needs a kick |
| RAM window | above 4 GiB, size TODO | **yes** |
| CPU topology A55×6 + A76×2 | A55×4, A78×3, X1×1; same MPIDR scheme `0xN00` | MPIDR checks in 0065/0078 already accept `≤0x700` |
| Touch GT9896 @ `0x5d`, SE7 `0xa84000` | S3908 @ `0x4B`, SE4 `0x990000` | **yes** (new driver) |
| Keys PM6150 PON / PM6150L GPIO2 | PMK8350 PON_HLOS (power, vol-up) / PM8350 GPIO6 (vol-down) | **yes** |
