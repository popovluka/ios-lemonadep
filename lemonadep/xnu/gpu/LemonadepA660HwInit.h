/* SPDX-License-Identifier: MIT */
#ifndef LEMONADEP_A660_HWINIT_H
#define LEMONADEP_A660_HWINIT_H
#include <stdint.h>
/* Derived (by lemonadep/xnu/test/derive-a660-hwinit.py, adapted from UniXNU/UniXNU's
 * scripts/derive-a740-hwinit.py for the A660 -- plain a6xx gen4, not an A7XX variant) from the
 * pinned ianchb/sm8550-mainline @ 42f3b40c (sha256-verified against UniXNU's own
 * scripts/data/adreno-inputs.json record): registers/adreno/{a6xx,adreno_common,a6xx_gmu}.xml and
 * adreno/a6xx_catalog.c (a660_hwcg, a660_protect_regs, DECLARE_ADRENO_PROTECT(a660_protect,48)).
 * Dword register offsets. Do not edit by hand; re-run the deriving script instead. */
#define A660_GBIF_HALT 0x03c45
#define A660_RBBM_GBIF_HALT 0x00016
#define A660_RBBM_SECVID_TSB_CNTL 0x0f803
#define A660_RBBM_SECVID_TSB_TRUSTED_BASE 0x0f800
#define A660_RBBM_SECVID_TSB_TRUSTED_SIZE 0x0f802
#define A660_RBBM_GBIF_CLIENT_QOS_CNTL 0x00011
#define A660_UCHE_GBIF_GX_CONFIG 0x00e3a
#define A660_RBBM_PERFCTR_GPU_BUSY_MASKED 0x0050b
#define A660_UCHE_TRAP_BASE 0x00e09
#define A660_UCHE_WRITE_THRU_BASE 0x00e07
#define A660_UCHE_GMEM_RANGE_MIN 0x00e0b
#define A660_UCHE_GMEM_RANGE_MAX 0x00e0d
#define A660_UCHE_CACHE_WAYS 0x00e17
#define A660_CP_AHB_CNTL 0x0098d
#define A660_RBBM_PERFCTR_CNTL 0x00500
#define A660_RBBM_INTERFACE_HANG_INT_CNTL 0x0001f
#define A660_UCHE_CLIENT_PF 0x00e19
#define A660_CP_PROTECT_CNTL 0x0084f
#define A660_UCHE_CMDQ_CONFIG 0x00e3c
#define A660_CP_APRIV_CNTL 0x00844
#define A660_RB_CMP_DBG_ECO_CNTL 0x08e28
#define A660_RBBM_INT_0_MASK 0x00038
#define A660_CP_SQE_INSTR_BASE 0x00830
#define A660_CP_RB_BASE 0x00800
#define A660_CP_RB_CNTL 0x00802
#define A660_CP_RB_RPTR_ADDR 0x00804
#define A660_CP_SQE_CNTL 0x00808
#define A660_CP_RB_RPTR 0x00806
#define A660_CP_RB_WPTR 0x00807
#define A660_RBBM_STATUS 0x00210
#define A660_RBBM_INT_0_STATUS 0x00201
#define A660_CP_HW_FAULT 0x00821
#define A660_CP_INTERRUPT_STATUS 0x00823
#define A660_RBBM_CLOCK_CNTL 0x000ae
#define A660_RBBM_SECVID_TRUST_CNTL 0x0f400
#define A660_RBBM_STATUS3 0x00213
#define A660_CP_SQE_STAT_ADDR 0x00908
#define A660_CP_SQE_STAT_DATA 0x00909
#define A660_RB_NC_MODE_CNTL 0x08e08
#define A660_TPL1_NC_MODE_CNTL 0x0b604
#define A660_SP_NC_MODE_CNTL 0x0ae02
#define A660_CP_APERTURE_CNTL_HOST 0x00a00
#define A660_UCHE_MODE_CNTL 0x00e01
#define A660_RBBM_NC_MODE_CNTL 0x00534
#define A660_RBBM_INT_CLEAR_CMD 0x00037
#define A660_CP_ALWAYS_ON_COUNTER 0x00980
#define A660_GPU_GMU_AO_GMU_CGC_MODE_CNTL 0x23b09
#define A660_GPU_GMU_AO_GMU_CGC_DELAY_CNTL 0x23b0a
#define A660_GPU_GMU_AO_GMU_CGC_HYST_CNTL 0x23b0b
#define A660_CP_PROTECT 0x00850
static const uint32_t LemonadepA660Hwcg[][2]={
    {0x000b0,0x02222222},   /* RBBM_CLOCK_CNTL_SP0 */
    {0x000b4,0x02222220},   /* RBBM_CLOCK_CNTL2_SP0 */
    {0x000b8,0x00000080},   /* RBBM_CLOCK_DELAY_SP0 */
    {0x000bc,0x0000f3cf},   /* RBBM_CLOCK_HYST_SP0 */
    {0x000c0,0x22222222},   /* RBBM_CLOCK_CNTL_TP0 */
    {0x000c4,0x22222222},   /* RBBM_CLOCK_CNTL2_TP0 */
    {0x000c8,0x22222222},   /* RBBM_CLOCK_CNTL3_TP0 */
    {0x000cc,0x00022222},   /* RBBM_CLOCK_CNTL4_TP0 */
    {0x000d0,0x11111111},   /* RBBM_CLOCK_DELAY_TP0 */
    {0x000d4,0x11111111},   /* RBBM_CLOCK_DELAY2_TP0 */
    {0x000d8,0x11111111},   /* RBBM_CLOCK_DELAY3_TP0 */
    {0x000dc,0x00011111},   /* RBBM_CLOCK_DELAY4_TP0 */
    {0x000e0,0x77777777},   /* RBBM_CLOCK_HYST_TP0 */
    {0x000e4,0x77777777},   /* RBBM_CLOCK_HYST2_TP0 */
    {0x000e8,0x77777777},   /* RBBM_CLOCK_HYST3_TP0 */
    {0x000ec,0x00077777},   /* RBBM_CLOCK_HYST4_TP0 */
    {0x000f0,0x22222222},   /* RBBM_CLOCK_CNTL_RB0 */
    {0x000f4,0x01002222},   /* RBBM_CLOCK_CNTL2_RB0 */
    {0x000f8,0x00002220},   /* RBBM_CLOCK_CNTL_CCU0 */
    {0x00100,0x00040f00},   /* RBBM_CLOCK_HYST_RB_CCU0 */
    {0x00104,0x25222022},   /* RBBM_CLOCK_CNTL_RAC */
    {0x00105,0x00005555},   /* RBBM_CLOCK_CNTL2_RAC */
    {0x00106,0x00000011},   /* RBBM_CLOCK_DELAY_RAC */
    {0x00107,0x00445044},   /* RBBM_CLOCK_HYST_RAC */
    {0x00108,0x04222222},   /* RBBM_CLOCK_CNTL_TSE_RAS_RBBM */
    {0x00111,0x00002222},   /* RBBM_CLOCK_MODE_VFD */
    {0x00114,0x00222222},   /* RBBM_CLOCK_MODE_GPC */
    {0x00117,0x00000002},   /* RBBM_CLOCK_DELAY_HLSQ_2 */
    {0x0011b,0x00002222},   /* RBBM_CLOCK_MODE_HLSQ */
    {0x00109,0x00004000},   /* RBBM_CLOCK_DELAY_TSE_RAS_RBBM */
    {0x00112,0x00002222},   /* RBBM_CLOCK_DELAY_VFD */
    {0x00115,0x00000200},   /* RBBM_CLOCK_DELAY_GPC */
    {0x0011c,0x00000000},   /* RBBM_CLOCK_DELAY_HLSQ */
    {0x0010a,0x00000000},   /* RBBM_CLOCK_HYST_TSE_RAS_RBBM */
    {0x00113,0x00000000},   /* RBBM_CLOCK_HYST_VFD */
    {0x00116,0x04104004},   /* RBBM_CLOCK_HYST_GPC */
    {0x0011d,0x00000000},   /* RBBM_CLOCK_HYST_HLSQ */
    {0x00120,0x00000222},   /* RBBM_CLOCK_CNTL_TEX_FCHE */
    {0x00121,0x00000111},   /* RBBM_CLOCK_DELAY_TEX_FCHE */
    {0x00122,0x00000000},   /* RBBM_CLOCK_HYST_TEX_FCHE */
    {0x0010b,0x22222222},   /* RBBM_CLOCK_CNTL_UCHE */
    {0x00110,0x00000004},   /* RBBM_CLOCK_HYST_UCHE */
    {0x0010f,0x00000002},   /* RBBM_CLOCK_DELAY_UCHE */
    {0x00533,0x00000182},   /* RBBM_ISDB_CNT */
    {0x00044,0x00000000},   /* RBBM_RAC_THRESHOLD_CNT */
    {0x00042,0x00000000},   /* RBBM_SP_HYST_CNT */
    {0x00118,0x00000222},   /* RBBM_CLOCK_CNTL_GMU_GX */
    {0x00119,0x00000111},   /* RBBM_CLOCK_DELAY_GMU_GX */
    {0x0011a,0x00000555},   /* RBBM_CLOCK_HYST_GMU_GX */
};
#define LEMONADEP_A660_HWCG_COUNT 49
/* {CP_PROTECT index, value} in a6xx_set_cp_protect order. */
static const uint32_t LemonadepA660Protect[][2]={
    { 0,0x13fc0000},
    { 1,0x00140501},
    { 2,0x0bd0050b},
    { 3,0x8000050e},
    { 4,0x80000510},
    { 5,0x80000534},
    { 6,0x82080800},
    { 7,0x802008a0},
    { 8,0x809008ab},
    { 9,0x02b808de},
    {10,0x81340900},
    {11,0x89c8098d},
    {12,0x80040e00},
    {13,0x80300e03},
    {14,0x830c3c00},
    {15,0x7ffc3cc4},
    {16,0x873c8630},
    {17,0x80008e00},
    {18,0x80008e08},
    {19,0x807c8e50},
    {20,0x89fc8e80},
    {21,0x876c9624},
    {22,0x80449e60},
    {23,0x861c9e78},
    {24,0x873ca630},
    {25,0x8000ae02},
    {26,0x84bcae50},
    {27,0x8000b604},
    {28,0x8018b608},
    {29,0x8004be02},
    {30,0x857cbe20},
    {31,0x97fcd000},
    {32,0xaffcf000},
    {33,0x7ffcfc00},
    {34,0xfffd8400},
    {35,0xfffda400},
    {36,0x910df400},
    {37,0x01edf844},
    {38,0x8001f860},
    {39,0x806df887},
    {47,0x8001f8c0},
};
#define LEMONADEP_A660_PROTECT_COUNT 41

/* GMU cold-start / RSC-PDC handshake (a6xx_gmu_fw_start, a6xx_rpmh_start,
 * a6xx_gmu_rpmh_init in the pinned a6xx_gmu.c), A660's real traced path --
 * see lemonadep/HANDOFF.md "GPU, Phase 2 continued" for the branch-by-branch
 * citations. Byte offsets (register offset << 2), not dword, to match the
 * ShengGmuIo read32/write32(window, byte_offset, ...) contract. */
#define A660_GMU_RSCC_CONTROL_REQ 0x08ec1c
#define A660_GMU_RSCC_CONTROL_ACK 0x08ec20
#define A660_GMU_GENERAL_7 0x07e730
#define A660_GPU_RSCC_RSC_STATUS0_DRV0 0x000010
#define A660_RSCC_PDC_SLAVE_ID_DRV0 0x00002c
#define A660_RSCC_HIDDEN_TCS_CMD0_DATA 0x000038
#define A660_RSCC_HIDDEN_TCS_CMD0_ADDR 0x000034
#define A660_RSCC_OVERRIDE_START_ADDR 0x000400
#define A660_RSCC_PDC_SEQ_START_ADDR 0x000020
#define A660_RSCC_PDC_MATCH_VALUE_LO 0x000024
#define A660_RSCC_PDC_MATCH_VALUE_HI 0x000028
#define A660_RSCC_SEQ_MEM_0_DRV0 0x000600
#define A660_RSCC_SEQ_BUSY_DRV0 0x000404
#define A660_PDC_GPU_ENABLE_PDC 0x004500
#define A660_PDC_GPU_SEQ_START_ADDR 0x004520
/* HIDDEN_TCS_CMD0 DATA/ADDR pairs step by +2 dwords per slot (0,2,4); A660
 * writes slot 4's DATA as 0x80000000 (not A740's 0x80000021 -- A660 is not
 * adreno_is_a740_family()). RSC sequencer microcode: A660 takes the
 * a650_family/a7xx branch (not the plain-a6xx values). PDC is in the
 * "configured in AOP" mode (pdc_in_aop=true for a650_family), so only the
 * two PDC_GPU_* writes at the end happen -- the PDC sequencer uCode and
 * TCS1/TCS3 command block (plain-a6xx/non-AOP path only) do not apply here. */
/* Values this sequence writes INTO the registers above, as distinct from the
 * A660_* MMIO offsets above (A660_RSCC_PDC_SEQ_START_ADDR is where to write;
 * RSCC_VAL_PDC_SEQ_START_ADDR, which happens to equal A660_PDC_GPU_SEQ_START_ADDR
 * numerically, is what to write there -- do not confuse the two). */
#define LEMONADEP_A660_HIDDEN_TCS_CMD0_SLOT4_DATA 0x80000000u
static const uint32_t LemonadepA660RscSeqMicrocode[5]={
    0xeaaae5a0u,0xe1a1ebabu,0xa2e0a581u,0xecac82e2u,0x0020edadu,
};
#define LEMONADEP_A660_RSCC_VAL_PDC_SEQ_START_ADDR 0x4520u
#define LEMONADEP_A660_RSCC_VAL_PDC_MATCH_VALUE_LO 0x4510u
#define LEMONADEP_A660_RSCC_VAL_PDC_MATCH_VALUE_HI 0x4514u
#define LEMONADEP_A660_PDC_VAL_GPU_ENABLE_PDC 0x80000001u

/* Rest of a6xx_gmu_fw_start() (Core window): HFI table pointer, AHB fence
 * range, CM3 config, chip ID + log buffer registration, GBIF QoS. A660 is NOT
 * adreno_is_a7xx()/a8xx(), so it takes the plain-a6xx branches, which genuinely
 * differ from ShengGmu.c's A740 values in two places -- confirmed against the
 * real a6xx_gmu_fw_start(), not assumed:
 *   - fence range is (upper=0xa,lower=0xa0), NOT A7XX/A8XX's (0x32,0x8a0/0x8c0)
 *     the exact value ShengGmu.c writes for the A740.
 *   - chip ID and the log buffer IOVA go to GMU_HFI_SFR_ADDR and
 *     GPU_GMU_CX_GMU_PWR_COL_CP_MSG, NOT GMU_GENERAL_10/GMU_GENERAL_8 (those
 *     are the A7XX/A8XX-only registers ShengGmu.c uses). */
#define A660_GMU_CM3_FW_INIT_RESULT 0x07e070
#define A660_GMU_CM3_BOOT_CONFIG 0x07e004
#define A660_GMU_HFI_QTBL_ADDR 0x07e614
#define A660_GMU_HFI_QTBL_INFO 0x07e610
#define A660_GMU_AHB_FENCE_RANGE_0 0x08ec44
#define A660_GMU_CM3_CFG 0x07e0b4
#define A660_GMU_HFI_SFR_ADDR 0x07e608
#define A660_GPU_GMU_CX_GMU_PWR_COL_CP_MSG 0x07e400
#define A660_GBIF_QSB_SIDE0 0x00f00c
#define LEMONADEP_A660_FENCE_RANGE_UPPER 0xau
#define LEMONADEP_A660_FENCE_RANGE_LOWER 0xa0u
/* AHB_FENCE_RANGE_0 = BIT(31) | upper<<18 | lower, per the real FIELD_PREP calls. */
#define LEMONADEP_A660_RSCC_VAL_CM3_CFG 0x4052u  /* avoids the snapshot/NMI jump */
#define LEMONADEP_A660_VAL_HFI_QTBL_INFO 1u
#define LEMONADEP_A660_VAL_CM3_BOOT_CONFIG 2u
/* a640_gbif (shared by A660, confirmed in a6xx_catalog.c): write this same value
 * to GBIF_QSB_SIDE0..SIDE3 (consecutive dwords, +1 each). */
#define LEMONADEP_A660_GBIF_QSB_VALUE 0x00071620u

/* a660_build_bw_table() in the pinned a6xx_hfi.c -- transcribed verbatim, not
 * derived (it is already hand-written per chip in the real source, dispatched
 * for us by adreno_is_a660(), an exact chip_id==660 match, confirmed our
 * a6xx_catalog.c entry has no .bcms override that would select a different,
 * dynamically-generated table instead). Part of the real, non-legacy
 * a6xx_hfi_start() sequence: send_perf_table, THIS, enable_acd, enable_ifpc,
 * send_core_fw_start, send_start (a6xx_hfi.c:972-1005; A660 is not
 * gmu->legacy -- that is only RGMU chips and adreno_is_a630_family(), GEN1). */
struct LemonadepA660BwTable {
    uint32_t bw_level_num;
    uint32_t ddr_cmds_num;
    uint32_t ddr_wait_bitmask;
    uint32_t ddr_cmds_addrs[3];
    uint32_t ddr_cmds_data0[3];  /* only level 0 ("off") is populated */
    uint32_t cnoc_cmds_num;
    uint32_t cnoc_wait_bitmask;
    uint32_t cnoc_cmds_addrs[1];
    uint32_t cnoc_cmds_data0[1];
    uint32_t cnoc_cmds_data1[1];
};
static const struct LemonadepA660BwTable LemonadepA660BwTable_ = {
    .bw_level_num = 1,
    .ddr_cmds_num = 3, .ddr_wait_bitmask = 0x01,
    .ddr_cmds_addrs = {0x50004,0x500a0,0x50000},
    .ddr_cmds_data0 = {0x40000000,0x40000000,0x40000000},
    .cnoc_cmds_num = 1, .cnoc_wait_bitmask = 0x01,
    .cnoc_cmds_addrs = {0x50070},
    .cnoc_cmds_data0 = {0x40000000}, .cnoc_cmds_data1 = {0x60000001},
};
#endif
