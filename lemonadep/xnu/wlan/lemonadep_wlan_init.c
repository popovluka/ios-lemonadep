/*
 * lemonadep Wi-Fi, stage 1 kernel glue: maps the windows, routes rail votes
 * to sweet_rpmh.c, runs lemonadep_wlan_pcie_up() (host-tested in
 * lemonadep/xnu/wlan/test_wlan_pcie.c) and logs the result as "SWEET WLAN:"
 * lines for the persistent log (read back with TWRP + tools/sweet-log-pull.sh).
 *
 * Off by default. sweetwlan=N stops cleanly after step N (no rebuild between
 * steps, same scheme as sweetgpu):
 *   1 rails  2 WLAN_EN  3 PCIe PHY rails  4 GDSC  5 clocks  6 resets
 *   7 PARF   8 PHY ready  9 PERST released  10 link up  11 read config ID
 * stage 2 (lemonadep_wlan_mhi.c), after a full stage 1:
 *   12 BAR0 assigned  13 MHI/BHI registers + apps SMMU probe (read only)
 *   14 SBL over BHI   15+ MHI READY
 * Steps 14+ need the firmware as payload blobs (mkpayload --extra-blob
 * wlanfw1:amss.bin --extra-blob wlanfw2:amss20.bin, lemonadep/fw-wlan/,
 * never committed); the chip DMAs straight from where the blob sits.
 * See lemonadep/HANDOFF.md "Wi-Fi, stage 1" and "Wi-Fi, stage 2".
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_rpmh_vreg_vote(const char *res, unsigned mv, int hpm);

#include "lemonadep/wlan/lemonadep_wlan_pcie.c"
#include "lemonadep/wlan/lemonadep_wlan_mhi.c"

static vm_offset_t wlan_win[WlanWinCount];

static int
wlan_read(void *c, unsigned w, uint32_t off, uint32_t *out)
{
	(void)c;
	*out = *(volatile uint32_t *)(wlan_win[w] + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return 0;
}
static int
wlan_write(void *c, unsigned w, uint32_t off, uint32_t v)
{
	(void)c;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(wlan_win[w] + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}
static int
wlan_vote(void *c, const char *res, unsigned mv, int hpm)
{
	(void)c;
	int e = sweet_rpmh_vreg_vote(res, mv, hpm);
	printf("SWEET WLAN: rpmh %s %u mV%s -> %d\n", res, mv, hpm ? " hpm" : "", e);
	return e;
}
static void
wlan_delay(void *c, unsigned us)
{
	(void)c;
	delay((int)us);
}

void lemonadep_wlan_init(void);

void
lemonadep_wlan_init(void)
{
	uint32_t stage = 0;
	(void)PE_parse_boot_argn("sweetwlan", &stage, sizeof(stage));
	if (!stage) {
		printf("SWEET WLAN: disabled (sweetwlan=N to enable)\n");
		return;
	}
	const lemonadep_wlan_phase stop_after =
	    stage < LemonadepWlanPhaseConfig ? (lemonadep_wlan_phase)stage : LemonadepWlanPhaseNone;

	static const struct { uint32_t pa, bytes; } maps[WlanWinCount] = {
		[WlanWinTlmm] = {LEMONADEP_TLMM_PA, LEMONADEP_TLMM_BYTES},
		[WlanWinGcc] = {LEMONADEP_GCC_PA, 0x91000u},     /* highest offset used: 0x9000c */
		[WlanWinParf] = {LEMONADEP_PCIE0_PARF_PA, 0x3000u},
		[WlanWinPhy] = {LEMONADEP_PCIE0_PHY_PA, 0x2000u},
		[WlanWinDbi] = {LEMONADEP_PCIE0_DBI_PA, 0x1000u}, /* dm_core + elbi */
		[WlanWinIatu] = {LEMONADEP_PCIE0_IATU_PA, 0x1000u},
		[WlanWinConf] = {LEMONADEP_PCIE0_CONF_PA, 0x1000u},
		[WlanWinMem] = {LEMONADEP_PCIE0_MEM_PA, LEMONADEP_PCIE0_MEM_MAP},
		[WlanWinSmmu] = {LEMONADEP_APPS_SMMU_PA, 0x1000u},   /* GR0: SMR/S2CR end at 0xe00 */
	};
	for (unsigned i = 0; i < WlanWinCount; ++i) {
		if (!wlan_win[i]) {
			wlan_win[i] = ml_io_map(maps[i].pa, maps[i].bytes);
		}
		if (!wlan_win[i]) {
			printf("SWEET WLAN: ml_io_map failed for window %u (pa %#x)\n", i, maps[i].pa);
			return;
		}
	}

	printf("SWEET WLAN: stage=%u stop_after=%d (QCA6490 on PCIe0)\n", stage, stop_after);
	const lemonadep_wlan_io io = {.cookie = NULL, .read32 = wlan_read, .write32 = wlan_write,
	    .rpmh_vote = wlan_vote, .delay_us = wlan_delay};
	lemonadep_wlan_result r;
	lemonadep_wlan_status st = lemonadep_wlan_pcie_up(&io, &r, stop_after);
	printf("SWEET WLAN: status=%d phase=%d%s%s\n", st, r.phase,
	    r.failed_what ? " failed=" : "", r.failed_what ? r.failed_what : "");
	printf("SWEET WLAN: bt_en_io=%#x gdsc=%#x phy_status=%#x (tries %u) elbi_stts=%#x ltssm=%#x "
	    "linkstatus=%#x (tries %u)\n", r.bt_en, r.gdsc, r.phy_status, r.phy_tries, r.elbi_stts,
	    (r.elbi_stts >> 12) & 0x3fu, r.link_status, r.link_tries);
	if (r.phase >= LemonadepWlanPhaseConfig) {
		printf("SWEET WLAN: config id=%#x (vendor %#x device %#x) class/rev=%#x\n", r.id,
		    r.id & 0xffffu, r.id >> 16, r.class_rev);
	}
	if (st != LemonadepWlanOk || r.phase != LemonadepWlanPhaseDone || stage < LemonadepMhiPhaseBar) {
		return;
	}

	/* Stage 2. Firmware blobs: wlanfw1 = amss.bin, wlanfw2 = amss20.bin. */
	lemonadep_mhi_params p = {{0, 0}, {0, 0}};
	(void)PE_parse_boot_argn("wlanfw1_pa", &p.fw_v1.pa, sizeof(p.fw_v1.pa));
	(void)PE_parse_boot_argn("wlanfw1_bytes", &p.fw_v1.bytes, sizeof(p.fw_v1.bytes));
	(void)PE_parse_boot_argn("wlanfw2_pa", &p.fw_v2.pa, sizeof(p.fw_v2.pa));
	(void)PE_parse_boot_argn("wlanfw2_bytes", &p.fw_v2.bytes, sizeof(p.fw_v2.bytes));
	printf("SWEET WLAN: firmware amss.bin pa=%#llx bytes=%#x, amss20.bin pa=%#llx bytes=%#x\n",
	    p.fw_v1.pa, p.fw_v1.bytes, p.fw_v2.pa, p.fw_v2.bytes);
	const lemonadep_mhi_phase mstop =
	    stage < LemonadepMhiPhaseDone ? (lemonadep_mhi_phase)stage : LemonadepMhiPhaseNone;
	lemonadep_mhi_result m;
	lemonadep_mhi_status ms = lemonadep_wlan_mhi_up(&io, &p, &m, mstop);
	printf("SWEET WLAN: mhi status=%d phase=%d\n", ms, m.phase);
	printf("SWEET WLAN: bar0 pa=%#x size=%#x rc_cmd=%#x ep_cmd=%#x\n", m.bar_pa, m.bar_size, m.rc_cmd, m.ep_cmd);
	if (m.phase >= LemonadepMhiPhaseRegs) {
		printf("SWEET WLAN: mhi reglen=%#x ver=%#x cfg=%#x bhioff=%#x bhieoff=%#x ctrl=%#x status=%#x "
		    "soc_hw_version=%#x (major %u) ee=%u bhi_status=%#x\n", m.reglen, m.ver, m.cfg, m.bhioff,
		    m.bhieoff, m.ctrl, m.status, m.soc_hw_version, (m.soc_hw_version >> 8) & 0xfu, m.ee,
		    m.bhi_status0);
		printf("SWEET WLAN: smmu scr0=%#x id0=%#x sgfsr=%#x sid %#x: smr[%d]=%#x s2cr=%#x -> %s\n",
		    m.scr0, m.id0, m.sgfsr, LEMONADEP_WLAN_SID, m.smr_index, m.smr, m.s2cr,
		    m.smmu_bypass ? "bypass" : "NOT bypass");
	}
	if (m.phase >= LemonadepMhiPhaseBhi && m.fw_name) {
		printf("SWEET WLAN: bhi %s pa=%#llx sbl=%#x status=%#x after %u ms errcode=%#x dbg=%#x/%#x/%#x\n",
		    m.fw_name, m.fw_pa, m.sbl_bytes, m.bhi_status, m.bhi_ms, m.bhi_errcode,
		    m.bhi_errdbg[0], m.bhi_errdbg[1], m.bhi_errdbg[2]);
	}
	if (m.phase >= LemonadepMhiPhaseReady) {
		printf("SWEET WLAN: ready ctrl=%#x status=%#x ee=%u after %u ms\n", m.ready_ctrl, m.ready_status,
		    m.ready_ee, m.ready_ms);
	}
}
