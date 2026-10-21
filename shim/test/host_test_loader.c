/*
 * Host tests for the board-independent shim pieces the lemonadep port added:
 * payload_scan.c (find + misaligned copy), fdt.c (generic walk), adt_patch.c.
 *   host_test_loader MINI.dtb LEMONADEP.adt      (see shim/host_tests.sh)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../adt_patch.h"
#include "../fdt.h"
#include "../payload_scan.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = aligned_alloc(16, (*len + 15) & ~15L);
	if (fread(buf, 1, *len, f) != (size_t)*len)
		exit(2);
	fclose(f);
	return buf;
}

static void test_scan(void)
{
	enum { N = 1 << 16, PAD = 64 };
	unsigned char *buf = aligned_alloc(16, N + 2 * PAD);
	for (int off = 0; off < 24; off++) {
		/* noise full of 'S' and near-misses, then the magic at an odd place */
		for (int i = 0; i < N + 2 * PAD; i++)
			buf[i] = (i * 7) % 3 ? 'S' : (unsigned char)i;
		memcpy(buf + PAD + 1000, "SWEETXN!", 8);
		uintptr_t lo = (uintptr_t)buf + PAD + off, hi = (uintptr_t)buf + PAD + N;
		CHECK(payload_find(lo, hi) == 0);
		memcpy((void *)(lo + 4093), "SWEETXNU", 8);
		CHECK(payload_find(lo, hi) == lo + 4093);
		/* magic at the very start (sweet: boot ramdisk only) */
		memcpy((void *)lo, "SWEETXNU", 8);
		CHECK(payload_find(lo, hi) == lo);
		/* a magic straddling hi must not be reported */
		memset((void *)lo, 0, 4101);
		memcpy((void *)(hi - 5), "SWEETXNU", 8);
		CHECK(payload_find(lo, hi) == 0);
	}
	free(buf);
}

static void test_copy(void)
{
	enum { N = 4096 };
	unsigned char *src = aligned_alloc(16, N + 32), *dst = aligned_alloc(16, N + 32);
	for (int i = 0; i < N + 32; i++)
		src[i] = (unsigned char)(i * 131 + 7);
	for (int mis = 0; mis < 8; mis++)
		for (int n = 1; n < 80; n += 13) {
			memset(dst, 0xee, N + 32);
			copy_any((uintptr_t)dst, (uintptr_t)src + mis, n);
			CHECK(memcmp(dst, src + mis, n) == 0);
		}
	free(src);
	free(dst);
}

static void test_fdt(uintptr_t fdt)
{
	uintptr_t node, v, it;
	uint32_t len;
	uint64_t a, s;

	CHECK(fdt_node(fdt, "/memory", &node));
	CHECK(strcmp(fdt_name(node), "memory") == 0);
	CHECK(fdt_reg(fdt, node, 2, 2, 0, &a, &s) && a == 0x80000000 && s == 0x40000000);
	CHECK(fdt_reg(fdt, node, 2, 2, 1, &a, &s) && a == 0x100000000ULL && s == 0x100000000ULL);
	CHECK(!fdt_reg(fdt, node, 2, 2, 2, &a, &s));

	CHECK(fdt_node(fdt, "/reserved-memory/splash_region", &node));
	CHECK(fdt_reg(fdt, node, 2, 2, 0, &a, &s) && a == 0xe4d00000 && s == 0x2300000);
	CHECK(fdt_node(fdt, "/reserved-memory/hyp_region", &node));	/* matches up to @unit */
	CHECK(fdt_prop(fdt, node, "no-map", &v, &len) && len == 0);
	CHECK(!fdt_node(fdt, "/reserved-memory/splash", &node));		/* no prefix matches */
	CHECK(!fdt_node(fdt, "/nonexistent", &node));
	CHECK(fdt_node(fdt, "/memory-extra", &node) && strcmp(fdt_name(node), "memory-extra@0") == 0);

	CHECK(fdt_node(fdt, "/reserved-memory", &node));
	int nchild = 0, nomap = 0;
	for (it = 0; fdt_child(fdt, node, &it);) {
		nchild++;
		nomap += fdt_prop(fdt, it, "no-map", &v, &len);
	}
	CHECK(nchild == 3 && nomap == 2);

	CHECK(fdt_node(fdt, "/chosen", &node));
	CHECK(fdt_prop(fdt, node, "bootargs", &v, &len) && strstr((const char *)v, "msm_drm.dsi_display0="));
	uint64_t rd;
	CHECK(fdt_chosen_u64(fdt, "linux,initrd-start", &rd) && rd == 0x90000000);
}

static void test_adt(uintptr_t adt, uint32_t size)
{
	uintptr_t node;
	const uint32_t w = 1080, h = 2412, st = 4352, reg[4] = { 0xe4d00000, 0, 0x2300000, 0 };

	CHECK(adt_find_node(adt, size, "sweet-fbcon", &node));
	CHECK(adt_set_prop(node, "width", &w, 4));
	CHECK(adt_set_prop(node, "height", &h, 4));
	CHECK(adt_set_prop(node, "stride", &st, 4));
	CHECK(adt_set_prop(node, "reg", reg, 16));
	CHECK(!adt_set_prop(node, "width", reg, 16));		/* length must match */
	CHECK(!adt_set_prop(node, "nonexistent", &w, 4));
	uintptr_t again;
	CHECK(adt_find_node(adt, size, "sweet-fbcon", &again) && again == node);
	CHECK(adt_find_node(adt, size, "gic", &node));		/* nested under arm-io */
	CHECK(!adt_find_node(adt, size, "sweet-fbco", &node));
	CHECK(!adt_find_node(adt, size - 64, "sweet-fbcon", &node));	/* truncated: refuse */
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s MINI.dtb LEMONADEP.adt PATCHED_OUT.adt\n", argv[0]);
		return 2;
	}
	long dl, al;
	void *dtb = slurp(argv[1], &dl), *adt = slurp(argv[2], &al);
	test_scan();
	test_copy();
	test_fdt((uintptr_t)dtb);
	test_adt((uintptr_t)adt, (uint32_t)al);
	FILE *f = fopen(argv[3], "wb");
	fwrite(adt, 1, al, f);
	fclose(f);
	free(dtb);
	free(adt);
	printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	fflush(stdout);
	return fails != 0;
}
