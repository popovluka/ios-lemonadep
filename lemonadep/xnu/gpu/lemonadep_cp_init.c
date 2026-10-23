/*
 * lemonadep: the real A660 CP_ME_INIT sequence. UniXNU/NativeAdreno's own
 * vendored NativeAdrenoCpInit.cpp implements a7xx_cp_init() (its own
 * comment says so directly: "The payload values are a7xx_cp_init in
 * a6xx_gpu.c"), which is genuinely a different packet, not the same one at
 * a different address -- confirmed from the real a6xx_gpu.c:
 *
 *   ret = adreno_is_a7xx(adreno_gpu) ? a7xx_cp_init(gpu) : a6xx_cp_init(gpu);
 *
 * A660 is not a7xx, so it takes a6xx_cp_init(): no CP_THREAD_CONTROL
 * prefix packet (that disables concurrent binning, an A7XX-only feature),
 * and an 8-word CP_ME_INIT payload with a different mask/layout than the
 * A7XX one -- not the power-up-reglist-carrying 7-word payload
 * NativeAdrenoCpInit.cpp builds. See lemonadep/HANDOFF.md "GPU, Phase 5"
 * for the full citation (a6xx_gpu.c:895-920 vs 924-968).
 *
 * New, lemonadep-specific logic, built on lemonadep_ring.c (this
 * project's own real A660 ring, not NativeAdrenoRing -- see HANDOFF.md
 * for why that one isn't usable here either) and AdrenoPM4Type7()
 * (confirmed generic: CP_ME_INIT = 0x48, untagged in adreno_pm4.xml,
 * shared across every a6xx generation).
 */
#include <stdint.h>

#include "AdrenoPM4.h"
#include "lemonadep_ring.c"

enum { LemonadepCpMeInitOpcode = 0x48, LemonadepCpMeInitWords = 9 /* 1 header + 8 payload */ };

/*
 * Runs the real a6xx (non-A7XX) CP_ME_INIT and waits for its fence.
 * Precondition: CP_SQE_INSTR_BASE already programmed by the caller, ring
 * already attached.
 */
lemonadep_ring_status
lemonadep_a6xx_cp_init(const lemonadep_ring_io *io, lemonadep_ring *ring, uint32_t timeout_us,
    uint32_t *fence)
{
	if (!io || !ring || !fence) {
		return LemonadepRingBadArg;
	}
	*fence = 0;
	uint32_t words[LemonadepCpMeInitWords] = {0};
	if (AdrenoPM4Type7(LemonadepCpMeInitOpcode, 8, &words[0]) != AdrenoPM4Success) {
		return LemonadepRingBadArg;
	}
	/* The real a6xx_cp_init() payload, OUT_RING for OUT_RING
	 * (a6xx_gpu.c:895-920) -- not the a7xx mask/layout. */
	words[1] = 0x0000002fu;
	words[2] = 0x00000003u; /* enable multiple hardware contexts */
	words[3] = 0x20000000u; /* enable error detection */
	words[4] = 0x00000000u; /* don't enable header dump (2 words) */
	words[5] = 0x00000000u;
	words[6] = 0x00000000u; /* no workarounds enabled */
	words[7] = 0x00000000u; /* pad */
	words[8] = 0x00000000u; /* pad */

	uint32_t value = 0;
	lemonadep_ring_status status = lemonadep_ring_submit(io, ring, words, LemonadepCpMeInitWords, &value);
	if (status != LemonadepRingOk) {
		return status;
	}
	status = lemonadep_ring_wait(io, ring, value, timeout_us);
	if (status != LemonadepRingOk) {
		return status;
	}
	*fence = value;
	return LemonadepRingOk;
}
