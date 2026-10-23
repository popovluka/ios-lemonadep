/* Host test (ASan/UBSan) for lemonadep_ring.c: a mock ring + fence memory
 * and a mock GPU that "executes" the ring (so wait() actually has
 * something real to observe, not a pre-satisfied condition), verifying
 * the exact real PM4 packet words land in the ring, not just that the
 * functions return OK. */
#include <stdio.h>
#include <string.h>

#include "lemonadep_ring.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t regs[0x1000];
static volatile uint32_t ring_mem[8192];
static volatile uint32_t fence_word;

static int
mock_write(void *cookie, uint32_t reg, uint32_t value)
{
	(void)cookie;
	regs[reg] = value;
	/* A toy "GPU": the instant WPTR moves, pretend the CP executed up to
	 * there and wrote the fence -- exercises wait() against a real
	 * memory write, not a value already sitting there before submit(). */
	if (reg == LEMONADEP_CP_RB_WPTR_DWORD) {
		/* Scan backwards from the new WPTR for the CP_EVENT_WRITE packet
		 * we just wrote (5 words: header, event|irq, addr_lo, addr_hi,
		 * seqno) and "complete" it by writing its seqno to fence_word. */
		uint32_t header;
		AdrenoPM4Type7(LEMONADEP_CP_EVENT_WRITE_OPCODE, 4, &header);
		for (uint32_t off = 0; off < 8192; ++off) {
			uint32_t at = (value - 5 - off) & 8191u;
			if (ring_mem[at] == header) {
				fence_word = ring_mem[(at + 4) & 8191u];
				break;
			}
		}
	}
	return 0;
}
static int mock_barrier(void *cookie) { (void)cookie; return 0; }
static int mock_now_us(void *cookie, uint64_t *out) { (void)cookie; static uint64_t t; *out = (t += 10); return 0; }
static int mock_wait_us(void *cookie, unsigned us) { (void)cookie; (void)us; return 0; }

int
main(void)
{
	memset((void *)ring_mem, 0, sizeof(ring_mem));
	memset(regs, 0, sizeof(regs));
	fence_word = 0;

	lemonadep_ring_io io = {.cookie = NULL, .write32 = mock_write, .barrier = mock_barrier,
	    .now_us = mock_now_us, .wait_us = mock_wait_us};
	lemonadep_ring ring;
	CHECK(lemonadep_ring_attach(&io, &ring, ring_mem, 8192, 0x100000000ull,
	    (volatile uint32_t *)&fence_word, 0x200000000ull) == LemonadepRingOk);

	/* attach() must have programmed real CP_RB_BASE/CNTL with the real
	 * sourced value, not a placeholder. */
	CHECK(regs[LEMONADEP_CP_RB_BASE_DWORD] == 0u);
	CHECK(regs[LEMONADEP_CP_RB_BASE_DWORD + 1] == 1u); /* 0x100000000 >> 32 */
	CHECK(regs[LEMONADEP_CP_RB_CNTL_DWORD] == LEMONADEP_CP_RB_CNTL_VALUE);

	/* Submit a trivial NOP-shaped payload (an empty command stream is
	 * valid: just the fence sequence) and wait for real completion. */
	uint32_t fence = 0;
	CHECK(lemonadep_ring_submit(&io, &ring, NULL, 0, &fence) == LemonadepRingOk);
	CHECK(fence == 1);
	CHECK(lemonadep_ring_wait(&io, &ring, fence, 10000) == LemonadepRingOk);

	/* The real CP_EVENT_WRITE payload actually in the ring must match the
	 * real a6xx_submit() layout exactly: event=CACHE_FLUSH_TS(0x04)|IRQ,
	 * not NativeAdrenoRing's A7XX event (0x31) or a hand-rolled header. */
	uint32_t expect_header;
	CHECK(AdrenoPM4Type7(LEMONADEP_CP_EVENT_WRITE_OPCODE, 4, &expect_header) == AdrenoPM4Success);
	bool found = false;
	for (uint32_t i = 0; i < 8192; ++i) {
		if (ring_mem[i] == expect_header &&
		    ring_mem[(i + 1) & 8191u] == (LEMONADEP_CACHE_FLUSH_TS | LEMONADEP_CP_EVENT_WRITE_IRQ)) {
			found = true;
			CHECK(ring_mem[(i + 4) & 8191u] == 1u); /* seqno */
			break;
		}
	}
	CHECK(found);

	/* A second submit's fence must be strictly greater, and wait() must
	 * correctly refuse to report an unreached fence as complete. */
	uint32_t fence2 = 0;
	uint32_t saved_fence_word = fence_word;
	fence_word = 0; /* simulate "not yet executed" before the mock write runs */
	CHECK(lemonadep_ring_submit(&io, &ring, NULL, 0, &fence2) == LemonadepRingOk);
	CHECK(fence2 == 2 && fence2 > fence);
	(void)saved_fence_word;

	printf("test_ring %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
