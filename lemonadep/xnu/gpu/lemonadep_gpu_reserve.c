/*
 * lemonadep: GPU power/clock reserve -- CX GDSC enable and the seven real
 * clock branches the GMU needs (qcom,adreno-gmu-660.1 node's own clocks
 * list, mainline sm8350.dtsi), before any GMU/GPU register is touched.
 * Offsets from the real drivers/clk/qcom/{gpucc,gcc}-sm8350.c; see
 * LemonadepGmuSource.h for the per-branch citation and halt_check kind.
 * GX GDSC needs no separate step here: its real power-up is the GMU/RSC
 * sequence in lemonadep_gmu_coldstart.c (the real driver's own gx_gdsc
 * .power_on is a documented no-op -- see HANDOFF.md).
 *
 * Host-testable: injected I/O over two windows (Gpucc, Gcc), same shape as
 * lemonadep_gmu_coldstart.c.
 */
#include <stdbool.h>
#include <stdint.h>

#include "LemonadepGmuSource.h"

enum { LemonadepPwrWindowGpucc, LemonadepPwrWindowGcc, LemonadepPwrWindowCount };
typedef struct {
	void *cookie;
	int (*read32)(void *cookie, unsigned window, uint32_t byte_offset, uint32_t *out);
	int (*write32)(void *cookie, unsigned window, uint32_t byte_offset, uint32_t value);
	int (*now_us)(void *cookie, uint64_t *out);
	int (*wait_us)(void *cookie, unsigned us);
} lemonadep_pwr_io;

typedef enum { LemonadepPwrOk, LemonadepPwrBadArg, LemonadepPwrIoError, LemonadepPwrTimeout } lemonadep_pwr_status;

static lemonadep_pwr_status
poll_clear(const lemonadep_pwr_io *io, unsigned window, uint32_t off, uint32_t mask)
{
	uint64_t start, now;
	uint32_t value;
	if (io->now_us(io->cookie, &start)) {
		return LemonadepPwrIoError;
	}
	for (;;) {
		if (io->read32(io->cookie, window, off, &value)) {
			return LemonadepPwrIoError;
		}
		if (!(value & mask)) {
			return LemonadepPwrOk;
		}
		if (io->now_us(io->cookie, &now) || now < start) {
			return LemonadepPwrIoError;
		}
		if (now - start >= 10000) {
			return LemonadepPwrTimeout;
		}
		if (io->wait_us(io->cookie, 10)) {
			return LemonadepPwrIoError;
		}
	}
}
/* Real clk_branch2 pattern: the same idle CLK_OFF|HWCG bit pattern used for
 * m_ahb/s_ahb in sweet_touch.c's touch_clocks() -- a branch whose CLK_OFF
 * reads 1 while HWCG (bit1) is also set is legitimately idle-gated, not a
 * failed enable (same citation: Linux clk_branch_in_hwcg_mode). */
static lemonadep_pwr_status
enable_branch(const lemonadep_pwr_io *io, unsigned window, uint32_t off, bool skip_poll,
    bool hwcg_capable)
{
	uint32_t value;
	if (io->read32(io->cookie, window, off, &value)) {
		return LemonadepPwrIoError;
	}
	if (io->write32(io->cookie, window, off, value | LEMONADEP_CLK_ENABLE_BIT)) {
		return LemonadepPwrIoError;
	}
	if (skip_poll) {
		return LemonadepPwrOk;
	}
	if (hwcg_capable) {
		if (io->read32(io->cookie, window, off, &value)) {
			return LemonadepPwrIoError;
		}
		if ((value & LEMONADEP_CLK_OFF_BIT) && (value & LEMONADEP_CLK_HWCG_BIT)) {
			return LemonadepPwrOk;
		}
	}
	return poll_clear(io, window, off, LEMONADEP_CLK_OFF_BIT);
}

lemonadep_pwr_status
lemonadep_gpu_reserve(const lemonadep_pwr_io *io)
{
	uint32_t value;
	if (!io || !io->read32 || !io->write32 || !io->now_us || !io->wait_us) {
		return LemonadepPwrBadArg;
	}
	/* CX GDSC: write SW_COLLAPSE=0 to enable, poll PWR_ON set
	 * (gdsc_update_collapse_bit/gdsc_poll_status). */
	if (io->read32(io->cookie, LemonadepPwrWindowGpucc, LEMONADEP_GPUCC_GDSC_CX_OFFSET, &value)) {
		return LemonadepPwrIoError;
	}
	if (io->write32(io->cookie, LemonadepPwrWindowGpucc, LEMONADEP_GPUCC_GDSC_CX_OFFSET,
	    value & ~LEMONADEP_GDSC_SW_COLLAPSE_BIT)) {
		return LemonadepPwrIoError;
	}
	for (uint64_t start, now;;) {
		if (io->now_us(io->cookie, &start)) {
			return LemonadepPwrIoError;
		}
		if (io->read32(io->cookie, LemonadepPwrWindowGpucc, LEMONADEP_GPUCC_GDSC_CX_OFFSET,
		    &value)) {
			return LemonadepPwrIoError;
		}
		if (value & LEMONADEP_GDSC_PWR_ON_BIT) {
			break;
		}
		if (io->now_us(io->cookie, &now) || now < start) {
			return LemonadepPwrIoError;
		}
		if (now - start >= 10000) {
			return LemonadepPwrTimeout;
		}
		if (io->wait_us(io->cookie, 10)) {
			return LemonadepPwrIoError;
		}
	}

	/* GPUCC branches (window Gpucc). */
	lemonadep_pwr_status status;
	if ((status = enable_branch(io, LemonadepPwrWindowGpucc,
	    LEMONADEP_GPUCC_AHB_CLK_OFFSET, false, false)) != LemonadepPwrOk) {
		return status;
	}
	if ((status = enable_branch(io, LemonadepPwrWindowGpucc,
	    LEMONADEP_GPUCC_CX_GMU_CLK_OFFSET, false, false)) != LemonadepPwrOk) {
		return status;
	}
	if ((status = enable_branch(io, LemonadepPwrWindowGpucc,
	    LEMONADEP_GPUCC_CXO_CLK_OFFSET, false, false)) != LemonadepPwrOk) {
		return status;
	}
	if ((status = enable_branch(io, LemonadepPwrWindowGpucc,
	    LEMONADEP_GPUCC_HLOS1_VOTE_GPU_SMMU_CLK_OFFSET, false, false)) != LemonadepPwrOk) {
		return status;
	}
	if ((status = enable_branch(io, LemonadepPwrWindowGpucc,
	    LEMONADEP_GPUCC_HUB_CX_INT_CLK_OFFSET, false, false)) != LemonadepPwrOk) {
		return status;
	}
	/* GCC branches (window Gcc), both hwcg_bit=1 in the real source. */
	if ((status = enable_branch(io, LemonadepPwrWindowGcc,
	    LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET, true, true)) != LemonadepPwrOk) {
		return status; /* BRANCH_HALT_SKIP: never polled, real driver behavior */
	}
	if ((status = enable_branch(io, LemonadepPwrWindowGcc,
	    LEMONADEP_GCC_GPU_MEMNOC_GFX_CLK_OFFSET, false, true)) != LemonadepPwrOk) {
		return status;
	}
	return LemonadepPwrOk;
}
