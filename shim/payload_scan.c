/*
 * Locate and copy the SWEETXNU payload inside an initrd that may start with
 * someone else's ramdisk (boot header v3+: vendor_boot ramdisk first). Pure
 * functions, also built into the host test (shim/host_test_loader.c).
 */
#include "payload_scan.h"

static inline uint64_t rd64(uintptr_t a)
{
	return *(volatile const uint64_t *)a;
}

static int magic_at(uintptr_t a)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < PAYLOAD_MAGIC_LEN; i++)
		if (*(volatile const char *)(a + i) != want[i])
			return 0;
	return 1;
}

uintptr_t payload_find(uintptr_t lo, uintptr_t hi)
{
	const uint64_t ones = 0x0101010101010101ULL, highs = 0x8080808080808080ULL;
	const uint64_t pat = ones * 'S';

	if (hi < lo + PAYLOAD_MAGIC_LEN)
		return 0;
	for (uintptr_t a = lo & ~7UL; a < hi; a += 8) {
		uint64_t x = rd64(a) ^ pat;
		if (!((x - ones) & ~x & highs))
			continue;	/* no 'S' byte in this word */
		for (unsigned b = 0; b < 8; b++) {
			uintptr_t p = a + b;
			if (p >= lo && p + PAYLOAD_MAGIC_LEN <= hi && magic_at(p))
				return p;
		}
	}
	return 0;
}

void copy_any(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	uint64_t words = (n + 7) / 8;
	unsigned sh = (src & 7) * 8;

	if (!sh) {
		for (uint64_t i = 0; i < words; i++)
			d[i] = rd64(src + 8 * i);
		return;
	}
	/* Misaligned source: aligned loads, shifted together (little endian). */
	uintptr_t s = src & ~7UL;
	uint64_t w0 = rd64(s);
	for (uint64_t i = 0; i < words; i++) {
		uint64_t w1 = rd64(s + 8 * (i + 1));
		d[i] = (w0 >> sh) | (w1 << (64 - sh));
		w0 = w1;
	}
}
