/* SPDX-License-Identifier: MIT */
/*
 * lemonadep: top-level GPU bring-up sequencing -- reserve power/clocks,
 * cold-start the GMU, attach the CP ring, and run the real a6xx
 * CP_ME_INIT. A successful *fence completion* here is the actual
 * milestone the user asked for: proof the Adreno 660 executed a real
 * submitted command, not just that register writes didn't fault (see
 * lemonadep/HANDOFF.md "GPU, Phase 6 continued").
 *
 * This file only sequences plain-C pieces already written and
 * host-tested individually, #included directly -- the same convention
 * lemonadep_cp_init.c already uses for lemonadep_ring.c. SMMU context
 * setup, buffer allocation (lemonadep_gpu_memory.cpp, C++/IOKit) and the
 * real MMIO io-struct wiring happen one layer up, in the IOKit service;
 * this function receives them already built. The icache firmware bytes
 * (load_firmware()'s LemonadepGmuFwTargetIcache case) are a plain memcpy
 * into the already-mapped icache buffer, the caller's job, same as the
 * other preconditions lemonadep_gmu_cold_start() itself documents.
 */
#include <stdint.h>

#include "lemonadep_gpu_reserve.c"
#include "lemonadep_gmu_coldstart.c"
#include "lemonadep_cp_init.c" /* pulls in lemonadep_ring.c */

typedef enum {
	LemonadepBootOk,
	LemonadepBootBadArg,
	LemonadepBootPowerFailed,
	LemonadepBootGmuFailed,
	LemonadepBootRingFailed,
	LemonadepBootCpInitFailed,
} lemonadep_boot_status;

/*
 * Preconditions the caller is responsible for, in order: SMMU banks
 * discovered, GMU+GPU SMMU contexts programmed and attached, every GMU/
 * ring/fence region allocated, zeroed and mapped at its real IOVA, the
 * icache firmware bytes already copied in, CP_SQE_INSTR_BASE already
 * programmed. On success, `fence_out` holds CP_ME_INIT's completed
 * sequence number.
 */
lemonadep_boot_status
lemonadep_gpu_boot(const lemonadep_pwr_io *pwr_io, const lemonadep_gmu_io *gmu_io,
    const lemonadep_gmu_firmware *fw, uint64_t hfi_iova, uint64_t log_iova, uint32_t log_size_bytes,
    const lemonadep_ring_io *ring_io, lemonadep_ring *ring, volatile uint32_t *ring_mem,
    uint32_t ring_dwords, uint64_t ring_iova, volatile uint32_t *fence_word, uint64_t fence_iova,
    uint32_t cp_init_timeout_us, uint32_t *fence_out, lemonadep_gmu_phase *gmu_phase)
{
	if (!pwr_io || !gmu_io || !fw || !ring_io || !ring || !fence_out) {
		return LemonadepBootBadArg;
	}
	*fence_out = 0;
	if (lemonadep_gpu_reserve(pwr_io) != LemonadepPwrOk) {
		return LemonadepBootPowerFailed;
	}
	/* Always runs to completion here -- a partial GMU with the ring/CP_ME_INIT
	 * still attempted after it would make no sense. Staged stop_after testing
	 * (HANDOFF.md "GPU, Phase 8 continued") is for lemonadep_gpu_init.c, which
	 * doesn't reach the ring/cp_init stage yet anyway. */
	if (lemonadep_gmu_cold_start(gmu_io, fw, hfi_iova, log_iova, log_size_bytes, gmu_phase,
	    LemonadepGmuPhaseNone) != LemonadepGmuOk) {
		return LemonadepBootGmuFailed;
	}
	if (lemonadep_ring_attach(ring_io, ring, ring_mem, ring_dwords, ring_iova, fence_word, fence_iova)
	    != LemonadepRingOk) {
		return LemonadepBootRingFailed;
	}
	if (lemonadep_a6xx_cp_init(ring_io, ring, cp_init_timeout_us, fence_out) != LemonadepRingOk) {
		return LemonadepBootCpInitFailed;
	}
	return LemonadepBootOk;
}
