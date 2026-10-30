/* SPDX-License-Identifier: MIT */
#ifndef LEMONADEP_SMMU_SOURCE_H
#define LEMONADEP_SMMU_SOURCE_H
/*
 * Giving a device DMA through lemonadep's application-processor SMMU
 * (LIVE apps-smmu@15000000, an Arm MMU-500, "qcom,skip-init").
 *
 * Bypass is not the way in. On this phone the hypervisor answers for the
 * SMMU's global registers and rewrites an S2CR bypass to FAULT: we wrote
 * 0x10300 and read back 0x20300 (logs/lemonadep-wlan10.txt). Mainline's
 * context-bank workaround (arm-smmu-qcom.c, SCTLR.M = 0) was taken, but the
 * chip's first DMA then put the SoC in an XBL ramdump
 * (logs/lemonadep-wlan11.txt).
 *
 * What the hypervisor does grant -- verified on this same phone by the
 * collaborator's port (furangelz/hackios-xnu-kernel, branch lemonadep,
 * xnu/files/bsd/dev/redwood_wifi.c "the radio's stream in the SMMU", and
 * their ios-lemonadep NOTES.md 2026-10-13: "the hypervisor grants it ...
 * the radio then read and wrote the memory it was shown without a fault")
 * is a real stage-1 translation context with page tables of ours:
 *
 *   - take the lowest stream-match register that is not valid, and the
 *     lowest context that no valid match register sends a stream through
 *     and whose SCTLR.M is 0. On their run the boot chain had match
 *     registers 0..7 and contexts 0..6 in use; our own run saw the same
 *     first free match register, 8 (logs/lemonadep-wlan10.txt).
 *   - program the context with the context switched off, then switch it on,
 *     and only then point the match register at it (order below).
 *   - read back afterwards: the hypervisor may keep less than was written.
 *   - the device is then given IOVAs, never physical addresses. Both this
 *     chip and theirs use the window the DT hands the driver: LIVE
 *     cnss_pci_iommu_group qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>
 *     (256 MB at 0xa0000000), the same values their WCN6750 node has.
 *
 * Their chip is a WCN6750 on AHB and ours a QCA6490 on PCIe, so none of
 * their radio driver applies -- but the SMMU is the same instance on the
 * same SoC, so the programming does.
 */
#include <stdint.h>

#define LEMONADEP_APPS_SMMU_BYTES 0x100000u   /* LIVE "reg" of apps-smmu */
/* Global page (GR0) */
#define SMMU_GR0_SCR0       0x000u   /* bit 0 SMMU off, bit 3 16-bit stream ids, bit 10 unmatched faults */
#define SMMU_GR0_IDR0       0x020u   /* [7:0] number of stream-match registers */
#define SMMU_GR0_IDR1       0x024u   /* bit 31 64K pages, [30:28] log2(pages)-1, [7:0] contexts */
#define SMMU_GR0_SGFSR      0x048u   /* what the SMMU as a whole refused */
#define SMMU_GR0_SGFSYNR0   0x050u
#define SMMU_GR0_SGFSYNR1   0x054u   /* names the stream */
#define SMMU_GR0_SMR(n)     (0x800u + 4u * (n))   /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_GR0_S2CR(n)    (0xc00u + 4u * (n))   /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_SMR_VALID      (1u << 31)
#define SMMU_S2CR_TRANSLATE 0u
/* Second page (GR1): one word per context */
#define SMMU_GR1_CBAR(n)    (4u * (n))            /* [17:16] kind of context */
#define SMMU_GR1_CBA2R(n)   (0x800u + 4u * (n))
#define SMMU_CBAR_STAGE_1   (1u << 16 | 3u << 8 | 0xfu << 12)  /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R_64_BIT   1u
/* A context's own page, at (pages + context) * page */
#define SMMU_CB_SCTLR       0x000u   /* bit 0 = translation on */
#define SMMU_CB_TCR2        0x010u
#define SMMU_CB_TTBR0       0x020u
#define SMMU_CB_TTBR1       0x028u
#define SMMU_CB_TCR         0x030u
#define SMMU_CB_MAIR0       0x038u
#define SMMU_CB_MAIR1       0x03cu
#define SMMU_CB_FSR         0x058u
#define SMMU_CB_FAR         0x060u
#define SMMU_CB_FSYNR0      0x068u
#define SMMU_CB_TLBIASID    0x610u
#define SMMU_CB_TLBSYNC     0x7f0u
#define SMMU_CB_TLBSTATUS   0x7f4u
/* on; faults recorded, no interrupt, and a fault does not hold up later requests */
#define SMMU_SCTLR_ON       (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS    (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)  /* one table of 4K pages, cached walks */
#define SMMU_TCR2_36_BIT_PA (1u | 1u << 4 | 7u << 15)   /* 36 bits of PA, 16 of context tag */
#define SMMU_MAIR0_LINUX    0xf404ff44u   /* 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL        0xc00001feu   /* every kind of fault, and "several" */
#define SMMU_TLB_TRIES      100u          /* 1 ms apart */

/* Page-table entries (stage 1, 4K pages), as Linux uses for a device. */
#define SMMU_PTE_TABLE      3ull
#define SMMU_PTE_PAGE       (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define SMMU_PTE_ADDRESS    0x0000fffffffff000ull

/* The chip's own address window (LIVE cnss_pci_iommu_group). */
#define LEMONADEP_WLAN_IOVA_BASE  0xa0000000ull
#define LEMONADEP_WLAN_IOVA_SIZE  0x10000000u
/* 39-bit VA, 3 levels of 4K: one root, one second level, 2 MB per third. */
#define SMMU_TABLE_BYTES    4096u
#define SMMU_PT_LEVEL1(a)   (((a) >> 30) & 0x1ffu)
#define SMMU_PT_LEVEL2(a)   (((a) >> 21) & 0x1ffu)
#define SMMU_PT_LEVEL3(a)   (((a) >> 12) & 0x1ffu)
#endif
