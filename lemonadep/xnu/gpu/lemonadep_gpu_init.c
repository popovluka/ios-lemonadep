/*
 * lemonadep: GPU bring-up, first on-device slice. Real register writes
 * only: CX GDSC + the seven GMU clock branches (lemonadep_gpu_reserve.c),
 * then the real GMU cold start (lemonadep_gmu_coldstart.c) -- both
 * already host-tested under ASan/UBSan (lemonadep/xnu/gpu/test_gpu_reserve.c,
 * test_gmu_coldstart.c).
 *
 * Deliberately stops there. The CP ring and CP_ME_INIT
 * (lemonadep_ring.c/lemonadep_cp_init.c, also already written and
 * host-tested -- lemonadep/xnu/gpu/test_gpu_boot.c) need the GPU/GMU SMMU
 * context actually programmed and real IOVA-backed memory
 * (lemonadep_gpu_memory.cpp) before submitting anything to the CP is
 * safe to try on real silicon -- an unmapped/untranslated ring address
 * reaching the real Adreno CP is not a risk to take without that in
 * place. Wiring that in is the next patch. See lemonadep/HANDOFF.md
 * "GPU, Phase 8" for the full writeup.
 *
 * Off by default (sweetgpu=N, N>=1, to enable): unlike touch/keys/usb,
 * every one of these register writes is unverified on real hardware.
 * N also selects how far GMU cold start is allowed to go before
 * stopping cleanly (lemonadep_gmu_cold_start()'s stop_after parameter),
 * so each further step can be tried on real hardware one at a time --
 * a different boot-arg value, no kernel rebuild -- instead of re-running
 * the whole untested tail again after a real on-device SoC-level fault
 * (XBL ramdump, not just a kernel panic) during it. See HANDOFF.md
 * "GPU, Phase 8 continued" for the incident this responds to:
 *   1: power reserve + PDC handshake only
 *   2: + firmware load (ITCM/DTCM writes)
 *   3: + fw_start's HFI/fence/chipid Core-window writes
 *   4: + the GBIF QoS writes (the only GPU-window writes in this file)
 *   5: + power_config
 *   6: + the GMU-start DTCM probe read (still before SYSRESET)
 *   7+: full run, including the SYSRESET toggle and ready-poll
 *
 * Firmware delivery: the real a660_gmu.bin bytes (gitignored, pulled
 * from the phone, never touch CI) are placed by tools/mkpayload.py's
 * --extra-blob option at a physical address reported via the
 * gmufw_pa/gmufw_bytes boot-args (lemonadep/HANDOFF.md "GPU, Phase 7
 * continued") -- read here via PE_parse_boot_argn + phystokv(), since
 * that address is inside the kernel's own managed RAM window, not a
 * device MMIO window (ml_io_map is for the latter).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t phystokv(uint64_t);

#include "lemonadep/LemonadepGmuSource.h"
#include "lemonadep/lemonadep_gpu_reserve.c"
#include "lemonadep/lemonadep_gmu_coldstart.c"

static int
lemonadep_pwr_read(void *cookie, unsigned window, uint32_t off, uint32_t *out)
{
	vm_offset_t *bases = (vm_offset_t *)cookie;
	*out = *(volatile uint32_t *)(bases[window] + off);
	return 0;
}
static int
lemonadep_pwr_write(void *cookie, unsigned window, uint32_t off, uint32_t value)
{
	vm_offset_t *bases = (vm_offset_t *)cookie;
	*(volatile uint32_t *)(bases[window] + off) = value;
	return 0;
}
static int
lemonadep_gmu_read(void *cookie, unsigned window, uint32_t off, uint32_t *out)
{
	vm_offset_t *bases = (vm_offset_t *)cookie;
	*out = *(volatile uint32_t *)(bases[window] + off);
	return 0;
}
static int
lemonadep_gmu_write(void *cookie, unsigned window, uint32_t off, uint32_t value)
{
	vm_offset_t *bases = (vm_offset_t *)cookie;
	*(volatile uint32_t *)(bases[window] + off) = value;
	return 0;
}
static int
lemonadep_barrier(void *cookie)
{
	(void)cookie;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}
static int
lemonadep_now_us(void *cookie, uint64_t *out)
{
	uint64_t ns;
	(void)cookie;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	*out = ns / 1000;
	return 0;
}
static int
lemonadep_wait_us(void *cookie, unsigned us)
{
	(void)cookie;
	delay((int)us);
	return 0;
}

void
lemonadep_gpu_init(void)
{
	uint32_t stage = 0;
	(void)PE_parse_boot_argn("sweetgpu", &stage, sizeof(stage));
	if (!stage) {
		printf("SWEET GPU: disabled (sweetgpu=N, N>=1, to enable; unverified on hardware)\n");
		return;
	}
	static const lemonadep_gmu_phase stage_stop[] = {
		LemonadepGmuPhaseNone, /* unused, stage 0 means disabled */
		LemonadepGmuPhasePdcHandshake,
		LemonadepGmuPhaseFirmwareLoad,
		LemonadepGmuPhaseFwStart,
		LemonadepGmuPhaseGbifCx,
		LemonadepGmuPhasePowerConfig,
		LemonadepGmuPhaseStartProbe,
	};
	const lemonadep_gmu_phase stop_after =
	    stage < sizeof(stage_stop) / sizeof(stage_stop[0]) ? stage_stop[stage] : LemonadepGmuPhaseNone;
	printf("SWEET GPU: stage=%u stop_after=%d\n", stage, stop_after);

	uint64_t fw_pa = 0;
	uint32_t fw_bytes = 0;
	if (!PE_parse_boot_argn("gmufw_pa", &fw_pa, sizeof(fw_pa)) ||
	    !PE_parse_boot_argn("gmufw_bytes", &fw_bytes, sizeof(fw_bytes))) {
		printf("SWEET GPU: no gmufw_pa/gmufw_bytes boot-args (mkpayload --extra-blob gmufw:... not used)\n");
		return;
	}
	if (fw_bytes != LEMONADEP_GMU_FW_BYTES) {
		printf("SWEET GPU: gmufw_bytes=0x%x != expected 0x%x\n", fw_bytes, LEMONADEP_GMU_FW_BYTES);
		return;
	}
	lemonadep_gmu_firmware fw = {
		.bytes = (const uint8_t *)phystokv(fw_pa),
		.length = fw_bytes,
	};
	printf("SWEET GPU: firmware at pa=0x%llx bytes=0x%x\n", fw_pa, fw_bytes);

	/* Power/clock windows (lemonadep_gpu_reserve.c's lemonadep_pwr_io). */
	vm_offset_t pwr_bases[LemonadepPwrWindowCount];
	pwr_bases[LemonadepPwrWindowGpucc] = ml_io_map(LEMONADEP_GPUCC_PA, LEMONADEP_GPUCC_BYTES);
	pwr_bases[LemonadepPwrWindowGcc] = ml_io_map(LEMONADEP_GCC_PA, LEMONADEP_GCC_GPU_WINDOW_BYTES);
	if (!pwr_bases[LemonadepPwrWindowGpucc] || !pwr_bases[LemonadepPwrWindowGcc]) {
		printf("SWEET GPU: ml_io_map failed (power windows)\n");
		return;
	}
	lemonadep_pwr_io pwr_io = {
		.cookie = pwr_bases, .read32 = lemonadep_pwr_read, .write32 = lemonadep_pwr_write,
		.now_us = lemonadep_now_us, .wait_us = lemonadep_wait_us,
	};
	lemonadep_pwr_status pwr_status = lemonadep_gpu_reserve(&pwr_io);
	printf("SWEET GPU: power/clock reserve status=%d\n", pwr_status);
	if (pwr_status != LemonadepPwrOk) {
		return;
	}

	/* GMU windows (lemonadep_gmu_coldstart.c's lemonadep_gmu_io). */
	vm_offset_t gmu_bases[LemonadepGmuWindowCount];
	gmu_bases[LemonadepGmuWindowCore] = ml_io_map(LEMONADEP_GMU_CORE_PA, LEMONADEP_GMU_CORE_BYTES);
	gmu_bases[LemonadepGmuWindowRscc] = ml_io_map(LEMONADEP_GMU_RSCC_PA, LEMONADEP_GMU_RSCC_BYTES);
	gmu_bases[LemonadepGmuWindowPdc] = ml_io_map(LEMONADEP_GMU_PDC_PA, LEMONADEP_GMU_PDC_BYTES);
	gmu_bases[LemonadepGmuWindowGpu] = ml_io_map(LEMONADEP_GMU_GPU_PA, LEMONADEP_GMU_GPU_BYTES);
	gmu_bases[LemonadepGmuWindowCxMisc] = ml_io_map(LEMONADEP_GMU_CX_MISC_PA, LEMONADEP_GMU_CX_MISC_BYTES);
	for (unsigned i = 0; i < LemonadepGmuWindowCount; i++) {
		if (!gmu_bases[i]) {
			printf("SWEET GPU: ml_io_map failed (GMU window %u)\n", i);
			return;
		}
	}
	lemonadep_gmu_io gmu_io = {
		.cookie = gmu_bases, .read32 = lemonadep_gmu_read, .write32 = lemonadep_gmu_write,
		.barrier = lemonadep_barrier, .now_us = lemonadep_now_us, .wait_us = lemonadep_wait_us,
	};

	/*
	 * hfi_iova/log_iova: real fixed IOVAs the GMU firmware expects
	 * (LemonadepGmuSource.h), but with no SMMU context programmed yet
	 * (not wired in this slice) they are not actually translated to
	 * anything -- fine for this slice: fw_start()/power_config() only
	 * program these as values the GMU reads out of its own registers
	 * once it runs, they are not dereferenced by this driver.
	 */
	const uint64_t log_iova = LEMONADEP_GMU_DEBUG_IOVA + LEMONADEP_GMU_DEBUG_BYTES;
	const uint64_t hfi_iova = log_iova + LEMONADEP_GMU_LOG_BYTES;
	lemonadep_gmu_phase gmu_phase = LemonadepGmuPhaseNone;
	lemonadep_gmu_status gmu_status = lemonadep_gmu_cold_start(&gmu_io, &fw, hfi_iova, log_iova,
	    LEMONADEP_GMU_LOG_BYTES, &gmu_phase, stop_after);
	printf("SWEET GPU: GMU cold start status=%d phase=%d\n", gmu_status, gmu_phase);
	if (gmu_status == LemonadepGmuOk && gmu_phase == LemonadepGmuPhaseDone) {
		printf("SWEET GPU: GMU reports ready. Stopping before CP ring/CP_ME_INIT "
		    "(needs SMMU context + real IOVA-backed memory, not yet wired)\n");
	} else if (gmu_status == LemonadepGmuOk) {
		printf("SWEET GPU: stopped cleanly as requested (sweetgpu=%u)\n", stage);
	}
	/*
	 * Pause here so these lines are readable on screen before boot
	 * continues and scrolls them away (wdt=-1/wdt_test=no_user_checkin
	 * already make the watchdog never fire, so this is safe).
	 */
	printf("SWEET GPU: pausing 8s so the lines above are readable\n");
	delay(8 * 1000 * 1000);
}
