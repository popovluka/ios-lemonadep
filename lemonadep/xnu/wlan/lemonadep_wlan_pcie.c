/* SPDX-License-Identifier: MIT */
/*
 * lemonadep Wi-Fi, stage 1: power the QCA6490, bring up PCIe root complex 0,
 * read the chip's config-space ID. Plain C with injected I/O, host-tested by
 * test_wlan_pcie.c; the kernel glue is bsd/dev/lemonadep_wlan.c.
 *
 * The order is the vendor kernel's, step for step (LOS-K, see
 * LemonadepWlanSource.h): cnss2/power.c (rails in cnss_vreg_list order, the
 * BT_EN-off delay, WLAN_EN active, 10 ms), then pci-msm.c msm_pcie_enable()
 * (assert PERST, PHY rails, GDSC + clocks + resets, PARF setup, the board's
 * PHY sequence, pipe clock, PHY ready, ep-latency, release PERST, iATU for
 * bus 1, LTSSM, link up, config space readable).
 *
 * Deliberately left out of stage 1: interrupts (everything is polled), the
 * SMMU stream for PCIe (no DMA yet), the interconnect bandwidth vote (only
 * needed for the chip's DMA into DDR), ASPM/L1ss, AER, and Gen2/Gen3: the
 * first link is held to Gen1 (2.5 GT/s), the most forgiving case.
 */
#include <stdbool.h>
#include <stdint.h>

#include "LemonadepWlanSource.h"
#include "LemonadepPcie0Phy.h"

enum {
	WlanWinTlmm, WlanWinGcc, WlanWinParf, WlanWinPhy, WlanWinDbi, WlanWinIatu, WlanWinConf,
	/* stage 2 (lemonadep_wlan_mhi.c): the RC's memory window, apps SMMU GR0 */
	WlanWinMem, WlanWinSmmu,
	WlanWinCount,
};

typedef struct {
	void *cookie;
	int (*read32)(void *cookie, unsigned window, uint32_t off, uint32_t *out);
	int (*write32)(void *cookie, unsigned window, uint32_t off, uint32_t value);
	/* RPMh VRM vote: voltage, optional HPM, enable. 0 on success. */
	int (*rpmh_vote)(void *cookie, const char *res, unsigned mv, int hpm);
	void (*delay_us)(void *cookie, unsigned us);
	/* Optional (stage 2): called before each access that could take the SoC
	 * down, so the kernel can log it and let the log reach the disk first. */
	void (*checkpoint)(void *cookie, const char *what);
	/* Optional (stage 2): a state change while polling the chip, logged
	 * without waiting (a, b, c are register values, ms the poll time). */
	void (*trace)(void *cookie, const char *what, uint32_t a, uint32_t b, uint32_t c, unsigned ms);
	/* Optional (stage 2): make `bytes` of kernel memory at `data` visible to
	 * the chip through an SMMU translation context and return the chip-side
	 * address. When set, the SMMU bypass entry of stage 14 is not attempted
	 * (on this phone the hypervisor turns bypass into fault). 0 on success. */
	int (*dma_map)(void *cookie, const void *data, uint32_t bytes, uint64_t *dma);
} lemonadep_wlan_io;

typedef enum {
	LemonadepWlanOk,
	LemonadepWlanBadArg,
	LemonadepWlanIoError,
	LemonadepWlanRailFailed,
	LemonadepWlanGdscTimeout,
	LemonadepWlanClockTimeout,
	LemonadepWlanPhyTimeout,
	LemonadepWlanLinkTimeout,
	LemonadepWlanNoDevice,
	LemonadepWlanWrongDevice,
} lemonadep_wlan_status;

typedef enum {
	LemonadepWlanPhaseNone,
	LemonadepWlanPhaseRails,      /* the six QCA6490 rails */
	LemonadepWlanPhaseWlanEn,     /* BT_EN check, WLAN_EN high, 10 ms */
	LemonadepWlanPhasePcieRails,  /* PERST asserted; PHY L5/L6 */
	LemonadepWlanPhaseGdsc,
	LemonadepWlanPhaseClocks,
	LemonadepWlanPhaseResets,
	LemonadepWlanPhaseParf,
	LemonadepWlanPhasePhy,        /* sequence + pipe clock + PHY ready */
	LemonadepWlanPhasePerst,      /* ep-latency, release PERST */
	LemonadepWlanPhaseLink,       /* iATU, Gen1, LTSSM, link up */
	LemonadepWlanPhaseConfig,     /* endpoint config space readable */
	LemonadepWlanPhaseDone,
} lemonadep_wlan_phase;

typedef struct {
	lemonadep_wlan_phase phase;   /* where it stopped or failed */
	const char *failed_what;      /* rail/clock name on a rail/clock failure */
	uint32_t bt_en, gdsc, phy_status, elbi_stts, link_status;
	unsigned phy_tries, link_tries;
	uint32_t id, class_rev;       /* endpoint config dwords 0 and 2 */
} lemonadep_wlan_result;

#define WTRY(expr) do { if (expr) return LemonadepWlanIoError; } while (0)
#define WPHASE(from, to) do { \
		if (stop_after != LemonadepWlanPhaseNone && stop_after == (from)) { return LemonadepWlanOk; } \
		r->phase = (to); \
	} while (0)

static int
rd(const lemonadep_wlan_io *io, unsigned w, uint32_t off, uint32_t *v)
{
	return io->read32(io->cookie, w, off, v);
}
static int
wr(const lemonadep_wlan_io *io, unsigned w, uint32_t off, uint32_t v)
{
	return io->write32(io->cookie, w, off, v);
}
static int
rmw(const lemonadep_wlan_io *io, unsigned w, uint32_t off, uint32_t clear, uint32_t set)
{
	uint32_t v;
	if (rd(io, w, off, &v)) {
		return 1;
	}
	return wr(io, w, off, (v & ~clear) | set);
}

/* TLMM: plain GPIO function, drive (mA/2 - 1 in [8:6]), pull, output, and the
 * eGPIO claim when the pad has one (touch learned this the hard way). */
static int
gpio_out(const lemonadep_wlan_io *io, unsigned pin, unsigned drive_ma, uint32_t pull, unsigned level)
{
	uint32_t cfg;
	if (rd(io, WlanWinTlmm, TLMM_CFG(pin), &cfg) ||
	    rmw(io, WlanWinTlmm, TLMM_IO(pin), 2u, level ? 2u : 0u)) {
		return 1;
	}
	cfg &= ~0x3ffu;
	cfg |= pull | (((drive_ma / 2u) - 1u) << 6) | TLMM_OE;
	if (cfg & TLMM_EGPIO_PRESENT) {
		cfg |= TLMM_EGPIO_ENABLE;
	}
	return wr(io, WlanWinTlmm, TLMM_CFG(pin), cfg);
}

/* RCG2: CFG = SRC_SEL [10:8] | SRC_DIV [4:0] (2*div-1) | HW_CLK_CTRL (bit 20,
 * HW_CLK_CTRL_MODE in gcc-lahaina.c), then CMD.UPDATE and wait for it to clear. */
static lemonadep_wlan_status
rcg_set(const lemonadep_wlan_io *io, uint32_t cmd, unsigned src, unsigned div)
{
	WTRY(rmw(io, WlanWinGcc, cmd + 4u, 0x7u << 8 | 0x1fu | 3u << 12, src << 8 | (2u * div - 1u) | 1u << 20));
	WTRY(rmw(io, WlanWinGcc, cmd, 0, 1u));
	for (unsigned i = 0; i < 500; ++i) {
		uint32_t v;
		WTRY(rd(io, WlanWinGcc, cmd, &v));
		if (!(v & 1u)) {
			return LemonadepWlanOk;
		}
		io->delay_us(io->cookie, 1);
	}
	return LemonadepWlanClockTimeout;
}

static lemonadep_wlan_status
branch_on(const lemonadep_wlan_io *io, const struct LemonadepGccBranch *b)
{
	WTRY(rmw(io, WlanWinGcc, b->vote, 0, 1u << b->bit));
	if (!b->poll) {
		return LemonadepWlanOk;
	}
	for (unsigned i = 0; i < 500; ++i) {
		uint32_t v;
		WTRY(rd(io, WlanWinGcc, b->halt, &v));
		if (!(v & GCC_CLK_OFF)) {
			return LemonadepWlanOk;
		}
		io->delay_us(io->cookie, 1);
	}
	return LemonadepWlanClockTimeout;
}

lemonadep_wlan_status
lemonadep_wlan_pcie_up(const lemonadep_wlan_io *io, lemonadep_wlan_result *r, lemonadep_wlan_phase stop_after)
{
	lemonadep_wlan_status st;
	if (!io || !r || !io->read32 || !io->write32 || !io->rpmh_vote || !io->delay_us) {
		return LemonadepWlanBadArg;
	}
	*r = (lemonadep_wlan_result){0};

	/* cnss2: rails in cnss_vreg_list order. */
	WPHASE(LemonadepWlanPhaseNone, LemonadepWlanPhaseRails);
	for (unsigned i = 0; i < sizeof(LemonadepWlanRails) / sizeof(LemonadepWlanRails[0]); ++i) {
		if (io->rpmh_vote(io->cookie, LemonadepWlanRails[i].res, LemonadepWlanRails[i].mv, 1)) {
			r->failed_what = LemonadepWlanRails[i].what;
			return LemonadepWlanRailFailed;
		}
	}

	/* cnss_select_pinctrl_enable(): BT_EN low -> 100 ms, then WLAN_EN, 10 ms. */
	WPHASE(LemonadepWlanPhaseRails, LemonadepWlanPhaseWlanEn);
	WTRY(rd(io, WlanWinTlmm, TLMM_IO(LEMONADEP_WLAN_BT_EN_GPIO), &r->bt_en));
	if (!(r->bt_en & 1u)) {
		io->delay_us(io->cookie, LEMONADEP_WLAN_BT_OFF_DELAY_US);
	}
	/* a chip left running by an earlier boot does not train again: power it off first (cnss_power_off) */
	WTRY(gpio_out(io, LEMONADEP_WLAN_EN_GPIO, LEMONADEP_WLAN_EN_DRIVE_MA, TLMM_PULL_UP, 0));
	io->delay_us(io->cookie, LEMONADEP_WLAN_BT_OFF_DELAY_US);
	WTRY(gpio_out(io, LEMONADEP_WLAN_EN_GPIO, LEMONADEP_WLAN_EN_DRIVE_MA, TLMM_PULL_UP, 1));
	io->delay_us(io->cookie, LEMONADEP_WLAN_EN_DELAY_US);

	/* msm_pcie_enable(): assert PERST (drive low), 1 ms; PHY rails. */
	WPHASE(LemonadepWlanPhaseWlanEn, LemonadepWlanPhasePcieRails);
	WTRY(gpio_out(io, LEMONADEP_PCIE0_PERST_GPIO, 2, TLMM_PULL_DOWN, 0));
	io->delay_us(io->cookie, PCIE_PERST_PROPAGATION_US);
	if (io->rpmh_vote(io->cookie, "ldob5", LEMONADEP_PCIE0_L5_MV, 1)) {
		r->failed_what = "vreg-0p9 (pm8350 L5)";
		return LemonadepWlanRailFailed;
	}
	if (io->rpmh_vote(io->cookie, "ldob6", LEMONADEP_PCIE0_L6_MV, 1)) {
		r->failed_what = "vreg-1p8 (pm8350 L6)";
		return LemonadepWlanRailFailed;
	}

	/* msm_pcie_clk_init(): GDSC (collapse vote) ... */
	WPHASE(LemonadepWlanPhasePcieRails, LemonadepWlanPhaseGdsc);
	WTRY(rmw(io, WlanWinGcc, GCC_PCIE0_GDSC_VOTE, 1u, 0));
	io->delay_us(io->cookie, 1);
	st = LemonadepWlanGdscTimeout;
	for (unsigned i = 0; i < GCC_PCIE0_GDSC_TIMEOUT_US; ++i) {
		WTRY(rd(io, WlanWinGcc, GCC_PCIE0_GDSC, &r->gdsc));
		if (r->gdsc & (1u << 31)) {
			st = LemonadepWlanOk;
			break;
		}
		io->delay_us(io->cookie, 1);
	}
	if (st != LemonadepWlanOk) {
		return st;
	}

	/* ... pipe mux to the PHY's pipe clock, clock rates, the voted branches ... */
	WPHASE(LemonadepWlanPhaseGdsc, LemonadepWlanPhaseClocks);
	WTRY(rmw(io, WlanWinGcc, GCC_PCIE0_PIPE_MUX, 3u, 0));
	if ((st = rcg_set(io, GCC_PCIE0_AUX_RCG, 0, 1)) != LemonadepWlanOk) {
		r->failed_what = "pcie_0_aux_clk_src";
		return st;
	}
	if ((st = rcg_set(io, GCC_PCIE0_RCHNG_RCG, 1, 6)) != LemonadepWlanOk) {
		r->failed_what = "pcie_0_phy_rchng_clk_src";
		return st;
	}
	for (unsigned i = 0; i < sizeof(LemonadepPcie0Branches) / sizeof(LemonadepPcie0Branches[0]); ++i) {
		if ((st = branch_on(io, &LemonadepPcie0Branches[i])) != LemonadepWlanOk) {
			r->failed_what = LemonadepPcie0Branches[i].name;
			return st;
		}
	}
	const struct LemonadepGccBranch clkref = {GCC_CLKREF_PCIE0, 0, GCC_CLKREF_PCIE0, 1, "pcie_0_clkref_en"};
	if ((st = branch_on(io, &clkref)) != LemonadepWlanOk) {
		r->failed_what = clkref.name;
		return st;
	}

	/* ... then each reset: assert, 1 ms, de-assert (core, then PHY). */
	WPHASE(LemonadepWlanPhaseClocks, LemonadepWlanPhaseResets);
	const uint32_t bcrs[2] = {GCC_PCIE0_BCR, GCC_PCIE0_PHY_BCR};
	for (unsigned i = 0; i < 2; ++i) {
		WTRY(rmw(io, WlanWinGcc, bcrs[i], 0, 1u));
		io->delay_us(io->cookie, 1000);
		WTRY(rmw(io, WlanWinGcc, bcrs[i], 1u, 0));
	}

	/* PARF: RC mode, L1 allowed, PHY out of reset, DBI at 0, SYS_CTRL, MHI
	 * clock, every PARF interrupt masked (stage 1 polls), slave space, write
	 * halt window on. */
	WPHASE(LemonadepWlanPhaseResets, LemonadepWlanPhaseParf);
	WTRY(wr(io, WlanWinParf, PCIE_PARF_DEVICE_TYPE, 0x4));
	WTRY(rmw(io, WlanWinParf, PCIE_PARF_PM_CTRL, 1u << 5, 0));
	WTRY(rmw(io, WlanWinParf, PCIE_PARF_PHY_CTRL, 1u << 0, 0));
	WTRY(wr(io, WlanWinParf, PCIE_PARF_DBI_BASE_ADDR, 0));
	WTRY(wr(io, WlanWinParf, PCIE_PARF_SYS_CTRL, 0x365e));
	WTRY(rmw(io, WlanWinParf, PCIE_PARF_MHI_CLOCK_RESET_CTRL, 0, 1u << 4));
	WTRY(wr(io, WlanWinParf, PCIE_PARF_INT_ALL_MASK, 0));
	WTRY(wr(io, WlanWinParf, PCIE_PARF_SLV_ADDR_SPACE_SIZE, LEMONADEP_PCIE0_SLV_ADDR_SPACE));
	WTRY(rmw(io, WlanWinParf, PCIE_PARF_AXI_MSTR_WR_ADDR_HALT, 0, 1u << 31));

	/* pcie_phy_init(): the board's sequence, 1 ms, pipe clock, PHY ready. */
	WPHASE(LemonadepWlanPhaseParf, LemonadepWlanPhasePhy);
	for (unsigned i = 0; i < sizeof(LemonadepPcie0PhySequence) / sizeof(LemonadepPcie0PhySequence[0]); ++i) {
		const struct LemonadepPcie0PhyWrite *p = &LemonadepPcie0PhySequence[i];
		WTRY(wr(io, WlanWinPhy, p->offset, p->value));
		if (p->delay_us) {
			io->delay_us(io->cookie, p->delay_us);
		}
	}
	io->delay_us(io->cookie, PCIE_PHY_STABILIZE_US);
	WTRY(rmw(io, WlanWinGcc, GCC_VOTE_B, 0, 1u << GCC_PCIE0_PIPE_VOTE_BIT));
	for (r->phy_tries = 0;; ++r->phy_tries) {
		WTRY(rd(io, WlanWinPhy, LEMONADEP_PCIE0_PHY_STATUS_OFF, &r->phy_status));
		if (!(r->phy_status & (1u << LEMONADEP_PCIE0_PHY_STATUS_BIT))) {
			break;
		}
		if (r->phy_tries >= PCIE_PHY_READY_TRIES) {
			return LemonadepWlanPhyTimeout;
		}
		io->delay_us(io->cookie, 1000);
	}

	/* ep-latency, then release PERST. */
	WPHASE(LemonadepWlanPhasePhy, LemonadepWlanPhasePerst);
	io->delay_us(io->cookie, LEMONADEP_PCIE0_EP_LATENCY_US);
	WTRY(gpio_out(io, LEMONADEP_PCIE0_PERST_GPIO, 2, TLMM_PULL_DOWN, 1));
	io->delay_us(io->cookie, PCIE_PERST_PROPAGATION_US);

	/* msm_pcie_config_controller(): iATU region 0 = CFG0 for bus 1 dev 0 fn 0,
	 * over the first 4 KiB of the conf window. Then msm_pcie_link_train(),
	 * held to Gen1. */
	WPHASE(LemonadepWlanPhasePerst, LemonadepWlanPhaseLink);
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_CTRL2, 0));
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_CTRL1, PCIE_IATU_TYPE_CFG0));
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_LBAR, LEMONADEP_PCIE0_CONF_PA));
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_UBAR, 0));
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_LAR, LEMONADEP_PCIE0_CONF_PA + 0xfffu));
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_LTAR, 1u << 24));
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_UTAR, 0));
	WTRY(wr(io, WlanWinIatu, PCIE_IATU_CTRL2, 1u << 31));
	WTRY(rmw(io, WlanWinDbi, PCIE_DBI_GEN3_GEN2_CTRL, 0x1f00u, 1u << 8));
	WTRY(rmw(io, WlanWinDbi, PCIE_DBI_CAP_LINKCTL2, 0xfu, 1u));
	WTRY(rmw(io, WlanWinParf, PCIE_PARF_LTSSM, 0, 1u << 8));
	for (r->link_tries = 0;; ++r->link_tries) {
		io->delay_us(io->cookie, PCIE_LINK_UP_POLL_US);
		WTRY(rd(io, WlanWinDbi, LEMONADEP_PCIE0_ELBI_OFF + PCIE_ELBI_SYS_STTS, &r->elbi_stts));
		WTRY(rd(io, WlanWinDbi, PCIE_DBI_CAP_LINKCTRLSTATUS, &r->link_status));
		if ((r->elbi_stts & PCIE_XMLH_LINK_UP) && (r->link_status & PCIE_DLL_ACTIVE)) {
			break;
		}
		if (r->link_tries >= PCIE_LINK_UP_TRIES) {
			return LemonadepWlanLinkTimeout;
		}
	}

	/* The endpoint's config space answers (not all-ones) within 1 s. */
	WPHASE(LemonadepWlanPhaseLink, LemonadepWlanPhaseConfig);
	for (unsigned us = 0;; us += 1000) {
		WTRY(rd(io, WlanWinConf, 0x0, &r->id));
		if (r->id != 0xffffffffu) {
			break;
		}
		if (us >= PCIE_EP_UP_TIMEOUT_US) {
			return LemonadepWlanNoDevice;
		}
		io->delay_us(io->cookie, 1000);
	}
	WTRY(rd(io, WlanWinConf, 0x8, &r->class_rev));
	if (r->id != (LEMONADEP_WLAN_PCI_DEVICE << 16 | LEMONADEP_WLAN_PCI_VENDOR)) {
		return LemonadepWlanWrongDevice;
	}
	WPHASE(LemonadepWlanPhaseConfig, LemonadepWlanPhaseDone);
	return LemonadepWlanOk;
}
