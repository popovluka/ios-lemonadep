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
| DRAM banks per SKU (8 GB LE2121/2123/2125 and 12 GB) | **TODO(lemonadep)**: read `/memory` from the live FDT (the shim gets it in `x0`; `fdt.c` must learn to read `/memory`) | none in source | TODO |
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
| Splash region | `0xe4d00000`, size `0x2300000` | OP `oplus/lemonadev/lemonade_common.dtsi:218-222` | M (DT tells Linux where ABL's splash is; confirm by reading the live FDT) |
| Panel candidates (ABL picks one at runtime and puts it on the kernel cmdline) | `samsung amb670yf01 dsc cmd`, `..._o_dsc_cmd`, `samsung sofef00_m video` | OP `devicetree/oplus/lemonadev/lemonadep-19815.dtsi:52-57` (touch `platform_support_project_commandline` / `panel`) | M |
| Retail panel | **TODO(lemonadep)**: read `msm_drm.dsi_display0=` from `/proc/cmdline` on stock | none | TODO |
| amb670yf01 mode | **`dsi_cmd_mode`**, DSC, `bpp 30` (10 bpc), 4 lanes, TE pin 1, `wr-mem-start 0x2c` | OP `display-devicetree/display/dsi-panel-samsung-amb670yf01-dsc-cmd.dtsi:3-31` | M |
| amb670yf01 default timing | `timing@fhd_sdc_60` (`qcom,mdss-dsi-timing-default`): **1080×2412 @ 60 Hz**, DSC slice 540×36 | same file `:957-1171` | M |
| amb670yf01 other timings | FHD 1080×2412 at 60/72/90/120; WQHD **1440×3216** at 60/72/90/120; `wqhd_memc_60` | same file (`timing@*`) | M |
| Physical size | 70 × 156 mm | same file `:24-25` | M |
| `_o` variant | also cmd mode, 30 bpp, default `fhd_sdc_60` 1080×2412 | OP `display-devicetree/display/dsi-panel-samsung-amb670yf01-o-dsc-cmd.dtsi:3-54` | M |
| sofef00_m variant | **video mode**, 24 bpp | OP `display-devicetree/display/dsi-panel-samsung_sofef00_m_video.dtsi:21,28` | M |
| Splash width / height / stride / pixel format that ABL actually uses | **TODO(lemonadep)**: no DT source. Measure on device: dump the region and look for the boot logo, or read MDSS SSPP registers | none | TODO |
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
| UFSHCI version, UFS device spec version | **TODO(lemonadep)**: read `REG_VER` (0x08) and `CAP` with the shim probe (`shim/ufs.c` already prints them) | none | TODO |
| userdata LUN | **TODO(lemonadep)**: shim probe lists the GPT per LUN | none | TODO |

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
| Entry EL from ABL | **TODO(lemonadep)**: read CurrentEL in the shim | none | TODO |
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
| IRQ / reset GPIO | TLMM **23** (IRQ, flags `0x2008`), TLMM **22** (reset) | OP `lemonadep-19815.dtsi:10-11,20-21` | M |
| Power | `enable2v8_gpio` TLMM 31, `enable1v8_gpio` TLMM 197, `vdd_2v8-supply = <&L3C>` = **pm8350c_l3** (RPMh resource `ldoc3`), 3.008 V | OP `lemonadep-19815.dtsi:14-18`; `qcom/lahaina-regulators.dtsi:571-580` | M |
| Coordinates | panel-coords **1440 × 3216**, display-coords 1440 × 3216, 10 fingers, TX/RX 17/38 | OP `lemonadep-19815.dtsi:26-31` | M |
| TLMM base | vendor `0x0F000000` + `0x1000000`; mainline **`0x0f100000` + `0x300000`** | OP `qcom/lahaina.dtsi:2182-2184`; ML `sm8350.dtsi:3236-3238` | ⚠ they disagree. Per-GPIO offsets **TODO(lemonadep)** from `pinctrl-lahaina.c` in OP-K before touching GPIOs |
| S3908 protocol / firmware | **TODO(lemonadep)**: Synaptics TCM (not Goodix); driver in OP-K `drivers/input/touchscreen/oplus_touchscreen*` | none | TODO |

## 7. RPMh (touch rail, later)

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| apps_rsc | drv-0 `0x18200000`, drv-1 `0x18210000`, **drv-2 `0x18220000`** (each `0x10000`); `qcom,tcs-offset = 0xd00`, `drv-id = 2`; TCS ACTIVE 2 / SLEEP 3 / WAKE 3 / CONTROL 0 | OP `qcom/lahaina.dtsi:592-607`; ML `sm8350.dtsi:3615-3628` | H (**the same values sweet's patch 0062 hard-codes**) |
| cmd-db | reserved memory `0x80860000` + `0x20000` (direct; sweet read it through an AOP msgram pointer `0x0c3f000c`) | OP `lahaina.dtsi:317`; ML `:432-434` | H |

## 8. USB (deferred; recorded for later)

| Component | Value | Source | Conf |
| --- | --- | --- | --- |
| DWC3 | `ssusb@a600000` (`androidboot.usbcontroller=a600000.dwc3`) | OP `qcom/lahaina-usb.dtsi:6`; LOS-C `BoardConfigCommon.mk` cmdline | M |
| HS PHY | **SNPS femto** `hsphy@88e3000` (sweet: QUSB2) | OP `lahaina-usb.dtsi:118-120` | M |
| SS/DP PHY | `ssphy@88e8000` | OP `lahaina-usb.dtsi:142-144` | M |

## 9. What this means for the sweet code (feeds STEP 3)

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
