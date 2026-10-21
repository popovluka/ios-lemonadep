/*
 * lemonadep: adopt the continuous-splash plane that ABL programmed into the
 * SM8350 display engine (MDSS/SDE) instead of assuming a geometry.
 *
 * Every offset below is from the OnePlus vendor sources (lemonadep/HWMAP_lemonadep.md):
 *   display-devicetree/display/lahaina-sde-common.dtsi  mdss_mdp @ 0x0ae00000,
 *     qcom,sde-ctl-off, qcom,sde-sspp-off, qcom,sde-intf-off/-type
 *   techpack/display/msm/sde/sde_hw_ctl.c       CTL_START, CTL_INTF_ACTIVE, CTL_FETCH_PIPE_ACTIVE, fetch_tbl
 *   techpack/display/msm/sde/sde_hw_sspp.c      SSPP_SRC_* (src_blk.base = 0)
 *   techpack/display/msm/sde/sde_hw_intf.c      INTF_TEAR_TEAR_CHECK_EN, INTF_TEAR_AUTOREFRESH_CONFIG
 *
 * The panel (Samsung AMB670YF01) is a DSI *command-mode* panel: it shows its
 * own frame memory and only gets a new frame when MDP sends one. Autorefresh
 * makes MDP send a frame on every tear-effect pulse, so CPU writes to the
 * splash buffer show up as on sweet's video-mode panel.
 * TODO(lemonadep): whether ABL's tear-check setup lets autorefresh run is
 * only known on the device (screen log line DISP AR).
 */
#include "board.h"
#include "display.h"

#if BOARD_HAS_MDSS

#define CTL_START		0x01c
#define CTL_INTF_ACTIVE		0x0f4
#define CTL_FETCH_PIPE_ACTIVE	0x0fc
#define INTF1_DSI0		(1u << 1)	/* BIT(INTF_1 - INTF_0); intf-type "dp","dsi","dsi","dp" */

#define SSPP_SRC_SIZE		0x00
#define SSPP_SRC_XY		0x08
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24
#define SSPP_SRC_FORMAT		0x30
#define SSPP_SRC_UNPACK_PATTERN	0x34

#define INTF1_OFF		0x36000		/* qcom,sde-intf-off[1], type "dsi" = DSI0 */
#define INTF_TEAR_TEAR_CHECK_EN		0x284
#define INTF_TEAR_AUTOREFRESH_CONFIG	0x2b4
#define AUTOREFRESH_EN		(1u << 31)
#define AUTOREFRESH_FRAMES	1		/* a frame on every TE */

#define NCTL			6
#define NPIPE			8

static inline uint32_t rd(uintptr_t off)
{
	return *(volatile uint32_t *)(MDP_BASE + off);
}

static inline void wr(uintptr_t off, uint32_t v)
{
	*(volatile uint32_t *)(MDP_BASE + off) = v;
}

static uint32_t ctl_off(int i)
{
	return 0x16000 + 0x1000 * i;		/* 0x16000 ... 0x1b000 */
}

static uint32_t pipe_off(int i)
{
	/* VIG0-3: 0x5000 0x7000 0x9000 0xb000; DMA0-3: 0x25000 0x27000 0x29000 0x2b000 */
	return (i < 4 ? 0x5000 : 0x25000) + 0x2000 * (i & 3);
}

static uint32_t pipe_fetch_bit(int i)
{
	/* fetch_tbl: VIG0-3 -> bits 16-19, DMA0-3 -> bits 0-3 */
	return 1u << (i < 4 ? 16 + i : i - 4);
}

int display_discover(uint64_t splash_lo, uint64_t splash_hi, struct disp_info *info)
{
	*info = (struct disp_info){ .ctl = -1, .pipe = -1 };

	for (int c = 0; c < NCTL; c++) {
		uint32_t ia = rd(ctl_off(c) + CTL_INTF_ACTIVE);
		if (ia & INTF1_DSI0) {
			info->ctl = c;
			info->intf_active = ia;
			info->fetch_active = rd(ctl_off(c) + CTL_FETCH_PIPE_ACTIVE);
			break;
		}
	}

	for (int i = 0; i < NPIPE; i++) {
		if (info->fetch_active && !(info->fetch_active & pipe_fetch_bit(i)))
			continue;
		uint32_t a = rd(pipe_off(i) + SSPP_SRC0_ADDR);
		if (a < splash_lo || a >= splash_hi)
			continue;
		info->npipes++;
		if (info->pipe >= 0 && a >= info->addr)
			continue;	/* keep the lowest address: left half of a split */
		info->pipe = i;
		info->addr = a;
		info->src_size = rd(pipe_off(i) + SSPP_SRC_SIZE);
		info->src_xy = rd(pipe_off(i) + SSPP_SRC_XY);
		info->ystride0 = rd(pipe_off(i) + SSPP_SRC_YSTRIDE0);
		info->format = rd(pipe_off(i) + SSPP_SRC_FORMAT);
		info->unpack = rd(pipe_off(i) + SSPP_SRC_UNPACK_PATTERN);
	}

	info->tear_check = rd(INTF1_OFF + INTF_TEAR_TEAR_CHECK_EN);
	info->autorefresh_before = rd(INTF1_OFF + INTF_TEAR_AUTOREFRESH_CONFIG);
	info->autorefresh_after = info->autorefresh_before;
	if (info->pipe < 0)
		return 0;

	info->bpp = ((info->format >> 9) & 3) + 1;	/* SRC_FORMAT[10:9] = bpp - 1 */
	info->stride = info->ystride0 & 0xffff;		/* plane 0 pitch */
	info->h = info->src_size >> 16;
	/* A split (two pipes, one buffer) shows half the width per pipe: use the pitch. */
	info->w = info->stride && info->stride % info->bpp == 0 ? info->stride / info->bpp
							: info->src_size & 0xffff;

	if (info->ctl >= 0 && !(info->autorefresh_before & AUTOREFRESH_EN)) {
		wr(INTF1_OFF + INTF_TEAR_AUTOREFRESH_CONFIG, AUTOREFRESH_EN | AUTOREFRESH_FRAMES);
		__asm__ volatile("dsb sy" ::: "memory");
		wr(ctl_off(info->ctl) + CTL_START, 1);
		info->autorefresh_after = rd(INTF1_OFF + INTF_TEAR_AUTOREFRESH_CONFIG);
	}
	/* Autorefresh did not stick: kick a frame after every console update instead. */
	if (info->ctl >= 0 && !(info->autorefresh_after & AUTOREFRESH_EN))
		info->kick = MDP_BASE + ctl_off(info->ctl) + CTL_START;
	return 1;
}

#else

int display_discover(uint64_t splash_lo, uint64_t splash_hi, struct disp_info *info)
{
	(void)splash_lo;
	(void)splash_hi;
	*info = (struct disp_info){ .ctl = -1, .pipe = -1 };
	return 0;
}

#endif
