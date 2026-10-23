/*
 * Host test (ASan/UBSan) for lemonadep_gpu_boot.c: the full real sequence
 * -- power/clock reserve, GMU cold start, CP ring attach, CP_ME_INIT --
 * driven end to end against the same three per-domain mocks the
 * individual unit tests already use (test_gpu_reserve.c,
 * test_gmu_coldstart.c, test_cp_init.c), each starting in the real cold-
 * reset state its own test models, not a stub that always succeeds.
 *
 * This is the host-side proof of the actual milestone: a submitted GPU
 * command (CP_ME_INIT) reaches a real completed fence, through the real
 * call order a device boot will use, before any of this ever touches
 * hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lemonadep_gpu_boot.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- power/clock mock (test_gpu_reserve.c's own) ---- */
static uint32_t pwr_regs[LemonadepPwrWindowCount][0x20000];
static int
pwr_mock_read(void *cookie, unsigned window, uint32_t off, uint32_t *out)
{
	(void)cookie;
	*out = pwr_regs[window][off / 4];
	return 0;
}
static int
pwr_mock_write(void *cookie, unsigned window, uint32_t off, uint32_t value)
{
	(void)cookie;
	pwr_regs[window][off / 4] = value;
	if (window == LemonadepPwrWindowGpucc && off == LEMONADEP_GPUCC_GDSC_CX_OFFSET) {
		if (!(value & LEMONADEP_GDSC_SW_COLLAPSE_BIT)) {
			pwr_regs[window][off / 4] |= LEMONADEP_GDSC_PWR_ON_BIT;
		}
	} else if (value & LEMONADEP_CLK_ENABLE_BIT) {
		if (!(window == LemonadepPwrWindowGcc && off == LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET)) {
			pwr_regs[window][off / 4] = value & ~LEMONADEP_CLK_OFF_BIT;
		}
	}
	return 0;
}
static int pwr_mock_now_us(void *cookie, uint64_t *out) { (void)cookie; static uint64_t t; *out = (t += 10); return 0; }
static int pwr_mock_wait_us(void *cookie, unsigned us) { (void)cookie; (void)us; return 0; }

/* ---- GMU register mock (test_gmu_coldstart.c's own) ---- */
#define MAX_GMU_REGS 0x40000u
static uint32_t gmu_regs[LemonadepGmuWindowCount][MAX_GMU_REGS / 4];
static int
gmu_mock_read(void *cookie, unsigned window, uint32_t off, uint32_t *out)
{
	(void)cookie;
	if (window >= LemonadepGmuWindowCount || off + 4 > MAX_GMU_REGS) return 1;
	*out = gmu_regs[window][off / 4];
	return 0;
}
static int
gmu_mock_write(void *cookie, unsigned window, uint32_t off, uint32_t value)
{
	(void)cookie;
	if (window >= LemonadepGmuWindowCount || off + 4 > MAX_GMU_REGS) return 1;
	gmu_regs[window][off / 4] = value;
	if (window == LemonadepGmuWindowCore && off == A660_GMU_RSCC_CONTROL_REQ) {
		gmu_regs[LemonadepGmuWindowCore][A660_GMU_RSCC_CONTROL_ACK / 4] = 1u << 1;
	}
	if (window == LemonadepGmuWindowCore && off == A660_GMU_CM3_SYSRESET && value == 0) {
		gmu_regs[LemonadepGmuWindowCore][A660_GMU_CM3_FW_INIT_RESULT / 4] =
		    LEMONADEP_A660_GMU_START_READY_LOW;
	}
	return 0;
}
static int gmu_mock_barrier(void *cookie) { (void)cookie; return 0; }
static int gmu_mock_now_us(void *cookie, uint64_t *out) { (void)cookie; static uint64_t t; *out = (t += 10); return 0; }
static int gmu_mock_wait_us(void *cookie, unsigned us) { (void)cookie; (void)us; return 0; }

/* ---- CP ring mock (test_cp_init.c's own: a GPU that actually executes
 * what's submitted, by scanning the ring for the real packet header). ---- */
static uint32_t ring_regs[0x1000];
static volatile uint32_t ring_mem[8192];
static volatile uint32_t ring_fence_word;
static int
ring_mock_write(void *cookie, uint32_t reg, uint32_t value)
{
	(void)cookie;
	ring_regs[reg] = value;
	if (reg == LEMONADEP_CP_RB_WPTR_DWORD) {
		uint32_t header;
		AdrenoPM4Type7(LemonadepCpMeInitOpcode, 8, &header);
		for (uint32_t off = 0; off < 8192; ++off) {
			uint32_t at = (value - 9 - off) & 8191u;
			if (ring_mem[at] == header) {
				uint32_t evheader;
				AdrenoPM4Type7(LEMONADEP_CP_EVENT_WRITE_OPCODE, 4, &evheader);
				for (uint32_t j = 0; j < 20; ++j) {
					uint32_t at2 = (at + 9 + j) & 8191u;
					if (ring_mem[at2] == evheader) {
						ring_fence_word = ring_mem[(at2 + 4) & 8191u];
						break;
					}
				}
				break;
			}
		}
	}
	return 0;
}
static int ring_mock_barrier(void *cookie) { (void)cookie; return 0; }
static int ring_mock_now_us(void *cookie, uint64_t *out) { (void)cookie; static uint64_t t; *out = (t += 10); return 0; }
static int ring_mock_wait_us(void *cookie, unsigned us) { (void)cookie; (void)us; return 0; }

int
main(void)
{
	memset(pwr_regs, 0, sizeof(pwr_regs));
	pwr_regs[LemonadepPwrWindowGpucc][LEMONADEP_GPUCC_GDSC_CX_OFFSET / 4] = LEMONADEP_GDSC_SW_COLLAPSE_BIT;
	unsigned gpucc_offs[] = {LEMONADEP_GPUCC_AHB_CLK_OFFSET, LEMONADEP_GPUCC_CX_GMU_CLK_OFFSET,
	    LEMONADEP_GPUCC_CXO_CLK_OFFSET, LEMONADEP_GPUCC_HLOS1_VOTE_GPU_SMMU_CLK_OFFSET,
	    LEMONADEP_GPUCC_HUB_CX_INT_CLK_OFFSET};
	for (unsigned i = 0; i < 5; ++i) {
		pwr_regs[LemonadepPwrWindowGpucc][gpucc_offs[i] / 4] = LEMONADEP_CLK_OFF_BIT;
	}
	pwr_regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_DDRSS_GPU_AXI_CLK_OFFSET / 4] =
	    LEMONADEP_CLK_OFF_BIT | LEMONADEP_CLK_HWCG_BIT;
	pwr_regs[LemonadepPwrWindowGcc][LEMONADEP_GCC_GPU_MEMNOC_GFX_CLK_OFFSET / 4] =
	    LEMONADEP_CLK_OFF_BIT | LEMONADEP_CLK_HWCG_BIT;
	lemonadep_pwr_io pwr_io = {.cookie = NULL, .read32 = pwr_mock_read, .write32 = pwr_mock_write,
	    .now_us = pwr_mock_now_us, .wait_us = pwr_mock_wait_us};

	memset(gmu_regs, 0, sizeof(gmu_regs));
	lemonadep_gmu_io gmu_io = {.cookie = NULL, .read32 = gmu_mock_read, .write32 = gmu_mock_write,
	    .barrier = gmu_mock_barrier, .now_us = gmu_mock_now_us, .wait_us = gmu_mock_wait_us};

	FILE *f = fopen("a660_gmu.bin", "rb");
	CHECK(f != NULL);
	if (!f) {
		printf("test_gpu_boot FAILED: copy lemonadep/fw-gpu/a660_gmu.bin next to this binary\n");
		return 1;
	}
	static uint8_t fw_bytes[LEMONADEP_GMU_FW_BYTES];
	size_t got = fread(fw_bytes, 1, sizeof(fw_bytes), f);
	fclose(f);
	CHECK(got == LEMONADEP_GMU_FW_BYTES);
	lemonadep_gmu_firmware fw = {.bytes = fw_bytes, .length = (uint32_t)got};

	memset((void *)ring_mem, 0, sizeof(ring_mem));
	memset(ring_regs, 0, sizeof(ring_regs));
	ring_fence_word = 0;
	lemonadep_ring_io ring_io = {.cookie = NULL, .write32 = ring_mock_write, .barrier = ring_mock_barrier,
	    .now_us = ring_mock_now_us, .wait_us = ring_mock_wait_us};
	lemonadep_ring ring;

	uint32_t fence = 0;
	lemonadep_boot_status status = lemonadep_gpu_boot(&pwr_io, &gmu_io, &fw,
	    /*hfi_iova=*/0x1230000, /*log_iova=*/0x4560000, /*log_size_bytes=*/0x4000,
	    &ring_io, &ring, ring_mem, 8192, 0x100000000ull,
	    (volatile uint32_t *)&ring_fence_word, 0x200000000ull,
	    /*cp_init_timeout_us=*/10000, &fence);
	printf("lemonadep_gpu_boot: status=%d fence=%u\n", status, fence);
	CHECK(status == LemonadepBootOk);
	CHECK(fence == 1);

	/* Every stage actually ran, not just that the top-level call returned
	 * OK: power really came up, the GMU really reports ready, and the
	 * real CP_ME_INIT payload is really in the ring. */
	CHECK(pwr_regs[LemonadepPwrWindowGpucc][LEMONADEP_GPUCC_GDSC_CX_OFFSET / 4] & LEMONADEP_GDSC_PWR_ON_BIT);
	CHECK(gmu_regs[LemonadepGmuWindowCore][A660_GMU_CM3_FW_INIT_RESULT / 4] != 0);
	uint32_t header;
	CHECK(AdrenoPM4Type7(LemonadepCpMeInitOpcode, 8, &header) == AdrenoPM4Success);
	bool found = false;
	for (uint32_t i = 0; i < 8192; ++i) {
		if (ring_mem[i] == header) { found = true; break; }
	}
	CHECK(found);

	printf("test_gpu_boot %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
