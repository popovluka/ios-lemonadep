#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Generic read-only walk. A node is the address of its FDT_BEGIN_NODE token.
 * Path components match a node name exactly or up to its "@unit" suffix, so
 * "/memory" finds both "memory" and "memory@80000000".
 */
uint32_t fdt_be32(uintptr_t a);
int fdt_node(uintptr_t fdt, const char *path, uintptr_t *node);
int fdt_prop(uintptr_t fdt, uintptr_t node, const char *name, uintptr_t *val, uint32_t *len);
/* *iter = 0 for the first child; returns 0 when there are no more. */
int fdt_child(uintptr_t fdt, uintptr_t node, uintptr_t *iter);
/* Node name (NUL-terminated, in the blob). */
const char *fdt_name(uintptr_t node);
/* Read entry i of a reg property with ac/sc address/size cells (1 or 2). */
int fdt_reg(uintptr_t fdt, uintptr_t node, int ac, int sc, int i, uint64_t *addr, uint64_t *size);
