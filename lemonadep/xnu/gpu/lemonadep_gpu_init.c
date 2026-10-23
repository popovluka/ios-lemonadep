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
 *   7: + GMU memory prep (HFI table, icache firmware copy into the
 *        reserved gpumem region) and a READ-ONLY Adreno SMMU probe, logged
 *   8: + program the GMU SMMU context, attach SIDs 5/0x405, map the
 *        icache/dummy/debug/log/hfi regions (still before SYSRESET)
 *   9+: full run, including the SYSRESET toggle and ready-poll
 * Stages 7+ need the reserved region: mkpayload --extra-blob gpumem:zero:SIZE
 * (lemonadep/m3-gpu-build.sh does it). See HANDOFF.md "GPU, Phase 9".
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
#include "lemonadep/lemonadep_gpu_smmu_map.c"
#include "lemonadep/lemonadep_gmu_mem.c"
#include "lemonadep/vendor/ArmSmmu4K/ArmSmmu4KMmio.h"

/* Clean [va, va+bytes) to the point of coherency. The Adreno SMMU is
 * dma-coherent, so this is belt and braces for tables and GMU memory the
 * CPU wrote through its cacheable mapping. Line size from CTR_EL0.DminLine. */
static void
lemonadep_clean_to_poc(const volatile void *va, size_t bytes)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 0xf);
	uintptr_t a = (uintptr_t)va & ~(line - 1);
	const uintptr_t end = (uintptr_t)va + bytes;
	for (; a < end; a += line) {
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}
static int
lemonadep_publish_table_page(void *cookie, volatile uint64_t *page, size_t bytes)
{
	(void)cookie;
	lemonadep_clean_to_poc(page, bytes);
	return 0;
}

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
		LemonadepGmuPhaseStartProbe, /* 7: + memory prep, read-only SMMU probe */
		LemonadepGmuPhaseStartProbe, /* 8: + GMU SMMU context programmed */
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

	/*
	 * Stages 7+: the GMU's real memory. Power first -- the SMMU sits in
	 * the CX GDSC domain and needs its vote clock, both enabled above.
	 */
	if (stage >= 7) {
		uint64_t gpumem_pa = 0;
		uint32_t gpumem_bytes = 0;
		if (!PE_parse_boot_argn("gpumem_pa", &gpumem_pa, sizeof(gpumem_pa)) ||
		    !PE_parse_boot_argn("gpumem_bytes", &gpumem_bytes, sizeof(gpumem_bytes)) ||
		    gpumem_bytes < LEMONADEP_GPUMEM_BYTES || (gpumem_pa & 4095)) {
			printf("SWEET GPU: stage %u needs gpumem_pa/gpumem_bytes >= 0x%x (mkpayload "
			    "--extra-blob gpumem:zero:SIZE)\n", stage, LEMONADEP_GPUMEM_BYTES);
			return;
		}
		uint8_t *gpumem = (uint8_t *)phystokv(gpumem_pa);
		printf("SWEET GPU: gpumem pa=0x%llx bytes=0x%x va=%p\n", gpumem_pa, gpumem_bytes, gpumem);

		lemonadep_hfi_table_init((volatile uint32_t *)(gpumem + LEMONADEP_GPUMEM_HFI_OFF),
		    (uint32_t)LEMONADEP_GMU_HFI_IOVA);
		uint32_t icache_bytes = lemonadep_gmu_icache_load(gpumem + LEMONADEP_GPUMEM_ICACHE_OFF,
		    fw.bytes, fw.length);
		lemonadep_clean_to_poc(gpumem + LEMONADEP_GPUMEM_ICACHE_OFF, LEMONADEP_GPUMEM_BYTES -
		    LEMONADEP_GPUMEM_ICACHE_OFF);
		printf("SWEET GPU: hfi table at iova 0x%x, icache firmware %u bytes\n",
		    (uint32_t)LEMONADEP_GMU_HFI_IOVA, icache_bytes);
		if (icache_bytes == 0) {
			return;
		}

		static ArmSmmu4KMmio smmu_mmio;
		static ArmSmmu4KIo smmu_io;
		smmu_mmio.mapping = (volatile unsigned char *)ml_io_map(LEMONADEP_ADRENO_SMMU_PA,
		    LEMONADEP_ADRENO_SMMU_BYTES);
		smmu_mmio.bytes = LEMONADEP_ADRENO_SMMU_BYTES;
		if (!smmu_mmio.mapping || ArmSmmu4KBindMmio(&smmu_mmio, &smmu_io) != ArmSmmu4KSuccess) {
			printf("SWEET GPU: SMMU map/bind failed\n");
			return;
		}
		static lemonadep_smmu_probe_info probe;
		if (lemonadep_smmu_probe(&smmu_io, &probe)) {
			printf("SWEET GPU: SMMU probe read failed\n");
			return;
		}
		printf("SWEET GPU: smmu scr0=0x%x id0=0x%x id1=0x%x id2=0x%x sgfsr=0x%x\n",
		    probe.scr0, probe.id0, probe.id1, probe.id2, probe.sgfsr);
		printf("SWEET GPU: smmu numcb=%u s2cb=%u smrg=%u sidbits=%u oas=%u global_ok=%u\n",
		    probe.numcb, probe.nums2cb, probe.numsmrg, probe.sid_bits, probe.output_bits, probe.global_ok);
		printf("SWEET GPU: smmu streams valid=%u trans=%u bypass=%u fault=%u banks_on=%u "
		    "-> bank=%d slots=%u,%u (%u free)\n", probe.smr_valid, probe.s2cr_trans,
		    probe.s2cr_bypass, probe.s2cr_fault, probe.banks_enabled, (int)probe.bank,
		    probe.slots[0], probe.slots[1], probe.free_slots);

		if (stage >= 8) {
			static lemonadep_gmu_smmu gmu_smmu;
			const ArmSmmu4KLiveVisibility vis = {.cookie = NULL,
			    .publish_table_page = lemonadep_publish_table_page};
			ArmSmmu4KStatus smmu_status = lemonadep_gmu_smmu_setup(&gmu_smmu, &smmu_io, &probe,
			    gpumem, gpumem_pa, &vis);
			printf("SWEET GPU: GMU SMMU setup status=%d step=%u sids=%u tables=%u leaves=%u "
			    "program_phase=%u fsr=0x%x attach_phase=%u\n", smmu_status, gmu_smmu.step,
			    gmu_smmu.sid_count, (unsigned)gmu_smmu.state.tables.table_pages,
			    (unsigned)gmu_smmu.state.tables.leaf_pages, gmu_smmu.program.phase,
			    gmu_smmu.program.fault_status, gmu_smmu.routes.phase);
			if (smmu_status != ArmSmmu4KSuccess) {
				/* Never release the GMU into unmapped memory. */
				printf("SWEET GPU: stopping before GMU cold start (SMMU not ready)\n");
				delay(8 * 1000 * 1000);
				return;
			}
		}
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
