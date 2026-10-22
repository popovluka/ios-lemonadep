#pragma once
/*
 * Per-board hardware constants for the shim. sweet is the default; build
 * with -DBOARD_LEMONADEP (BOARD=lemonadep ./build.sh) for the OnePlus 9 Pro.
 * Sources for every lemonadep value: lemonadep/HWMAP_lemonadep.md.
 */

#if defined(BOARD_LEMONADEP)

#define BOARD_NAME		"lemonadep"
/* lemonade_common.dtsi &splash_memory (overrides qcom/lahaina.dtsi 0xe1000000) */
#define FB_BASE			0xe4d00000UL
#define FB_SIZE			0x2300000UL
/* Geometry is not in any source: display.c reads it from the MDSS SSPP ABL left running. */
#define FB_W			0
#define FB_H			0
#define FB_STRIDE		0
#define FB_BPP			4
/* qcom/lahaina.dtsi intc (= sm8350.dtsi) */
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
/* arch_timer: <GIC_PPI 13>, <14>, <11>, ... -> phys INTID 30, virt INTID 27 */
#define VTIMER_INTID		27
#define PTIMER_INTID		30
/*
 * EL1 physical timer: CNTP_* writes are undefined at EL1 (the Qualcomm
 * hypervisor keeps CNTHCTL_EL2.EL1PCEN clear); measured, M1a run 3:
 * "msr cntp_ctl_el0" took EC 0x00. The virtual timer and CNTPCT work.
 */
#define BOARD_EL1_PTIMER	0
/* qcom,msm-imem@146bf000 restart_reason@65c */
#define IMEM_RESTART_REASON	0x146bf65cUL
/* TODO(lemonadep): no PS_HOLD node in lahaina.dtsi or sm8350.dtsi; PSCI reset only */
#define PSHOLD			0UL
/* ufshc@1d84000 ufs_mem */
#define UFS_BASE		0x01d84000UL
/* MDSS / SDE (display-devicetree/display/lahaina-sde-common.dtsi mdss_mdp) */
#define MDP_BASE		0x0ae00000UL
#define BOARD_HAS_MDSS		1
/* Refuse to start the kernel outside a live /memory bank or on a no-map carve-out. */
#define BOARD_CHECK_DRAM	1

#else /* sweet */

#define BOARD_NAME		"sweet"
#define FB_BASE			0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_SIZE			0x1700000UL
#define FB_W			1080
#define FB_H			2400
#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
#define FB_BPP			4
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define VTIMER_INTID		19	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID		18	/* DT: <GIC_PPI 2 ...> */
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define BOARD_EL1_PTIMER	1
#define PSHOLD			0x0c264000UL
#define UFS_BASE		0x01d84000UL	/* ufshc@1d84000, sweet live FDT */
#define BOARD_HAS_MDSS		0
#define BOARD_CHECK_DRAM	0

#endif

#define RESTART_BOOTLOADER	0x77665500U
