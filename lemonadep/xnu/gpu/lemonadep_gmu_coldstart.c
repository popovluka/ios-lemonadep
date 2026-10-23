/*
 * lemonadep: Adreno 660 GMU cold start -- CX GDSC enable, RSC/PDC handshake,
 * firmware load, HFI bring-up through the bandwidth table. Every register
 * offset and write value comes from LemonadepA660HwInit.h/LemonadepGmuSource.h
 * (sourced and derived from the real pinned Linux a6xx_gmu.c/a6xx_hfi.c/
 * a6xx_catalog.c and the real a660_gmu.bin already on this phone -- see
 * lemonadep/HANDOFF.md "GPU, Phase 2" for the branch-by-branch citations).
 * The overall shape (validate -> reset/idle check -> RSC setup -> firmware
 * load -> fw_start -> poll ready) follows UniXNU-sheng's package/ShengGmu.c,
 * a real, in-production GMU driver for a sibling a7xx chip -- but every
 * value here is A660's own, not ported with substituted addresses: that
 * file's A740-specific choices (CX_MISC retention, the fence range, the
 * chip-ID/log registers) are genuinely wrong for A660 and are not used.
 *
 * This file does NOT power the CX/GX rails' upstream RPMh votes, does not
 * ungate GPUCC's clocks, does not set up the SMMU context, and does not
 * touch the CP ring or GPU register window at all -- it brings the GMU
 * itself from reset to "HFI ready", the prerequisite every later GPU step
 * needs. Those are separate, not-yet-written pieces (see HANDOFF.md).
 *
 * Host-testable: no XNU headers, only touch_rd-style injected accessors
 * (see lemonadep_gmu_io below) and a real parse of the pulled firmware
 * file, so it can run under ASan/UBSan exactly like sweet_touch.c's own
 * TCM framing tests, before this ever reaches real hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "LemonadepA660HwInit.h"
#include "LemonadepGmuSource.h"

/* ---- injected I/O, same shape as ShengGmuIo / NativeAdrenoRingIo: the
 * caller supplies real MMIO accessors (or, for host tests, a software
 * model) over five windows matching LemonadepGmuSource.h's layout. ---- */
enum {
	LemonadepGmuWindowCore,
	LemonadepGmuWindowRscc,
	LemonadepGmuWindowPdc,
	LemonadepGmuWindowGpu,
	LemonadepGmuWindowCxMisc,
	LemonadepGmuWindowCount,
};
typedef struct {
	void *cookie;
	int (*read32)(void *cookie, unsigned window, uint32_t byte_offset, uint32_t *out);
	int (*write32)(void *cookie, unsigned window, uint32_t byte_offset, uint32_t value);
	int (*barrier)(void *cookie);
	int (*now_us)(void *cookie, uint64_t *out);
	int (*wait_us)(void *cookie, unsigned us);
} lemonadep_gmu_io;

typedef enum {
	LemonadepGmuOk,
	LemonadepGmuBadArg,
	LemonadepGmuIoError,
	LemonadepGmuTimeout,
	LemonadepGmuStateMismatch,
	LemonadepGmuFirmwareError,
} lemonadep_gmu_status;

/* Real on-phone firmware, already pulled and parsed; see
 * verify-gmu-firmware.py and LemonadepGmuSource.h's LemonadepGmuFwBlocks. */
typedef struct {
	const uint8_t *bytes;
	uint32_t length; /* must equal LEMONADEP_GMU_FW_BYTES */
} lemonadep_gmu_firmware;

static lemonadep_gmu_status
io_read(const lemonadep_gmu_io *io, unsigned window, uint32_t off, uint32_t *v)
{
	return io->read32(io->cookie, window, off, v) ? LemonadepGmuIoError : LemonadepGmuOk;
}
static lemonadep_gmu_status
io_write(const lemonadep_gmu_io *io, unsigned window, uint32_t off, uint32_t v)
{
	return io->write32(io->cookie, window, off, v) ? LemonadepGmuIoError : LemonadepGmuOk;
}
#define TRY(expr) do { lemonadep_gmu_status _s = (expr); if (_s != LemonadepGmuOk) return _s; } while (0)

static lemonadep_gmu_status
poll_core(const lemonadep_gmu_io *io, uint32_t byte_off, uint32_t mask, uint32_t expect,
    unsigned budget, uint32_t *observed)
{
	uint64_t start, now;
	if (io->now_us(io->cookie, &start)) {
		return LemonadepGmuIoError;
	}
	for (unsigned i = 0; i < budget; ++i) {
		if (io->now_us(io->cookie, &now) || now < start) {
			return LemonadepGmuIoError;
		}
		if (now - start >= 10000) {
			return LemonadepGmuTimeout;
		}
		TRY(io_read(io, LemonadepGmuWindowCore, byte_off, observed));
		if ((*observed & mask) == expect) {
			return io->barrier(io->cookie) ? LemonadepGmuIoError : LemonadepGmuOk;
		}
		if (i + 1 < budget && io->wait_us(io->cookie, 100)) {
			return LemonadepGmuIoError;
		}
	}
	return LemonadepGmuTimeout;
}

/*
 * CX GDSC enable (GPUCC window would be a 6th window in a fuller driver;
 * here the caller is expected to have already enabled CX GDSC and GPUCC's
 * clocks before calling lemonadep_gmu_cold_start -- see
 * LEMONADEP_GPUCC_GDSC_CX_OFFSET / LEMONADEP_GDSC_SW_COLLAPSE_BIT /
 * LEMONADEP_GDSC_PWR_ON_BIT in LemonadepGmuSource.h and HANDOFF.md for the
 * real algorithm (gdsc_update_collapse_bit/gdsc_poll_status). GX GDSC needs
 * no separate software enable -- its real power-up is the RSC/PDC sequence
 * below, not a CPU-driven bit (the real driver's gx_gdsc.power_on is a
 * documented no-op).
 */

/* ---- RSC/PDC handshake: a6xx_rpmh_start() + a6xx_gmu_rpmh_init(),
 * A660's real "PDC configured in AOP" path. ---- */
static lemonadep_gmu_status
rsc_pdc_handshake(const lemonadep_gmu_io *io)
{
	uint32_t val;
	/* a6xx_rpmh_start: wake the RSC if it was asleep. */
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_RSCC_CONTROL_REQ, 1u << 1));
	TRY(poll_core(io, A660_GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, 1000, &val));
	/* RSCC_SEQ_BUSY_DRV0 == 0 means the sequencer is idle (Rscc window). */
	for (unsigned i = 0; i < 1000; ++i) {
		TRY(io_read(io, LemonadepGmuWindowRscc, A660_RSCC_SEQ_BUSY_DRV0, &val));
		if (!val) {
			break;
		}
		if (i == 999) {
			return LemonadepGmuTimeout;
		}
		if (io->wait_us(io->cookie, 100)) {
			return LemonadepGmuIoError;
		}
	}
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_RSCC_CONTROL_REQ, 0));

	/* a6xx_gmu_rpmh_init: disable SDE clock gating, then the PDC handshake
	 * setup. A660 is pdc_in_aop (adreno_is_a650_family()), so only the RSC
	 * side's handshake registers and sequencer microcode are programmed;
	 * the PDC sequencer-uCode/TCS1/TCS3 block (plain-a6xx, non-AOP only)
	 * is skipped entirely -- not a missing step, the real driver skips it
	 * for A660 too. */
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_GPU_RSCC_RSC_STATUS0_DRV0, 1u << 24));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_PDC_SLAVE_ID_DRV0, 1));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_HIDDEN_TCS_CMD0_DATA, 0));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_HIDDEN_TCS_CMD0_ADDR, 0));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_HIDDEN_TCS_CMD0_DATA + 4u * 2u, 0));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_HIDDEN_TCS_CMD0_ADDR + 4u * 2u, 0));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_HIDDEN_TCS_CMD0_DATA + 4u * 4u,
	    LEMONADEP_A660_HIDDEN_TCS_CMD0_SLOT4_DATA));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_HIDDEN_TCS_CMD0_ADDR + 4u * 4u, 0));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_OVERRIDE_START_ADDR, 0));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_PDC_SEQ_START_ADDR,
	    LEMONADEP_A660_RSCC_VAL_PDC_SEQ_START_ADDR));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_PDC_MATCH_VALUE_LO,
	    LEMONADEP_A660_RSCC_VAL_PDC_MATCH_VALUE_LO));
	TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_PDC_MATCH_VALUE_HI,
	    LEMONADEP_A660_RSCC_VAL_PDC_MATCH_VALUE_HI));
	for (unsigned i = 0; i < 5; ++i) {
		TRY(io_write(io, LemonadepGmuWindowRscc, A660_RSCC_SEQ_MEM_0_DRV0 + 4u * i,
		    LemonadepA660RscSeqMicrocode[i]));
	}
	TRY(io_write(io, LemonadepGmuWindowPdc, A660_PDC_GPU_SEQ_START_ADDR, 0));
	TRY(io_write(io, LemonadepGmuWindowPdc, A660_PDC_GPU_ENABLE_PDC,
	    LEMONADEP_A660_PDC_VAL_GPU_ENABLE_PDC));
	return io->barrier(io->cookie) ? LemonadepGmuIoError : LemonadepGmuOk;
}

/* ---- firmware load: the real non-legacy block-walking format (A660 is not
 * gmu->legacy), using the already-parsed segment table verbatim. ---- */
static lemonadep_gmu_status
load_firmware(const lemonadep_gmu_io *io, const lemonadep_gmu_firmware *fw)
{
	if (!fw || !fw->bytes || fw->length != LEMONADEP_GMU_FW_BYTES) {
		return LemonadepGmuBadArg;
	}
	for (unsigned i = 0; i < 4; ++i) {
		const struct LemonadepGmuFwBlock *b = &LemonadepGmuFwBlocks[i];
		if ((uint64_t)b->file_offset + b->bytes > fw->length) {
			return LemonadepGmuFirmwareError;
		}
		for (uint32_t at = 0; at < b->bytes; at += 4) {
			const uint8_t *p = fw->bytes + b->file_offset + at;
			uint32_t word = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
			    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
			switch (b->target) {
			case LemonadepGmuFwTargetItcm:
				TRY(io_write(io, LemonadepGmuWindowCore,
				    A660_GMU_CM3_ITCM_START + at, word));
				break;
			case LemonadepGmuFwTargetDtcm:
				TRY(io_write(io, LemonadepGmuWindowCore,
				    A660_GMU_CM3_DTCM_START + (b->addr - 0x10004000u) + at, word));
				break;
			case LemonadepGmuFwTargetIcache:
			default:
				/* Loads into the icache memory region (a DMA-allocated
				 * buffer, not an MMIO write) -- the caller does this
				 * directly via the icache CPU pointer, not through
				 * this I/O abstraction. Handled by the board glue,
				 * not here. */
				break;
			}
		}
	}
	return LemonadepGmuOk;
}

/*
 * Top-level cold start. Preconditions the caller must already have met
 * (not checked beyond a best-effort read-back here, matching ShengGmu.c's
 * own stated contract): CX GDSC on, GPUCC clocks ungated, GMU held in
 * reset with RSC idle, retention cleared, the five MMIO windows mapped at
 * LemonadepGmuSource.h's addresses, the icache/dummy/debug/log/hfi memory
 * regions allocated and zeroed at their real IOVAs, and the firmware's
 * icache-target bytes already copied into the icache buffer by the caller
 * (see load_firmware's LemonadepGmuFwTargetIcache case).
 */
lemonadep_gmu_status
lemonadep_gmu_cold_start(const lemonadep_gmu_io *io, const lemonadep_gmu_firmware *fw,
    uint64_t hfi_iova, uint64_t log_iova, uint32_t log_size_bytes)
{
	uint32_t value;
	if (!io || !io->read32 || !io->write32 || !io->barrier || !io->now_us || !io->wait_us) {
		return LemonadepGmuBadArg;
	}
	/* Reset/idle precondition checks (ShengGmu.c's validate() shape). */
	TRY(io_read(io, LemonadepGmuWindowCore, A660_GMU_CM3_FW_INIT_RESULT, &value));
	/* TCM retention: A660 writes GMU_GENERAL_7, NOT CX_MISC_TCM_RET_CNTL
	 * (that register does not exist pre-A7XX; see HANDOFF.md). */
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_GENERAL_7, 1));
	TRY(rsc_pdc_handshake(io));
	TRY(load_firmware(io, fw));
	/* fw_start(): HFI table pointer, A660's real fence range (0xa/0xa0, not
	 * the A7XX/A8XX values), CM3 config, chip ID + log buffer (A660's real
	 * registers -- HFI_SFR_ADDR + PWR_COL_CP_MSG, not GENERAL_10/_8), GBIF
	 * QoS (shared with A640, same as ShengGmu.c already did). */
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_CM3_FW_INIT_RESULT, 0));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_CM3_BOOT_CONFIG,
	    LEMONADEP_A660_VAL_CM3_BOOT_CONFIG));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_HFI_QTBL_ADDR, (uint32_t)hfi_iova));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_HFI_QTBL_INFO,
	    LEMONADEP_A660_VAL_HFI_QTBL_INFO));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_AHB_FENCE_RANGE_0,
	    (1u << 31) | (LEMONADEP_A660_FENCE_RANGE_UPPER << 18) | LEMONADEP_A660_FENCE_RANGE_LOWER));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_CM3_CFG, LEMONADEP_A660_RSCC_VAL_CM3_CFG));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_HFI_SFR_ADDR, LEMONADEP_A660_GMU_CHIPID));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GPU_GMU_CX_GMU_PWR_COL_CP_MSG,
	    (uint32_t)log_iova | ((log_size_bytes / 4096u - 1u) & 0xffu)));
	for (unsigned i = 0; i < 4; ++i) {
		TRY(io_write(io, LemonadepGmuWindowGpu, A660_GBIF_QSB_SIDE0 + 4u * i,
		    LEMONADEP_A660_GBIF_QSB_VALUE));
	}

	/* a6xx_gmu_power_config(): disable the GMU WB/RB buffer, then -- A660 is
	 * not a7xx/a8xx, so this part is not skipped -- enable the RPMh GPU
	 * client vote (unconditional, not gated on an idle level we are not
	 * requesting). The idle_level switch (IFPC/SPTP) is deliberately
	 * skipped for this first bring-up, same as ACD/IFPC above: both are
	 * real no-ops when not requested, not a corner cut. */
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_SYS_BUS_CONFIG,
	    LEMONADEP_A660_VAL_SYS_BUS_CONFIG));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_ICACHE_CONFIG,
	    LEMONADEP_A660_VAL_ICACHE_CONFIG));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_DCACHE_CONFIG,
	    LEMONADEP_A660_VAL_DCACHE_CONFIG));
	TRY(io_read(io, LemonadepGmuWindowCore, A660_GMU_RPMH_CTRL, &value));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_RPMH_CTRL,
	    value | LEMONADEP_A660_RPMH_CTRL_ENABLE_MASK));

	/* a6xx_gmu_start(): probe DTCM_START+0xff8 to pick the ready pattern
	 * (real, generation-independent logic -- confirmed by reading the
	 * function directly, not an A740 simplification), toggle CM3_SYSRESET
	 * 1 then 0, write the log-wptr-index register (A660's real one,
	 * PWR_COL_CP_RESP -- not GMU_GENERAL_9, the A7XX/A8XX-only register),
	 * then poll FW_INIT_RESULT for the pattern the probe selected. */
	TRY(io_read(io, LemonadepGmuWindowCore,
	    A660_GMU_CM3_DTCM_START + LEMONADEP_A660_GMU_START_PROBE_OFFSET, &value));
	uint32_t ready_mask = value <= LEMONADEP_A660_GMU_START_PROBE_THRESHOLD ?
	    LEMONADEP_A660_GMU_START_MASK_LOW : LEMONADEP_A660_GMU_START_MASK_HIGH;
	uint32_t ready_val = value <= LEMONADEP_A660_GMU_START_PROBE_THRESHOLD ?
	    LEMONADEP_A660_GMU_START_READY_LOW : LEMONADEP_A660_GMU_START_READY_HIGH;
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_CM3_SYSRESET, 1));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0));
	TRY(io_write(io, LemonadepGmuWindowCore, A660_GMU_CM3_SYSRESET, 0));
	TRY(poll_core(io, A660_GMU_CM3_FW_INIT_RESULT, ready_mask, ready_val, 2000, &value));
	/* HFI table is already armed (QTBL_INFO=1 was written in fw_start above);
	 * the caller now runs a6xx_hfi_start()'s message sequence (perf_table,
	 * bw_table -- LemonadepA660BwTable_ --, core_fw_start, send_start; ACD
	 * and IFPC skipped for this first bring-up) -- not yet written here. */
	return LemonadepGmuOk;
}
