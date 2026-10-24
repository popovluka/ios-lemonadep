/* SPDX-License-Identifier: MIT */
/*
 * lemonadep: reset the phone into the bootloader (fastboot) or recovery from
 * the kernel, the way the vendor kernel's "reboot bootloader" does it
 * (LOS-K @ 187d13c, drivers/power/reset/qcom-reboot-reason.c + msm-poweroff.c):
 *   1. the reboot reason into PMK8350 SDAM: LIVE reboot_reason node ->
 *      nvmem cell sdam@7100 "restart@48", bits <1 7> (bit offset 1, 7 bits):
 *      bootloader = 0x02, recovery = 0x01 (qcom-reboot-reason.c reasons[]);
 *   2. the IMEM restart_reason word (LIVE qcom,msm-imem@146bf000 +
 *      restart_reason@65c): 0x77665500 bootloader, 0x77665502 recovery
 *      (msm-poweroff.c msm_restart_prepare);
 *   3. PSCI SYSTEM_RESET (LIVE psci method "smc"; PSCI 0.2 function id).
 * Plain C with injected I/O; host-tested by test_reboot.c.
 *
 * The SDAM byte is written over SPMI through the PMIC arbiter's owner
 * channel (spmi-pmic-arb.c, v5): pick the APID whose PPID is SID 0 /
 * peripheral 0x71 and whose ownership entry is our EE (0), write the data
 * to WDATA0, the command (opcode EXT_WRITEL = 0, addr low byte << 4, byte
 * count - 1) to CMD, poll STATUS for DONE (and not DENIED/FAILURE/DROPPED).
 * The old value is read first through the observer channel, so bit 0 stays.
 */
#include <stdbool.h>
#include <stdint.h>

/* LIVE qcom,spmi@c440000: core, chnls, obsrvr, cnfg; qcom,ee = 0 */
#define LR_SPMI_CORE_PA    0x0c440000u
#define LR_SPMI_CORE_BYTES 0x1100u
#define LR_SPMI_CHNLS_PA   0x0c600000u
#define LR_SPMI_OBS_PA     0x0e600000u
#define LR_SPMI_CNFG_PA    0x0c40a000u
#define LR_SPMI_EE         0u
#define LR_APID_MAP(n)     (0x900u + 4u * (n))     /* pmic_arb_apid_map_offset_v5 */
#define LR_APID_COUNT      ((LR_SPMI_CORE_BYTES - 0x900u) / 4u)
#define LR_OWNERSHIP(n)    (0x700u + 4u * (n))     /* SPMI_OWNERSHIP_TABLE_REG, owner [2:0] */
#define LR_CHAN_RW(apid)   (0x10000u * (apid))      /* in chnls */
#define LR_CHAN_OBS(apid)  (0x10000u * LR_SPMI_EE + 0x80u * (apid))   /* in obsrvr */
#define LR_ARB_CMD         0x00u
#define LR_ARB_STATUS      0x08u
#define LR_ARB_WDATA0      0x10u
#define LR_ARB_RDATA0      0x18u
#define LR_ARB_DONE        (1u << 0)
#define LR_ARB_FAILURE     (1u << 1)
#define LR_ARB_DENIED      (1u << 2)
#define LR_ARB_DROPPED     (1u << 3)
#define LR_OP_EXT_WRITEL   0u
#define LR_OP_EXT_READL    1u
#define LR_ARB_TIMEOUT_US  1000u                    /* PMIC_ARB_TIMEOUT_US */
/* LIVE sdam@7100 on qcom,pmk8350@0 (SID 0), cell restart@48 bits <1 7> */
#define LR_SDAM_SID        0u
#define LR_SDAM_ADDR       0x7148u
#define LR_REASON_RECOVERY   0x01u
#define LR_REASON_BOOTLOADER 0x02u
/* LIVE qcom,msm-imem@146bf000, restart_reason@65c */
#define LR_IMEM_PA         0x146bf000u
#define LR_IMEM_RESTART    0x65cu
#define LR_IMEM_BOOTLOADER 0x77665500u
#define LR_IMEM_RECOVERY   0x77665502u
#define LR_PSCI_SYSTEM_RESET 0x84000009u

enum { LrWinCore, LrWinChnls, LrWinObs, LrWinCnfg, LrWinImem, LrWinCount };

typedef struct {
	void *cookie;
	uint32_t (*read32)(void *cookie, unsigned win, uint32_t off);
	void (*write32)(void *cookie, unsigned win, uint32_t off, uint32_t v);
	void (*delay_us)(void *cookie, unsigned us);
} lemonadep_reboot_io;

typedef struct {
	int apid;                 /* -1: none found */
	uint32_t owner, old, new_, status;
} lemonadep_reboot_result;

static int
lr_find_apid(const lemonadep_reboot_io *io, uint16_t ppid, uint32_t *owner)
{
	int found = -1;
	for (uint32_t i = 0; i < LR_APID_COUNT; ++i) {
		uint32_t map = io->read32(io->cookie, LrWinCore, LR_APID_MAP(i));
		if (map == 0 || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		uint32_t o = io->read32(io->cookie, LrWinCnfg, LR_OWNERSHIP(i)) & 7u;
		if (o == LR_SPMI_EE) {           /* the entry this EE may write through */
			*owner = o;
			return (int)i;
		}
		if (found < 0) {
			found = (int)i;
			*owner = o;
		}
	}
	return found;
}

static uint32_t
lr_wait(const lemonadep_reboot_io *io, unsigned win, uint32_t chan)
{
	uint32_t st = 0;
	for (unsigned t = 0; t < LR_ARB_TIMEOUT_US; ++t) {
		st = io->read32(io->cookie, win, chan + LR_ARB_STATUS);
		if (st & LR_ARB_DONE) {
			break;
		}
		io->delay_us(io->cookie, 1);
	}
	return st;
}

static bool
lr_ok(uint32_t st)
{
	return (st & LR_ARB_DONE) && !(st & (LR_ARB_FAILURE | LR_ARB_DENIED | LR_ARB_DROPPED));
}

/* Steps 1 and 2. Returns 0 when the SDAM write went through; the IMEM word is
 * written either way (it is plain memory). The caller then does step 3. */
int
lemonadep_reboot_prepare(const lemonadep_reboot_io *io, bool recovery, lemonadep_reboot_result *r)
{
	*r = (lemonadep_reboot_result){.apid = -1};
	io->write32(io->cookie, LrWinImem, LR_IMEM_RESTART, recovery ? LR_IMEM_RECOVERY : LR_IMEM_BOOTLOADER);

	const uint16_t ppid = (uint16_t)(LR_SDAM_SID << 8 | (LR_SDAM_ADDR >> 8));
	r->apid = lr_find_apid(io, ppid, &r->owner);
	if (r->apid < 0 || r->owner != LR_SPMI_EE) {
		return -1;
	}
	const uint32_t apid = (uint32_t)r->apid;
	const uint32_t cmd_rd = LR_OP_EXT_READL << 27 | (LR_SDAM_ADDR & 0xffu) << 4;
	io->write32(io->cookie, LrWinObs, LR_CHAN_OBS(apid) + LR_ARB_CMD, cmd_rd);
	r->status = lr_wait(io, LrWinObs, LR_CHAN_OBS(apid));
	if (!lr_ok(r->status)) {
		return -2;
	}
	r->old = io->read32(io->cookie, LrWinObs, LR_CHAN_OBS(apid) + LR_ARB_RDATA0) & 0xffu;
	const uint32_t reason = recovery ? LR_REASON_RECOVERY : LR_REASON_BOOTLOADER;
	r->new_ = (r->old & 1u) | (reason << 1);         /* nvmem cell bits <1 7> */
	io->write32(io->cookie, LrWinChnls, LR_CHAN_RW(apid) + LR_ARB_WDATA0, r->new_);
	io->write32(io->cookie, LrWinChnls, LR_CHAN_RW(apid) + LR_ARB_CMD,
	    LR_OP_EXT_WRITEL << 27 | (LR_SDAM_ADDR & 0xffu) << 4);
	r->status = lr_wait(io, LrWinChnls, LR_CHAN_RW(apid));
	return lr_ok(r->status) ? 0 : -3;
}
