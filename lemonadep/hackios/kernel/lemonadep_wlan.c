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
 *   14 a bypass stream-map entry for 0x1c01 (if it has none)
 *   15 SBL over BHI   16+ MHI READY
 * Steps 15+ need the firmware as payload blobs (mkpayload --extra-blob
 * wlanfw1:amss.bin --extra-blob wlanfw2:amss20.bin, lemonadep/fw-wlan/,
 * never committed); the chip DMAs straight from where the blob sits.
 * See lemonadep/HANDOFF.md "Wi-Fi, stage 1" and "Wi-Fi, stage 2".
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <kern/locks.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>

/*
 * Not ml_io_map(): that carves from io_submap, 8 MiB for the whole kernel
 * (osfmk/arm/io_map.c IO_MAP_SIZE) with KMA_NOFAIL, and stage 2's 8 MiB PCIe
 * memory window can never fit -- the kernel died there, before its first log
 * flush, on every stage-2 boot (2026-10-24, logs/lemonadep-wlan3..7). The
 * unmappable variant takes kernel_map instead; flags VM_WIMG_IO = 0x7
 * (osfmk/arm/memory_types.h).
 */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
#define WLAN_VM_WIMG_IO 0x7u
extern int sweet_rpmh_vreg_vote(const char *res, unsigned mv, int hpm);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);   /* as sweet_ufs.c */
extern uint64_t kvtophys(vm_offset_t va);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern lck_mtx_t redwood_smmu_lock;     /* redwood_wifi.c: one finder of free SMMU contexts at a time */

#include "lemonadep/wlan/lemonadep_wlan_pcie.c"
#include "lemonadep/wlan/lemonadep_wlan_mhi.c"

static vm_offset_t wlan_win[WlanWinCount];

/*
 * HackiOS: the run's log does not come back over USB on this phone yet, so the results also go
 * into the notes drawn on the panel when the run ends (redwood_dpu.c, 8 lines of 44).
 */
extern void redwood_dpu_note(unsigned line, const char *text);
static void
wlan_note(unsigned line, const char *fmt, ...)
{
	char text[45];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	redwood_dpu_note(line, text);
}

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
/*
 * The first stage-2 boot left no log at all: the console ring reaches the
 * disk every ufslogsecs (10 s default), and whatever stopped the SoC came
 * first. So each risky access is announced and then waited on long enough
 * for the line to be flushed (boot with ufslogsecs=1).
 */
#define WLAN_CHECKPOINT_US 2500000u
static void
wlan_checkpoint(void *c, const char *what)
{
	(void)c;
	printf("SWEET WLAN: next: %s\n", what);
	delay((int)WLAN_CHECKPOINT_US);
}

static void
wlan_trace(void *c, const char *what, uint32_t a, uint32_t b, uint32_t cc, unsigned ms)
{
	(void)c;
	if (!strcmp(what, "bhi")) {
		printf("SWEET WLAN: t=%u ms bhi_status=%#x ee=%u mhi_status=%#x\n", ms, a, b, cc);
	} else {
		printf("SWEET WLAN: t=%u ms %s ctrl=%#x ee=%u mhi_status=%#x\n", ms, what, a, b, cc);
	}
}

/*
 * The chip's view of memory (HackiOS, sweetwlan >= 15): its stream through a stage-1 context of
 * the apps SMMU with page tables of ours, as furangelz's redwood_wifi.c (radio_space_*) gives the
 * WCN6750's -- the order of writes and the values are his, which this phone's hypervisor granted
 * on 13 October. A bypass entry is not tried: here the hypervisor turns S2CR bypass into fault
 * (2026-10-24, logs/lemonadep-wlan10.txt), and the context-bank "bypass quirk" crashed the SoC on
 * the chip's first DMA. Stream 0x1c00 mask 1 covers 0x1c01 (dt iommu-map: RID 0x100 -> 0x1c01).
 * Addresses for the chip from 0xa0000000 (dt qcom,iommu-dma-addr-pool), handed out upwards.
 */
#define WSP_STREAM              0x1c00u
#define WSP_STREAM_MASK         0x0001u
#define WSP_ADDRESSES           0xa0000000ull
#define WSP_ADDRESSES_SIZE      0x10000000ull
#define WSP_TABLES              48u             /* of 4K: root, level 2, and 2 MB of addresses each other */
#define WSP_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define WSP_PTE_ADDRESS         0x0000fffffffff000ull
#define WSP_SCR0                0x000u
#define WSP_IDR0                0x020u
#define WSP_IDR1                0x024u
#define WSP_SGFSR               0x048u
#define WSP_SGFSYNR1            0x054u
#define WSP_SMR(n)              (0x800u + 4u * (n))
#define WSP_S2CR(n)             (0xc00u + 4u * (n))
#define WSP_CBAR(n)             (4u * (n))
#define WSP_CBAR_STAGE_1        (1u << 16 | 3u << 8 | 0xfu << 12)
#define WSP_CBA2R(n)            (0x800u + 4u * (n))
#define WSP_CB_SCTLR            0x000u
#define WSP_CB_TCR2             0x010u
#define WSP_CB_TTBR0            0x020u
#define WSP_CB_TTBR1            0x028u
#define WSP_CB_TCR              0x030u
#define WSP_CB_MAIR0            0x038u
#define WSP_CB_MAIR1            0x03cu
#define WSP_CB_FSR              0x058u
#define WSP_CB_FAR              0x060u
#define WSP_CB_FSYNR0           0x068u
#define WSP_CB_TLBIASID         0x610u
#define WSP_CB_TLBSYNC          0x7f0u
#define WSP_CB_TLBSTATUS        0x7f4u
#define WSP_SCTLR_ON            (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define WSP_TCR_39_BITS         (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define WSP_TCR2                (1u | 1u << 4 | 7u << 15)
#define WSP_MAIR0               0xf404ff44u
#define WSP_FSR_ALL             0xc00001feu

static struct {
	bool found, on;
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables;
	uint32_t table_count;
	uint64_t next;
} wsp;

static uint32_t
wsp_rd(uint32_t off)
{
	uint32_t v = 0;
	(void)wlan_read(NULL, WlanWinSmmu, off, &v);
	return v;
}
static void
wsp_wr(uint32_t off, uint32_t v)
{
	(void)wlan_write(NULL, WlanWinSmmu, off, v);
}
static uint32_t
wsp_cb(void)
{
	return (wsp.pages + wsp.context) * wsp.page;     /* the context's own page, from the SMMU's base */
}

static bool
wsp_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = wsp_rd(WSP_SCR0), idr0 = wsp_rd(WSP_IDR0), idr1 = wsp_rd(WSP_IDR1);
	uint32_t matches = idr0 & 0xffu, contexts = idr1 & 0xffu;

	if (scr0 & 1u << 3) {
		printf("SWEET WLAN: smmu uses 16-bit stream ids (scr0 %#x): not written for\n", scr0);
		return false;
	}
	wsp.page = idr1 >> 31 ? 0x10000u : 0x1000u;
	wsp.pages = 2u << (idr1 >> 28 & 7u);
	if (2u * wsp.pages * wsp.page != SMMU_APPS_BYTES) {
		printf("SWEET WLAN: smmu geometry %u x %#x is not the dt's %#x\n", wsp.pages, wsp.page, SMMU_APPS_BYTES);
		return false;
	}
	wsp.match = ~0u;
	for (uint32_t n = 0; n < matches && WSP_SMR(n) < 0xc00u; n++) {
		uint32_t smr = wsp_rd(WSP_SMR(n)), s2cr = wsp_rd(WSP_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xffu] = true;
			if (((smr & 0x7fffu) & ~(smr >> 16 & 0x7fffu)) == (0x1c01u & ~(smr >> 16 & 0x7fffu))) {
				printf("SWEET WLAN: smmu match register %u already takes stream 0x1c01 (smr %#x s2cr %#x)\n", n, smr, s2cr);
				return false;
			}
		} else if (wsp.match == ~0u) {
			wsp.match = n;
			wsp.old_smr = smr;
			wsp.old_s2cr = s2cr;
		}
	}
	for (wsp.context = 0; wsp.context < contexts && used[wsp.context]; wsp.context++) {
	}
	if (wsp.match == ~0u || wsp.context >= contexts) {
		printf("SWEET WLAN: smmu has no free match register or context\n");
		return false;
	}
	wsp.old_sctlr = wsp_rd(wsp_cb() + WSP_CB_SCTLR);
	printf("SWEET WLAN: smmu free: match %u (smr %#x s2cr %#x), context %u (sctlr %#x), pages %u x %#x\n", wsp.match,
	    wsp.old_smr, wsp.old_s2cr, wsp.context, wsp.old_sctlr, wsp.pages, wsp.page);
	if (wsp.old_sctlr & 1u) {
		printf("SWEET WLAN: smmu context %u is switched on though unused: left alone\n", wsp.context);
		return false;
	}
	wsp.found = true;
	return true;
}

static void
wsp_faults(const char *when)
{
	if (!wsp.on) {
		return;
	}
	uint32_t fsr = wsp_rd(wsp_cb() + WSP_CB_FSR), sg = wsp_rd(WSP_SGFSR);
	uint64_t far = (uint64_t)wsp_rd(wsp_cb() + WSP_CB_FAR + 4) << 32 | wsp_rd(wsp_cb() + WSP_CB_FAR);
	printf("SWEET WLAN: smmu %s: context %u fsr %#x far %#llx fsynr0 %#x; sgfsr %#x sgfsynr1 %#x\n", when, wsp.context,
	    fsr, far, wsp_rd(wsp_cb() + WSP_CB_FSYNR0), sg, wsp_rd(WSP_SGFSYNR1));
}

static uint64_t *
wsp_table(uint32_t n)
{
	return (uint64_t *)(wsp.tables + 4096u * n);
}
static uint64_t *
wsp_next(uint64_t *entry)
{
	if (*entry & 1u) {
		for (uint32_t i = 0; i < wsp.table_count; i++) {
			if (kvtophys((vm_offset_t)wsp_table(i)) == (*entry & WSP_PTE_ADDRESS)) {
				return wsp_table(i);
			}
		}
		return NULL;
	}
	if (wsp.table_count == WSP_TABLES) {
		return NULL;
	}
	uint64_t *t = wsp_table(wsp.table_count++);
	*entry = kvtophys((vm_offset_t)t) | 3u;
	return t;
}

static bool
wsp_on(void)
{
	uint32_t sees = 1u << 31 | WSP_STREAM_MASK << 16 | WSP_STREAM, cb = wsp_cb();
	uint64_t root = kvtophys(wsp.tables) | (uint64_t)wsp.context << 48;

	wlan_checkpoint(NULL, "apps SMMU: our context + page tables for stream 0x1c00/1");
	wsp_wr(cb + WSP_CB_SCTLR, 0);
	wsp_wr(wsp.page + WSP_CBA2R(wsp.context), 1u);
	wsp_wr(wsp.page + WSP_CBAR(wsp.context), WSP_CBAR_STAGE_1);
	wsp_wr(cb + WSP_CB_TCR2, WSP_TCR2);
	wsp_wr(cb + WSP_CB_TCR, WSP_TCR_39_BITS);
	wsp_wr(cb + WSP_CB_TTBR0, (uint32_t)root);
	wsp_wr(cb + WSP_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	wsp_wr(cb + WSP_CB_TTBR1, 0);
	wsp_wr(cb + WSP_CB_TTBR1 + 4, 0);
	wsp_wr(cb + WSP_CB_MAIR0, WSP_MAIR0);
	wsp_wr(cb + WSP_CB_MAIR1, 0);
	wsp_wr(cb + WSP_CB_FSR, WSP_FSR_ALL);
	wsp_wr(cb + WSP_CB_TLBIASID, wsp.context);
	wsp_wr(cb + WSP_CB_TLBSYNC, 1);
	for (unsigned ms = 0; wsp_rd(cb + WSP_CB_TLBSTATUS) & 1u; ms++) {
		if (ms == 100) {
			printf("SWEET WLAN: smmu TLB sync does not finish\n");
			return false;
		}
		delay(1000);
	}
	wsp_wr(cb + WSP_CB_SCTLR, WSP_SCTLR_ON);
	wsp.on = true;
	wsp_wr(WSP_S2CR(wsp.match), wsp.context);
	wsp_wr(WSP_SMR(wsp.match), sees);
	uint32_t smr = wsp_rd(WSP_SMR(wsp.match)), s2cr = wsp_rd(WSP_S2CR(wsp.match));
	uint32_t cbar = wsp_rd(wsp.page + WSP_CBAR(wsp.context)), sctlr = wsp_rd(cb + WSP_CB_SCTLR);
	uint32_t t0 = wsp_rd(cb + WSP_CB_TTBR0), t1 = wsp_rd(cb + WSP_CB_TTBR0 + 4);
	printf("SWEET WLAN: smmu wrote: smr %#x s2cr %#x cbar %#x sctlr %#x ttbr0 %#x:%08x\n", smr, s2cr, cbar, sctlr, t1, t0);
	if (smr != sees || (s2cr & 0x300ffu) != wsp.context || (cbar & 0x30000u) != (WSP_CBAR_STAGE_1 & 0x30000u) ||
	    !(sctlr & 1u) || t0 != (uint32_t)root || t1 != (uint32_t)(root >> 32)) {
		printf("SWEET WLAN: SMMU DID NOT TAKE IT: stream 0x1c01 is not through context %u\n", wsp.context);
		return false;
	}
	wsp_faults("on");
	return true;
}

/* io->dma_map: `bytes` at kernel address `data`, page by page, behind the chip's next addresses. */
static int
wlan_dma_map(void *c, const void *data, uint32_t bytes, uint64_t *dma)
{
	(void)c;
	vm_offset_t va = trunc_page((vm_offset_t)data);
	uint64_t size = round_page((vm_offset_t)data + bytes) - va, at;
	int e = 0;

	lck_mtx_lock(&redwood_smmu_lock);
	if (wsp.tables == 0) {
		if (kmem_alloc(kernel_map, &wsp.tables, round_page(4096u * WSP_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			e = 1;
			goto out;
		}
		wsp.table_count = 1;
	}
	if (wsp.next + size > WSP_ADDRESSES_SIZE) {
		e = 2;
		goto out;
	}
	at = WSP_ADDRESSES + wsp.next;
	for (uint64_t off = 0; off < size; off += 4096u) {
		uint64_t a = at + off, *l2 = wsp_next(&wsp_table(0)[a >> 30 & 0x1ffu]);
		uint64_t *l3 = l2 ? wsp_next(&l2[a >> 21 & 0x1ffu]) : NULL;
		if (!l3) {
			e = 3;
			goto out;
		}
		l3[a >> 12 & 0x1ffu] = (kvtophys(va + off) & WSP_PTE_ADDRESS) | WSP_PTE;
	}
	for (vm_offset_t line = wsp.tables; line < wsp.tables + 4096u * WSP_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	for (vm_offset_t line = va; line < va + size; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));       /* the image as the chip will read it */
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wsp.next += size;
	*dma = at + ((vm_offset_t)data - va);
	printf("SWEET WLAN: dma map %u bytes at pa %#llx.. -> chip %#llx\n", bytes, kvtophys((vm_offset_t)data), *dma);
	if ((!wsp.found && !wsp_find()) || (!wsp.on && !wsp_on())) {
		e = 4;
	}
out:
	lck_mtx_unlock(&redwood_smmu_lock);
	if (e) {
		printf("SWEET WLAN: dma map failed (%d)\n", e);
	}
	return e;
}

static uint32_t wlan_stage;

static void
wlan_stage2(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	const uint32_t stage = wlan_stage;
	const lemonadep_wlan_io io = {.cookie = NULL, .read32 = wlan_read, .write32 = wlan_write,
	    .rpmh_vote = wlan_vote, .delay_us = wlan_delay, .checkpoint = wlan_checkpoint,
	    .trace = wlan_trace, .dma_map = wlan_dma_map};
	wlan_checkpoint(NULL, "stage 2 (boot log flushed first)");
	/* Stage 2. Firmware blobs: wlanfw1 = amss.bin, wlanfw2 = amss20.bin. */
	lemonadep_mhi_params p = {{0, 0}, {0, 0}};
	(void)PE_parse_boot_argn("wlanfw1_pa", &p.fw_v1.pa, sizeof(p.fw_v1.pa));
	(void)PE_parse_boot_argn("wlanfw1_bytes", &p.fw_v1.bytes, sizeof(p.fw_v1.bytes));
	(void)PE_parse_boot_argn("wlanfw2_pa", &p.fw_v2.pa, sizeof(p.fw_v2.pa));
	(void)PE_parse_boot_argn("wlanfw2_bytes", &p.fw_v2.bytes, sizeof(p.fw_v2.bytes));
	{
		/* HackiOS: the phone's own firmware in the small image's fw/ (fkernel/fw-wlan/, never in git) */
		uint64_t size = 0;
		p.fw_v1.data = redwood_image_file("fw", "amss.bin", &size, NULL);
		p.fw_v1.bytes = p.fw_v1.data ? (uint32_t)size : 0;
		size = 0;
		p.fw_v2.data = redwood_image_file("fw", "amss20.bin", &size, NULL);
		p.fw_v2.bytes = p.fw_v2.data ? (uint32_t)size : 0;
	}
	printf("SWEET WLAN: firmware amss.bin pa=%#llx bytes=%#x, amss20.bin pa=%#llx bytes=%#x\n",
	    p.fw_v1.pa, p.fw_v1.bytes, p.fw_v2.pa, p.fw_v2.bytes);
	const lemonadep_mhi_phase mstop =
	    stage < LemonadepMhiPhaseDone ? (lemonadep_mhi_phase)stage : LemonadepMhiPhaseNone;
	lemonadep_mhi_result m;
	lemonadep_mhi_status ms = lemonadep_wlan_mhi_up(&io, &p, &m, mstop);
	printf("SWEET WLAN: mhi status=%d phase=%d\n", ms, m.phase);
	wlan_note(3, "mhi st=%d ph=%d bar %#x", ms, m.phase, m.bar_pa);
	wlan_note(4, "socver %#x ee %u bhi %#x", m.soc_hw_version, m.ee, m.bhi_status0);
	wlan_note(5, "smmu scr0 %#x id0 %#x", m.scr0, m.id0);
	wlan_note(6, "smr[%d] %#x s2cr %#x", m.smr_index, m.smr, m.s2cr);
	if (m.phase >= LemonadepMhiPhaseBhi) {
		wlan_note(5, "smmu m%u cb%u %s dma %#llx", wsp.match, wsp.context, wsp.on ? "on" : "off", m.fw_dma);
		wlan_note(6, "bhi %#x %ums err %#x", m.bhi_status, m.bhi_ms, m.bhi_errcode);
		wlan_note(7, "ready st %#x ee %u %ums", m.ready_status, m.ready_ee, m.ready_ms);
	}
	wsp_faults("after");
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
	if (m.smmu_written) {
		printf("SWEET WLAN: smmu wrote entry %d: s2cr %#x -> read %#x, smr %#x -> read %#x\n", m.smr_free,
		    m.s2cr_want, m.s2cr_rb, m.smr_want, m.smr_rb);
	}
	if (m.smmu_quirk) {
		printf("SWEET WLAN: smmu bypass quirk: id1=%#x cb=%u sctlr was %#x, cbar %#x -> %#x\n",
		    m.id1, m.cb, m.cb_sctlr0, m.cbar0, m.cbar_rb);
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
		[WlanWinSmmu] = {LEMONADEP_APPS_SMMU_PA, SMMU_APPS_BYTES},   /* GR0, GR1, context banks */
	};
	for (unsigned i = 0; i < WlanWinCount; ++i) {
		if (!wlan_win[i]) {
			wlan_win[i] = ml_io_map_unmappable(maps[i].pa, maps[i].bytes, WLAN_VM_WIMG_IO);
		}
		if (!wlan_win[i]) {
			printf("SWEET WLAN: ml_io_map_unmappable failed for window %u (pa %#x)\n", i, maps[i].pa);
			return;
		}
	}

	printf("SWEET WLAN: stage=%u stop_after=%d (QCA6490 on PCIe0)\n", stage, stop_after);
	const lemonadep_wlan_io io = {.cookie = NULL, .read32 = wlan_read, .write32 = wlan_write,
	    .rpmh_vote = wlan_vote, .delay_us = wlan_delay};
	lemonadep_wlan_result r;
	lemonadep_wlan_status st = lemonadep_wlan_pcie_up(&io, &r, stop_after);
	wlan_note(0, "WLAN pcie st=%d ph=%d %s", st, r.phase, r.failed_what ? r.failed_what : "");
	wlan_note(1, "ltssm %#x link %#x", (r.elbi_stts >> 12) & 0x3fu, r.link_status);
	wlan_note(2, "id %#x class %#x", r.id, r.class_rev);
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

	/* Stage 2 runs on its own thread, after the boot log so far has reached
	 * the disk, with a logged pause before each risky access. */
	wlan_stage = stage;
	thread_t t;
	if (kernel_thread_start(wlan_stage2, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	} else {
		printf("SWEET WLAN: could not start the stage 2 thread\n");
	}
}
