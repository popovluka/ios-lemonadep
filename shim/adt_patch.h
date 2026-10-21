#pragma once
#include <stdint.h>

/*
 * In-place edits of the ADT after the loader copied it, for values only the
 * running board knows (lemonadep: splash geometry). Layout as tools/adt.py:
 * node = {u32 nprops, u32 nchildren, props..., children...},
 * prop = {char name[32], u32 size (bit31 placeholder), value padded to 4}.
 */

/* Depth-first search for the node whose "name" is name. */
int adt_find_node(uintptr_t adt, uint32_t adt_size, const char *name, uintptr_t *node);
/* Overwrite an existing property of the same length (multiple of 4). */
int adt_set_prop(uintptr_t node, const char *prop, const uint32_t *val, uint32_t len);
