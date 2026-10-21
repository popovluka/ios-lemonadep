/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 *
 * With boot image header v3+ (lemonadep) ABL puts vendor_boot's ramdisk in
 * front of ours, so the payload is searched for and may be misaligned.
 */
#include <stdint.h>
#include "adt_patch.h"
#include "board.h"
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "payload_scan.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL
#define MAX_SEGS		64

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* Payload header + segment table, copied out of the ramdisk to aligned memory. */
static uint64_t hdr_buf[(sizeof(struct payload) + MAX_SEGS * sizeof(struct payload_seg)) / 8];

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

#if BOARD_CHECK_DRAM
/*
 * The kernel maps [phys_base, phys_base + mem_size) cacheable. Refuse unless
 * that range lies inside one live /memory bank and touches no no-map
 * carve-out of the live /reserved-memory (Qualcomm XPUs fault on those).
 */
static const char *check_dram(uintptr_t dtb, uint64_t lo, uint64_t size)
{
	uint64_t hi = lo + size, a, s;
	uintptr_t node, it, v;
	uint32_t len;
	int inside = 0;

	if (!fdt_node(dtb, "/memory", &node))
		return "no /memory in live FDT";
	for (int i = 0; fdt_reg(dtb, node, 2, 2, i, &a, &s); i++) {
		con_hex2("MEMORY", "base=", (uint32_t)(a >> 16), "size=", (uint32_t)(s >> 16));
		if (lo >= a && hi <= a + s)
			inside = 1;
	}
	if (!inside)
		return "kernel RAM outside /memory";
	if (!fdt_node(dtb, "/reserved-memory", &node))
		return "no /reserved-memory in live FDT";
	for (it = 0; fdt_child(dtb, node, &it);) {
		if (!fdt_prop(dtb, it, "no-map", &v, &len))
			continue;
		for (int i = 0; fdt_reg(dtb, it, 2, 2, i, &a, &s); i++)
			if (a < hi && lo < a + s) {
				con_str("OVERLAP", fdt_name(it));
				return "kernel RAM overlaps no-map carve-out";
			}
	}
	return 0;
}
#endif

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	uintptr_t pl = payload_find(rd_start, rd_end);
	if (!pl)
		return "no SWEETXNU payload";
	if (pl != rd_start)
		con_hex2("PAYLOAD", "at=", pl, "off=", pl - rd_start);
	if (pl + sizeof(struct payload) > rd_end)
		return "bad payload header";
	copy_any((uintptr_t)hdr_buf, pl, sizeof(struct payload));
	const struct payload *p = (const struct payload *)hdr_buf;
	if (p->version != 1 || (p->adt_off & 7) || p->nsegs > MAX_SEGS ||
	    pl + p->adt_off + p->adt_size > rd_end)
		return "bad payload header";
	copy_any((uintptr_t)p->segs, pl + sizeof(struct payload), p->nsegs * sizeof(struct payload_seg));

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";
#if BOARD_CHECK_DRAM
	const char *bad = check_dram(dtb, p->phys_base, p->mem_size);
	if (bad)
		return bad;
#endif

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		if (pl + s->file_off + s->file_size > rd_end)
			return "segment past ramdisk end";
		copy_any(dst, pl + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy_any(adt_pa, pl + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

#if FB_W == 0
	/* Splash geometry is only known now (display.c measured it): fill in the ADT. */
	uintptr_t fbcon;
	if (!con_w)
		return "fb geometry unknown: not starting xnu";
	if (!adt_find_node(adt_pa, (uint32_t)p->adt_size, "sweet-fbcon", &fbcon))
		return "ADT has no sweet-fbcon";
	uint64_t fb = (uintptr_t)con_fb, fb_len = FB_BASE + FB_SIZE - fb;
	const uint32_t reg[4] = { (uint32_t)fb, (uint32_t)(fb >> 32), (uint32_t)fb_len, (uint32_t)(fb_len >> 32) };
	const uint32_t w = con_w, h = con_h, st = con_stride;
	if (!adt_set_prop(fbcon, "width", &w, 4) || !adt_set_prop(fbcon, "height", &h, 4) ||
	    !adt_set_prop(fbcon, "stride", &st, 4) || !adt_set_prop(fbcon, "reg", reg, 16))
		return "ADT sweet-fbcon props missing";
	if (con_bpp != 4)
		con_str("WARN", "fb not 32bpp: xnu fbcon will misdraw");
#endif

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = (uintptr_t)con_fb;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = con_stride;
	ba->Video.v_width = con_w;
	ba->Video.v_height = con_h;
	ba->Video.v_depth = 8 * con_bpp;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
