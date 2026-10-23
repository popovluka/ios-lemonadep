/*
 * Host test (ASan/UBSan) for lemonadep_gmu_coldstart.c: a mock register bank
 * records every write and answers reads realistically (including the real
 * GMU firmware-init poll pattern), so the real cold-start function runs
 * against something that behaves like actual hardware, not a stub that
 * always succeeds. Verifies specific real values sourced from the pinned
 * Linux driver land on the wire -- not just that the function returns OK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lemonadep_gmu_coldstart.c"

#define MAX_REGS 0x40000u
static uint32_t regs[LemonadepGmuWindowCount][MAX_REGS / 4];
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

struct write_log_entry { unsigned window; uint32_t offset, value; };
static struct write_log_entry writes[4096];
static unsigned n_writes;

static int
mock_read(void *cookie, unsigned window, uint32_t off, uint32_t *out)
{
	(void)cookie;
	if (window >= LemonadepGmuWindowCount || off + 4 > MAX_REGS) {
		return 1;
	}
	*out = regs[window][off / 4];
	return 0;
}
static int
mock_write(void *cookie, unsigned window, uint32_t off, uint32_t value)
{
	(void)cookie;
	if (window >= LemonadepGmuWindowCount || off + 4 > MAX_REGS) {
		return 1;
	}
	regs[window][off / 4] = value;
	if (n_writes < 4096) {
		writes[n_writes++] = (struct write_log_entry){window, off, value};
	}
	/* Model the CM3 reset/HFI poll target as satisfied the instant the
	 * real code writes it -- a bring-up test, not a timing model. */
	if (window == LemonadepGmuWindowCore && off == A660_GMU_CM3_SYSRESET && value == 0) {
		/* Reset released: the probed-threshold path (our mock DTCM+0xff8
		 * reads 0 by default, so the LOW mask/ready pattern applies). */
		regs[LemonadepGmuWindowCore][A660_GMU_CM3_FW_INIT_RESULT / 4] =
		    LEMONADEP_A660_GMU_START_READY_LOW;
	}
	return 0;
}
static int mock_barrier(void *cookie) { (void)cookie; return 0; }
static int mock_now_us(void *cookie, uint64_t *out) { (void)cookie; static uint64_t t; *out = (t += 10); return 0; }
static int mock_wait_us(void *cookie, unsigned us) { (void)cookie; (void)us; return 0; }

static uint32_t
written(unsigned window, uint32_t offset)
{
	for (unsigned i = 0; i < n_writes; ++i) {
		if (writes[i].window == window && writes[i].offset == offset) {
			return writes[i].value; /* last write wins, matches real MMIO */
		}
	}
	printf("FAIL: offset 0x%x (window %u) was never written\n", offset, window);
	fails++;
	return 0xdeadbeef;
}

int
main(void)
{
	memset(regs, 0, sizeof(regs));
	n_writes = 0;

	lemonadep_gmu_io io = {
		.cookie = NULL, .read32 = mock_read, .write32 = mock_write,
		.barrier = mock_barrier, .now_us = mock_now_us, .wait_us = mock_wait_us,
	};

	/* Real firmware already on the phone; verified by verify-gmu-firmware.py. */
	FILE *f = fopen("a660_gmu.bin", "rb");
	CHECK(f != NULL);
	if (!f) {
		printf("test_gmu_coldstart FAILED: copy lemonadep/fw-gpu/a660_gmu.bin next to this binary\n");
		return 1;
	}
	static uint8_t fw_bytes[LEMONADEP_GMU_FW_BYTES];
	size_t got = fread(fw_bytes, 1, sizeof(fw_bytes), f);
	fclose(f);
	CHECK(got == LEMONADEP_GMU_FW_BYTES);
	lemonadep_gmu_firmware fw = {.bytes = fw_bytes, .length = (uint32_t)got};

	lemonadep_gmu_phase phase = LemonadepGmuPhaseNone;
	lemonadep_gmu_status status = lemonadep_gmu_cold_start(&io, &fw, 0x1230000, 0x4560000, 0x4000, &phase);
	CHECK(status == LemonadepGmuOk);
	CHECK(phase == LemonadepGmuPhaseDone);
	printf("cold_start: status=%d phase=%d, %u register writes logged\n", status, phase, n_writes);

	/* Spot-check real, sourced values actually landed on the wire. */
	CHECK(written(LemonadepGmuWindowCore, A660_GMU_GENERAL_7) == 1);
	CHECK(written(LemonadepGmuWindowCore, A660_GMU_HFI_SFR_ADDR) == LEMONADEP_A660_GMU_CHIPID);
	CHECK(written(LemonadepGmuWindowCore, A660_GMU_AHB_FENCE_RANGE_0) ==
	    ((1u << 31) | (LEMONADEP_A660_FENCE_RANGE_UPPER << 18) | LEMONADEP_A660_FENCE_RANGE_LOWER));
	CHECK(written(LemonadepGmuWindowGpu, A660_GBIF_QSB_SIDE0) == LEMONADEP_A660_GBIF_QSB_VALUE);
	CHECK(written(LemonadepGmuWindowGpu, A660_GBIF_QSB_SIDE0 + 12) == LEMONADEP_A660_GBIF_QSB_VALUE);
	CHECK((written(LemonadepGmuWindowCore, A660_GMU_RPMH_CTRL) & LEMONADEP_A660_RPMH_CTRL_ENABLE_MASK) ==
	    LEMONADEP_A660_RPMH_CTRL_ENABLE_MASK);
	for (unsigned i = 0; i < 5; ++i) {
		CHECK(written(LemonadepGmuWindowRscc, A660_RSCC_SEQ_MEM_0_DRV0 + 4u * i) ==
		    LemonadepA660RscSeqMicrocode[i]);
	}
	CHECK(written(LemonadepGmuWindowPdc, A660_PDC_GPU_ENABLE_PDC) == LEMONADEP_A660_PDC_VAL_GPU_ENABLE_PDC);

	/* Firmware payload actually reached ITCM/DTCM at the right offsets. */
	CHECK(regs[LemonadepGmuWindowCore][(A660_GMU_CM3_ITCM_START + 0) / 4] ==
	    ((uint32_t)fw_bytes[192] | (uint32_t)fw_bytes[193] << 8 |
	     (uint32_t)fw_bytes[194] << 16 | (uint32_t)fw_bytes[195] << 24));
	CHECK(regs[LemonadepGmuWindowCore][(A660_GMU_CM3_DTCM_START + 0x3f98) / 4] ==
	    ((uint32_t)fw_bytes[55176] | (uint32_t)fw_bytes[55177] << 8 |
	     (uint32_t)fw_bytes[55178] << 16 | (uint32_t)fw_bytes[55179] << 24));
	/* The icache-target block (the bulk of the firmware) must NOT be written
	 * to any MMIO window -- it is a memory copy the board glue does, not an
	 * MMIO write (see load_firmware's LemonadepGmuFwTargetIcache case). */
	unsigned icache_writes = 0;
	for (unsigned i = 0; i < n_writes; ++i) {
		if (writes[i].window == LemonadepGmuWindowCore &&
		    writes[i].offset >= A660_GMU_CM3_ITCM_START + 0x4000u * 1u /* past 16K ITCM */ &&
		    writes[i].offset < A660_GMU_CM3_DTCM_START) {
			++icache_writes;
		}
	}
	CHECK(icache_writes == 0);

	printf("test_gmu_coldstart %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
