#pragma once
#include <stdint.h>

/* What ABL left in the display engine (lemonadep MDSS); raw values for the screen log. */
struct disp_info {
	int ctl;			/* CTL driving DSI0 (INTF_1), -1 if none */
	uint32_t intf_active, fetch_active;
	int pipe;			/* SSPP index 0-7 (VIG0-3, DMA0-3) scanning the splash, -1 if none */
	uint32_t npipes;		/* pipes whose SRC0_ADDR is inside the splash region */
	uint32_t addr, src_size, src_xy, ystride0, format, unpack;
	uint32_t w, h, stride, bpp;	/* derived framebuffer geometry */
	uint32_t tear_check, autorefresh_before, autorefresh_after;
	uintptr_t kick;			/* CTL_START address if frames must be kicked, else 0 */
};

/*
 * Find the plane ABL scans out of [splash_lo, splash_hi) and make CPU writes
 * reach a command-mode panel: enable INTF tear-check autorefresh, or fall
 * back to kicking CTL_START after every console update. Returns 1 when a
 * plane was found (info->w/h/stride/bpp valid).
 */
int display_discover(uint64_t splash_lo, uint64_t splash_hi, struct disp_info *info);
