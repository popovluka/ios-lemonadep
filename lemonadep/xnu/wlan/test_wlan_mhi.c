/*
 * Host test (ASan/UBSan) for lemonadep_wlan_mhi.c against a mock QCA6490 that
 * only cooperates when the sequence is right: BAR0 (2 MiB, 64-bit) decodes
 * only once assigned, covered by the root port's memory window and with
 * memory enabled on both ends; the BHI download completes only with bus
 * master on, the image address/size of the chosen firmware and a non-zero
 * session id; READY follows a successful download. The apps SMMU's stream
 * table is set per scenario.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lemonadep_wlan_pcie.c"
#include "lemonadep_wlan_mhi.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define WIN_BYTES LEMONADEP_PCIE0_MEM_MAP
#define BAR_SIZE  0x200000u
#define BHIOFF    0x24000u   /* any offset inside BAR0 */
static uint32_t *win[WlanWinCount];
static uint32_t bar0_mask_lo, bar0, bar1; /* sizing state */
static unsigned bhi_polls, ready_polls, reads_after_db;
static uint32_t chip_ee, chip_major;
static bool chip_bhi_fails;
static uint64_t want_pa;
static uint32_t want_size;

static uint32_t *reg(unsigned w, uint32_t off) { return &win[w][off / 4]; }

static bool
bar_decodes(void)
{
	uint32_t rc = *reg(WlanWinDbi, PCI_CMD_STATUS), ep = *reg(WlanWinConf, PCI_CMD_STATUS);
	uint32_t mw = *reg(WlanWinDbi, PCI_BRIDGE_MEM_WINDOW);
	uint32_t base = (mw & 0xfff0u) << 16, limit = (mw & 0xfff00000u) | 0xfffffu;
	return (rc & PCI_CMD_MEMORY) && (ep & PCI_CMD_MEMORY) && bar0 >= LEMONADEP_PCIE0_MEM_PA &&
	       bar0 >= base && bar0 + BAR_SIZE - 1u <= limit && !(bar0 & (BAR_SIZE - 1u)) && bar1 == 0;
}

static int
mread(void *c, unsigned w, uint32_t off, uint32_t *out)
{
	(void)c;
	if (w >= WlanWinCount || off + 4 > WIN_BYTES) return 1;
	uint32_t v = *reg(w, off);
	if (w == WlanWinConf && off == PCI_BAR0) {
		v = bar0_mask_lo ? bar0_mask_lo : bar0 | PCI_BAR_MEM64;
	} else if (w == WlanWinConf && off == PCI_BAR1) {
		v = bar0_mask_lo ? 0xffffffffu : bar1;
	} else if (w == WlanWinMem) {
		if (!bar_decodes() || off < bar0 - LEMONADEP_PCIE0_MEM_PA ||
		    off >= bar0 - LEMONADEP_PCIE0_MEM_PA + BAR_SIZE) {
			*out = 0xffffffffu;
			return 0;
		}
		uint32_t b = off - (bar0 - LEMONADEP_PCIE0_MEM_PA);
		switch (b) {
		case MHI_VER: v = 0x01000000u; break;
		case MHI_REGLEN: v = 0x100u; break;
		case MHI_BHIOFF: v = BHIOFF; break;
		case MHI_BHIEOFF: v = BHIOFF + 0x100u; break;
		case MHI_SOC_HW_VERSION: v = 0x40170000u | (chip_major << 8) | 1u; break;
		case MHI_CTRL: v = ready_polls >= 5 ? 0 : MHI_CTRL_RESET; break;
		case MHI_STATUS:
			if (bhi_polls >= 3 && !chip_bhi_fails) ++ready_polls;
			v = ready_polls >= 5 ? MHI_STATUS_READY : 0;
			break;
		case BHIOFF + BHI_EXECENV: v = ready_polls >= 5 ? MHI_EE_SBL : chip_ee; break;
		case BHIOFF + BHI_STATUS:
			if (reads_after_db) {
				++bhi_polls;
			}
			v = bhi_polls >= 3 ? (chip_bhi_fails ? BHI_STATUS_ERROR : BHI_STATUS_SUCCESS) << 30 : 0;
			break;
		case BHIOFF + BHI_ERRCODE: v = 0xdead; break;
		default: v = *reg(w, off);
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
	if (w == WlanWinConf && off == PCI_BAR0) {
		bar0_mask_lo = v == 0xffffffffu ? (~(BAR_SIZE - 1u) | PCI_BAR_MEM64) : 0;
		if (v != 0xffffffffu) bar0 = v & ~0xfu;
		return 0;
	}
	if (w == WlanWinConf && off == PCI_BAR1) {
		if (v != 0xffffffffu) bar1 = v;
		return 0;
	}
	if (w == WlanWinMem) {
		if (!bar_decodes()) return 0; /* dropped */
		uint32_t b = off - (bar0 - LEMONADEP_PCIE0_MEM_PA);
		*reg(w, off) = v;
		if (b == BHIOFF + BHI_IMGTXDB && v) {
			const uint32_t base = bar0 - LEMONADEP_PCIE0_MEM_PA + BHIOFF;
			uint64_t pa = (uint64_t)*reg(w, base + BHI_IMGADDR_HIGH) << 32 | *reg(w, base + BHI_IMGADDR_LOW);
			bool master = (*reg(WlanWinDbi, PCI_CMD_STATUS) & PCI_CMD_MASTER) &&
			    (*reg(WlanWinConf, PCI_CMD_STATUS) & PCI_CMD_MASTER);
			if (master && pa == want_pa && *reg(w, base + BHI_IMGSIZE) == want_size &&
			    *reg(w, base + BHI_STATUS) == 0) {
				reads_after_db = 1;
			}
		}
		return 0;
	}
	*reg(w, off) = v;
	return 0;
}
static void mdelay(void *c, unsigned us) { (void)c; (void)us; }

static void
reset(void)
{
	for (unsigned i = 0; i < WlanWinCount; ++i) memset(win[i], 0, WIN_BYTES);
	bar0_mask_lo = bar0 = bar1 = 0;
	bhi_polls = ready_polls = reads_after_db = 0;
	chip_ee = MHI_EE_PBL;
	chip_major = 2;
	chip_bhi_fails = false;
	/* apps SMMU: 4 SMRs, unmatched streams bypass (USFCFG 0), SMR 1 = UFS-like */
	*reg(WlanWinSmmu, SMMU_ID0) = 4;
	*reg(WlanWinSmmu, SMMU_SMR(1)) = SMMU_SMR_VALID | 0x0080u;
	*reg(WlanWinSmmu, SMMU_S2CR(1)) = 0x00000003u; /* translate, CB 3 */
}

int
main(void)
{
	for (unsigned i = 0; i < WlanWinCount; ++i) win[i] = calloc(1, WIN_BYTES);
	const lemonadep_wlan_io io = {.cookie = NULL, .read32 = mread, .write32 = mwrite, .delay_us = mdelay};
	const lemonadep_mhi_params p = {.fw_v1 = {0x881000000ull, 5178848u}, .fw_v2 = {0x882000000ull, 5169152u}};
	lemonadep_mhi_result r;

	/* ---- full run, major 2: amss20.bin's first 512 KiB, READY, SBL ---- */
	reset();
	want_pa = p.fw_v2.pa;
	want_size = LEMONADEP_MHI_SBL_BYTES;
	lemonadep_mhi_status st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	printf("full: status=%d phase=%d bar=%#x/%#x ver=%#x soc=%#x ee=%u smr=%d bypass=%d fw=%s bhi=%#x (%u ms) "
	    "ready ee=%u (%u ms)\n", st, r.phase, r.bar_pa, r.bar_size, r.ver, r.soc_hw_version, r.ee, r.smr_index,
	    r.smmu_bypass, r.fw_name, r.bhi_status, r.bhi_ms, r.ready_ee, r.ready_ms);
	CHECK(st == LemonadepMhiOk && r.phase == LemonadepMhiPhaseDone);
	CHECK(r.bar_size == BAR_SIZE && r.bar_pa == 0x60400000u);
	CHECK(*reg(WlanWinDbi, PCI_BRIDGE_BUSES) == 0x00010100u);
	CHECK((r.rc_cmd & 6u) == 6u && (r.ep_cmd & 6u) == 6u);
	CHECK(r.ver == 0x01000000u && r.bhioff == BHIOFF && r.ee == MHI_EE_PBL);
	CHECK(r.smr_index == -1 && r.smmu_bypass);
	CHECK(!strcmp(r.fw_name, "amss20.bin") && r.fw_pa == p.fw_v2.pa && r.sbl_bytes == 0x80000u);
	CHECK(r.bhi_status >> 30 == BHI_STATUS_SUCCESS && r.ready_ee == MHI_EE_SBL);

	/* ---- major 1 picks amss.bin ---- */
	reset();
	chip_major = 1;
	want_pa = p.fw_v1.pa;
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiOk && !strcmp(r.fw_name, "amss.bin"));

	/* ---- stop after the register read: nothing written to BHI ---- */
	reset();
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseRegs);
	CHECK(st == LemonadepMhiOk && r.phase == LemonadepMhiPhaseRegs && r.ver == 0x01000000u);
	CHECK(*reg(WlanWinMem, r.bar_pa - LEMONADEP_PCIE0_MEM_PA + BHIOFF + BHI_IMGTXDB) == 0);

	/* ---- an SMR for 0x1c00/0x1c01 (mask 1) that translates: no BHI ---- */
	reset();
	*reg(WlanWinSmmu, SMMU_SMR(2)) = SMMU_SMR_VALID | 1u << 16 | 0x1c00u;
	*reg(WlanWinSmmu, SMMU_S2CR(2)) = 0x00000005u;
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiSmmuNotBypass && r.smr_index == 2 && !r.smmu_bypass && r.phase == LemonadepMhiPhaseBhi);
	CHECK(*reg(WlanWinMem, r.bar_pa - LEMONADEP_PCIE0_MEM_PA + BHIOFF + BHI_IMGTXDB) == 0);

	/* ---- same SMR but S2CR bypass: allowed ---- */
	reset();
	want_pa = p.fw_v2.pa;
	*reg(WlanWinSmmu, SMMU_SMR(2)) = SMMU_SMR_VALID | 1u << 16 | 0x1c00u;
	*reg(WlanWinSmmu, SMMU_S2CR(2)) = 1u << 16;
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiOk && r.smr_index == 2 && r.smmu_bypass);

	/* ---- unmatched streams fault (USFCFG): no BHI ---- */
	reset();
	*reg(WlanWinSmmu, SMMU_sCR0) = SMMU_sCR0_USFCFG;
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiSmmuNotBypass && r.smr_index == -1);

	/* ---- no firmware blob for this version ---- */
	reset();
	const lemonadep_mhi_params p1 = {.fw_v1 = p.fw_v1};
	st = lemonadep_wlan_mhi_up(&io, &p1, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiNoFirmware && !strcmp(r.fw_name, "amss20.bin"));

	/* ---- the chip is not in PBL ---- */
	reset();
	chip_ee = MHI_EE_EDL;
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiBadEe && r.ee == MHI_EE_EDL);

	/* ---- BHI reports an error: error registers captured, no READY wait ---- */
	reset();
	want_pa = p.fw_v2.pa;
	chip_bhi_fails = true;
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiBhiError && r.bhi_errcode == 0xdead && r.phase == LemonadepMhiPhaseBhi);

	/* ---- the chip never takes the image (wrong address): timeout after 10 s of polls ---- */
	reset();
	want_pa = 0;
	st = lemonadep_wlan_mhi_up(&io, &p, &r, LemonadepMhiPhaseNone);
	CHECK(st == LemonadepMhiBhiTimeout && r.bhi_ms == LEMONADEP_MHI_TIMEOUT_MS);

	for (unsigned i = 0; i < WlanWinCount; ++i) free(win[i]);
	printf("test_wlan_mhi %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
