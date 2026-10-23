/*
 * Host test (ASan/UBSan) for lemonadep_wlan_pcie.c against a mock that only
 * reports success once the real sequence actually did the right thing: the
 * GDSC powers up only when its collapse vote is cleared, the PHY reports ready
 * only after the whole 75-write board sequence landed in order and the pipe
 * clock was voted, the link trains only with WLAN_EN high, PERST released and
 * LTSSM enabled, and config space answers 17cb:1103 only through an iATU
 * region programmed as CFG0 for bus 1.
 */
#include <stdio.h>
#include <string.h>

#include "lemonadep_wlan_pcie.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define WIN_BYTES 0xd0000u
static uint32_t win[WlanWinCount][WIN_BYTES / 4];
static unsigned phy_written; /* how many sequence entries arrived in order */
static unsigned long long now_us;
static char votes[16][16];
static unsigned vote_mv[16], vote_hpm[16], nvotes;
static int fail_vote = -1;
static bool chip_dead;

static uint32_t *reg(unsigned w, uint32_t off) { return &win[w][off / 4]; }

static int
mread(void *c, unsigned w, uint32_t off, uint32_t *out)
{
	(void)c;
	if (w >= WlanWinCount || off + 4 > WIN_BYTES) return 1;
	uint32_t v = *reg(w, off);
	if (w == WlanWinGcc && off == GCC_PCIE0_GDSC) {
		v = (*reg(WlanWinGcc, GCC_PCIE0_GDSC_VOTE) & 1u) ? 0 : (1u << 31);
	} else if (w == WlanWinGcc) {
		/* A polled branch reads CLK_OFF until its vote bit is set. */
		for (unsigned i = 0; i < sizeof(LemonadepPcie0Branches) / sizeof(LemonadepPcie0Branches[0]); ++i) {
			const struct LemonadepGccBranch *b = &LemonadepPcie0Branches[i];
			if (off == b->halt) {
				v = (*reg(WlanWinGcc, b->vote) & (1u << b->bit)) ? 0 : GCC_CLK_OFF;
			}
		}
		if (off == GCC_CLKREF_PCIE0) {
			v = (v & 1u) ? v & ~GCC_CLK_OFF : v | GCC_CLK_OFF;
		}
	} else if (w == WlanWinPhy && off == LEMONADEP_PCIE0_PHY_STATUS_OFF) {
		bool ready = phy_written == sizeof(LemonadepPcie0PhySequence) / sizeof(LemonadepPcie0PhySequence[0]) &&
		    (*reg(WlanWinGcc, GCC_VOTE_B) & (1u << GCC_PCIE0_PIPE_VOTE_BIT));
		v = ready ? 0 : (1u << LEMONADEP_PCIE0_PHY_STATUS_BIT);
	} else if (w == WlanWinDbi || w == WlanWinConf) {
		bool link = !chip_dead && (*reg(WlanWinTlmm, TLMM_IO(LEMONADEP_WLAN_EN_GPIO)) & 2u) &&
		    (*reg(WlanWinTlmm, TLMM_IO(LEMONADEP_PCIE0_PERST_GPIO)) & 2u) &&
		    (*reg(WlanWinParf, PCIE_PARF_LTSSM) & (1u << 8));
		if (w == WlanWinDbi && off == LEMONADEP_PCIE0_ELBI_OFF + PCIE_ELBI_SYS_STTS) {
			v = link ? (PCIE_XMLH_LINK_UP | (0x11u << 12)) : 0;
		} else if (w == WlanWinDbi && off == PCIE_DBI_CAP_LINKCTRLSTATUS) {
			v = link ? (PCIE_DLL_ACTIVE | (1u << 16)) : 0;
		} else if (w == WlanWinConf) {
			bool cfg0 = (*reg(WlanWinIatu, PCIE_IATU_CTRL2) & (1u << 31)) &&
			    *reg(WlanWinIatu, PCIE_IATU_CTRL1) == PCIE_IATU_TYPE_CFG0 &&
			    *reg(WlanWinIatu, PCIE_IATU_LBAR) == LEMONADEP_PCIE0_CONF_PA &&
			    *reg(WlanWinIatu, PCIE_IATU_LTAR) == (1u << 24);
			v = !(link && cfg0) ? 0xffffffffu : off == 0 ? 0x110317cbu : off == 8 ? 0x02800001u : 0;
		}
	}
	*out = v;
	return 0;
}
static int
mwrite(void *c, unsigned w, uint32_t off, uint32_t v)
{
	(void)c;
	if (w >= WlanWinCount || off + 4 > WIN_BYTES) return 1;
	if (w == WlanWinPhy) {
		const struct LemonadepPcie0PhyWrite *p = &LemonadepPcie0PhySequence[phy_written];
		if (phy_written < sizeof(LemonadepPcie0PhySequence) / sizeof(LemonadepPcie0PhySequence[0]) &&
		    p->offset == off && p->value == v) {
			++phy_written;
		}
	}
	if (w == WlanWinGcc && (v & 1u) && (off == GCC_PCIE0_AUX_RCG || off == GCC_PCIE0_RCHNG_RCG)) {
		v &= ~1u; /* CMD.UPDATE completes at once */
	}
	*reg(w, off) = v;
	return 0;
}
static int
mvote(void *c, const char *res, unsigned mv, int hpm)
{
	(void)c;
	if ((int)nvotes == fail_vote) return 1;
	snprintf(votes[nvotes], sizeof(votes[0]), "%s", res);
	vote_mv[nvotes] = mv;
	vote_hpm[nvotes++] = (unsigned)hpm;
	return 0;
}
static void mdelay(void *c, unsigned us) { (void)c; now_us += us; }

static void
reset(void)
{
	memset(win, 0, sizeof(win));
	phy_written = 0;
	now_us = 0;
	nvotes = 0;
	fail_vote = -1;
	*reg(WlanWinGcc, GCC_PCIE0_GDSC_VOTE) = 1u; /* collapsed at boot */
	*reg(WlanWinTlmm, TLMM_CFG(LEMONADEP_WLAN_EN_GPIO)) = TLMM_EGPIO_PRESENT;
}

int
main(void)
{
	const lemonadep_wlan_io io = {.cookie = NULL, .read32 = mread, .write32 = mwrite,
	    .rpmh_vote = mvote, .delay_us = mdelay};
	lemonadep_wlan_result r;

	/* ---- full run ---- */
	reset();
	lemonadep_wlan_status st = lemonadep_wlan_pcie_up(&io, &r, LemonadepWlanPhaseNone);
	printf("full: status=%d phase=%d id=%#x class=%#x phy_tries=%u link_tries=%u t=%llu us\n",
	    st, r.phase, r.id, r.class_rev, r.phy_tries, r.link_tries, now_us);
	CHECK(st == LemonadepWlanOk && r.phase == LemonadepWlanPhaseDone && r.id == 0x110317cbu);
	/* rails: cnss order, DT values, no HPM; then PHY rails with HPM */
	const char *want[8] = {"smpb10", "ldoe7", "smpe2", "smpb11", "smpc1", "smpb12", "ldob5", "ldob6"};
	const unsigned want_mv[8] = {1800, 2800, 950, 950, 1880, 1350, 880, 1200};
	CHECK(nvotes == 8);
	for (unsigned i = 0; i < 8 && i < nvotes; ++i) {
		CHECK(!strcmp(votes[i], want[i]) && vote_mv[i] == want_mv[i] && vote_hpm[i] == (i >= 6));
	}
	/* WLAN_EN: gpio function, 16 mA, pull-up, output enabled + high, eGPIO claimed */
	uint32_t en = *reg(WlanWinTlmm, TLMM_CFG(LEMONADEP_WLAN_EN_GPIO));
	CHECK((en & 0x3ffu) == (TLMM_PULL_UP | (7u << 6) | TLMM_OE) && (en & TLMM_EGPIO_ENABLE));
	CHECK(*reg(WlanWinTlmm, TLMM_IO(LEMONADEP_WLAN_EN_GPIO)) & 2u);
	/* PERST released, 2 mA pull-down output */
	CHECK((*reg(WlanWinTlmm, TLMM_CFG(LEMONADEP_PCIE0_PERST_GPIO)) & 0x3ffu) == (TLMM_PULL_DOWN | TLMM_OE));
	CHECK(*reg(WlanWinTlmm, TLMM_IO(LEMONADEP_PCIE0_PERST_GPIO)) & 2u);
	/* clocks: aux 19.2 MHz (XO/1), rchng 100 MHz (GPLL0/6), pipe mux on PHY clock */
	CHECK((*reg(WlanWinGcc, GCC_PCIE0_AUX_RCG + 4) & 0x71fu) == 1u);
	CHECK((*reg(WlanWinGcc, GCC_PCIE0_RCHNG_RCG + 4) & 0x71fu) == ((1u << 8) | 11u));
	CHECK((*reg(WlanWinGcc, GCC_PCIE0_PIPE_MUX) & 3u) == 0);
	CHECK(phy_written == 75);
	/* PARF, iATU, Gen1 */
	CHECK(*reg(WlanWinParf, PCIE_PARF_DEVICE_TYPE) == 4 && *reg(WlanWinParf, PCIE_PARF_SYS_CTRL) == 0x365e);
	CHECK(*reg(WlanWinParf, PCIE_PARF_SLV_ADDR_SPACE_SIZE) == 0x4000000u);
	CHECK(*reg(WlanWinIatu, PCIE_IATU_LAR) == LEMONADEP_PCIE0_CONF_PA + 0xfff);
	CHECK((*reg(WlanWinDbi, PCIE_DBI_CAP_LINKCTL2) & 0xf) == 1);
	CHECK(r.bt_en == 0 && now_us >= LEMONADEP_WLAN_BT_OFF_DELAY_US + LEMONADEP_WLAN_EN_DELAY_US);

	/* ---- stop after the PHY: PERST must still be asserted, no LTSSM ---- */
	reset();
	st = lemonadep_wlan_pcie_up(&io, &r, LemonadepWlanPhasePhy);
	CHECK(st == LemonadepWlanOk && r.phase == LemonadepWlanPhasePhy);
	CHECK(!(*reg(WlanWinTlmm, TLMM_IO(LEMONADEP_PCIE0_PERST_GPIO)) & 2u));
	CHECK(!(*reg(WlanWinParf, PCIE_PARF_LTSSM) & (1u << 8)));

	/* ---- a failed rail stops before WLAN_EN ---- */
	reset();
	fail_vote = 3;
	st = lemonadep_wlan_pcie_up(&io, &r, LemonadepWlanPhaseNone);
	CHECK(st == LemonadepWlanRailFailed && r.phase == LemonadepWlanPhaseRails && r.failed_what &&
	    !strncmp(r.failed_what, "vdd-wlan-dig", 12));
	CHECK(!(*reg(WlanWinTlmm, TLMM_IO(LEMONADEP_WLAN_EN_GPIO)) & 2u));

	/* ---- a chip that never trains: clean link timeout after the vendor's 20 polls ---- */
	reset();
	chip_dead = true;
	st = lemonadep_wlan_pcie_up(&io, &r, LemonadepWlanPhaseNone);
	chip_dead = false;
	CHECK(st == LemonadepWlanLinkTimeout && r.phase == LemonadepWlanPhaseLink && r.link_tries == PCIE_LINK_UP_TRIES);

	printf("test_wlan_pcie %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
