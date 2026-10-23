/* SPDX-License-Identifier: MIT */
#ifndef LEMONADEP_WLAN_SOURCE_H
#define LEMONADEP_WLAN_SOURCE_H
/*
 * Hardware facts for lemonadep's Wi-Fi (QCA6490 on PCIe RC0), stage 1:
 * power the chip, bring up PCIe0, read its config-space ID. Every value is
 * cited. Sources:
 *   LIVE  the phone's own DT (/sys/firmware/fdt from TWRP, 2026-10-23;
 *         kept out of git, see lemonadep/tools/dts-resolve.py)
 *   LOS-K LineageOS/android_kernel_oneplus_sm8350 @ 187d13c (HWMAP's pin):
 *         gcc-lahaina.c, gdsc-regulator.c, pci-msm.c, cnss2/{power,bus}.c/.h,
 *         dts vendor/qcom/lahaina-pcie.dtsi + lahaina-v2.1.dtsi
 */
#include <stdint.h>

/* ---- QCA6490 (LIVE qcom,cnss-qca6490@b0000000) ---- */
#define LEMONADEP_WLAN_PCI_VENDOR   0x17cbu  /* cnss2/bus.h QCA6490_VENDOR_ID */
#define LEMONADEP_WLAN_PCI_DEVICE   0x1103u  /* cnss2/bus.h QCA6490_DEVICE_ID */
#define LEMONADEP_WLAN_EN_GPIO      64u      /* wlan-en-gpio = <&tlmm 64 0> */
#define LEMONADEP_WLAN_BT_EN_GPIO   65u      /* qcom,bt-en-gpio = <&tlmm 65 0> */
/* cnss_wlan_en_active: function gpio, drive-strength 16 mA, output-high, bias-pull-up. */
#define LEMONADEP_WLAN_EN_DRIVE_MA  16u
/* cnss2/power.c: WLAN_ENABLE_DELAY (10 ms, "at least 4ms ... qca6390") between
 * WLAN_EN active and PCIe reset de-assert; and with BT_EN low, a 100 ms wait
 * before WLAN_EN ("PMU sequencing" for QCA6390/QCA6490). */
#define LEMONADEP_WLAN_EN_DELAY_US     10000u
#define LEMONADEP_WLAN_BT_OFF_DELAY_US 100000u

/* Rails, in cnss2/power.c cnss_vreg_list order (only the ones LIVE has):
 * cmd-db resource (LIVE qcom,resource-name), mV from LIVE qcom,vdd-*-config
 * <min max load delay unvote>. All loads are 0, so no mode (HPM) vote --
 * Linux requests none. Delays are 0. */
struct LemonadepWlanRail { const char *res; unsigned mv; const char *what; };
static const struct LemonadepWlanRail LemonadepWlanRails[6] = {
	{"smpb10", 1800, "vdd-wlan-io (pm8350 S10)"},
	{"ldoe7",  2800, "wlan-ant-switch (pmr735a L7)"},
	{"smpe2",   950, "vdd-wlan-aon (pmr735a S2)"},
	{"smpb11",  950, "vdd-wlan-dig (pm8350 S11)"},
	{"smpc1",  1880, "vdd-wlan-rfa1 (pm8350c S1)"},
	{"smpb12", 1350, "vdd-wlan-rfa2 (pm8350 S12)"},
};

/* ---- PCIe RC0 (LIVE qcom,pcie@1c00000, LOS-K lahaina-pcie.dtsi pcie0) ---- */
#define LEMONADEP_PCIE0_PARF_PA   0x01c00000u  /* reg "parf", 0x3000 */
#define LEMONADEP_PCIE0_PHY_PA    0x01c06000u  /* reg "phy", 0x2000 */
#define LEMONADEP_PCIE0_DBI_PA    0x60000000u  /* reg "dm_core" 0xf1d + "elbi" at +0xf20 */
#define LEMONADEP_PCIE0_ELBI_OFF  0x0f20u
#define LEMONADEP_PCIE0_IATU_PA   0x60001000u  /* reg "iatu", 0x1000 */
#define LEMONADEP_PCIE0_CONF_PA   0x60100000u  /* reg "conf", 0x100000 */
#define LEMONADEP_PCIE0_PERST_GPIO 94u         /* perst-gpio; pinctrl: gpio, 2 mA, pull-down */
#define LEMONADEP_PCIE0_PHY_STATUS_OFF 0x214u  /* qcom,phy-status-offset */
#define LEMONADEP_PCIE0_PHY_STATUS_BIT 6u      /* qcom,phy-status-bit: set = not ready */
#define LEMONADEP_PCIE0_SLV_ADDR_SPACE 0x4000000u /* qcom,slv-addr-space-size */
#define LEMONADEP_PCIE0_EP_LATENCY_US  10000u  /* qcom,ep-latency = <10> ms */
/* PCIe PHY rails: vreg-0p9 = pm8350 L5 at 880 mV / 47.9 mA, vreg-1p8 = pm8350 L6
 * at 1200 mV / 15 mA (LIVE qcom,vreg-*-voltage-level). Loads above the PMIC5
 * LDO's LPM limit, so HPM, like the touch rail. */
#define LEMONADEP_PCIE0_L5_MV 880u
#define LEMONADEP_PCIE0_L6_MV 1200u

/* pci-msm.c register offsets and values used by msm_pcie_enable() */
#define PCIE_PARF_SYS_CTRL            0x0000u  /* = 0x365E */
#define PCIE_PARF_PM_CTRL             0x0020u  /* clear bit 5 (REQ_NOT_ENTR_L1) */
#define PCIE_PARF_PHY_CTRL            0x0040u  /* clear bit 0 */
#define PCIE_PARF_MHI_CLOCK_RESET_CTRL 0x0174u /* set bit 4 */
#define PCIE_PARF_AXI_MSTR_WR_ADDR_HALT 0x01a8u /* set bit 31 */
#define PCIE_PARF_LTSSM               0x01b0u  /* set bit 8: LTSSM enable */
#define PCIE_PARF_INT_ALL_MASK        0x022cu
#define PCIE_PARF_DBI_BASE_ADDR       0x0350u  /* = 0 */
#define PCIE_PARF_SLV_ADDR_SPACE_SIZE 0x0358u
#define PCIE_PARF_DEVICE_TYPE         0x1000u  /* = 4: root complex */
#define PCIE_ELBI_SYS_STTS            0x0008u  /* bit 10 XMLH_LINK_UP, [17:12] LTSSM */
#define PCIE_XMLH_LINK_UP             0x400u
#define PCIE_DBI_CAP_LINKCTRLSTATUS   0x0080u  /* PCIE20_CAP 0x70 + 0x10; bit 29 DLL active */
#define PCIE_DBI_CAP_LINKCTL2         0x00a0u  /* PCIE20_CAP + PCI_EXP_LNKCTL2; TLS [3:0] */
#define PCIE_DBI_GEN3_GEN2_CTRL       0x080cu  /* field 0x1f00 = 1 */
#define PCIE_DLL_ACTIVE               (1u << 29)
/* iATU, unrolled (the "iatu" window exists): region n at n*0x200 */
#define PCIE_IATU_CTRL1   0x00u
#define PCIE_IATU_CTRL2   0x04u
#define PCIE_IATU_LBAR    0x08u
#define PCIE_IATU_UBAR    0x0cu
#define PCIE_IATU_LAR     0x10u
#define PCIE_IATU_LTAR    0x14u
#define PCIE_IATU_UTAR    0x18u
#define PCIE_IATU_TYPE_CFG0 0x04u
/* pci-msm.c timing */
#define PCIE_PERST_PROPAGATION_US 1000u   /* PERST_PROPAGATION_DELAY_US_MIN */
#define PCIE_PHY_STABILIZE_US     1000u   /* PHY_STABILIZATION_DELAY_US_MIN */
#define PCIE_PHY_READY_TRIES      10u     /* PHY_READY_TIMEOUT_COUNT, 1 ms apart */
#define PCIE_LINK_UP_TRIES        20u     /* LINK_UP_CHECK_MAX_COUNT, 5 ms apart */
#define PCIE_LINK_UP_POLL_US      5000u
#define PCIE_EP_UP_TIMEOUT_US     1000000u /* EP_UP_TIMEOUT_US, 1 ms polls */

/* ---- GCC (base 0x00100000), gcc-lahaina.c ---- */
#define LEMONADEP_GCC_PA 0x00100000u
#define GCC_PCIE0_GDSC          0x6b004u /* LIVE gdsc@16b004; PWR_ON = bit 31 */
#define GCC_PCIE0_GDSC_VOTE     0x52128u /* LIVE qcom,collapse-vote = <&syscon@152128 0>:
                                          * clear bit 0 to vote the GDSC on (gdsc-regulator.c) */
#define GCC_PCIE0_GDSC_TIMEOUT_US 500u   /* LIVE qcom,gds-timeout = <0x1f4> */
#define GCC_PCIE0_PIPE_MUX      0x6b054u /* gcc_pcie_0_pipe_clk_src: 0 = PHY pipe clk, 2 = XO */
#define GCC_PCIE0_AUX_RCG       0x6b058u /* gcc_pcie_0_aux_clk_src: 19.2 MHz = XO (src 0) / 1 */
#define GCC_PCIE0_RCHNG_RCG     0x6b03cu /* gcc_pcie_0_phy_rchng_clk_src: 100 MHz = GPLL0 (src 1) / 6 */
#define GCC_PCIE0_BCR           0x6b000u /* GCC_PCIE_0_BCR (pcie_0_core_reset) */
#define GCC_PCIE0_PHY_BCR       0x6c01cu /* GCC_PCIE_0_PHY_BCR (pcie_0_phy_reset) */
#define GCC_CLKREF_PCIE0        0x8c004u /* gcc_pcie_0_clkref_en: enable + halt, BRANCH_HALT */
#define GCC_VOTE_A              0x52000u /* APCS clock branch enable vote */
#define GCC_VOTE_B              0x52008u
/* Voted branches: {vote reg, bit, halt reg, poll halt?}. Polling skipped where
 * gcc-lahaina.c says BRANCH_HALT_SKIP, or a hwcg_bit is set (the branch may read
 * CLK_OFF while hardware-gated -- the same rule touch already follows). */
struct LemonadepGccBranch { uint32_t vote, bit, halt; unsigned poll; const char *name; };
static const struct LemonadepGccBranch LemonadepPcie0Branches[] = {
	{GCC_VOTE_B, 3, 0x6b028, 1, "pcie_0_aux"},
	{GCC_VOTE_B, 2, 0x6b024, 0, "pcie_0_cfg_ahb"},       /* VOTED + hwcg */
	{GCC_VOTE_B, 1, 0x6b01c, 0, "pcie_0_mstr_axi"},      /* SKIP */
	{GCC_VOTE_B, 0, 0x6b014, 0, "pcie_0_slv_axi"},       /* VOTED + hwcg */
	{GCC_VOTE_B, 5, 0x6b010, 1, "pcie_0_slv_q2a_axi"},
	{GCC_VOTE_A, 18, 0x9000c, 0, "aggre_noc_pcie_tbu"},  /* VOTED + hwcg */
	{GCC_VOTE_A, 22, 0x6b038, 1, "pcie0_phy_rchng"},
	{GCC_VOTE_A, 19, 0x8d080, 0, "ddrss_pcie_sf_tbu"},   /* SKIP */
	{GCC_VOTE_A, 12, 0x6b080, 0, "aggre_noc_pcie_0_axi"}, /* SKIP */
	{GCC_VOTE_A, 11, 0x8d084, 0, "aggre_noc_pcie_1_axi"}, /* SKIP */
};
/* gcc_pcie_0_pipe_clk: enabled after the PHY sequence (pci-msm.c pipe clk init). */
#define GCC_PCIE0_PIPE_VOTE_BIT 4u /* GCC_VOTE_B, halt 0x6b030 SKIP */
#define GCC_CLK_OFF (1u << 31)

/* ---- TLMM (base 0x0F100000, same layout touch uses on this phone) ---- */
#define LEMONADEP_TLMM_PA      0x0f100000u
#define LEMONADEP_TLMM_BYTES   0xd0000u
#define TLMM_CFG(n)   ((uint32_t)(n) * 0x1000u)
#define TLMM_IO(n)    (TLMM_CFG(n) + 4u)   /* bit 0 in, bit 1 out */
#define TLMM_PULL_DOWN 1u
#define TLMM_PULL_UP   3u
#define TLMM_OE        (1u << 9)
#define TLMM_EGPIO_PRESENT (1u << 11)      /* pinctrl-sm8350.c */
#define TLMM_EGPIO_ENABLE  (1u << 12)
#endif
