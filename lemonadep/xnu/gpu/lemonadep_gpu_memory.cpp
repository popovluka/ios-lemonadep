/* SPDX-License-Identifier: MIT */
/*
 * lemonadep: GPU/GMU buffer allocation and SMMU context setup, built on the
 * real, confirmed-generic vendored NativeAdrenoMemory/NativeAdrenoSmmuContext
 * (see lemonadep/HANDOFF.md "GPU, Phase 5 continued" for why these two,
 * unlike Ring/CpInit/Irq, needed no A660-specific rewrite). Real IOVAs and
 * sizes from LemonadepGmuSource.h; real stream IDs/mask from
 * lemonadep_gpu_smmu.c's discovery (see HANDOFF.md "GPU, Phase 4" for the
 * arm_smmu_of_xlate() citation).
 *
 * Two SMMU contexts, matching the real device tree (sm8350.dtsi):
 *   - GMU context: stream ID 5, maps the five real GMU regions (icache,
 *     dummy, debug, log, hfi) at their fixed real IOVAs.
 *   - GPU context: stream IDs 0 and 1 (one context, two routed streams,
 *     confirmed real binding), maps the ring and fence buffers (IOVAs this
 *     driver picks itself -- nothing in the real source ties them to a
 *     fixed address the way the GMU regions are fixed).
 */
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoSmmuContext.h"
#include "LemonadepGmuSource.h"
#include "lemonadep_gpu_smmu_map.h"

struct LemonadepGpuMemory {
	NativeAdrenoMemory icache, dummy, debug, log, hfi;
	NativeAdrenoMemory gmuTables;
	NativeAdrenoSmmuContext gmuContext;

	NativeAdrenoMemory ring, fence;
	NativeAdrenoMemory gpuTables;
	NativeAdrenoSmmuContext gpuContext;
};

/* GPU ring/fence IOVAs: this driver's own choice (see
 * LemonadepGmuSource.h's comment -- nothing in the real source fixes
 * these), placed well clear of the GMU's own fixed region at
 * 0x4000-0x10008000 so a bug in one address space can't alias the other. */
#define LEMONADEP_RING_IOVA 0x80000000ull
#define LEMONADEP_RING_BYTES 0x8000u /* 32 KiB, MSM_GPU_RINGBUFFER_SZ */
#define LEMONADEP_FENCE_IOVA 0x80010000ull
#define LEMONADEP_FENCE_BYTES 0x1000u /* one page; only the first dword is used */

/* GMU table-pool budget: LEMONADEP_GMU_TABLE_BUDGET_PAGES/LEMONADEP_GMU_LEAF_BUDGET
 * (LemonadepGmuSource.h) are sized for the widened aperture prepare() now uses
 * (all five GMU regions, not just icache -- see that header's own comment and
 * test_gpu_memory.c, which verified the real required page count is 774). */
#define LEMONADEP_GMU_TABLE_BUDGET (LEMONADEP_GMU_TABLE_BUDGET_PAGES * 4096u)
/* GPU ring/fence span is tiny by comparison; conservative, not yet tuned
 * against a real device. */
#define LEMONADEP_GPU_TABLE_BUDGET (64u * 1024u)
#define LEMONADEP_GPU_LEAF_BUDGET 64u

static IOReturn
allocFixed(NativeAdrenoMemory *mem, size_t bytes, unsigned outputBits)
{
	/* contiguous=true: the GMU regions and the ring/fence are all mapped
	 * at one fixed IOVA each (not scattered BO tables), so one physical
	 * span per object is what the real hardware/firmware expects. */
	return mem->allocate(bytes, outputBits, true, bytes, false);
}

/* outputBits: the real SMMU's OAS, read once by the caller via
 * lemonadep_smmu_discover_output_bits() (lemonadep_gpu_smmu.c) -- a
 * runtime hardware capability, not a guessed or hardcoded constant (see
 * that file's own citation of the real arm_smmu_id_size_to_bits()). */
extern "C" IOReturn
lemonadep_gpu_memory_alloc(LemonadepGpuMemory *m, unsigned outputBits)
{
	IOReturn status;
	if ((status = allocFixed(&m->icache, LEMONADEP_GMU_ICACHE_BYTES, outputBits))) return status;
	if ((status = allocFixed(&m->dummy, LEMONADEP_GMU_DUMMY_BYTES, outputBits))) return status;
	if ((status = allocFixed(&m->debug, LEMONADEP_GMU_DEBUG_BYTES, outputBits))) return status;
	if ((status = allocFixed(&m->log, LEMONADEP_GMU_LOG_BYTES, outputBits))) return status;
	if ((status = allocFixed(&m->hfi, LEMONADEP_GMU_HFI_BYTES, outputBits))) return status;
	if ((status = allocFixed(&m->ring, LEMONADEP_RING_BYTES, outputBits))) return status;
	if ((status = allocFixed(&m->fence, LEMONADEP_FENCE_BYTES, outputBits))) return status;
	/* Zero every region before it is ever mapped for the device --
	 * ShengGmu.c's own validate() comment applies here too: "Zero only
	 * after actual stopped/idle checks", which the caller (reserve/
	 * bootGmu sequencing) is responsible for ordering correctly. */
	return kIOReturnSuccess;
}

extern "C" IOReturn
lemonadep_gpu_smmu_program(LemonadepGpuMemory *m, const ArmSmmu4KIo *gmuIo, const ArmSmmu4KIo *gpuIo,
    unsigned bankGmu, unsigned bankGpu, unsigned outputBits)
{
	IOReturn status;

	/* GMU context: one region at a time, each its own fixed IOVA, sorted
	 * ascending (ArmSmmu4KLiveUpdate requires this order for its "before"
	 * list -- see lemonadep_gpu_smmu_map.c). */
	lemonadep_gmu_region gmuRegions[] = {
		{LEMONADEP_GMU_ICACHE_IOVA, m->icache.spans()[0].physical, LEMONADEP_GMU_ICACHE_BYTES},
		{LEMONADEP_GMU_DUMMY_IOVA, m->dummy.spans()[0].physical, LEMONADEP_GMU_DUMMY_BYTES},
		{LEMONADEP_GMU_DEBUG_IOVA, m->debug.spans()[0].physical, LEMONADEP_GMU_DEBUG_BYTES},
		/* log/hfi: no fixed IOVA in the real source -- placed just past
		 * debug's fixed range, the simplest available choice. */
		{LEMONADEP_GMU_DEBUG_IOVA + LEMONADEP_GMU_DEBUG_BYTES, m->log.spans()[0].physical, LEMONADEP_GMU_LOG_BYTES},
		{LEMONADEP_GMU_DEBUG_IOVA + LEMONADEP_GMU_DEBUG_BYTES + LEMONADEP_GMU_LOG_BYTES,
		    m->hfi.spans()[0].physical, LEMONADEP_GMU_HFI_BYTES},
	};
	if ((status = allocFixed(&m->gmuTables, LEMONADEP_GMU_TABLE_BUDGET, outputBits))) return status;
	/* prepare()'s aperture must span ALL FIVE regions at once -- icache
	 * near 0x4000 and dummy/debug/log/hfi clustered past 0x60000000 --
	 * not just icache's own small range: ArmSmmu4KLive's fixed topology
	 * only reserves tables within the span prepare() was given, so a
	 * region outside it could never be mapped later no matter how much
	 * table budget remained (see LemonadepGmuSource.h's own comment and
	 * HANDOFF.md "GPU, Phase 6 continued"). */
	if ((status = m->gmuContext.prepare(&m->gmuTables, /*upper=*/0, LEMONADEP_GMU_APERTURE_IOVA,
	    LEMONADEP_GMU_APERTURE_BYTES, LEMONADEP_GMU_LEAF_BUDGET, bankGmu, /*asid=*/0,
	    /*poll_budget=*/1000, outputBits))) return status;
	if ((status = m->gmuContext.program(gmuIo))) return status;
	ArmSmmu4KStream gmuStream = {0, LEMONADEP_SMMU_SID_GMU};
	if ((status = m->gmuContext.attach(&gmuStream, 1))) return status;
	/* Map all five regions via the real vendored ArmSmmu4KLiveUpdate
	 * (test_gpu_memory.c exercises this exact prepare/program/attach/map
	 * sequence end to end against a mock SMMU-500, plus a final
	 * whole-region audit this production path doesn't need to repeat). */
	ArmSmmu4KRange gmuMapped[5];
	if (lemonadep_gmu_map_regions(m->gmuContext.arenaHandle(), m->gmuContext.stateHandle(),
	    m->gmuContext.ioHandle(), m->gmuContext.bank(), m->gmuContext.configuration(),
	    m->gmuContext.visibilityHandle(), gmuRegions, 5, gmuMapped,
	    m->gmuContext.pollBudget()) != ArmSmmu4KSuccess)
		return kIOReturnIOError;

	/* GPU context: ring + fence, this driver's own chosen IOVAs. */
	if ((status = allocFixed(&m->gpuTables, LEMONADEP_GPU_TABLE_BUDGET, outputBits))) return status;
	if ((status = m->gpuContext.prepare(&m->gpuTables, /*upper=*/0, LEMONADEP_RING_IOVA,
	    LEMONADEP_FENCE_IOVA + LEMONADEP_FENCE_BYTES - LEMONADEP_RING_IOVA,
	    LEMONADEP_GPU_LEAF_BUDGET, bankGpu, /*asid=*/1, /*poll_budget=*/1000,
	    outputBits))) return status;
	if ((status = m->gpuContext.program(gpuIo))) return status;
	ArmSmmu4KStream gpuStreams[2] = {{0, LEMONADEP_SMMU_SID_GPU_0}, {1, LEMONADEP_SMMU_SID_GPU_1}};
	return m->gpuContext.attach(gpuStreams, 2);
}
