/* SPDX-License-Identifier: MIT */
/*
 * lemonadep: incremental SMMU mapping of the GMU's five fixed regions
 * (icache/dummy/debug/log/hfi) into an already attach()ed context, via
 * the real vendored ArmSmmu4KLiveUpdate (see lemonadep/HANDOFF.md "GPU,
 * Phase 6 continued" for why prepare()'s aperture had to widen first).
 * Plain C, no IOKit, so this is host-testable exactly like
 * lemonadep_gpu_smmu.c -- see test_gpu_memory.c for the full real
 * prepare+program+attach+map cycle this drives, including the mock
 * SMMU-500 register file ArmSmmu4KProgramDisabledContext/ObserveOwnedContext
 * need.
 */
#include "lemonadep_gpu_smmu_map.h"

/*
 * Maps `regions` one at a time via ArmSmmu4KLiveUpdate, each call's
 * "before" list growing with everything already mapped -- exactly what
 * that function documents as required (before = all currently mapped
 * resources, sorted by IOVA). `regions` must already be sorted by iova
 * ascending and disjoint (LemonadepGmuSource.h's layout already is: icache
 * < dummy < debug < log < hfi). `scratch` must hold at least `count`
 * ArmSmmu4KRange entries and is left holding the mapped ranges on success.
 */
ArmSmmu4KStatus
lemonadep_gmu_map_regions(const ArmSmmu4KLiveArena *arena, ArmSmmu4KLiveState *state,
    const ArmSmmu4KIo *io, unsigned bank, const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveVisibility *visibility, const lemonadep_gmu_region *regions,
    size_t count, ArmSmmu4KRange *scratch, unsigned poll_budget)
{
	if (!regions || !scratch) {
		return ArmSmmu4KArgument;
	}
	for (size_t i = 0; i < count; ++i) {
		ArmSmmu4KRange change = {
			.iova = regions[i].iova,
			.physical = regions[i].physical,
			.bytes = regions[i].bytes,
			/* Normal DMA-coherent data buffers (not GMU-executable --
			 * the GMU core fetches icache contents into its own
			 * internal cache, it does not execute through this SMMU
			 * mapping), matching the context's coherent_walk=1. */
			.permissions = ArmSmmu4KRead | ArmSmmu4KWrite,
			.writeback = 1,
			.shareability = 3,
		};
		ArmSmmu4KLiveResult result;
		ArmSmmu4KStatus status = ArmSmmu4KLiveUpdate(arena, state, io, bank, context,
		    visibility, scratch, i, &change, 1, poll_budget, &result);
		if (status != ArmSmmu4KSuccess) {
			return status;
		}
		scratch[i] = change;
	}
	return ArmSmmu4KSuccess;
}
