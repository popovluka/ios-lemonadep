/*
 * Host test for lemonadep_gpu_smmu_map.c, and for the GMU SMMU context's
 * widened prepare() aperture (LEMONADEP_GMU_APERTURE_* /
 * LEMONADEP_GMU_TABLE_BUDGET_PAGES in LemonadepGmuSource.h).
 *
 * Drives the REAL vendored ArmSmmu4K kit end to end -- LivePrepare,
 * ProgramDisabledContext, AttachStreams, then LiveUpdate once per GMU
 * region -- against a mock SMMU-500 register file that only reports an
 * enabled/attached state once the real sequence actually wrote it (not a
 * stub that always succeeds), the same discipline test_gpu_reserve.c and
 * test_gpu_coldstart.c already use for their mock register banks. This is
 * the plain-C core NativeAdrenoSmmuContext (C++/IOKit, not host-testable)
 * will drive the same way in lemonadep_gpu_memory.cpp.
 *
 * The SMMU ID0/ID1/ID2 values below are a self-consistent test fixture
 * (as lemonadep_smmu_discover_banks' own test already uses a synthetic
 * NUMCB), not the real SM8350 Adreno SMMU-500's literal register value --
 * that value isn't needed here: this test exercises the aperture/budget
 * math and the real LiveUpdate call sequence, not hardware ID discovery.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lemonadep_gpu_smmu_map.c"
#include "lemonadep_gpu_smmu.c"
#include "vendor/ArmSmmu4K/ArmSmmu4KStreams.h"
#include "LemonadepGmuSource.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define REG_BYTES 0x20000u
static uint8_t regs[REG_BYTES];

static int
mock_read32(void *cookie, size_t off, uint32_t *out)
{
	(void)cookie;
	if (off + 4 > REG_BYTES) return 1;
	memcpy(out, regs + off, 4);
	return 0;
}
static int
mock_write32(void *cookie, size_t off, uint32_t value)
{
	(void)cookie;
	if (off + 4 > REG_BYTES) return 1;
	/* Every context bank's FSR sits at cb+0x58 (cb is 4096-aligned, so
	 * this matches it generically for any bank): real hardware clears
	 * fault bits written as 1 (W1C) rather than storing the write
	 * verbatim -- ArmSmmu4KProgramDisabledContext relies on exactly this
	 * to clear stale faults with a single UINT32_MAX write. */
	if ((off & 0xfffu) == 0x58u) {
		uint32_t current;
		memcpy(&current, regs + off, 4);
		current &= ~value;
		memcpy(regs + off, &current, 4);
		return 0;
	}
	memcpy(regs + off, &value, 4);
	return 0;
}
static int
mock_read64(void *cookie, size_t off, uint64_t *out)
{
	(void)cookie;
	if (off + 8 > REG_BYTES) return 1;
	memcpy(out, regs + off, 8);
	return 0;
}
static int
mock_write64(void *cookie, size_t off, uint64_t value)
{
	(void)cookie;
	if (off + 8 > REG_BYTES) return 1;
	memcpy(regs + off, &value, 8);
	return 0;
}
static int
mock_barrier(void *cookie)
{
	(void)cookie;
	return 0;
}
/* Discovery's own io uses a 32-bit-offset callback; adapt. */
static int
discover_read32(void *cookie, uint32_t off, uint32_t *out)
{
	return mock_read32(cookie, off, out);
}
static int
publish_page(void *cookie, volatile uint64_t *page, size_t bytes)
{
	(void)cookie; (void)page; (void)bytes;
	/* Test fixture: the table pool is a plain host buffer, not a real
	 * cached mapping, so there is nothing to clean/publish. A real
	 * NativeAdrenoMemory-backed callback (NativeAdrenoBindSmmuVisibility)
	 * does the genuine cache maintenance this stands in for. */
	return 0;
}

int
main(void)
{
	/* ID0: S1TS (bit30, stage-1 support) + NUMSMRG=8 (bits0-7, stream
	 * mapping groups) + NUMSMRG nonzero flag (bit27) + SIDSIZE=4 bits
	 * (bits9-12, wide enough for SID 5) + EXIDS clear (bit8). */
	const uint32_t id0 = (1u << 30) | (1u << 27) | 8u | (4u << 9);
	/* ID1: NUMCB=16 (bits0-7), NUMS2CB=0 (bits16-23), N=3 => 16 CB pages
	 * (bits28-30), PAGESIZE=4K (bit31 clear). */
	const uint32_t id1 = 16u | (3u << 28);
	/* ID2: bit12 set + PABITS index 2 (=40 bits) + VABITS index 0
	 * (=32 bits, matching our 32-bit GMU input_bits exactly). */
	const uint32_t id2 = (1u << 12) | (2u << 4) | (0u << 8);
	memcpy(regs + 0x20, &id0, 4);
	memcpy(regs + 0x24, &id1, 4);
	memcpy(regs + 0x28, &id2, 4);
	/* sCR0: global translation enabled (bit1,2), EXIDS clear (bit3),
	 * reserved bit10 set -- (scr0&0x407)==0x406 as ArmSmmu4KStreams'
	 * global() requires. */
	const uint32_t scr0 = 0x406;
	memcpy(regs + 0x0, &scr0, 4);

	lemonadep_smmu_discover_io discover_io = {.cookie = NULL, .read32 = discover_read32};
	unsigned bank_gpu, bank_gmu, numcb, output_bits;
	CHECK(lemonadep_smmu_discover_banks(&discover_io, &bank_gpu, &bank_gmu, &numcb) == LemonadepSmmuOk);
	CHECK(bank_gmu == 1 && numcb == 16);
	CHECK(lemonadep_smmu_discover_output_bits(&discover_io, &output_bits) == LemonadepSmmuOk);
	CHECK(output_bits == 40);
	printf("discovered: bank_gmu=%u output_bits=%u\n", bank_gmu, output_bits);

	/* Slot 0 (the GMU stream's slot) must start FAULTed, as a real reset
	 * SMMU-500 leaves every stream -- AttachStreams' preflight scan
	 * refuses to attach a slot it doesn't find already faulted. */
	const uint32_t fault_s2cr = 2u << 16;
	memcpy(regs + 0xc00, &fault_s2cr, 4);

	const ArmSmmu4KIo io = {
		.cookie = NULL, .register_bytes = REG_BYTES,
		.read32 = mock_read32, .read64 = mock_read64,
		.write32 = mock_write32, .write64 = mock_write64, .barrier = mock_barrier,
	};

	/* Table pool: a real, 4096-aligned, zeroed buffer -- NOT the design's
	 * tuned minimum (LEMONADEP_GMU_TABLE_BUDGET_PAGES is already a
	 * generous multiple of the true requirement, verified below). */
	uint64_t *pool = aligned_alloc(4096, (size_t)LEMONADEP_GMU_TABLE_BUDGET_PAGES * 4096);
	CHECK(pool != NULL);
	memset(pool, 0, (size_t)LEMONADEP_GMU_TABLE_BUDGET_PAGES * 4096);

	ArmSmmu4KLiveArena arena = {
		.config = {.input_bits = 32, .output_bits = output_bits, .table_physical = 0,
		    .leaf_budget = LEMONADEP_GMU_LEAF_BUDGET},
		.iova = LEMONADEP_GMU_APERTURE_IOVA, .bytes = LEMONADEP_GMU_APERTURE_BYTES,
		.upper_region = 0, .mapping = pool,
		.capacity_pages = LEMONADEP_GMU_TABLE_BUDGET_PAGES,
		.alias_scratch = NULL, .alias_scratch_entries = 0,
	};
	ArmSmmu4KLiveState state = {0};
	CHECK(ArmSmmu4KLivePrepare(&arena, &state) == ArmSmmu4KSuccess);
	printf("table_pages=%zu (budget=%u pages)\n", state.tables.table_pages,
	    LEMONADEP_GMU_TABLE_BUDGET_PAGES);
	/* The real requirement (774 pages for this exact aperture) must stay
	 * well under the chosen budget -- if a future IOVA/size edit in
	 * LemonadepGmuSource.h ever grows the aperture past what
	 * LEMONADEP_GMU_TABLE_BUDGET_PAGES covers, this is the check that
	 * catches it instead of a silent ArmSmmu4KCapacity at runtime. */
	CHECK(state.tables.table_pages == 774);
	CHECK(state.tables.table_pages < LEMONADEP_GMU_TABLE_BUDGET_PAGES);

	ArmSmmu4KLiveResult audit;
	CHECK(ArmSmmu4KLiveAudit(&arena, &state, NULL, 0, &audit) == ArmSmmu4KSuccess);
	CHECK(audit.complete);

	const ArmSmmu4KContextConfig context = {
		.lower = &arena.config, .upper = NULL,
		.lower_tables = &state.tables, .upper_tables = NULL,
		.id0 = 0, .id2 = 0, .asid = 0,
		.coherent_walk = 1, .stall_on_fault = 0, .continue_after_fault = 1,
	};
	ArmSmmu4KProgramResult program_result;
	ArmSmmu4KStatus program_status = ArmSmmu4KProgramDisabledContext(&io, bank_gmu, &context, 1000, &program_result);
	printf("program_status=%d phase=%u\n", program_status, program_result.phase);
	CHECK(program_status == ArmSmmu4KSuccess);
	CHECK(program_result.enabled);

	ArmSmmu4KStreamState stream_state = {0};
	ArmSmmu4KStreamResult stream_result;
	const ArmSmmu4KStream gmu_stream = {.slot = 0, .sid = LEMONADEP_SMMU_SID_GMU};
	CHECK(ArmSmmu4KAttachStreams(&io, &stream_state, &gmu_stream, 1, bank_gmu, &context, &stream_result)
	    == ArmSmmu4KSuccess);
	CHECK(stream_state.attached);

	/* The five real GMU regions, sorted ascending by IOVA exactly as
	 * LemonadepGmuSource.h lays them out (icache < dummy < debug < log <
	 * hfi) -- ArmSmmu4KLiveUpdate requires that order. Physical addresses
	 * are this test's own fixture, placed clear of the table pool's own
	 * [0, capacity_pages*4096) span. */
	const uint64_t backing_pa = 0x10000000ull;
	const lemonadep_gmu_region regions[] = {
		{.iova = LEMONADEP_GMU_ICACHE_IOVA, .physical = backing_pa, .bytes = LEMONADEP_GMU_ICACHE_BYTES},
		{.iova = LEMONADEP_GMU_DUMMY_IOVA, .physical = backing_pa + 0x01000000, .bytes = LEMONADEP_GMU_DUMMY_BYTES},
		{.iova = LEMONADEP_GMU_DEBUG_IOVA, .physical = backing_pa + 0x01010000, .bytes = LEMONADEP_GMU_DEBUG_BYTES},
		{.iova = LEMONADEP_GMU_DEBUG_IOVA + LEMONADEP_GMU_DEBUG_BYTES,
		    .physical = backing_pa + 0x01020000, .bytes = LEMONADEP_GMU_LOG_BYTES},
		{.iova = LEMONADEP_GMU_DEBUG_IOVA + LEMONADEP_GMU_DEBUG_BYTES + LEMONADEP_GMU_LOG_BYTES,
		    .physical = backing_pa + 0x01030000, .bytes = LEMONADEP_GMU_HFI_BYTES},
	};
	const size_t region_count = sizeof(regions) / sizeof(regions[0]);
	ArmSmmu4KRange scratch[5];
	const ArmSmmu4KLiveVisibility visibility = {.cookie = NULL, .publish_table_page = publish_page};
	CHECK(lemonadep_gmu_map_regions(&arena, &state, &io, bank_gmu, &context, &visibility,
	    regions, region_count, scratch, 1000) == ArmSmmu4KSuccess);
	CHECK(!state.poisoned);
	CHECK(state.tables.leaf_pages ==
	    (LEMONADEP_GMU_ICACHE_BYTES + LEMONADEP_GMU_DUMMY_BYTES + LEMONADEP_GMU_DEBUG_BYTES +
	     LEMONADEP_GMU_LOG_BYTES + LEMONADEP_GMU_HFI_BYTES) / 4096u);

	/* Re-audit the whole region after mapping: every descriptor the walk
	 * finds must match what was actually written, not merely "the call
	 * returned success". */
	ArmSmmu4KLiveResult final_audit;
	CHECK(ArmSmmu4KLiveAudit(&arena, &state, scratch, region_count, &final_audit) == ArmSmmu4KSuccess);
	CHECK(final_audit.complete);

	free(pool);
	printf("test_gpu_memory %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
