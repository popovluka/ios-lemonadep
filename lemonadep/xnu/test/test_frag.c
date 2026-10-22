/* Run 1001's start.s blocks (verbatim) on aarch64 under qemu-user. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct video { uint64_t base, display, rowbytes, width, height, depth; };
struct ba { uint16_t rev, ver; uint64_t vbase, pbase, msize, top; struct video v; };
_Static_assert(__builtin_offsetof(struct ba, v) == 40, "layout");

void run_band(struct ba *);
void run_fbmap(struct ba *, uint64_t *root);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define ATTRS (0x400ULL | 0x0020000000000000ULL | 0x0040000000000000ULL | (2ULL << 8) | (1ULL << 2) | 1ULL)

static void band(uint32_t w, uint32_t h, uint32_t rowbytes)
{
	size_t size = (size_t)rowbytes * h;
	uint8_t *fb = calloc(1, size + 4096);
	struct ba a = { .v = { (uintptr_t)fb, 1, rowbytes, w, h, 32 } };
	run_band(&a);
	size_t white = 0, bad = 0;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < rowbytes / 4; x++) {
			uint32_t p = ((uint32_t *)(fb + (size_t)y * rowbytes))[x];
			int in = y >= h - 32 && x < 64;
			white += p == 0xffffffffu;
			bad += in ? p != 0xffffffffu : p != 0;
		}
	for (size_t i = size; i < size + 4096; i++)
		bad += fb[i] != 0;
	printf("band %ux%u rb=%u: white=%zu bad=%zu\n", w, h, rowbytes, white, bad);
	CHECK(white == 64 * 32 && bad == 0);
	free(fb);
}

static void band_none(void)
{
	struct ba a = { 0 };
	run_band(&a);	/* base 0: must not write (would fault here) */
	uint8_t fb[64];
	memset(fb, 0, sizeof(fb));
	struct ba b = { .v = { (uintptr_t)fb, 1, 16, 4, 16, 32 } };	/* height < 32 */
	run_band(&b);
	for (int i = 0; i < 64; i++)
		CHECK(fb[i] == 0);
}

/* root + 2 pages: V=P L2 table at root + 2*16K */
static void fbmap(uint64_t base, uint64_t rowbytes, uint64_t h, const uint64_t *want_idx, int nwant)
{
	uint64_t *root = aligned_alloc(16384, 3 * 16384);
	memset(root, 0, 3 * 16384);
	uint64_t *l2 = root + 2 * 16384 / 8;
	struct ba a = { .v = { base, 1, rowbytes, 0, h, 32 } };
	run_fbmap(&a, root);
	int set = 0;
	for (int i = 0; i < 2048; i++)
		if (l2[i]) {
			set++;
			int ok = 0;
			for (int k = 0; k < nwant; k++)
				ok |= (uint64_t)i == want_idx[k] && l2[i] == (((uint64_t)i << 25) | ATTRS);
			if (!ok)
				printf("  unexpected l2[%#x] = %#llx\n", i, (unsigned long long)l2[i]);
			CHECK(ok);
		}
	for (int i = 0; i < 2 * 2048; i++)
		CHECK(root[i] == 0);
	printf("fbmap base=%#llx len=%#llx: %d block(s)\n", (unsigned long long)base,
	       (unsigned long long)(rowbytes * h), set);
	CHECK(set == nwant);
	free(root);
}

int main(void)
{
	band(1080, 2400, 4320);		/* sweet: row 2368, as the old constant */
	band(1080, 2412, 4352);		/* lemonadep FHD, padded pitch */
	band(1440, 3216, 5760);		/* lemonadep WQHD */
	band_none();

	const uint64_t sweet[] = { 0x9c000000 >> 25 };
	fbmap(0x9c000000, 4320, 2400, sweet, 1);		/* identical to the old single block */
	const uint64_t lemon[] = { 0xe4000000 >> 25 };
	fbmap(0xe4d00000, 4352, 2412, lemon, 1);
	fbmap(0xe4d00000, 5760, 3216, lemon, 1);
	const uint64_t two[] = { 0xe4000000 >> 25, 0xe6000000 >> 25 };
	fbmap(0xe5f00000, 4352, 2412, two, 2);		/* crosses a 32MB boundary */
	fbmap(0, 4352, 2412, NULL, 0);			/* no framebuffer: no mapping */
	fbmap(0xe4d00000, 0, 2412, NULL, 0);
	printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
