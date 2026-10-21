/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

/* ---- generic walk (lemonadep: /memory, /reserved-memory, splash reg) ---- */

#define FDT_MAX_TOKENS	0x400000	/* bound every walk; a corrupt blob must not hang us */

uint32_t fdt_be32(uintptr_t a)
{
	return be32(a);
}

static int fdt_ok(uintptr_t fdt)
{
	return fdt && be32(fdt) == FDT_MAGIC;
}

const char *fdt_name(uintptr_t node)
{
	return (const char *)(node + 4);
}

/* node -> first token after its name */
static uintptr_t node_body(uintptr_t node)
{
	return align4(skip_str(node + 4));
}

/* p at FDT_BEGIN_NODE -> first token after the matching FDT_END_NODE (0 on garbage) */
static uintptr_t skip_node(uintptr_t p)
{
	int depth = 0;
	for (int n = 0; n < FDT_MAX_TOKENS; n++) {
		uint32_t tok = be32(p);
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			p = align4(skip_str(p + 4));
			break;
		case FDT_END_NODE:
			p += 4;
			if (--depth == 0)
				return p;
			break;
		case FDT_PROP:
			p = align4(p + 12 + be32(p + 4));
			break;
		case FDT_NOP:
			p += 4;
			break;
		default:
			return 0;
		}
	}
	return 0;
}

int fdt_child(uintptr_t fdt, uintptr_t node, uintptr_t *iter)
{
	if (!fdt_ok(fdt))
		return 0;
	uintptr_t p = *iter ? skip_node(*iter) : node_body(node);
	for (int n = 0; p && n < FDT_MAX_TOKENS; n++) {
		uint32_t tok = be32(p);
		if (tok == FDT_BEGIN_NODE) {
			*iter = p;
			return 1;
		}
		if (tok == FDT_PROP)
			p = align4(p + 12 + be32(p + 4));
		else if (tok == FDT_NOP)
			p += 4;
		else
			return 0;	/* FDT_END_NODE: no more children */
	}
	return 0;
}

int fdt_prop(uintptr_t fdt, uintptr_t node, const char *name, uintptr_t *val, uint32_t *len)
{
	if (!fdt_ok(fdt))
		return 0;
	uintptr_t strings = fdt + be32(fdt + 12);
	uintptr_t p = node_body(node);
	for (int n = 0; n < FDT_MAX_TOKENS; n++) {
		uint32_t tok = be32(p);
		if (tok == FDT_NOP) {
			p += 4;
			continue;
		}
		if (tok != FDT_PROP)
			return 0;	/* properties come before children */
		uint32_t l = be32(p + 4);
		if (streq((const char *)(strings + be32(p + 8)), name)) {
			*val = p + 12;
			*len = l;
			return 1;
		}
		p = align4(p + 12 + l);
	}
	return 0;
}

/* Does node name n match path component c[0..clen)? Exact, or up to '@'. */
static int name_match(const char *n, const char *c, int clen)
{
	for (int i = 0; i < clen; i++)
		if (n[i] != c[i])
			return 0;
	return n[clen] == 0 || n[clen] == '@';
}

int fdt_node(uintptr_t fdt, const char *path, uintptr_t *node)
{
	if (!fdt_ok(fdt) || *path != '/')
		return 0;
	uintptr_t cur = fdt + be32(fdt + 8);	/* root FDT_BEGIN_NODE */
	while (be32(cur) == FDT_NOP)
		cur += 4;
	while (*path == '/')
		path++;
	while (*path) {
		int clen = 0;
		while (path[clen] && path[clen] != '/')
			clen++;
		uintptr_t it = 0;
		int found = 0;
		while (fdt_child(fdt, cur, &it))
			if (name_match(fdt_name(it), path, clen)) {
				found = 1;
				break;
			}
		if (!found)
			return 0;
		cur = it;
		path += clen;
		while (*path == '/')
			path++;
	}
	*node = cur;
	return 1;
}

static uint64_t cells(uintptr_t v, int n)
{
	return n == 2 ? (uint64_t)be32(v) << 32 | be32(v + 4) : be32(v);
}

int fdt_reg(uintptr_t fdt, uintptr_t node, int ac, int sc, int i, uint64_t *addr, uint64_t *size)
{
	uintptr_t v;
	uint32_t len, stride = 4u * (ac + sc);
	if (!fdt_prop(fdt, node, "reg", &v, &len) || (uint32_t)(i + 1) * stride > len)
		return 0;
	v += i * stride;
	*addr = cells(v, ac);
	*size = cells(v + 4 * ac, sc);
	return 1;
}
