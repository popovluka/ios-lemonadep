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

/* The real a660_gmu.bin already pulled from this phone's own /vendor
 * (lemonadep/fw-gpu/, fw-gpu/pull.sh) and parsed with the real non-legacy
 * block-loader algorithm (lemonadep/xnu/gpu/verify-gmu-firmware.py; see
 * HANDOFF.md "GPU, Phase 2" for why A660 takes this path, not
 * ShengGmu.c's fixed-region one). Re-run that script and update this
 * block if the firmware file ever changes. */
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
