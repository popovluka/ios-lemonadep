#pragma once
#include <stdint.h>

#define PAYLOAD_MAGIC_LEN	8

/*
 * Find the first "SWEETXNU" in [lo, hi). With a boot image header v3+ ABL
 * hands over vendor_boot's ramdisk(s) followed by ours as ONE initrd, so the
 * payload is not at initrd-start and need not be 8-byte aligned. Reads are
 * aligned 8-byte loads (Device memory with the MMU off) and may touch up to
 * 7 bytes on either side of the range. Returns 0 if absent.
 */
uintptr_t payload_find(uintptr_t lo, uintptr_t hi);

/* memcpy for Device memory: dst 8-byte aligned, src any alignment, n rounded up to 8. */
void copy_any(uintptr_t dst, uintptr_t src, uint64_t n);
