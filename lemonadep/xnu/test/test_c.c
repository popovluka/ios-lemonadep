/*
 * Host (ASan/UBSan) test of 1001's C code, compiled verbatim:
 *   - osfmk/arm64/sweet_early.c (whole file, real pexpert/arm64/boot.h)
 *   - sweet_early_map_final() extracted from osfmk/arm64/arm_vm_init.c
 * Usage: test_c CASE   (sweet_early keeps static state: one case per process)
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pexpert/arm64/boot.h>
#include <arm64/sweet_early.h>

/* ---- mocks for sweet_early.c ---- */
unsigned char iso_font[256 * 16];
void
_doprnt(const char *fmt, va_list *argp, void (*putc)(char), int radix)
{
	(void)argp;
	(void)radix;
	while (*fmt)
		putc(*fmt++);
}

/* ---- mocks for sweet_early_map_final() ---- */
typedef uint64_t vm_map_address_t;
typedef uint64_t tt_entry_t;
#define ARM_TT_L2_SIZE		0x0000000002000000ULL	/* proc_reg.h ARM_16K_TT_L2_SIZE */
#define ARM_TT_L2_OFFMASK	0x0000000001ffffffULL
#define ARM_TT_L2_SHIFT		25
#define ARM_TT_L2_INDEX_MASK	0x0000000ffe000000ULL
#define ARM_TT_L1_SHIFT		36
#define ARM_TT_L1_INDEX_MASK	0x00007ff000000000ULL
#define ARM_TTE_VALID		0x1ULL
#define ARM_TTE_TYPE_MASK	0x2ULL
#define ARM_TTE_TYPE_TABLE	0x2ULL
#define ARM_TTE_TYPE_BLOCK	0x0ULL
#define ARM_TTE_EMPTY		0x0ULL
#define ARM_TTE_TABLE_MASK	0x0000fffffffff000ULL
#define ARM_TTE_BLOCK_SH(x)	((x) << 8)
#define ARM_TTE_BLOCK_ATTRINDX(x) ((x) << 2)
#define ARM_TTE_BLOCK_AF	0x400ULL
#define ARM_TTE_BLOCK_PNX	0x0020000000000000ULL
#define ARM_TTE_BLOCK_NX	0x0040000000000000ULL
#define SH_OUTER_MEMORY		0x2
#define CACHE_ATTRINDX_WRITECOMB 0x1
/* A 16K-granule kernel VA; the mock L1 table covers every L1 index. */
#define VM_MIN_KERNEL_ADDRESS	0xffffff9000000000ULL
static tt_entry_t cpu_tte[2048];
static vm_map_address_t gVirtBase;
#define phystokv(pa) ((uintptr_t)(pa))
#include "map_final.inc"

#define ATTRS (ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | \
	       ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | \
	       ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static tt_entry_t *l2_for_alias(void)
{
	static tt_entry_t *l2;
	if (!l2) {
		l2 = aligned_alloc(16384, 16384);
		memset(l2, 0, 16384);
		cpu_tte[(SWEET_EARLY_FB_VA & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT] =
		    (uintptr_t)l2 | ARM_TTE_VALID | ARM_TTE_TYPE_TABLE;
	}
	return l2;
}

static void map_case(uint64_t base, uint64_t rowbytes, uint64_t h, uint64_t vgap_blocks,
    uint64_t want_off, int want_blocks)
{
	tt_entry_t *l2 = l2_for_alias();
	memset(l2, 0, 16384);
	gVirtBase = SWEET_EARLY_FB_VA + vgap_blocks * ARM_TT_L2_SIZE;
	boot_args a;
	memset(&a, 0, sizeof(a));
	a.Video.v_baseAddr = base;
	a.Video.v_rowBytes = rowbytes;
	a.Video.v_height = h;
	vm_map_address_t va = sweet_early_map_final(&a);
	unsigned slot0 = (SWEET_EARLY_FB_VA & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT;
	int set = 0;
	for (int i = 0; i < 2048; i++)
		set += l2[i] != 0;
	if (want_blocks) {
		CHECK(va == SWEET_EARLY_FB_VA + want_off);
		for (int i = 0; i < want_blocks; i++)
			CHECK(l2[slot0 + i] == (((base & ~ARM_TT_L2_OFFMASK) + i * ARM_TT_L2_SIZE) | ATTRS));
	} else {
		CHECK(va == 0);
	}
	CHECK(set == want_blocks);
	printf("map base=%#llx len=%#llx gap=%llu: va off=%#llx blocks=%d\n", (unsigned long long)base,
	       (unsigned long long)(rowbytes * h), (unsigned long long)vgap_blocks,
	       (unsigned long long)(va ? va - SWEET_EARLY_FB_VA : 0), set);
}

static void map_occupied(void)
{
	tt_entry_t *l2 = l2_for_alias();
	memset(l2, 0, 16384);
	gVirtBase = SWEET_EARLY_FB_VA + 4 * ARM_TT_L2_SIZE;
	unsigned slot0 = (SWEET_EARLY_FB_VA & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT;
	l2[slot0 + 1] = 0x1234;		/* second block already used: refuse, write nothing */
	boot_args a;
	memset(&a, 0, sizeof(a));
	a.Video.v_baseAddr = 0xe5f00000;
	a.Video.v_rowBytes = 4352;
	a.Video.v_height = 2412;
	CHECK(sweet_early_map_final(&a) == 0);
	CHECK(l2[slot0] == 0 && l2[slot0 + 1] == 0x1234);
	printf("map occupied: refused\n");
}

/* ---- sweet_early.c drawing ---- */
static uint32_t *fb;
static size_t fb_words;

static void setup(uint32_t w, uint32_t h, uint32_t rowbytes, uint32_t depth)
{
	fb_words = (size_t)rowbytes / 4 * h;
	fb = calloc(fb_words, 4);	/* exact size: ASan catches any write past it */
	boot_args a;
	memset(&a, 0, sizeof(a));
	a.Video.v_baseAddr = (uintptr_t)fb;
	a.Video.v_rowBytes = rowbytes;
	a.Video.v_width = w;
	a.Video.v_height = h;
	a.Video.v_depth = depth;
	sweet_early_setup(&a);
}

static size_t nonzero(void)
{
	size_t n = 0;
	for (size_t i = 0; i < fb_words; i++)
		n += fb[i] != 0;
	return n;
}

static void draw_all(uint32_t h, uint32_t rowbytes)
{
	memset(iso_font, 0xff, sizeof(iso_font));
	for (unsigned s = 0; s <= 20; s++)
		sweet_early_mark(s, "a long enough label to wrap the line on a 1080 pixel wide screen");
	for (int i = 0; i < 5000; i++)	/* wraps rows many times */
		sweet_early_putc((char)('A' + i % 26));
	/* stage band for stage 20: last band, bottom 32 rows */
	uint32_t stride = rowbytes / 4;
	CHECK(fb[(size_t)(h - 1) * stride + 20 * 48] != 0);
	CHECK(fb[(size_t)(h - 1) * stride + 21 * 48] == 0);
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	if (!strcmp(c, "map")) {
		map_case(0x9c000000, 4320, 2400, 1, 0, 1);		/* sweet: same VA, same single block */
		map_case(0xe4d00000, 4352, 2412, 1, 0xd00000, 1);	/* lemonadep FHD */
		map_case(0xe4d00000, 5760, 3216, 1, 0xd00000, 1);	/* lemonadep WQHD */
		map_case(0xe5f00000, 4352, 2412, 2, 0x1f00000, 2);	/* crosses a block */
		map_case(0xe5f00000, 4352, 2412, 1, 0, 0);		/* needs 2, gap has 1: off */
		map_case(0, 4352, 2412, 4, 0, 0);			/* no framebuffer */
		map_occupied();
	} else if (!strcmp(c, "sweet")) {
		setup(1080, 2400, 4320, 32);
		draw_all(2400, 4320);
		printf("sweet geometry: %zu pixels drawn\n", nonzero());
	} else if (!strcmp(c, "lemonadep")) {
		setup(1080, 2412, 4352, 32);
		draw_all(2412, 4352);
		printf("lemonadep geometry: %zu pixels drawn\n", nonzero());
	} else if (!strcmp(c, "depth24")) {
		setup(1080, 2412, 3264, 24);
		sweet_early_mark(3, "x");
		sweet_early_putc('x');
		CHECK(nonzero() == 0);
		printf("24 bpp: console stays off\n");
	} else if (!strcmp(c, "nofb")) {
		boot_args a;
		memset(&a, 0, sizeof(a));
		sweet_early_setup(&a);
		sweet_early_mark(3, "x");	/* early_fb NULL: must not touch it */
		printf("no framebuffer: console stays off\n");
	} else if (!strcmp(c, "relocate0")) {
		setup(1080, 2400, 4320, 32);
		sweet_early_relocate(0);	/* mark 8 drawn, then off */
		size_t before = nonzero();
		sweet_early_mark(9, "after");
		va_list ap;
		sweet_early_panic("boom", &ap);
		CHECK(before > 0 && nonzero() == before);
		printf("relocate(0): disabled after stage 8\n");
	} else {
		fprintf(stderr, "case?\n");
		return 2;
	}
	printf("%s %s (%d failures)\n", c, fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
