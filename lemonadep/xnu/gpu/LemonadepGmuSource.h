/* SPDX-License-Identifier: MIT */
#ifndef LEMONADEP_GMU_SOURCE_H
#define LEMONADEP_GMU_SOURCE_H
#include <stdint.h>
/*
 * The board-specific facts the real GMU cold-start sequence
 * (LemonadepA660HwInit.h; see lemonadep/HANDOFF.md "GPU, Phase 2") needs
 * that aren't register offsets: the five MMIO window physical
 * addresses/sizes and the real pulled firmware's size/hash. Written by
 * hand from sourced facts below, not derived by a script (unlike
 * LemonadepA660HwInit.h) -- there is no XML/catalog table for these, only
 * the real device tree node and the firmware file already on the phone.
 */

/* qcom,adreno-660.1 node, mainline sm8350.dtsi (fetched 2026-10-23; see the
 * ML-GPU source tag in HWMAP_lemonadep.md). Window order matches
 * ShengGmuWindows (UniXNU/NativeAdreno's NativeAdrenoGmuPlatform.h):
 * Core, Rscc, Pdc, Gpu, CxMisc. */
#define LEMONADEP_GMU_CORE_PA 0x03d6a000u
#define LEMONADEP_GMU_CORE_BYTES 0x34000u
#define LEMONADEP_GMU_RSCC_PA 0x03de0000u
#define LEMONADEP_GMU_RSCC_BYTES 0x10000u
#define LEMONADEP_GMU_PDC_PA 0x0b290000u
#define LEMONADEP_GMU_PDC_BYTES 0x10000u
/* kgsl_3d0_reg_memory -- the main GPU register window (CP ring, RBBM, UCHE). */
#define LEMONADEP_GMU_GPU_PA 0x03d00000u
#define LEMONADEP_GMU_GPU_BYTES 0x40000u
/* cx_mem -- confirmed this is the CX_MISC window by reading the real
 * a6xx_gpu.c: `a6xx_gpu->cx_misc_mmio = msm_ioremap(pdev, "cx_mem")`, not
 * cx_dbgc (a separate, unrelated debug-cluster window this driver doesn't
 * need). */
#define LEMONADEP_GMU_CX_MISC_PA 0x03d9e000u
#define LEMONADEP_GMU_CX_MISC_BYTES 0x1000u

/* GPUCC (clock controller, GPU_CX_GDSC/GPU_GX_GDSC power domains) and the
 * Adreno SMMU, same dtsi node, needed for the clock/power/IOMMU bring-up
 * this file does not cover yet. */
#define LEMONADEP_GPUCC_PA 0x03d90000u
#define LEMONADEP_GPUCC_BYTES 0x9000u
/* GCC base, already sourced for touch/keys (HWMAP_lemonadep.md section 5):
 * 0x100000, real size 0x1f0000, but this driver only reaches the two GPU
 * branches above (highest real offset used: 0x71154+4), so it maps a
 * smaller window of its own rather than sharing sweet_touch.c's GCC
 * mapping across drivers. */
#define LEMONADEP_GCC_PA 0x00100000u
#define LEMONADEP_GCC_GPU_WINDOW_BYTES 0x72000u
#define LEMONADEP_ADRENO_SMMU_PA 0x03da0000u
#define LEMONADEP_ADRENO_SMMU_BYTES 0x20000u

/* pil_gpu_mem reserved-memory node, same dtsi (also recorded in
 * HWMAP_lemonadep.md section 1): the zap-shader PIL load target. */
#define LEMONADEP_PIL_GPU_MEM_PA 0x8b51a000ull
#define LEMONADEP_PIL_GPU_MEM_BYTES 0x2000u

/* GIC SPIs, same dtsi node: GPU fault/idle IRQ, GMU HFI and GMU IRQs. */
#define LEMONADEP_GPU_IRQ_SPI 300u
#define LEMONADEP_GMU_HFI_IRQ_SPI 304u
#define LEMONADEP_GMU_IRQ_SPI 305u

/* CX GDSC (GPUCC window, drivers/clk/qcom/gpucc-sm8350.c gpu_cx_gdsc.gdscr):
 * real software enable needed before anything else -- write SW_COLLAPSE
 * (bit 0) to 0, poll PWR_ON (bit 31) set (drivers/clk/qcom/gdsc.c
 * gdsc_update_collapse_bit/gdsc_poll_status). GX GDSC (gpu_gx_gdsc.gdscr
 * = 0x100c) is a documented software no-op (.power_on =
 * gdsc_gx_do_nothing_enable in the real driver) -- its real power-up is
 * the GMU/RSC hardware sequence already in LemonadepA660HwInit.h, not a
 * separate software step. */
#define LEMONADEP_GPUCC_GDSC_CX_OFFSET 0x106cu
#define LEMONADEP_GDSC_SW_COLLAPSE_BIT (1u << 0)
#define LEMONADEP_GDSC_PWR_ON_BIT (1u << 31)

/* Clocks GMU needs, from the qcom,adreno-gmu-660.1 node's own clocks list
 * (mainline sm8350.dtsi): five GPUCC branches + two GCC branches, offsets
 * from the real drivers/clk/qcom/{gpucc,gcc}-sm8350.c (fetched 2026-10-23).
 * Standard clk_branch2 semantics: enable_reg bit0 = CLK_ENABLE, same
 * register's bit31 = CLK_OFF (poll clear), except the BRANCH_HALT_SKIP one
 * (gcc_ddrss_gpu_axi_clk -- never poll, same as BRANCH_HALT_DELAY/VOTED
 * branches with an hwcg_bit set can legitimately read CLK_OFF=1 while
 * idle, exactly the touch_clocks() HWCG pattern already used for m_ahb/
 * s_ahb; see HWMAP_lemonadep.md section 8). */
#define LEMONADEP_GPUCC_AHB_CLK_OFFSET 0x1078u            /* BRANCH_HALT_DELAY */
#define LEMONADEP_GPUCC_CX_GMU_CLK_OFFSET 0x1098u          /* BRANCH_HALT */
#define LEMONADEP_GPUCC_CXO_CLK_OFFSET 0x109cu              /* BRANCH_HALT */
#define LEMONADEP_GPUCC_HLOS1_VOTE_GPU_SMMU_CLK_OFFSET 0x5000u /* BRANCH_HALT_VOTED */
#define LEMONADEP_GPUCC_HUB_CX_INT_CLK_OFFSET 0x1204u      /* BRANCH_HALT */
#define LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET 0x71154u    /* BRANCH_HALT_SKIP, hwcg_bit=1: never poll */
#define LEMONADEP_GCC_GPU_MEMNOC_GFX_CLK_OFFSET 0x7100cu   /* BRANCH_HALT_VOTED, hwcg_bit=1 */
#define LEMONADEP_CLK_ENABLE_BIT (1u << 0)
#define LEMONADEP_CLK_OFF_BIT (1u << 31)
#define LEMONADEP_CLK_HWCG_BIT (1u << 1)

/* The real a660_gmu.bin already pulled from this phone's own /vendor
 * (lemonadep/fw-gpu/, fw-gpu/pull.sh) and parsed with the real non-legacy
 * block-loader algorithm (lemonadep/xnu/gpu/verify-gmu-firmware.py; see
 * HANDOFF.md "GPU, Phase 2" for why A660 takes this path, not
 * ShengGmu.c's fixed-region one). Re-run that script and update this
 * block if the firmware file ever changes. */
/* GMU memory regions (icache/dummy/debug/log/hfi): sizes and IOVAs, verified
 * against the real a6xx_gmu_init() allocation calls for A660 specifically
 * (a6xx_gmu.c:2316-2378), not assumed equal to ShengGmu.c's A740 values just
 * because they're the same numbers. They turn out to genuinely be the same,
 * because A660 takes the identical real branches A740 does for every one of
 * these sizes (adreno_is_a650_family() for icache, and the explicit
 * `adreno_is_a660_family() || adreno_is_a7xx() || adreno_is_a8xx()` branch
 * for the debug/dummy pair -- the real source's own comment there: "A660
 * now requires handling 'prealloc requests' in GMU firmware... hardcode
 * allocations based on the known firmware"). log/hfi have no fixed IOVA
 * (auto-assigned by the real allocator); icache/dummy/debug do. */
#define LEMONADEP_GMU_ICACHE_BYTES 0xffc000u  /* SZ_16M - SZ_16K */
#define LEMONADEP_GMU_ICACHE_IOVA 0x4000ull
#define LEMONADEP_GMU_DUMMY_BYTES 0x4000u     /* SZ_16K, a660_family's size */
#define LEMONADEP_GMU_DUMMY_IOVA 0x60000000ull
#define LEMONADEP_GMU_DEBUG_BYTES 0x7000u     /* SZ_4K * 7 */
#define LEMONADEP_GMU_DEBUG_IOVA 0x60400000ull
#define LEMONADEP_GMU_LOG_BYTES 0x4000u       /* SZ_16K */
#define LEMONADEP_GMU_HFI_BYTES 0x4000u       /* SZ_16K */

/* The GMU SMMU context's prepare() aperture must span all five regions at
 * once (icache near 0x4000, dummy/debug/log/hfi clustered past
 * 0x60000000) -- ArmSmmu4KLive's fixed topology only reserves
 * intermediate/leaf tables within the span prepare() was given, so a
 * region outside that span can never be mapped later no matter how much
 * table budget is left (see lemonadep/HANDOFF.md "GPU, Phase 6
 * continued"). Derived from the real IOVAs/sizes above, not hand-typed,
 * to avoid repeating the mmio_offset/firmware-offset class of mistake
 * this project already caught twice. */
#define LEMONADEP_GMU_APERTURE_IOVA 0ull
#define LEMONADEP_GMU_APERTURE_BYTES \
	(LEMONADEP_GMU_DEBUG_IOVA + LEMONADEP_GMU_DEBUG_BYTES + LEMONADEP_GMU_LOG_BYTES + LEMONADEP_GMU_HFI_BYTES)
/* Must be >= APERTURE_BYTES/4096 (ArmSmmu4KLive's layout() rejects a
 * smaller budget outright, treating it as the ceiling on how much of the
 * aperture could ever be mapped) and <= NativeAdrenoSmmuContext::MaxLeaves
 * (524288); margin added, not tuned to the exact minimum. */
#define LEMONADEP_GMU_LEAF_BUDGET ((unsigned)(LEMONADEP_GMU_APERTURE_BYTES / 4096u) + 16u)
/* Table-pool page budget for that aperture. The exact page count
 * ArmSmmu4KLive's layout() needs (774) is verified empirically by
 * test_gpu_memory.c, not hand-computed from the LPAE level formula --
 * this is a generous multiple of it, not the tuned minimum. */
#define LEMONADEP_GMU_TABLE_BUDGET_PAGES 1024u

#define LEMONADEP_GMU_FW_BYTES 55256u
#define LEMONADEP_GMU_FW_SHA256_HEX \
	"6bb9360a1601253a86ee3ae1b9b916f5f1832a72d55e9b8573567f1740c94e2b"
/* Real, parsed, payload-bearing blocks only (the loader skips the 11
 * zero-size ones; header+payload bytes sum to exactly 55256, confirming
 * the parse). Each entry: {firmware byte offset, length, target}. Targets:
 * 0 = ITCM (CM3_ITCM_START + offset/4), 1 = DTCM (CM3_DTCM_START +
 * offset/4), 2 = icache memory (icache.cpu + (addr - 0x4000)). */
enum { LemonadepGmuFwTargetItcm, LemonadepGmuFwTargetDtcm, LemonadepGmuFwTargetIcache };
struct LemonadepGmuFwBlock { uint32_t file_offset, addr, bytes, target; };
static const struct LemonadepGmuFwBlock LemonadepGmuFwBlocks[4] = {
	{192,    0x00000000u,   476u, LemonadepGmuFwTargetItcm},
	{684,    0x00004000u, 54476u, LemonadepGmuFwTargetIcache},
	{55176,  0x10007f98u,    40u, LemonadepGmuFwTargetDtcm},
	{55232,  0x10007fdcu,    24u, LemonadepGmuFwTargetDtcm},
};
#endif
