/* Host test for lemonadep_gpu_smmu.c's bank discovery. */
#include <stdio.h>

#include "lemonadep_gpu_smmu.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t mock_id1;
static int
mock_read(void *cookie, uint32_t off, uint32_t *out)
{
	(void)cookie;
	if (off != LEMONADEP_SMMU_GR0_ID1_OFFSET) {
		return 1;
	}
	*out = mock_id1;
	return 0;
}

int
main(void)
{
	lemonadep_smmu_discover_io io = {.cookie = NULL, .read32 = mock_read};
	unsigned gpu, gmu, numcb;

	/* A real SM8350 Adreno SMMU-500 plausibly has well more than 2 banks
	 * (flagship-class IOMMU instances typically do); this models a
	 * generous but still bounded real value, not an assumption this code
	 * depends on beyond ">= 2". */
	mock_id1 = 16;
	CHECK(lemonadep_smmu_discover_banks(&io, &gpu, &gmu, &numcb) == LemonadepSmmuOk);
	CHECK(numcb == 16 && gpu != gmu && gpu < numcb && gmu < numcb);
	printf("discover: numcb=%u bank_gpu=%u bank_gmu=%u\n", numcb, gpu, gmu);

	/* A pathological/misread ID1 reporting too few banks must be refused,
	 * not silently produce an out-of-range or colliding bank number. */
	mock_id1 = 1;
	CHECK(lemonadep_smmu_discover_banks(&io, &gpu, &gmu, &numcb) == LemonadepSmmuNoBanks);

	/* NUMCB occupies only bits 7:0 -- a real ID1 has other fields set too
	 * (PageSize, nSMRG, etc. in the upper bits); those must not leak into
	 * the extracted count. */
	mock_id1 = 0xA5A50010u; /* NUMCB=0x10=16, garbage in the upper bits */
	CHECK(lemonadep_smmu_discover_banks(&io, &gpu, &gmu, &numcb) == LemonadepSmmuOk);
	CHECK(numcb == 16);

	printf("test_gpu_smmu %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
