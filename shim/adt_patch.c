/* In-place ADT property edits (see adt_patch.h). Aligned 32-bit accesses only. */
#include "adt_patch.h"

#define ADT_NAME_LEN	32
#define ADT_MAX_DEPTH	16

static inline uint32_t rd32(uintptr_t a)
{
	return *(volatile const uint32_t *)a;
}

static int name_is(uintptr_t a, const char *s, int max)
{
	int i = 0;
	for (; i < max && s[i]; i++)
		if (*(volatile const char *)(a + i) != s[i])
			return 0;
	return i == max || *(volatile const char *)(a + i) == 0;
}

/* Find prop in node; returns its value address and length (flag masked). */
static uintptr_t prop_find(uintptr_t node, const char *prop, uint32_t *len)
{
	uint32_t n = rd32(node);
	uintptr_t p = node + 8;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t l = rd32(p + ADT_NAME_LEN) & 0x7fffffffu;
		if (name_is(p, prop, ADT_NAME_LEN)) {
			*len = l;
			return p + ADT_NAME_LEN + 4;
		}
		p += ADT_NAME_LEN + 4 + ((l + 3) & ~3u);
	}
	return 0;
}

/* Address just past node and all of its children; 0 if it runs past end. */
static uintptr_t node_end(uintptr_t node, uintptr_t end, int depth)
{
	if (depth > ADT_MAX_DEPTH || node + 8 > end)
		return 0;
	uint32_t n = rd32(node), c = rd32(node + 4);
	uintptr_t p = node + 8;
	for (uint32_t i = 0; i < n; i++) {
		if (p + ADT_NAME_LEN + 4 > end)
			return 0;
		p += ADT_NAME_LEN + 4 + (((rd32(p + ADT_NAME_LEN) & 0x7fffffffu) + 3) & ~3u);
	}
	for (uint32_t i = 0; i < c && p; i++)
		p = node_end(p, end, depth + 1);
	return p && p <= end ? p : 0;
}

static int find(uintptr_t node, uintptr_t end, const char *name, uintptr_t *out, int depth)
{
	uint32_t len;
	uintptr_t v = prop_find(node, "name", &len);
	if (v && name_is(v, name, (int)len)) {
		*out = node;
		return 1;
	}
	if (depth > ADT_MAX_DEPTH)
		return 0;
	uint32_t n = rd32(node), c = rd32(node + 4);
	uintptr_t p = node + 8;
	for (uint32_t i = 0; i < n; i++)
		p += ADT_NAME_LEN + 4 + (((rd32(p + ADT_NAME_LEN) & 0x7fffffffu) + 3) & ~3u);
	for (uint32_t i = 0; i < c && p; i++) {
		if (find(p, end, name, out, depth + 1))
			return 1;
		p = node_end(p, end, depth + 1);
	}
	return 0;
}

int adt_find_node(uintptr_t adt, uint32_t adt_size, const char *name, uintptr_t *node)
{
	if (!node_end(adt, adt + adt_size, 0))
		return 0;	/* malformed: do not walk it */
	return find(adt, adt + adt_size, name, node, 0);
}

int adt_set_prop(uintptr_t node, const char *prop, const uint32_t *val, uint32_t len)
{
	uint32_t have;
	uintptr_t v = prop_find(node, prop, &have);
	if (!v || have != len || (len & 3))
		return 0;
	for (uint32_t i = 0; i < len / 4; i++)
		*(volatile uint32_t *)(v + 4 * i) = val[i];
	return 1;
}
