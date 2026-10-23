/* Host test for lemonadep_cp_init.c, reusing test_ring.c's mock GPU that
 * actually executes submitted packets. */
#include <stdio.h>
#include <string.h>

#include "lemonadep_cp_init.c"

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
	if (reg == LEMONADEP_CP_RB_WPTR_DWORD) {
		uint32_t header;
		AdrenoPM4Type7(LemonadepCpMeInitOpcode, 8, &header);
		for (uint32_t off = 0; off < 8192; ++off) {
			uint32_t at = (value - 9 - off) & 8191u;
			if (ring_mem[at] == header) {
				/* The real CP_ME_INIT doesn't itself write a fence --
				 * only emitted via the ring's own CP_EVENT_WRITE in
				 * submit(), already covered by test_ring.c. Here, find
				 * THAT packet (emitted right after CP_ME_INIT) instead. */
				uint32_t evheader;
				AdrenoPM4Type7(LEMONADEP_CP_EVENT_WRITE_OPCODE, 4, &evheader);
				for (uint32_t j = 0; j < 20; ++j) {
					uint32_t at2 = (at + 9 + j) & 8191u;
					if (ring_mem[at2] == evheader) {
						fence_word = ring_mem[(at2 + 4) & 8191u];
						break;
					}
				}
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

	uint32_t fence = 0;
	CHECK(lemonadep_a6xx_cp_init(&io, &ring, 10000, &fence) == LemonadepRingOk);
	CHECK(fence == 1);

	/* The real CP_ME_INIT header and the real a6xx (not A7XX) payload
	 * must actually be in the ring. */
	uint32_t header;
	CHECK(AdrenoPM4Type7(LemonadepCpMeInitOpcode, 8, &header) == AdrenoPM4Success);
	bool found = false;
	for (uint32_t i = 0; i < 8192; ++i) {
		if (ring_mem[i] == header) {
			found = true;
			CHECK(ring_mem[(i + 1) & 8191u] == 0x0000002fu);
			CHECK(ring_mem[(i + 2) & 8191u] == 0x00000003u);
			CHECK(ring_mem[(i + 3) & 8191u] == 0x20000000u);
			CHECK(ring_mem[(i + 4) & 8191u] == 0u);
			CHECK(ring_mem[(i + 5) & 8191u] == 0u);
			break;
		}
	}
	CHECK(found);

	/* No CP_THREAD_CONTROL (opcode 0x17) packet anywhere -- that's the
	 * A7XX-only concurrent-binning-disable prefix, must not be emitted
	 * for A660. */
	uint32_t tc_header;
	AdrenoPM4Type7(0x17, 1, &tc_header);
	bool found_tc = false;
	for (uint32_t i = 0; i < 8192; ++i) {
		if (ring_mem[i] == tc_header) {
			found_tc = true;
			break;
		}
	}
	CHECK(!found_tc);

	printf("test_cp_init %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
