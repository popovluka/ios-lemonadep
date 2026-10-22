/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "display.h"
#include "fdt.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"

#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICACTIVER0		(GICR_SGI + 0x380)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	if (PSHOLD)
		wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
#if BOARD_EL1_PTIMER
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
#endif
	wsr("icc_eoir1_el1", iar);
	isb();
}

/*
 * Fault-tolerant MMIO probing: while probe_armed, a data abort from the
 * current EL (EC 0x25) is counted and the faulting instruction skipped
 * instead of ending in the panic screen. Used to map which GIC registers
 * the lemonadep firmware lets EL1 write.
 */
static volatile int probe_armed;
static volatile uint32_t probe_faults, probe_esr;

static int try_wr32(uintptr_t a, uint32_t v)
{
	probe_faults = 0;
	probe_armed = 1;
	__asm__ volatile("dsb sy; isb" ::: "memory");
	wr32(a, v);
	__asm__ volatile("dsb sy; isb" ::: "memory");
	probe_armed = 0;
	return probe_faults != 0;
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	if (slot == 4 && probe_armed && (rsr("esr_el1") >> 26) == 0x25) {
		probe_faults++;
		probe_esr = (uint32_t)rsr("esr_el1");
		wsr("elr_el1", rsr("elr_el1") + 4);	/* skip the faulting access */
		return;
	}
#if BOARD_HAS_MDSS
	/* Keep the page on screen: its last lines are what led here. */
	con_skip();
#else
	con_clear(BLUE);
#endif
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			/* 32-bit only: SM8350 (lemonadep) aborts byte writes to GICR */
			wr32(r + GICR_IPRIORITYR + 16, (rd32(r + GICR_IPRIORITYR + 16) & 0x00ffffffU) | 0xa0000000U);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	}
#if BOARD_EL1_PTIMER
	else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
#endif
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
#if BOARD_EL1_PTIMER
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
#else
	ist = rsr("cntv_ctl_el0");
#endif
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
#if BOARD_EL1_PTIMER
	wsr("cntp_ctl_el0", 0);
#endif
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	/*
	 * lemonadep: writes to GICR_IPRIORITYR0 (SGIs 0-3) took a synchronous
	 * external abort, while Linux on the same phone writes all eight
	 * words after disabling and deactivating SGIs/PPIs. Map it: every
	 * write below is fault-tolerant; a bit set in a mask = that word faulted.
	 */
	con_hex2("GICR", "ctlr=", rd32(r + 0x0), "iidr=", rd32(r + 0x4));
	con_hex2("GICR", "pwrr=", rd32(r + 0x24), "typer=", (uint32_t)rd64(r + GICR_TYPER));
	con_hex2("SGIPPI", "en=", rd32(r + GICR_ISENABLER0), "act=", rd32(r + GICR_ISACTIVER0));
	uint32_t grp_fault = try_wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	con_hex2("IGROUPR", "now=", rd32(r + GICR_IGROUPR0), "fault=", grp_fault);
	uint32_t pa = 0, pb = 0, esr = 0;
	for (int w = 0; w < 8; w++) {
		if (try_wr32(r + GICR_IPRIORITYR + 4 * w, 0xa0a0a0a0U)) {
			pa |= 1u << w;
			esr = probe_esr;
		}
	}
	con_hex2("PRIO A", "fault=", pa, "esr=", esr);	/* as XNU 0004 does it */
	uint32_t cf = try_wr32(r + GICR_ICACTIVER0, 0xffffffff) | try_wr32(r + GICR_ICENABLER0, 0xffffffff) << 1;
	for (int w = 0; w < 8; w++)
		if (try_wr32(r + GICR_IPRIORITYR + 4 * w, 0xa0a0a0a0U))
			pb |= 1u << w;
	con_hex2("PRIO B", "fault=", pb, "clr=", cf);	/* after Linux's disable + deactivate */
	con_hex2("PRIO", "w0=", rd32(r + GICR_IPRIORITYR), "w6=", rd32(r + GICR_IPRIORITYR + 24));
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t ef = try_wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0) | (ef ? 0x80000000u : 0));

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	(void)try_wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

#if BOARD_HAS_MDSS
extern char _head[];

/* Value of "key" (up to a space) in /chosen/bootargs, truncated to fit a screen line. */
static void bootarg(uintptr_t dtb, const char *key, char *out, int max)
{
	uintptr_t node, v;
	uint32_t len;
	out[0] = 0;
	if (!fdt_node(dtb, "/chosen", &node) || !fdt_prop(dtb, node, "bootargs", &v, &len))
		return;
	const char *s = (const char *)v;
	for (uint32_t i = 0; i < len; i++) {
		int k = 0;
		while (key[k] && i + k < len && s[i + k] == key[k])
			k++;
		if (key[k] || (i && s[i - 1] != ' '))
			continue;
		int n = 0;
		for (uint32_t j = i + k; j < len && s[j] && s[j] != ' ' && n < max - 1; j++)
			out[n++] = s[j];
		out[n] = 0;
		return;
	}
}

static void hold(unsigned s)
{
	con_skip();
	con_hex2("NEXT", "page in s=", s, 0, 0);
	delay_ms(s * 1000);
}

/*
 * lemonadep milestone 1: adopt ABL's splash plane (display.c), then put
 * every fact the port still needs from the device on screen (GAPS.md).
 */
static void board_early(uintptr_t dtb)
{
	uint64_t lo = FB_BASE, size = FB_SIZE;
	uintptr_t node;
	int live = fdt_node(dtb, "/reserved-memory/splash_region", &node) &&
		   fdt_reg(dtb, node, 2, 2, 0, &lo, &size);
	struct disp_info d;

	if (display_discover(lo, lo + size, &d)) {
		con_setup(d.addr, d.w, d.h, d.stride, d.bpp);
		con_kick = d.kick;
	} else {
		/* No plane found: paint the whole region white (any format) as a sign of life. */
		for (uint64_t a = lo; a < lo + size; a += 8)
			*(volatile uint64_t *)a = ~0ULL;
		con_setup(lo, 0, 0, 0, 4);
	}
	con_clear(BLACK);
	con_str("SHIM", "lemonadep milestone 1");
	con_hex2("EL", "cur=", rsr("CurrentEL") >> 2, "img=", (uintptr_t)_head);
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("SPLASH", live ? "dt=" : "def=", (uint32_t)lo, "size=", (uint32_t)size);
	con_hex2("DISP", "ctl=", d.ctl, "pipe=", d.pipe);
	con_hex2(" ctl", "intf=", d.intf_active, "fetch=", d.fetch_active);
	con_hex2(" pipe", "addr=", d.addr, "n=", d.npipes);
	con_hex2(" pipe", "size=", d.src_size, "xy=", d.src_xy);
	con_hex2(" pipe", "ystr=", d.ystride0, "fmt=", d.format);
	con_hex2(" pipe", "unpk=", d.unpack, 0, 0);
	con_hex2("GEOM", "w=", d.w, "h=", d.h);
	con_hex2("GEOM", "str=", d.stride, "bpp=", d.bpp);
	con_hex2("DISP TE", "en=", d.tear_check, "kick=", (uint32_t)d.kick);
	con_hex2("DISP AR", "was=", d.autorefresh_before, "now=", d.autorefresh_after);
	if (fdt_node(dtb, "/memory", &node)) {
		uint64_t a, s;
		for (int i = 0; i < 4 && fdt_reg(dtb, node, 2, 2, i, &a, &s); i++) {
			con_hex2("MEM", "base=", (uint32_t)(a >> 32), "lo=", (uint32_t)a);
			con_hex2("MEM", "size=", (uint32_t)(s >> 32), "lo=", (uint32_t)s);
		}
	} else {
		con_str("MEM", "no /memory");
	}
	char panel[40];
	bootarg(dtb, "msm_drm.dsi_display0=", panel, sizeof(panel));
	con_str("PANEL", panel[0] ? panel : "(none)");
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
}
#endif

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
#if BOARD_HAS_MDSS
	board_early(dtb);
#else
	con_clear(RED);
	con_str("SHIM", "sweet milestone 4: xnu loader");
	con_skip();
#endif
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();
#if BOARD_HAS_MDSS
	/* No payload: the board facts above stay readable for a while, then GIC/SMP. */
	hold(30);
	con_clear(BLACK);
	test_gic();
	con_skip();
	test_smp();
	hold(30);
	con_clear(BLACK);
#endif

	/* No payload: probe the internal UFS while the MMU is still off. */
	stage = "ufs";
	test_ufs();
	con_skip();
	/* Keep the probe results on screen (later tests clear it); reset after 3 min. */
	con_str("UFS", "probe done, reset in ~3 min");
	delay_ms(180000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
