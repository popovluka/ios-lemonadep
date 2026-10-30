/*
 * lemonadep: timed reset into fastboot (or recovery) so test boots come back
 * by themselves, and panics/restarts land there too instead of hanging.
 *
 * Boot-arg sweetreboot=<seconds>[,recovery]; absent = off, nothing changes.
 * The deadline is polled from the persistent-log thread (sweet_ufs.c), not a
 * sleeping thread of its own: PixeliOS found a dedicated sleeping thread
 * "never fired on hardware in 3 of 4 runs" (pixelios-kernel 0083/0086). The
 * first poll past the deadline only logs, so that line reaches the disk with
 * that tick's flush; the next poll resets.
 *
 * Reset sequence: lemonadep/reboot/lemonadep_reboot_core.c (vendor-sourced,
 * host-tested). Everything is mapped when armed, so the panic path maps
 * nothing and takes no locks.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>

#include "lemonadep/reboot/lemonadep_reboot_core.c"

/* kernel_map, not the 8 MiB io_submap (see bsd/dev/lemonadep_wlan.c) */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
#define LR_VM_WIMG_IO 0x7u
extern int (*PE_halt_restart)(unsigned int type);

static vm_offset_t lr_win[LrWinCount];
static uint32_t lr_chnls_base;          /* LR_CHAN_RW(apid): the one page mapped */
static bool lr_recovery, lr_armed, lr_announced;
static uint64_t lr_deadline;            /* mach absolute time */

static uint32_t
lr_read32(void *c, unsigned w, uint32_t off)
{
	(void)c;
	if (w == LrWinChnls) {
		off -= lr_chnls_base;
	}
	uint32_t v = *(volatile uint32_t *)(lr_win[w] + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}
static void
lr_write32(void *c, unsigned w, uint32_t off, uint32_t v)
{
	(void)c;
	if (w == LrWinChnls) {
		off -= lr_chnls_base;
	}
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(lr_win[w] + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static void
lr_delay(void *c, unsigned us)
{
	(void)c;
	delay((int)us);
}

static void __attribute__((noreturn))
lr_reset(void)
{
	const lemonadep_reboot_io io = {.read32 = lr_read32, .write32 = lr_write32, .delay_us = lr_delay};
	lemonadep_reboot_result r;
	int e = lemonadep_reboot_prepare(&io, lr_recovery, &r);
	printf("SWEET REBOOT: %s: sdam apid=%d owner=%u %#x -> %#x status=%#x (%d), PSCI SYSTEM_RESET\n",
	    lr_recovery ? "recovery" : "bootloader", r.apid, r.owner, r.old, r.new_, r.status, e);
	/*
	 * HackiOS (logkeep=1): a WARM reset, so that RAM -- the kept log -- survives it: PSCI 1.1
	 * SYSTEM_RESET2 with type 0 (SYSTEM_WARM_RESET), as Linux's psci_sys_reset2() for
	 * reboot_mode=warm and the vendor's "reboot bootloader" (msm-poweroff.c: a reason means a
	 * warm reset). Asked for with PSCI_FEATURES first; plain SYSTEM_RESET (cold) otherwise.
	 */
	{
		register uint64_t f0 __asm__("x0") = 0x8400000aull;     /* PSCI_FEATURES */
		register uint64_t f1 __asm__("x1") = 0x84000012ull;     /* SYSTEM_RESET2 (SMC32) */
		__asm__ volatile("smc #0" : "+r"(f0), "+r"(f1) : : "x2", "x3", "memory");
		if ((int32_t)f0 >= 0) {
			register uint64_t w0 __asm__("x0") = 0x84000012ull;
			register uint64_t w1 __asm__("x1") = 0;           /* SYSTEM_WARM_RESET */
			register uint64_t w2 __asm__("x2") = 0;
			__asm__ volatile("smc #0" : "+r"(w0), "+r"(w1), "+r"(w2) : : "x3", "memory");
		}
	}
	register uint64_t x0 __asm__("x0") = LR_PSCI_SYSTEM_RESET;
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
	for (;;) {
		__asm__ volatile("wfe");
	}
}

/* IOPlatformExpert.h: kPERestartCPU 1, kPEPanicRestartCPU 4, kPEPanicRestartCPUNoCallouts 9 */
static int
lr_halt_restart(unsigned int type)
{
	if (type == 1 || type == 4 || type == 9) {
		lr_reset();
	}
	return -1;
}

void lemonadep_reboot_arm(void);
void lemonadep_reboot_poll(void);
void lemonadep_reboot_now(void) __attribute__((noreturn));

/* HackiOS (lemonadep_logkeep.c): reset into fastboot now; lemonadep_reboot_arm() has mapped everything */
void
lemonadep_reboot_now(void)
{
	lr_recovery = false;
	lr_reset();
}

void
lemonadep_reboot_arm(void)
{
	char arg[32] = {0};
	uint32_t logkeep = 0;
	if (!lr_armed && PE_parse_boot_argn("logkeep", &logkeep, sizeof(logkeep)) && logkeep &&
	    !PE_parse_boot_arg_str("sweetreboot", arg, sizeof(arg))) {
		strlcpy(arg, "999999", sizeof(arg));    /* HackiOS: mapped now, the reset comes from loader_return() */
	}
	if (lr_armed || (!arg[0] && (!PE_parse_boot_arg_str("sweetreboot", arg, sizeof(arg)) || !arg[0]))) {
		return;
	}
	uint32_t secs = 0;
	const char *p = arg;
	for (; *p >= '0' && *p <= '9'; ++p) {
		secs = secs * 10u + (uint32_t)(*p - '0');
	}
	lr_recovery = *p == ',' && !strncmp(p + 1, "recovery", 8);
	if (secs == 0) {
		return;
	}
	static const struct { uint32_t pa, bytes; } maps[LrWinCount] = {
		[LrWinCore] = {LR_SPMI_CORE_PA, 0x2000u},
		[LrWinObs] = {LR_SPMI_OBS_PA, 0x10000u},
		[LrWinCnfg] = {LR_SPMI_CNFG_PA, 0x1000u},
		[LrWinImem] = {LR_IMEM_PA, 0x1000u},
	};
	for (unsigned i = 0; i < LrWinCount; ++i) {
		if (i != LrWinChnls && !(lr_win[i] = ml_io_map_unmappable(maps[i].pa, maps[i].bytes, LR_VM_WIMG_IO))) {
			printf("SWEET REBOOT: map %#x failed, off\n", maps[i].pa);
			return;
		}
	}
	const lemonadep_reboot_io io = {.read32 = lr_read32, .write32 = lr_write32, .delay_us = lr_delay};
	uint32_t owner = 0;
	int apid = lr_find_apid(&io, (uint16_t)(LR_SDAM_SID << 8 | (LR_SDAM_ADDR >> 8)), &owner);
	if (apid >= 0 && owner == LR_SPMI_EE) {
		lr_chnls_base = LR_CHAN_RW((uint32_t)apid);
		lr_win[LrWinChnls] = ml_io_map_unmappable(LR_SPMI_CHNLS_PA + lr_chnls_base, 0x1000u, LR_VM_WIMG_IO);
	}
	printf("SWEET REBOOT: to %s in %u s (sdam apid=%d owner=%u%s)\n", lr_recovery ? "recovery" : "fastboot",
	    secs, apid, owner, apid >= 0 && owner == LR_SPMI_EE ? "" : ": NOT writable, reason will be missing");
	if (apid >= 0 && owner == LR_SPMI_EE && !lr_win[LrWinChnls]) {
		return;
	}
	uint64_t span;
	nanoseconds_to_absolutetime((uint64_t)secs * NSEC_PER_SEC, &span);
	lr_deadline = mach_absolute_time() + span;
	PE_halt_restart = lr_halt_restart;
	lr_armed = true;
}

void
lemonadep_reboot_poll(void)
{
	if (!lr_armed || mach_absolute_time() < lr_deadline) {
		return;
	}
	if (!lr_announced) {
		lr_announced = true;
		printf("SWEET REBOOT: time is up, resetting at the next log tick\n");
		return;
	}
	lr_reset();
}
