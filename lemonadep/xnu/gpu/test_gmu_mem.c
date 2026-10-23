/*
 * Host test (ASan/UBSan) for lemonadep_gmu_mem.c: the HFI queue table, the
 * icache firmware copy (real a660_gmu.bin), the read-only SMMU probe, and
 * the full GMU SMMU context setup, against a mock SMMU-500 whose state is
 * deliberately awkward -- an already-enabled bank, a bank a live stream
 * translates through, and stream slots in each of live/bypass/fault --
 * so the probe has to actually pick safely, not just return something.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lemonadep_gpu_smmu_map.c"
#include "lemonadep_gmu_mem.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define REG_BYTES 0x20000u
static uint8_t regs[REG_BYTES];
static unsigned writes;

static uint32_t rd(size_t off) { uint32_t v; memcpy(&v, regs + off, 4); return v; }
static void wr(size_t off, uint32_t v) { memcpy(regs + off, &v, 4); }

static int
mock_read32(void *c, size_t off, uint32_t *out)
{
	(void)c;
	if (off + 4 > REG_BYTES) return 1;
	*out = rd(off);
	return 0;
}
static int
mock_write32(void *c, size_t off, uint32_t v)
{
	(void)c;
	if (off + 4 > REG_BYTES) return 1;
	++writes;
	if ((off & 0xfffu) == 0x58u) { /* FSR: write-1-to-clear */
		wr(off, rd(off) & ~v);
		return 0;
	}
	wr(off, v);
	return 0;
}
static int
mock_read64(void *c, size_t off, uint64_t *out)
{
	(void)c;
	if (off + 8 > REG_BYTES) return 1;
	memcpy(out, regs + off, 8);
	return 0;
}
static int
mock_write64(void *c, size_t off, uint64_t v)
{
	(void)c;
	if (off + 8 > REG_BYTES) return 1;
	++writes;
	memcpy(regs + off, &v, 8);
	return 0;
}
static int mock_barrier(void *c) { (void)c; return 0; }
static int publish(void *c, volatile uint64_t *p, size_t n) { (void)c; (void)p; (void)n; return 0; }

#define CB(bank) ((16u + (bank)) * 0x1000u)

static void
reset_smmu(uint32_t scr0)
{
	memset(regs, 0, sizeof(regs));
	writes = 0;
	wr(0x0, scr0);
	/* ID0: S1TS, NUMSMRG present, 8 SMRs, SIDSIZE=15 (holds 0x405), no EXIDS. */
	wr(0x20, (1u << 30) | (1u << 27) | 8u | (15u << 9));
	/* ID1: 16 banks, NUMS2CB=2 (banks 0,1 are stage-2 territory), 16 pages, 4K. */
	wr(0x24, 16u | (2u << 16) | (3u << 28));
	/* ID2: VA 32 (code 0), PA 40 (code 2), bit12. */
	wr(0x28, (1u << 12) | (2u << 4));
	/* Bank 2 already enabled by someone else: must be skipped. */
	wr(CB(2), 1);
	/* Slot 0: a live stream translating through bank 3: bank 3 must be skipped too. */
	wr(0x800, (1u << 31) | 0x10u);
	wr(0xc00, 3u);
	/* Slot 1: bypass -- not attachable as-is. Slots 2..7: FAULT. */
	wr(0xc04, 1u << 16);
	for (unsigned i = 2; i < 8; ++i) {
		wr(0xc00 + 4 * i, 2u << 16);
	}
}

int
main(void)
{
	/* ---- HFI table: a6xx_hfi_init() semantics ---- */
	static uint32_t hfi[LEMONADEP_GMU_HFI_BYTES / 4];
	memset(hfi, 0xa5, sizeof(hfi));
	lemonadep_hfi_table_init(hfi, 0x6040b000u);
	CHECK(hfi[1] == 168 && hfi[2] == 6 && hfi[3] == 12 && hfi[4] == 3 && hfi[5] == 3);
	for (unsigned q = 0; q < 3; ++q) {
		const uint32_t *h = hfi + 6 + 12 * q;
		CHECK(h[0] == 1 && h[1] == 0x6040b000u + 4096u * (q + 1) && h[2] == ((10u << 8) | q));
		CHECK(h[3] == 1024 && h[6] == 1 && h[7] == 1 && h[8] == 1 && h[9] == 0 && h[10] == 0 && h[11] == 0);
	}
	CHECK(hfi[LEMONADEP_GMU_HFI_BYTES / 4 - 1] == 0); /* rest zeroed, not left as 0xa5 */

	/* ---- icache copy from the real firmware ---- */
	FILE *f = fopen("a660_gmu.bin", "rb");
	if (!f) {
		printf("test_gmu_mem FAILED: copy lemonadep/fw-gpu/a660_gmu.bin next to this binary\n");
		return 1;
	}
	static uint8_t fw[LEMONADEP_GMU_FW_BYTES];
	CHECK(fread(fw, 1, sizeof(fw), f) == sizeof(fw));
	fclose(f);
	uint8_t *gpumem = aligned_alloc(4096, LEMONADEP_GPUMEM_BYTES);
	CHECK(gpumem != NULL);
	memset(gpumem, 0, LEMONADEP_GPUMEM_BYTES);
	uint8_t *icache = gpumem + LEMONADEP_GPUMEM_ICACHE_OFF;
	CHECK(lemonadep_gmu_icache_load(icache, fw, sizeof(fw)) == 54476u);
	CHECK(memcmp(icache, fw + 684, 54476) == 0);
	CHECK(lemonadep_gmu_icache_load(icache, fw, 1000) == 0); /* truncated firmware refused */

	const ArmSmmu4KIo io = {.cookie = NULL, .register_bytes = REG_BYTES, .read32 = mock_read32,
	    .read64 = mock_read64, .write32 = mock_write32, .write64 = mock_write64, .barrier = mock_barrier};
	const ArmSmmu4KLiveVisibility vis = {.cookie = NULL, .publish_table_page = publish};
	const uint64_t gpumem_pa = 0x80000000ull;

	/* ---- probe: read-only, picks safely ---- */
	reset_smmu(0x406);
	lemonadep_smmu_probe_info p;
	CHECK(lemonadep_smmu_probe(&io, &p) == 0);
	CHECK(writes == 0);
	CHECK(p.global_ok && p.numcb == 16 && p.nums2cb == 2 && p.numsmrg == 8 && p.output_bits == 40);
	CHECK(p.bank == 4);   /* 0,1 stage-2 range; 2 enabled; 3 used by a live stream */
	CHECK(p.free_slots == 2 && p.slots[0] == 2 && p.slots[1] == 3);
	CHECK(p.smr_valid == 1 && p.s2cr_bypass == 1 && p.s2cr_trans == 1 && p.s2cr_fault == 6);
	printf("probe: bank=%u slots=%u,%u global_ok=%u oas=%u\n", p.bank, p.slots[0], p.slots[1],
	    p.global_ok, p.output_bits);

	/* ---- full setup ---- */
	lemonadep_gmu_smmu s;
	ArmSmmu4KStatus st = lemonadep_gmu_smmu_setup(&s, &io, &p, gpumem, gpumem_pa, &vis);
	printf("setup: status=%d step=%u sids=%u table_pages=%zu leaves=%zu\n", st, s.step, s.sid_count,
	    s.state.tables.table_pages, s.state.tables.leaf_pages);
	CHECK(st == ArmSmmu4KSuccess && s.step == LemonadepGmuSmmuStepDone && s.sid_count == 2);
	CHECK(rd(CB(4)) & 1u);                                     /* bank 4 enabled */
	CHECK(rd(0x808) == ((1u << 31) | 5u) && rd(0xc08) == 4u);  /* slot 2 -> SID 5 */
	CHECK(rd(0x80c) == ((1u << 31) | 0x405u) && rd(0xc0c) == 4u); /* slot 3 -> SID 0x405 */
	CHECK(rd(0x800) == ((1u << 31) | 0x10u) && rd(0xc00) == 3u); /* the live stream untouched */
	CHECK(rd(CB(2)) == 1u);                                    /* the other enabled bank untouched */
	ArmSmmu4KLiveResult audit;
	CHECK(ArmSmmu4KLiveAudit(&s.arena, &s.state, s.mapped, 5, &audit) == ArmSmmu4KSuccess && audit.complete);
	CHECK(s.mapped[0].iova == LEMONADEP_GMU_ICACHE_IOVA &&
	    s.mapped[0].physical == gpumem_pa + LEMONADEP_GPUMEM_ICACHE_OFF);
	CHECK(s.mapped[4].iova == LEMONADEP_GMU_HFI_IOVA &&
	    s.mapped[4].physical == gpumem_pa + LEMONADEP_GPUMEM_HFI_OFF);

	/* ---- refuses to touch anything when the global state is wrong ---- */
	reset_smmu(0x1); /* CLIENTPD set: SMMU globally disabled */
	CHECK(lemonadep_smmu_probe(&io, &p) == 0 && !p.global_ok);
	memset(gpumem, 0, LEMONADEP_GMU_TABLE_BUDGET_PAGES * 4096u);
	CHECK(lemonadep_gmu_smmu_setup(&s, &io, &p, gpumem, gpumem_pa, &vis) == ArmSmmu4KUnsupported);
	CHECK(writes == 0);

	free(gpumem);
	printf("test_gmu_mem %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
