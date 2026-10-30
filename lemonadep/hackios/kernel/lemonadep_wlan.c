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
 * Every line also straight into the loader's log as it is said (loader_return.c): the loader keeps
 * its first 256 KB and its last 1 MB, and the kernel's message buffer, appended at the end of a run,
 * pushed these lines into the dropped middle (2026-10-30).
 */
extern void loader_log_putc(char c);
static void wlan_printf(const char *fmt, ...) __printflike(1, 2);
static void
wlan_printf(const char *fmt, ...)
{
	char line[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	for (const char *c = line; *c; c++) {
		loader_log_putc(*c);
	}
}
#define printf wlan_printf

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
#define WSP_STREAM              0x1c01u     /* exact: this hypervisor refuses a masked SMR (0x1c00 mask 1 read back 0, 2026-10-30); 0x1c01 mask 0 stuck on 10-24 */
#define WSP_STREAM_MASK         0x0000u
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
	uint32_t rb_smr, rb_s2cr, rb_cbar, rb_sctlr, rb_t0, rb_t1;     /* read back after wsp_on()'s writes */
	bool taken;
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

		if (smr == (1u << 31 | WSP_STREAM)) {
			/*
			 * Exactly ours: left by an earlier run in this loader session (the SMMU is not reset
			 * between runs). Taken over -- match register and context -- and programmed afresh.
			 */
			printf("SWEET WLAN: smmu match register %u is ours from an earlier run (s2cr %#x): reused\n", n, s2cr);
			wsp.match = n;
			wsp.context = s2cr & 0xffu;
			wsp.old_smr = 0;
			wsp.old_s2cr = 0;
			wsp.old_sctlr = 0;
			wsp.found = true;
			return true;
		}
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

	wlan_checkpoint(NULL, "apps SMMU: our context + page tables for stream 0x1c01");
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
	wsp.rb_smr = smr;
	wsp.rb_s2cr = s2cr;
	wsp.rb_cbar = cbar;
	wsp.rb_sctlr = sctlr;
	wsp.rb_t0 = t0;
	wsp.rb_t1 = t1;
	if (smr != sees || (s2cr & 0x300ffu) != wsp.context || (cbar & 0x30000u) != (WSP_CBAR_STAGE_1 & 0x30000u) ||
	    !(sctlr & 1u) || t0 != (uint32_t)root || t1 != (uint32_t)(root >> 32)) {
		printf("SWEET WLAN: SMMU DID NOT TAKE IT: stream 0x1c01 is not through context %u\n", wsp.context);
		return false;
	}
	wsp.taken = true;
	wsp_faults("on");
	return true;
}

/*
 * Which stream id the PCIe controller puts on the chip's requests: pcie-qcom.c
 * qcom_pcie_config_sid_1_9_0() (SM8250/SM8350 controllers), from the dt's iommu-map
 * <0x0 &apps_smmu 0x1c00 1>, <0x100 &apps_smmu 0x1c01 1>: a table in PARF at 0x2000, 256 words
 * indexed by crc8 (poly 0x07, msb first, init 0) of the BDF big-endian; a word is BDF [31:16],
 * SID offset from 0x1c00 [15:8], next index [7:0]. Nobody fills it before us under UEFI, and an
 * empty table leaves the chip's reads on 0x1c00, which matches nothing (unmatched streams fault
 * here): the SoC died at the chip's first read on 10-24 and on 10-30 with two different SMMU setups.
 */
#define WLAN_PARF_BDF_TO_SID    0x2000u
static uint8_t
wlan_crc8(const uint8_t *data, unsigned n)
{
	uint8_t crc = 0;
	while (n--) {
		crc ^= *data++;
		for (int b = 0; b < 8; b++) {
			crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
		}
	}
	return crc;
}
static uint32_t wlan_sid_word, wlan_sid_rb, wlan_sid_cfg, wlan_sid_cfg_now;
#define WLAN_PARF_BDF_TO_SID_CFG 0x2c00u       /* pcie-qcom.c: bit 0 = BDF_TO_SID_BYPASS */
static uint8_t wlan_sid_hash;
static void
wlan_config_sid(void)
{
	const uint16_t bdf = 0x0100;            /* bus 1, device 0, function 0: the QCA6490 */
	const uint8_t be[2] = { (uint8_t)(bdf >> 8), (uint8_t)bdf };

	(void)wlan_read(NULL, WlanWinParf, WLAN_PARF_BDF_TO_SID_CFG, &wlan_sid_cfg);
	for (uint32_t i = 0; i < 256; i++) {
		(void)wlan_write(NULL, WlanWinParf, WLAN_PARF_BDF_TO_SID + 4 * i, 0);
	}
	/* entry for BDF 0 (-> 0x1c00, offset 0) is all zero: nothing to write; one entry, no chain */
	wlan_sid_hash = wlan_crc8(be, 2);
	wlan_sid_word = (uint32_t)bdf << 16 | (0x1c01u - 0x1c00u) << 8;
	(void)wlan_write(NULL, WlanWinParf, WLAN_PARF_BDF_TO_SID + 4u * wlan_sid_hash, wlan_sid_word);
	(void)wlan_read(NULL, WlanWinParf, WLAN_PARF_BDF_TO_SID + 4u * wlan_sid_hash, &wlan_sid_rb);
	/*
	 * And use it: the boot chain leaves BDF_TO_SID_BYPASS set (read 0x1 on 2026-10-30), with which
	 * the table is ignored and every request carries the base stream 0x1c00 -- which matches
	 * nothing, faults, and took the SoC down at the chip's first read.
	 */
	(void)wlan_write(NULL, WlanWinParf, WLAN_PARF_BDF_TO_SID_CFG, wlan_sid_cfg & ~1u);
	(void)wlan_read(NULL, WlanWinParf, WLAN_PARF_BDF_TO_SID_CFG, &wlan_sid_cfg_now);
	printf("SWEET WLAN: pcie bdf-to-sid: [%u] = %#x (read %#x): BDF 01:00.0 -> stream 0x1c01\n", wlan_sid_hash,
	    wlan_sid_word, wlan_sid_rb);
}

/*
 * The chip's MSI target (dt qcom,pcie0_msi@17a10040: the GIC's set-SPI register, SPIs 768..799, so
 * vector v is interrupt 800 + v), behind an address of the chip's like furangelz's RADIO_MSI_REGISTER:
 * one page, device memory, never executed. Returns the chip-side address of the register.
 */
#define WSP_PTE_DEVICE          (WSP_PTE | 2ull << 2 | 1ull << 53 | 1ull << 54)
static uint64_t
wsp_map_device(uint64_t pa)
{
	uint64_t at = WSP_ADDRESSES + wsp.next, *l2, *l3;

	if (wsp.tables == 0 || wsp.next + 4096 > WSP_ADDRESSES_SIZE) {
		return 0;
	}
	l2 = wsp_next(&wsp_table(0)[at >> 30 & 0x1ffu]);
	l3 = l2 ? wsp_next(&l2[at >> 21 & 0x1ffu]) : NULL;
	if (!l3) {
		return 0;
	}
	l3[at >> 12 & 0x1ffu] = (pa & ~0xfffull) | WSP_PTE_DEVICE;
	for (vm_offset_t line = wsp.tables; line < wsp.tables + 4096u * WSP_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wsp.next += 4096;
	if (wsp.on) {
		wsp_wr(wsp_cb() + WSP_CB_TLBIASID, wsp.context);
		wsp_wr(wsp_cb() + WSP_CB_TLBSYNC, 1);
	}
	return at + (pa & 0xfffu);
}

/* The chip's MSI capability, programmed as pci-msm/cnss2 leave it: one vector here. */
static uint32_t msi_cap, msi_ctrl_now, msi_addr_lo, msi_data;
static void
wlan_enable_msi(void)
{
	uint32_t v, cap;

	(void)wlan_read(NULL, WlanWinConf, 0x34, &v);
	for (cap = v & 0xfcu; cap; cap = (v >> 8) & 0xfcu) {
		(void)wlan_read(NULL, WlanWinConf, cap, &v);
		if ((v & 0xffu) == 0x05u) {
			break;
		}
	}
	msi_cap = cap;
	if (!cap) {
		printf("SWEET WLAN: msi: no MSI capability\n");
		return;
	}
	uint64_t addr = wsp_map_device(0x17a10040ull);
	if (!addr) {
		printf("SWEET WLAN: msi: could not map the target\n");
		return;
	}
	bool is64 = (v >> 16) & 0x80u;
	msi_addr_lo = (uint32_t)addr;
	msi_data = 800;
	(void)wlan_write(NULL, WlanWinConf, cap + 4, (uint32_t)addr);
	if (is64) {
		(void)wlan_write(NULL, WlanWinConf, cap + 8, (uint32_t)(addr >> 32));
		(void)wlan_write(NULL, WlanWinConf, cap + 12, msi_data);
	} else {
		(void)wlan_write(NULL, WlanWinConf, cap + 8, msi_data);
	}
	/* control: enable, one message (MME 0) */
	(void)wlan_read(NULL, WlanWinConf, cap, &v);
	v = (v & ~(0x70u << 16)) | (1u << 16);
	(void)wlan_write(NULL, WlanWinConf, cap, v);
	(void)wlan_read(NULL, WlanWinConf, cap, &msi_ctrl_now);
	printf("SWEET WLAN: msi: cap %#x ctrl %#x (64-bit %d), address %#llx (-> 0x17a10040), data %u\n", cap,
	    msi_ctrl_now >> 16, is64, addr, msi_data);
}

/* io->dma_map: `bytes` at kernel address `data`, page by page, behind the chip's next addresses. */
static int
wlan_dma_map(void *c, const void *data, uint32_t bytes, uint64_t *dma)
{
	(void)c;
	vm_offset_t va = trunc_page((vm_offset_t)data);
	uint64_t size = round_page((vm_offset_t)data + bytes) - va, at;
	int e = 0;

	static bool sid_done;           /* once: rewriting it while the chip does DMA would send it to 0x1c00 */
	if (!sid_done) {
		wlan_config_sid();
		sid_done = true;
	}
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
	} else {
		static bool msi_done;
		uint32_t msi = 0;
		if (!msi_done && PE_parse_boot_argn("wlan_msi", &msi, sizeof(msi)) && msi) {
			wlan_enable_msi();
			msi_done = true;
		}
		uint32_t nodb = 0;
		if (PE_parse_boot_argn("wlan_nodb", &nodb, sizeof(nodb)) && nodb) {
			printf("SWEET WLAN: wlan_nodb=1: everything but the BHI doorbell; stopping here\n");
			e = 9;                  /* the MHI stage stops before any chip DMA (status 15) */
		}
	}
out:
	lck_mtx_unlock(&redwood_smmu_lock);
	if (e) {
		printf("SWEET WLAN: dma map failed (%d)\n", e);
	}
	return e;
}

/*
 * The chip's own registers the way ath11k reaches them (pcic.c): below 0x80000 straight through
 * BAR0, above through the window register (0x310c: enable | offset[24:19]) and BAR0 + 0x80000.
 */
static uint32_t wlan_bar;               /* BAR0's offset in WlanWinMem */
static uint32_t wlan_window = ~0u;
static uint32_t
chip_rd(uint32_t off)
{
	uint32_t v = 0;
	if (off < 0x80000u) {
		(void)wlan_read(NULL, WlanWinMem, wlan_bar + off, &v);
		return v;
	}
	uint32_t w = off >> 19 & 0x3fu;
	if (w != wlan_window) {
		(void)wlan_write(NULL, WlanWinMem, wlan_bar + 0x310cu, 0x40000000u | w);
		(void)wlan_read(NULL, WlanWinMem, wlan_bar + 0x310cu, &v);
		if (v != (0x40000000u | w)) {
			printf("SWEET WLAN: window register 0x310c = %#x after writing %#x (offset %#x)\n", v,
			    0x40000000u | w, off);
		}
		wlan_window = w;
	}
	(void)wlan_read(NULL, WlanWinMem, wlan_bar + 0x80000u + (off & 0x7ffffu), &v);
	return v;
}
static void
chip_wr(uint32_t off, uint32_t v)
{
	if (off < 0x80000u) {
		(void)wlan_write(NULL, WlanWinMem, wlan_bar + off, v);
		return;
	}
	(void)chip_rd(off);                     /* selects the window */
	(void)wlan_write(NULL, WlanWinMem, wlan_bar + 0x80000u + (off & 0x7ffffu), v);
}

/*
 * ath11k_pci_sw_reset(ab, power_on = true) (pci.c, Linux 6.6), before the first BHI: without it
 * the boot ROM took the SBL (BHI status success) and never left PBL (2026-10-30). fix_l1ss is
 * left out (the link is held to Gen1 and ASPM is not enabled here).
 */
static uint32_t sw_ltssm, sw_hotrst, sw_cookie, sw_warm, sw_cause, sw_gr, sw_qfprom, sw_qfprom_now;
static void
wlan_sw_reset(void)
{
	delay(100000);
	/* ath11k_pci_enable_ltssm */
	uint32_t v = chip_rd(0x1e081b0u);                       /* PCIE_PCIE_PARF_LTSSM */
	for (int i = 0; v != 0x111u && i < 5; i++) {
		if (v == 0xffffffffu) {
			delay(5000);
		}
		chip_wr(0x1e081b0u, 0x111u);
		v = chip_rd(0x1e081b0u);
	}
	sw_ltssm = v;
	chip_wr(0x1e402bcu, chip_rd(0x1e402bcu) | 0x10u);       /* GCC_GCC_PCIE_HOT_RST */
	sw_hotrst = chip_rd(0x1e402bcu);
	delay(5000);
	chip_wr(0x1e08228u, 0xffffffffu);                       /* ath11k_pci_clear_all_intrs */
	/* cnss_pci_set_wlaon_pwr_ctrl(set_vddd4blow false, set_shutdown false): VDD4BLOW_SW_EN and SHUTDOWN_EN off */
	sw_qfprom = chip_rd(0x1f8031cu);
	chip_wr(0x1f8031cu, sw_qfprom & ~0x5u);
	sw_qfprom_now = chip_rd(0x1f8031cu);
	/* ath11k_mhi_clear_vector */
	chip_wr(0x360u, 0);
	chip_wr(0x368u, 0);
	chip_wr(0x394u, 0);
	chip_wr(0x39cu, 0);
	/* ath11k_pci_clear_dbg_registers */
	sw_cookie = chip_rd(0x1f80500u);                        /* PCIE_Q6_COOKIE_ADDR */
	delay(10000);
	chip_wr(0x1f80504u, 0);                                 /* WLAON_WARM_SW_ENTRY: no warm path, no dead loop */
	delay(10000);
	sw_warm = chip_rd(0x1f80504u);
	sw_cause = chip_rd(0x1f8060cu);                         /* WLAON_SOC_RESET_CAUSE_REG, clears on read */
	/* ath11k_pci_soc_global_reset */
	v = chip_rd(0x3008u);
	chip_wr(0x3008u, v | 1u);
	delay(10000);
	chip_wr(0x3008u, v & ~1u);
	delay(10000);
	sw_gr = chip_rd(0x3008u);
	wlan_window = ~0u;                                      /* (the reset may have cleared it) */
	/* ath11k_mhi_set_mhictrl_reset */
	(void)chip_rd(0x48u);
	chip_wr(0x38u, 0x2u);
	printf("SWEET WLAN: sw reset: ltssm %#x hot_rst %#x cookie %#x warm_entry %#x reset_cause %#x global_reset %#x "
	    "qfprom_pwr_ctrl %#x -> %#x\n", sw_ltssm, sw_hotrst, sw_cookie, sw_warm, sw_cause, sw_gr, sw_qfprom, sw_qfprom_now);
}

/*
 * The host side of MHI once the SBL says READY (Linux drivers/bus/mhi/host: init.c mhi_init_dev_ctxt,
 * mhi_init_mmio; pm.c mhi_ready_state_transition; boot.c mhi_fw_load_bhie; ath11k mhi.c's
 * QCA6390 configuration): contexts and rings in memory the chip reaches through the SMMU, M0, then
 * the whole amss.bin over BHIe and the wait for mission mode (EE AMSS). Polled; MSI only wakes nobody.
 */
struct mhi_ctx { uint32_t a, b, c; uint64_t rbase, rlen, rp, wp; } __attribute__((packed, aligned(4)));
struct mhi_el { uint64_t ptr; uint32_t d0, d1; };
#define MHI_NCH         128
#define MHI_CTRL_BYTES  0x10000u
enum { O_CHAN = 0, O_ER = 0x2000, O_CMD = 0x2100, O_ER0 = 0x2200, O_ER1 = 0x3000, O_CMDR = 0x4000,
	O_CH0 = 0x4800, O_CH1 = 0x4a00, O_CH20 = 0x5000, O_CH21 = 0x5400, O_RXBUF = 0x8000 };
static const void *wlan_fw_full;
static uint32_t wlan_fw_full_bytes;
static uint8_t *mh;                     /* kernel view of the control region */
static uint64_t mh_iova;                /* the chip's */
static uint32_t er_rp[2];               /* our read index into each event ring (elements) */
static const uint32_t er_n[2] = { 32, 256 };
static const uint32_t er_off[2] = { O_ER0, O_ER1 };
static uint32_t mh_m0, mh_ee_ev, mh_events;

static void
mh_clean(uint32_t off, uint32_t len)
{
	for (uintptr_t l = (uintptr_t)(mh + off) & ~63ul; l < (uintptr_t)(mh + off + len); l += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (l));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}
static void
mh_inval(uint32_t off, uint32_t len)
{
	for (uintptr_t l = (uintptr_t)(mh + off) & ~63ul; l < (uintptr_t)(mh + off + len); l += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (l));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}
static void
mh_db(uint32_t reg, uint64_t v)         /* 64-bit doorbell: high, then low (mhi_write_db) */
{
	chip_wr(reg + 4, (uint32_t)(v >> 32));
	chip_wr(reg, (uint32_t)v);
}
static void
mh_ctx(uint32_t off, uint32_t a, uint32_t b, uint32_t c, uint32_t ring, uint32_t len)
{
	struct mhi_ctx *x = (struct mhi_ctx *)(mh + off);
	x->a = a;
	x->b = b;
	x->c = c;
	x->rbase = x->rp = x->wp = mh_iova + ring;
	x->rlen = len;
}

/* Every event the chip has written since the last look, logged; state, EE and command events noted. */
static uint32_t erdboff;
static void
mh_events_poll(void)
{
	for (int r = 0; r < 2; r++) {
		struct mhi_ctx *x = (struct mhi_ctx *)(mh + O_ER + r * sizeof(struct mhi_ctx));
		mh_inval(O_ER, 2 * sizeof(struct mhi_ctx));
		uint64_t dev_rp = x->rp;
		uint32_t dev_i = (uint32_t)((dev_rp - (mh_iova + er_off[r])) / 16);
		int n = 0;
		while (er_rp[r] != dev_i && dev_i < er_n[r] && n++ < 64) {
			mh_inval(er_off[r] + er_rp[r] * 16, 16);
			struct mhi_el *e = (struct mhi_el *)(mh + er_off[r] + er_rp[r] * 16);
			uint32_t type = e->d1 >> 16 & 0xff, code = e->d0 >> 24;
			mh_events++;
			printf("SWEET WLAN: mhi ev[%d][%u] type %#x code/state/ee %#x len %u ch %u ptr %#llx\n", r, er_rp[r],
			    type, code, e->d0 & 0xffff, e->d1 >> 24, e->ptr);
			if (type == 0x20 && code == 2) {
				mh_m0 = 1;
			}
			if (type == 0x40) {
				mh_ee_ev = code;
			}
			er_rp[r] = (er_rp[r] + 1) % er_n[r];
			/* hand the element back: wp follows our read pointer, one behind (mhi_recycle_ev_ring) */
			uint64_t wp = mh_iova + er_off[r] + ((er_rp[r] + er_n[r] - 1) % er_n[r]) * 16;
			x->wp = wp;
			mh_clean(O_ER, 2 * sizeof(struct mhi_ctx));
			mh_db(erdboff + 8 * r, wp);
		}
	}
}

static void
wlan_mhi_host(const lemonadep_mhi_result *m, const void *fw, uint32_t fw_bytes)
{
	vm_offset_t region = 0;
	uint32_t chdboff, v;

	if (kmem_alloc(kernel_map, &region, MHI_CTRL_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		printf("SWEET WLAN: mhi host: no memory\n");
		return;
	}
	mh = (uint8_t *)region;
	if (wlan_dma_map(NULL, mh, MHI_CTRL_BYTES, &mh_iova)) {
		return;
	}
	/* channel contexts: 0/1 LOOPBACK, 20 IPCR out, 21 IPCR in (ath11k_mhi_channels_qca6390) */
	const uint32_t chcfg = 2u << 8;         /* state DISABLED, MHI_DB_BRST_DISABLE, pollcfg 0 */
	mh_ctx(O_CHAN + 0 * sizeof(struct mhi_ctx), chcfg, 1, 0, O_CH0, 32 * 16);
	mh_ctx(O_CHAN + 1 * sizeof(struct mhi_ctx), chcfg, 2, 0, O_CH1, 32 * 16);
	mh_ctx(O_CHAN + 20 * sizeof(struct mhi_ctx), chcfg, 1, 1, O_CH20, 64 * 16);
	mh_ctx(O_CHAN + 21 * sizeof(struct mhi_ctx), chcfg, 2, 1, O_CH21, 64 * 16);
	/* event rings: ctrl (32, irq 1), data (256, irq 2, 1 ms moderation); one MSI vector here: msivec 0 */
	mh_ctx(O_ER + 0 * sizeof(struct mhi_ctx), 0, 1, 0, O_ER0, 32 * 16);
	mh_ctx(O_ER + 1 * sizeof(struct mhi_ctx), 1u << 16, 1, 0, O_ER1, 256 * 16);
	mh_ctx(O_CMD, 0, 0, 0, O_CMDR, 128 * 16);
	mh_clean(0, MHI_CTRL_BYTES);

	chdboff = chip_rd(0x18);
	erdboff = chip_rd(0x20);
	chip_wr(0x5c, (uint32_t)((mh_iova + O_CHAN) >> 32));
	chip_wr(0x58, (uint32_t)(mh_iova + O_CHAN));
	chip_wr(0x64, (uint32_t)((mh_iova + O_ER) >> 32));
	chip_wr(0x60, (uint32_t)(mh_iova + O_ER));
	chip_wr(0x6c, (uint32_t)((mh_iova + O_CMD) >> 32));
	chip_wr(0x68, (uint32_t)(mh_iova + O_CMD));
	chip_wr(0x84, 0);
	chip_wr(0x80, (uint32_t)WSP_ADDRESSES);
	chip_wr(0x9c, 0);
	chip_wr(0x98, (uint32_t)WSP_ADDRESSES);
	chip_wr(0x8c, 0);
	chip_wr(0x88, (uint32_t)(WSP_ADDRESSES + WSP_ADDRESSES_SIZE - 1));
	chip_wr(0xa4, 0);
	chip_wr(0xa0, (uint32_t)(WSP_ADDRESSES + WSP_ADDRESSES_SIZE - 1));
	v = chip_rd(0x10);
	v = (v & ~(0xffu << 16)) | 2u << 16;    /* NER 2 */
	v &= ~(0xffu << 24);                     /* NHWER 0 */
	chip_wr(0x10, v);
	printf("SWEET WLAN: mhi host: ctrl at chip %#llx; chdboff %#x erdboff %#x mhicfg %#x\n", mh_iova, chdboff, erdboff,
	    chip_rd(0x10));
	/* all event ring elements to the chip, and the doorbells (mhi_ready_state_transition) */
	for (int r = 0; r < 2; r++) {
		struct mhi_ctx *x = (struct mhi_ctx *)(mh + O_ER + r * sizeof(struct mhi_ctx));
		x->wp = mh_iova + er_off[r] + (er_n[r] - 1) * 16;
		mh_clean(O_ER, 2 * sizeof(struct mhi_ctx));
		mh_db(erdboff + 8 * r, x->wp);
	}
	/* M0 */
	v = chip_rd(0x38);
	chip_wr(0x38, (v & ~(0xffu << 8) & ~2u) | 2u << 8);
	for (int i = 0; i < 5000 && !mh_m0; i++) {
		mh_events_poll();
		if ((chip_rd(0x48) >> 8 & 0xff) == 2) {
			mh_m0 = 1;
		}
		delay(1000);
	}
	printf("SWEET WLAN: mhi host: M0 %s (status %#x ctrl %#x ee %u), %u events\n", mh_m0 ? "reached" : "NOT reached",
	    chip_rd(0x48), chip_rd(0x38), chip_rd(m->bhioff + 0x28), mh_events);
	wlan_note(7, "M0 %u st %#x ee %u ev %u", mh_m0, chip_rd(0x48), chip_rd(m->bhioff + 0x28), mh_events);
	if (!mh_m0) {
		return;
	}

	/* BHIe: the whole image in 512 KB segments, the vector table after them (mhi_alloc_bhie_table) */
	uint32_t segs = (fw_bytes + 0x7ffffu) / 0x80000u;
	vm_offset_t img = 0, vec = 0;
	uint64_t img_iova, vec_iova;
	if (kmem_alloc(kernel_map, &img, round_page(fw_bytes), KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &vec, round_page(segs * 16), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		printf("SWEET WLAN: bhie: no memory\n");
		return;
	}
	memcpy((void *)img, fw, fw_bytes);
	if (wlan_dma_map(NULL, (void *)img, fw_bytes, &img_iova)) {
		return;
	}
	uint64_t *ve = (uint64_t *)vec;
	for (uint32_t i = 0; i < segs; i++) {
		ve[2 * i] = img_iova + (uint64_t)i * 0x80000u;
		ve[2 * i + 1] = (i + 1 < segs) ? 0x80000u : fw_bytes - i * 0x80000u;
	}
	if (wlan_dma_map(NULL, (void *)vec, segs * 16, &vec_iova)) {
		return;
	}
	uint32_t be = m->bhieoff;
	chip_wr(be + 0x30, (uint32_t)(vec_iova >> 32));
	chip_wr(be + 0x2c, (uint32_t)vec_iova);
	chip_wr(be + 0x34, segs * 16);
	chip_wr(be + 0x3c, (chip_rd(be + 0x3c) & ~0x3fffffffu) | 0x2bad1e5u);
	printf("SWEET WLAN: bhie: %u bytes in %u segments at chip %#llx, vector table %#llx; doorbell rung\n", fw_bytes, segs,
	    img_iova, vec_iova);
	uint32_t st = 0;
	unsigned ms;
	for (ms = 0; ms < 30000; ms++) {
		st = chip_rd(be + 0x44);
		if (st >> 30) {
			break;
		}
		if (ms % 50 == 0) {
			mh_events_poll();
		}
		delay(1000);
	}
	printf("SWEET WLAN: bhie: txvecstatus %#x after %u ms (2 = done, 3 = error); ee %u\n", st, ms, chip_rd(m->bhioff + 0x28));
	/* mission mode: EE 2 (AMSS) */
	uint32_t ee = 0;
	for (ms = 0; ms < 30000; ms++) {
		ee = chip_rd(m->bhioff + 0x28);
		if (ms % 20 == 0) {
			mh_events_poll();
		}
		if (ee == 2 || mh_ee_ev == 2) {
			break;
		}
		delay(1000);
	}
	mh_events_poll();
	printf("SWEET WLAN: mhi host: EE %u (event EE %u) after %u ms, status %#x, %u events -- %s\n", ee, mh_ee_ev, ms,
	    chip_rd(0x48), mh_events, ee == 2 ? "MISSION MODE" : "no mission mode");
	wlan_note(7, "bhie %#x EE %u ev %u %s", st, ee, mh_events, ee == 2 ? "AMSS" : "");
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
		/*
		 * Like Linux (mhi_fw_load_bhi from a dma_alloc_coherent buffer): the SBL part copied into
		 * page-aligned kernel memory of its own, not handed out where it lies in the ramdisk. The
		 * boot ROM rejected what it read in place with 0xef0e0200 (2026-10-30), a code another
		 * QCA6490 port got only from an image whose segment table was unusable.
		 */
		uint32_t fw20 = 0;
		if (PE_parse_boot_argn("wlan_fw20", &fw20, sizeof(fw20)) && fw20) {
			uint64_t s20 = 0;
			const uint8_t *d20 = redwood_image_file("fw", "amss20.bin", &s20, NULL);
			printf("SWEET WLAN: wlan_fw20=1: amss20.bin (%llu bytes) in place of amss.bin (test)\n", s20);
			if (d20 && s20) {
				p.fw_v1.data = d20;
				p.fw_v1.bytes = (uint32_t)s20;
			}
		}
		wlan_fw_full = p.fw_v1.data;
		wlan_fw_full_bytes = p.fw_v1.bytes;
		if (p.fw_v1.data && p.fw_v1.bytes) {
			vm_offset_t copy = 0;
			uint32_t n = p.fw_v1.bytes < 0x80000u ? p.fw_v1.bytes : 0x80000u, cut = 0;
			if (PE_parse_boot_argn("wlan_sblsize", &cut, sizeof(cut)) && cut && cut < n) {
				n = cut;        /* test: BHI given only this much of the image */
			}
			if (kmem_alloc(kernel_map, &copy, round_page(n), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
			    VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
				memcpy((void *)copy, p.fw_v1.data, n);
				uint32_t corrupt = 0;
				if (PE_parse_boot_argn("wlan_corrupt", &corrupt, sizeof(corrupt)) && corrupt) {
					((uint8_t *)copy)[0] = 0;       /* test: does the ROM see our bytes? (bad magic) */
				}
				const uint8_t *h = (const uint8_t *)copy;
				printf("SWEET WLAN: sbl copy %u bytes at %#lx (pa %#llx): %02x%02x%02x%02x class %u phoff %#x phnum %u\n",
				    n, copy, kvtophys(copy), h[0], h[1], h[2], h[3], h[4], *(const uint32_t *)(h + 0x1c),
				    *(const uint16_t *)(h + 0x2c));
				p.fw_v1.data = (const void *)copy;
				p.fw_v1.bytes = n;
			}
		}
		size = 0;
		p.fw_v2.data = redwood_image_file("fw", "amss20.bin", &size, NULL);
		p.fw_v2.bytes = p.fw_v2.data ? (uint32_t)size : 0;
	}
	printf("SWEET WLAN: firmware amss.bin pa=%#llx bytes=%#x, amss20.bin pa=%#llx bytes=%#x\n",
	    p.fw_v1.pa, p.fw_v1.bytes, p.fw_v2.pa, p.fw_v2.bytes);
	const lemonadep_mhi_phase mstop =
	    stage < LemonadepMhiPhaseDone ? (lemonadep_mhi_phase)stage : LemonadepMhiPhaseNone;
	lemonadep_mhi_result m;
	lemonadep_mhi_status ms;
	uint32_t noreset = 0;
	(void)PE_parse_boot_argn("wlan_noreset", &noreset, sizeof(noreset));
	if (!noreset && (mstop == LemonadepMhiPhaseNone || mstop > LemonadepMhiPhaseRegs)) {
		ms = lemonadep_wlan_mhi_up(&io, &p, &m, LemonadepMhiPhaseRegs);
		if (ms == LemonadepMhiOk && m.bar_pa) {
			wlan_bar = m.bar_pa - LEMONADEP_PCIE0_MEM_PA;
			wlan_sw_reset();
		}
	}
	if (m.bar_pa) {
		wlan_bar = m.bar_pa - LEMONADEP_PCIE0_MEM_PA;
		wlan_window = ~0u;
		printf("SWEET WLAN: before BHI: TCSR_PBL_LOGGING %#x BOOT_CFG %#x BOOTSTRAP %#x SRAM0 %#x SRAM9 %#x\n",
		    chip_rd(0x01B000F8u), chip_rd(0x01E22B34u), chip_rd(0x01910008u), chip_rd(0x01403DA0u), chip_rd(0x01403DC4u));
	}
	ms = lemonadep_wlan_mhi_up(&io, &p, &m, mstop);
	printf("SWEET WLAN: mhi status=%d phase=%d\n", ms, m.phase);
	if (ms == LemonadepMhiOk && m.phase >= LemonadepMhiPhaseDone && wlan_fw_full && wlan_fw_full_bytes) {
		wlan_bar = m.bar_pa - LEMONADEP_PCIE0_MEM_PA;
		wlan_window = ~0u;
		wlan_mhi_host(&m, wlan_fw_full, wlan_fw_full_bytes);
	}
	wlan_note(3, "mhi st=%d ph=%d bar %#x", ms, m.phase, m.bar_pa);
	wlan_note(4, "socver %#x ee %u bhi %#x", m.soc_hw_version, m.ee, m.bhi_status0);
	wlan_note(5, "smmu scr0 %#x id0 %#x", m.scr0, m.id0);
	wlan_note(6, "smr[%d] %#x s2cr %#x", m.smr_index, m.smr, m.s2cr);
	if (m.phase >= LemonadepMhiPhaseBhi) {
		wlan_note(5, "m%u cb%u %s sid[%u] %#x cfg %#x>%#x", wsp.match, wsp.context, wsp.taken ? "OK" : "NO",
		    wlan_sid_hash, wlan_sid_rb, wlan_sid_cfg, wlan_sid_cfg_now);
		if (wsp.taken && m.phase > LemonadepMhiPhaseBhi) {
			wlan_note(6, "bhi %#x %ums err %#x", m.bhi_status, m.bhi_ms, m.bhi_errcode);
			wlan_note(7, "ready st %#x ee %u %ums", m.ready_status, m.ready_ee, m.ready_ms);
		} else if (wsp.taken) {
		} else {
			wlan_note(6, "cbar %#x sctlr %#x", wsp.rb_cbar, wsp.rb_sctlr);
			wlan_note(7, "ttbr0 %#x:%08x want %#llx", wsp.rb_t1, wsp.rb_t0, kvtophys(wsp.tables) |
			    (uint64_t)wsp.context << 48);
		}
	}
	if (ms == LemonadepMhiReadyTimeout && m.bar_pa) {
		/*
		 * After a good BHI the chip never said READY. Is the SBL running with the chip asleep
		 * (registers read as 0 until woken: ath11k pci.c ath11k_pci_force_wake(), PCIE_SOC_WAKE at
		 * BAR0 + 0x3004 = 1), or did the boot ROM never leave? Read, wake, read again.
		 */
		const uint32_t bar = m.bar_pa - LEMONADEP_PCIE0_MEM_PA, bhi = bar + m.bhioff;
		uint32_t v0, e0, s0, c0, v1, e1, s1, c1;
		(void)wlan_read(NULL, WlanWinMem, bar + MHI_VER, &v0);
		(void)wlan_read(NULL, WlanWinMem, bhi + BHI_EXECENV, &e0);
		(void)wlan_read(NULL, WlanWinMem, bar + MHI_STATUS, &s0);
		(void)wlan_read(NULL, WlanWinMem, bar + MHI_CTRL, &c0);
		(void)wlan_write(NULL, WlanWinMem, bar + 0x3004u, 1);
		delay(50000);
		(void)wlan_read(NULL, WlanWinMem, bar + MHI_VER, &v1);
		(void)wlan_read(NULL, WlanWinMem, bhi + BHI_EXECENV, &e1);
		(void)wlan_read(NULL, WlanWinMem, bar + MHI_STATUS, &s1);
		(void)wlan_read(NULL, WlanWinMem, bar + MHI_CTRL, &c1);
		printf("SWEET WLAN: after ready timeout: ver %#x ee %u status %#x ctrl %#x; woken: ver %#x ee %u status %#x "
		    "ctrl %#x\n", v0, e0, s0, c0, v1, e1, s1, c1);
		wlan_note(6, "e%u s%x c%x lt%x gr%x w%x", e0, s0, c0, sw_ltssm, sw_gr, sw_warm);
		wlan_note(7, "wake v%x e%u s%x c%x f%x", v1, e1, s1, c1, wsp.on ? wsp_rd(wsp_cb() + WSP_CB_FSR) : 0);
		/* cnss2 cnss_pci_dump_bl_sram_mem() for QCA6490 (LineageOS sm8350 kernel, reg.h) */
		wlan_bar = bar;
		wlan_window = ~0u;
		uint32_t stage = chip_rd(0x01B000F8u), sbl_start = chip_rd(0x01E0E238u), sbl_size = chip_rd(0x01E0E23Cu);
		uint32_t boot_cfg = chip_rd(0x01E22B34u), bootstrap = chip_rd(0x01910008u);
		printf("SWEET WLAN: TCSR_PBL_LOGGING %#x SBL log start %#x size %#x PBL_WLAN_BOOT_CFG %#x PBL_BOOTSTRAP_STATUS %#x\n",
		    stage, sbl_start, sbl_size, boot_cfg, bootstrap);
		for (uint32_t i = 0; i < 40; i += 4) {
			printf("SWEET WLAN: PBL SRAM[%#x] = %#x\n", 0x01403DA0u + i, chip_rd(0x01403DA0u + i));
		}
		if (sbl_start >= 0x01400000u && sbl_start + sbl_size <= 0x01800000u) {
			for (uint32_t i = 0; i < sbl_size && i < 48; i += 4) {
				printf("SWEET WLAN: SBL SRAM[%#x] = %#x\n", sbl_start + i, chip_rd(sbl_start + i));
			}
		}
		printf("SWEET WLAN: sr_status %#x:%#x wcss_status %#x:%#x q6 cookie %#x warm entry %#x reset cause %#x\n",
		    chip_rd(0x01E030C8u), chip_rd(0x01E030CCu), chip_rd(0x01E0313Cu), chip_rd(0x01E03140u),
		    chip_rd(0x01f80500u), chip_rd(0x1f80504u), chip_rd(0x01f8060cu));
	}
	if (wsp.taken && m.phase <= LemonadepMhiPhaseBhi) {
		wlan_note(6, "no doorbell; sid cfg %#x", wlan_sid_cfg);
		wlan_note(7, "smmu fsr %#x sgfsr %#x", wsp_rd(wsp_cb() + WSP_CB_FSR), wsp_rd(WSP_SGFSR));
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
