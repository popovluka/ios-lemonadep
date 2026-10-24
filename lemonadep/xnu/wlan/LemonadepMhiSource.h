/* SPDX-License-Identifier: MIT */
#ifndef LEMONADEP_MHI_SOURCE_H
#define LEMONADEP_MHI_SOURCE_H
/*
 * Hardware facts for lemonadep's Wi-Fi, stage 2: BAR0, MHI/BHI registers,
 * the first (BHI) firmware download. Every value is cited. Sources:
 *   LIVE  the phone's own DT (see LemonadepWlanSource.h)
 *   LOS-K LineageOS/android_kernel_oneplus_sm8350 @ 187d13c:
 *         drivers/bus/mhi/core/{mhi_internal.h,mhi_init.c,mhi_boot.c,mhi_pm.c},
 *         include/linux/mhi.h, drivers/net/wireless/cnss2/pci.c,
 *         drivers/pci/controller/pci-msm.c, drivers/iommu/arm-smmu.h
 */
#include <stdint.h>

/* ---- PCIe0 memory window (LIVE qcom,pcie@1c00000 ranges, the 0x2000000 =
 * non-prefetchable memory entry): CPU address == PCI address, untranslated.
 * pci-msm.c only programs iATU region 0 (config cycles, msm_pcie_cfg_bdf);
 * memory requests outside it go out as-is. ---- */
#define LEMONADEP_PCIE0_MEM_PA     0x60300000u
#define LEMONADEP_PCIE0_MEM_BYTES  0x03d00000u
/* What the kernel maps of it: enough for a BAR0 of up to 4 MiB placed at the
 * first aligned address (worst case 0x60400000 + 4 MiB). */
#define LEMONADEP_PCIE0_MEM_MAP    0x00800000u
#define LEMONADEP_MHI_BAR_MAX      0x00400000u

/* Type 0/1 config header (PCI spec; pci-msm.c PCIE20_COMMAND_STATUS = 0x04) */
#define PCI_CMD_STATUS        0x04u
#define PCI_CMD_MEMORY        (1u << 1)
#define PCI_CMD_MASTER        (1u << 2)
#define PCI_BAR0              0x10u
#define PCI_BAR1              0x14u
#define PCI_BAR_MEM64         0x4u       /* BAR type [2:1] = 2 */
#define PCI_BRIDGE_BUSES      0x18u      /* primary | secondary << 8 | subordinate << 16 */
#define PCI_BRIDGE_MEM_WINDOW 0x20u      /* base [15:4] = addr[31:20], limit [31:20] */

/* ---- MHI (mhi_internal.h); regs = BAR0 (cnss2 PCI_BAR_NUM 0) ---- */
#define MHI_REGLEN      0x00u
#define MHI_VER         0x08u
#define MHI_CFG         0x10u
#define MHI_BHIOFF      0x28u
#define MHI_BHIEOFF     0x2cu
#define MHI_CTRL        0x38u      /* RESET bit 1, MHISTATE [15:8] */
#define MHI_CTRL_RESET  (1u << 1)
#define MHI_STATUS      0x48u      /* READY bit 0, SYSERR bit 2, MHISTATE [15:8] */
#define MHI_STATUS_READY  (1u << 0)
#define MHI_STATUS_SYSERR (1u << 2)
#define MHI_SOC_HW_VERSION 0x224u  /* mhi_init.c: read from regs; major [15:8] */
/* BHI, at regs + BHIOFF */
#define BHI_IMGADDR_LOW   0x08u
#define BHI_IMGADDR_HIGH  0x0cu
#define BHI_IMGSIZE       0x10u
#define BHI_IMGTXDB       0x18u
#define BHI_TXDB_SEQ_MASK 0x3fffffffu
#define BHI_INTVEC        0x20u
#define BHI_EXECENV       0x28u
#define BHI_STATUS        0x2cu      /* [31:30]: 0 reset, 2 success, 3 error */
#define BHI_ERRCODE       0x30u
#define BHI_ERRDBG1       0x34u
#define BHI_ERRDBG2       0x38u
#define BHI_ERRDBG3       0x3cu
#define BHI_STATUS_SUCCESS 2u
#define BHI_STATUS_ERROR   3u
/* enum mhi_ee (include/linux/mhi.h) */
#define MHI_EE_PBL   0u
#define MHI_EE_SBL   1u
#define MHI_EE_AMSS  2u
#define MHI_EE_PTHRU 5u
#define MHI_EE_EDL   6u

/* cnss2/pci.c cnss_pci_register_mhi(): fbc_download, sbl_size = seg_len = 512K;
 * cnss_pci_update_fw_name(): QCA6490 major version 2 -> amss20.bin, else amss.bin
 * (major keeps only its low 4 bits, DEVICE_MAJOR_VERSION_MASK). */
#define LEMONADEP_MHI_SBL_BYTES  0x80000u
#define LEMONADEP_MHI_FW_V2      2u
/* cnss2/pci.c: QCA6490 DMA mask is 36 bits. */
#define LEMONADEP_MHI_DMA_LIMIT  (1ull << 36)
/* LIVE cnss node mhi,timeout = <10000> ms (polled here, 1 ms apart). */
#define LEMONADEP_MHI_TIMEOUT_MS 10000u

/* ---- apps SMMU (LIVE apps-smmu@15000000, qcom,skip-init: the bootloader's
 * stream mapping is kept). PCIe0's endpoint (RID 0x100) is stream 0x1c01
 * (LIVE iommu-map = <0x100 &apps_smmu 0x1c01 1>). arm-smmu.h, GR0 page. ---- */
#define LEMONADEP_APPS_SMMU_PA  0x15000000u
#define LEMONADEP_WLAN_SID      0x1c01u
#define SMMU_sCR0               0x000u
#define SMMU_sCR0_CLIENTPD      (1u << 0)
#define SMMU_sCR0_EXIDENABLE    (1u << 3)
#define SMMU_sCR0_USFCFG        (1u << 10)   /* 1: unmatched streams fault */
#define SMMU_ID0                0x020u       /* NUMSMRG [7:0] */
#define SMMU_sGFSR              0x048u
#define SMMU_SMR(n)             (0x800u + 4u * (n))
#define SMMU_S2CR(n)            (0xc00u + 4u * (n))
#define SMMU_SMR_VALID          (1u << 31)   /* without EXIDS */
#define SMMU_S2CR_EXIDVALID     (1u << 10)   /* with EXIDS */
#define SMMU_S2CR_TYPE(v)       (((v) >> 16) & 3u)   /* 0 trans, 1 bypass, 2 fault */
#define SMMU_S2CR_BYPASS        1u
#define SMMU_S2CR_SHCFG_NSH     3u           /* S2CR_SHCFG [9:8], what arm_smmu_write_s2cr() sets */
#endif
