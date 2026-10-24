/*
 * Host test (ASan/UBSan) for lemonadep_reboot_core.c against a mock PMIC
 * arbiter: PPID 0x071 is mapped twice (APID 5 owned by another EE, APID 9
 * by EE 0); a read through the observer channel returns the SDAM byte; a
 * write through the owner channel lands only with the v5 command format,
 * opcode EXT_WRITEL and the SDAM register's low byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lemonadep_reboot_core.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t core[0x2000 / 4], cnfg[0x1000 / 4], imem[0x1000 / 4];
static uint32_t obs_status, chn_status, rdata, wdata, sdam = 0x01; /* bit 0 belongs to someone else */
static uint32_t obs_cmd_seen, chn_cmd_seen;
static int write_apid = -1;
static bool deny;

static uint32_t
rd(void *c, unsigned w, uint32_t off)
{
	(void)c;
	switch (w) {
	case LrWinCore: return core[off / 4];
	case LrWinCnfg: return cnfg[off / 4];
	case LrWinImem: return imem[off / 4];
	case LrWinObs:
		if (off % 0x80 == LR_ARB_STATUS) return obs_status;
		if (off % 0x80 == LR_ARB_RDATA0) return rdata;
		return 0;
	case LrWinChnls:
		if (off % 0x10000 == LR_ARB_STATUS) return chn_status;
		return 0;
	}
	return 0;
}
static void
wr(void *c, unsigned w, uint32_t off, uint32_t v)
{
	(void)c;
	if (w == LrWinImem) { imem[off / 4] = v; return; }
	if (w == LrWinObs && off % 0x80 == LR_ARB_CMD) {
		obs_cmd_seen = v;
		if (off / 0x80 == 9 && v == (LR_OP_EXT_READL << 27 | 0x48u << 4)) {
			rdata = sdam;
			obs_status = LR_ARB_DONE;
		} else {
			obs_status = LR_ARB_DONE | LR_ARB_FAILURE;
		}
		return;
	}
	if (w == LrWinChnls && off % 0x10000 == LR_ARB_WDATA0) { wdata = v; return; }
	if (w == LrWinChnls && off % 0x10000 == LR_ARB_CMD) {
		chn_cmd_seen = v;
		write_apid = (int)(off / 0x10000);
		if (deny) { chn_status = LR_ARB_DONE | LR_ARB_DENIED; return; }
		if (write_apid == 9 && v == (LR_OP_EXT_WRITEL << 27 | 0x48u << 4)) {
			sdam = wdata & 0xff;
			chn_status = LR_ARB_DONE;
		} else {
			chn_status = LR_ARB_DONE | LR_ARB_FAILURE;
		}
	}
}
static void dl(void *c, unsigned us) { (void)c; (void)us; }

static void
reset(void)
{
	memset(core, 0, sizeof(core)); memset(cnfg, 0, sizeof(cnfg)); memset(imem, 0, sizeof(imem));
	core[LR_APID_MAP(5) / 4] = 0x071u << 8; cnfg[LR_OWNERSHIP(5) / 4] = 3;   /* another EE */
	core[LR_APID_MAP(9) / 4] = 0x071u << 8; cnfg[LR_OWNERSHIP(9) / 4] = 0;   /* apps */
	core[LR_APID_MAP(2) / 4] = 0x013u << 8;                                  /* PON, unrelated */
	sdam = 0x01; obs_status = chn_status = 0; deny = false; write_apid = -1;
}

int
main(void)
{
	const lemonadep_reboot_io io = {.read32 = rd, .write32 = wr, .delay_us = dl};
	lemonadep_reboot_result r;

	reset();
	int e = lemonadep_reboot_prepare(&io, false, &r);
	printf("bootloader: e=%d apid=%d owner=%u %#x -> %#x imem=%#x\n", e, r.apid, r.owner, r.old, r.new_,
	    imem[LR_IMEM_RESTART / 4]);
	CHECK(e == 0 && r.apid == 9 && write_apid == 9);
	CHECK(sdam == (0x01u | 0x02u << 1));                 /* reason in [7:1], bit 0 kept */
	CHECK(imem[LR_IMEM_RESTART / 4] == 0x77665500u);

	reset();
	e = lemonadep_reboot_prepare(&io, true, &r);
	CHECK(e == 0 && sdam == (0x01u | 0x01u << 1) && imem[LR_IMEM_RESTART / 4] == 0x77665502u);

	/* only a foreign-EE mapping: no SPMI write attempted, IMEM still set */
	reset();
	core[LR_APID_MAP(9) / 4] = 0;
	e = lemonadep_reboot_prepare(&io, false, &r);
	CHECK(e == -1 && r.apid == 5 && r.owner == 3 && write_apid == -1 && imem[LR_IMEM_RESTART / 4] == 0x77665500u);

	/* denied write is reported */
	reset();
	deny = true;
	e = lemonadep_reboot_prepare(&io, false, &r);
	CHECK(e == -3 && (r.status & LR_ARB_DENIED) && sdam == 0x01u);

	printf("test_reboot %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
