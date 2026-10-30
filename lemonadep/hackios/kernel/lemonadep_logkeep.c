/*
 * lemonadep under HackiOS: a run's log that outlives the run, and a run that ends in fastboot.
 *
 * On this phone the loader loses its USB link the moment a kernel starts (2026-10-30, six runs,
 * with every driver set), so a run's log never comes back the way xl expects. With boot-arg
 * logkeep=1:
 *   - a thread copies the kernel message buffer (printf, IOLog) every 500 ms into RAM at
 *     LK_PA, the start of the last 2 MB of the loader's payload region, which UEFI keeps for us
 *     and nothing writes before the loader is asked for it;
 *   - at the end of the run (loader_return(): the time is up, or a panic) it is copied once
 *     more, the reboot reason "bootloader" goes into the PMIC and PSCI resets the phone
 *     (lemonadep_reboot.c, from ios-sweet overlay 1010, seen working on 2026-10-24);
 *   - RAM survives the warm reset: the next loader hands it out with "oem logkeep" and
 *     "fastboot get_staged" (xnuloader/loader.c).
 * So a test run needs nobody at the phone, and a hard crash still leaves what was said up to
 * the last copy.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/msgbuf.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void lemonadep_reboot_arm(void);
extern void lemonadep_reboot_now(void) __attribute__((noreturn));
extern char loader_log[];
extern uint32_t loader_log_len;

#define LK_PA           0xF3E00000ull
#define LK_SIZE         0x1e0000u       /* up to the display snapshots at 0xF3FE0000 */
#define LK_MAGIC        0x50454b4cu     /* "LKEP" */
#define LK_VM_WIMG_IO   0x7u

extern void IOSleep(unsigned int milliseconds);
extern void redwood_dpu_restore(void);  /* the boot framebuffer back on the panel, with the run's notes */

static volatile uint8_t *lk;
static bool lk_on;
static uint32_t lk_ticks;
static uint32_t lk_word;        /* the bytes of the word being filled, carried between calls */

/* 32-bit stores only: the window is device memory, where a wider or unaligned store may fault */
static uint32_t
lk_put(uint32_t at, const char *s, uint32_t n)
{
	volatile uint32_t *w = (volatile uint32_t *)(lk + 16);

	for (uint32_t i = 0; i < n && at < LK_SIZE - 20; i++, at++) {
		lk_word = (at & 3) ? lk_word : 0;
		lk_word |= (uint32_t)(uint8_t)s[i] << (8 * (at & 3));
		w[at / 4] = lk_word;    /* each word written as it fills: a partial last word is there too */
	}
	return at;
}

static void
lk_copy(const char *trailer)
{
	struct msgbuf *mb = msgbufp;
	volatile uint32_t *h = (volatile uint32_t *)lk;
	uint32_t at = 0;
	char line[96];

	if (!lk_on || mb == NULL || mb->msg_bufc == NULL || mb->msg_size <= 0 || mb->msg_bufx < 0 ||
	    mb->msg_bufx > mb->msg_size) {
		return;
	}
	h[0] = 0;
	snprintf(line, sizeof(line), "== logkeep copy %u%s ==\n", lk_ticks, trailer);
	at = lk_put(at, line, (uint32_t)strlen(line));
	/* oldest first: [bufx, size) has text only once the buffer has wrapped; zeros are skipped */
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? mb->msg_bufx : 0, to = pass == 0 ? mb->msg_size : mb->msg_bufx;
		for (int i = from; i < to; i++) {
			if (mb->msg_bufc[i] != 0) {
				at = lk_put(at, &mb->msg_bufc[i], 1);
			}
		}
	}
	h[1] = at;
	__asm__ volatile ("dsb sy" ::: "memory");
	h[0] = LK_MAGIC;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
lk_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		lk_ticks++;
		lk_copy("");
		IOSleep(500);
	}
}

void lemonadep_logkeep_init(void);
void lemonadep_logkeep_return(void);

void
lemonadep_logkeep_init(void)
{
	uint32_t on = 0;
	thread_t t;

	if (!PE_parse_boot_argn("logkeep", &on, sizeof(on)) || !on) {
		return;
	}
	lk = (volatile uint8_t *)ml_io_map_unmappable(LK_PA, LK_SIZE, LK_VM_WIMG_IO);
	if (lk == NULL) {
		printf("logkeep: cannot map %#llx\n", LK_PA);
		return;
	}
	lemonadep_reboot_arm();         /* maps the PMIC arbiter now; panics restart into fastboot too */
	lk_on = true;
	printf("logkeep: the message buffer is copied to %#llx every 500 ms; the run ends in fastboot\n", LK_PA);
	if (kernel_thread_start(lk_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}

/* From loader_return(), with the other processors stopped: the last copy, then into fastboot. */
void
lemonadep_logkeep_return(void)
{
	if (!lk_on) {
		return;
	}
	printf("logkeep: run over, last copy, then the reset into fastboot\n");
	lk_copy(", the last: run over");
	/*
	 * The RAM copy did not survive the reset on 2026-10-30 (twice; with PSCI SYSTEM_RESET2 warm
	 * too), so the run's notes (redwood_dpu_note()) stay on the panel for logkeep_secs (30) to be
	 * photographed before the reset.
	 */
	uint32_t secs = 30;
	(void)PE_parse_boot_argn("logkeep_secs", &secs, sizeof(secs));
	redwood_dpu_restore();
	delay((int)(secs * 1000000u));
	lemonadep_reboot_now();
}
