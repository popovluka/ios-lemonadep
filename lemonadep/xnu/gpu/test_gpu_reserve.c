/* Host test (ASan/UBSan) for lemonadep_gpu_reserve.c: a mock GDSC/clock
 * bank that only reports success once the right bit is actually set (not a
 * stub that always succeeds), so this proves the real enable sequence,
 * not just that the function returns OK on a mock that can't fail. */
#include <stdio.h>
#include <string.h>

#include "lemonadep_gpu_reserve.c"

static uint32_t regs[LemonadepPwrWindowCount][0x20000];
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int
mock_read(void *cookie, unsigned window, uint32_t off, uint32_t *out)
{
	(void)cookie;
	*out = regs[window][off / 4];
	return 0;
}
static int
mock_write(void *cookie, unsigned window, uint32_t off, uint32_t value)
{
	(void)cookie;
	regs[window][off / 4] = value;
	/* Model hardware: the GDSC's PWR_ON bit follows SW_COLLAPSE being
	 * cleared, and a branch's CLK_OFF bit clears once CLK_ENABLE is set
	 * -- except the DDRSS branch, deliberately left stuck at CLK_OFF=1 to
	 * prove enable_branch's skip_poll path really never polls it (if it
	 * did, this test would time out instead of passing). */
	if (window == LemonadepPwrWindowGpucc && off == LEMONADEP_GPUCC_GDSC_CX_OFFSET) {
		if (!(value & LEMONADEP_GDSC_SW_COLLAPSE_BIT)) {
			regs[window][off / 4] |= LEMONADEP_GDSC_PWR_ON_BIT;
		}
	} else if (value & LEMONADEP_CLK_ENABLE_BIT) {
		if (!(window == LemonadepPwrWindowGcc && off == LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET)) {
			regs[window][off / 4] = value & ~LEMONADEP_CLK_OFF_BIT;
		}
	}
	return 0;
}
static int mock_now_us(void *cookie, uint64_t *out) { (void)cookie; static uint64_t t; *out = (t += 10); return 0; }
static int mock_wait_us(void *cookie, unsigned us) { (void)cookie; (void)us; return 0; }

int
main(void)
{
	memset(regs, 0, sizeof(regs));
	/* Every branch starts gated (CLK_OFF=1), GDSC collapsed (SW_COLLAPSE=1,
	 * PWR_ON=0) -- the real cold reset state, not a pre-enabled mock. */
	regs[LemonadepPwrWindowGpucc][LEMONADEP_GPUCC_GDSC_CX_OFFSET / 4] = LEMONADEP_GDSC_SW_COLLAPSE_BIT;
	unsigned gpucc_offs[] = {LEMONADEP_GPUCC_AHB_CLK_OFFSET, LEMONADEP_GPUCC_CX_GMU_CLK_OFFSET,
	    LEMONADEP_GPUCC_CXO_CLK_OFFSET, LEMONADEP_GPUCC_HLOS1_VOTE_GPU_SMMU_CLK_OFFSET,
	    LEMONADEP_GPUCC_HUB_CX_INT_CLK_OFFSET};
	for (unsigned i = 0; i < 5; ++i) {
		regs[LemonadepPwrWindowGpucc][gpucc_offs[i] / 4] = LEMONADEP_CLK_OFF_BIT;
	}
	regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET / 4] =
	    LEMONADEP_CLK_OFF_BIT | LEMONADEP_CLK_HWCG_BIT;
	regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_GPU_MEMNOC_GFX_CLK_OFFSET / 4] =
	    LEMONADEP_CLK_OFF_BIT | LEMONADEP_CLK_HWCG_BIT;

	lemonadep_pwr_io io = {.cookie = NULL, .read32 = mock_read, .write32 = mock_write,
	    .now_us = mock_now_us, .wait_us = mock_wait_us};
	lemonadep_pwr_status status = lemonadep_gpu_reserve(&io);
	CHECK(status == LemonadepPwrOk);

	CHECK(regs[LemonadepPwrWindowGpucc][LEMONADEP_GPUCC_GDSC_CX_OFFSET / 4] & LEMONADEP_GDSC_PWR_ON_BIT);
	CHECK(!(regs[LemonadepPwrWindowGpucc][LEMONADEP_GPUCC_GDSC_CX_OFFSET / 4] & LEMONADEP_GDSC_SW_COLLAPSE_BIT));
	for (unsigned i = 0; i < 5; ++i) {
		CHECK(regs[LemonadepPwrWindowGpucc][gpucc_offs[i] / 4] & LEMONADEP_CLK_ENABLE_BIT);
		CHECK(!(regs[LemonadepPwrWindowGpucc][gpucc_offs[i] / 4] & LEMONADEP_CLK_OFF_BIT));
	}
	CHECK(regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET / 4] & LEMONADEP_CLK_ENABLE_BIT);
	/* Deliberately still CLK_OFF -- proves the BRANCH_HALT_SKIP path never polled it. */
	CHECK(regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET / 4] & LEMONADEP_CLK_OFF_BIT);
	CHECK(regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_GPU_MEMNOC_GFX_CLK_OFFSET / 4] & LEMONADEP_CLK_ENABLE_BIT);
	CHECK(!(regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_GPU_MEMNOC_GFX_CLK_OFFSET / 4] & LEMONADEP_CLK_OFF_BIT));

	printf("test_gpu_reserve %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
