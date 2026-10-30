/* SPDX-License-Identifier: MIT */
/*
 * lemonadep Wi-Fi, stage 2: after lemonadep_wlan_pcie_up() has the link up,
 * give the QCA6490 its BAR0, read its MHI/BHI registers, look at how the apps
 * SMMU treats its DMA, then hand its boot ROM (PBL) the first 512 KiB of the
 * firmware (the SBL) over BHI and wait for MHI READY. Plain C with injected
 * I/O like stage 1, host-tested by test_wlan_mhi.c.
 *
 * The order is the vendor kernel's (LOS-K, see LemonadepMhiSource.h):
 * pci_assign_resource + pci_enable_device + pci_set_master (cnss_pci_enable_bus;
 * pci-msm.c sets memory + bus master on the root port), the MHI version read in
 * of_register_mhi_controller (mhi_init.c), mhi_async_power_up (BHIOFF/BHIEOFF
 * range checks, BHI_INTVEC = 0, EE must be PBL or AMSS), mhi_fw_load_handler +
 * mhi_fw_load_bhi (STATUS = 0, IMGADDR high/low, IMGSIZE, IMGTXDB, poll
 * STATUS) and mhi_ready_state_transition (RESET clear and READY set).
 *
 * Not here yet: the MHI context/rings, M0, BHIe (the rest of the image) and
 * interrupts -- everything is polled. The BHI step only runs when the apps
 * SMMU lets the chip's stream through untranslated (bypass), so the chip DMAs
 * from the physical address the firmware blob sits at; anything else is
 * logged and stopped (no stream table is touched here).
 */
#include <stdbool.h>
#include <stdint.h>

#include "LemonadepMhiSource.h"

typedef enum {
	LemonadepMhiPhaseNone,
	LemonadepMhiPhaseBar = 12,    /* bridge window, BAR0 sized + assigned, mem + master */
	LemonadepMhiPhaseRegs,        /* MHI/BHI registers, chip version, SMMU probe */
	LemonadepMhiPhaseSmmu,        /* a bypass stream-map entry for 0x1c01 if none */
	LemonadepMhiPhaseBhi,         /* SBL over BHI */
	LemonadepMhiPhaseReady,       /* MHI READY */
	LemonadepMhiPhaseDone,
} lemonadep_mhi_phase;

typedef enum {
	LemonadepMhiOk,
	LemonadepMhiBadArg,
	LemonadepMhiIoError,
	LemonadepMhiBadBar,           /* BAR0 not 64-bit memory, or too big for the window */
	LemonadepMhiBadRegs,          /* BHI/BHIE offset outside BAR0, or all-ones */
	LemonadepMhiBadEe,            /* not in PBL (or AMSS) at power-up */
	LemonadepMhiNoFirmware,
	LemonadepMhiSmmuNotBypass,    /* stream 0x1c01 would be translated or faulted */
	LemonadepMhiSmmuNoFreeEntry,
	LemonadepMhiSmmuWriteRefused, /* the SMR/S2CR read back differently */
	LemonadepMhiSmmuCbInUse,      /* the bypass-emulation context bank is taken */
	LemonadepMhiBhiError,         /* BHI_STATUS = error */
	LemonadepMhiBhiTimeout,
	LemonadepMhiReadyTimeout,
	LemonadepMhiSysErr,
	LemonadepMhiDmaMapFailed,     /* io->dma_map refused the image */
} lemonadep_mhi_status;

typedef struct {
	uint64_t pa;                  /* physical address the chip reads the image from */
	uint32_t bytes;
	const void *data;             /* the image in kernel memory, for io->dma_map */
} lemonadep_mhi_fw;

typedef struct {
	lemonadep_mhi_fw fw_v1, fw_v2;   /* amss.bin, amss20.bin (absent: bytes 0) */
} lemonadep_mhi_params;

typedef struct {
	lemonadep_mhi_phase phase;
	/* BAR */
	uint32_t bar_size, bar_pa, rc_cmd, ep_cmd;
	/* MHI/BHI */
	uint32_t reglen, ver, cfg, bhioff, bhieoff, ctrl, status, soc_hw_version, ee, bhi_status0;
	/* SMMU (read-only) */
	uint32_t scr0, id0, sgfsr, smr, s2cr;
	int smr_index;                /* -1: no SMR matches the stream */
	int smr_free;                 /* first unused stream-map entry, -1: none */
	bool smmu_bypass, smmu_written;
	uint32_t smr_want, s2cr_want, smr_rb, s2cr_rb;
	/* bypass emulated through a context bank (the Qualcomm quirk) */
	bool smmu_quirk;
	uint32_t id1, cb, cb_sctlr0, cbar0, cbar_rb;
	/* BHI */
	const char *fw_name;
	uint64_t fw_pa;
	uint64_t fw_dma;              /* what IMGADDR was given: fw_pa, or io->dma_map's address */
	bool iommu;                   /* the image went through io->dma_map */
	uint32_t sbl_bytes, bhi_status, bhi_errcode, bhi_errdbg[3];
	unsigned bhi_ms;
	/* READY */
	uint32_t ready_ctrl, ready_status, ready_ee;
	unsigned ready_ms;
} lemonadep_mhi_result;

#define MTRY(expr) do { if (expr) return LemonadepMhiIoError; } while (0)
#define CHECKPOINT(what) do { if (io->checkpoint) io->checkpoint(io->cookie, (what)); } while (0)
#define MPHASE(from, to) do { \
		if (stop_after != LemonadepMhiPhaseNone && stop_after == (from)) { return LemonadepMhiOk; } \
		r->phase = (to); \
	} while (0)

/* BAR0 lives in WlanWinMem at bar_pa - LEMONADEP_PCIE0_MEM_PA. */
static int
bar_rd(const lemonadep_wlan_io *io, const lemonadep_mhi_result *r, uint32_t off, uint32_t *v)
{
	return io->read32(io->cookie, WlanWinMem, r->bar_pa - LEMONADEP_PCIE0_MEM_PA + off, v);
}
static int
bar_wr(const lemonadep_wlan_io *io, const lemonadep_mhi_result *r, uint32_t off, uint32_t v)
{
	return io->write32(io->cookie, WlanWinMem, r->bar_pa - LEMONADEP_PCIE0_MEM_PA + off, v);
}

/* Does SMR n match stream sid? arm-smmu.c arm_smmu_find_sme() semantics:
 * equal outside the mask. Without EXIDS the ID/MASK fields are 15 bits and
 * VALID is SMR bit 31; with EXIDS VALID moves to S2CR (EXIDVALID). */
static bool
smr_matches(uint32_t smr, uint32_t s2cr, bool exids, uint32_t sid)
{
	uint32_t id = smr & 0xffffu, mask = smr >> 16;
	if (exids) {
		if (!(s2cr & SMMU_S2CR_EXIDVALID)) {
			return false;
		}
	} else {
		if (!(smr & SMMU_SMR_VALID)) {
			return false;
		}
		id &= 0x7fffu;
		mask &= 0x7fffu;
	}
	return ((id ^ sid) & ~mask & (exids ? 0xffffu : 0x7fffu)) == 0;
}

lemonadep_mhi_status
lemonadep_wlan_mhi_up(const lemonadep_wlan_io *io, const lemonadep_mhi_params *p,
    lemonadep_mhi_result *r, lemonadep_mhi_phase stop_after)
{
	if (!io || !p || !r || !io->read32 || !io->write32 || !io->delay_us) {
		return LemonadepMhiBadArg;
	}
	*r = (lemonadep_mhi_result){0};
	r->smr_index = -1;
	r->smr_free = -1;

	/* pci_assign_resource(): size BAR0 (all-ones, read back), place it at the
	 * window's first aligned address. QCA6490's BAR0 is 64-bit memory. */
	MPHASE(LemonadepMhiPhaseNone, LemonadepMhiPhaseBar);
	CHECKPOINT("size BAR0 (config writes)");
	uint32_t lo, hi;
	MTRY(wr(io, WlanWinConf, PCI_BAR0, 0xffffffffu));
	MTRY(wr(io, WlanWinConf, PCI_BAR1, 0xffffffffu));
	MTRY(rd(io, WlanWinConf, PCI_BAR0, &lo));
	MTRY(rd(io, WlanWinConf, PCI_BAR1, &hi));
	if ((lo & 0x7u) != PCI_BAR_MEM64 || hi != 0xffffffffu || !(lo & ~0xfu)) {
		r->bar_size = lo;
		return LemonadepMhiBadBar;
	}
	r->bar_size = ~(lo & ~0xfu) + 1u;
	if (r->bar_size > LEMONADEP_MHI_BAR_MAX || (r->bar_size & (r->bar_size - 1u))) {
		return LemonadepMhiBadBar;
	}
	r->bar_pa = (LEMONADEP_PCIE0_MEM_PA + r->bar_size - 1u) & ~(r->bar_size - 1u);
	MTRY(wr(io, WlanWinConf, PCI_BAR0, r->bar_pa));
	MTRY(wr(io, WlanWinConf, PCI_BAR1, 0));
	/* The root port as Linux leaves it: bus 0 -> 1..1, its memory window over
	 * BAR0 (1 MiB granular), memory + bus master on both ends. */
	MTRY(wr(io, WlanWinDbi, PCI_BRIDGE_BUSES, 0x00010100u));
	MTRY(wr(io, WlanWinDbi, PCI_BRIDGE_MEM_WINDOW,
	    ((r->bar_pa + r->bar_size - 1u) & 0xfff00000u) | ((r->bar_pa >> 16) & 0xfff0u)));
	MTRY(rmw(io, WlanWinDbi, PCI_CMD_STATUS, 0, PCI_CMD_MEMORY | PCI_CMD_MASTER));
	MTRY(rmw(io, WlanWinConf, PCI_CMD_STATUS, 0, PCI_CMD_MEMORY | PCI_CMD_MASTER));
	MTRY(rd(io, WlanWinDbi, PCI_CMD_STATUS, &r->rc_cmd));
	MTRY(rd(io, WlanWinConf, PCI_CMD_STATUS, &r->ep_cmd));

	/* MHI registers (all-ones = the BAR does not decode), the chip version,
	 * the BHI block. Then the apps SMMU's stream mapping, read only. */
	MPHASE(LemonadepMhiPhaseBar, LemonadepMhiPhaseRegs);
	CHECKPOINT("first BAR0 read (MHI registers)");
	MTRY(bar_rd(io, r, MHI_REGLEN, &r->reglen));
	MTRY(bar_rd(io, r, MHI_VER, &r->ver));
	MTRY(bar_rd(io, r, MHI_CFG, &r->cfg));
	MTRY(bar_rd(io, r, MHI_BHIOFF, &r->bhioff));
	MTRY(bar_rd(io, r, MHI_BHIEOFF, &r->bhieoff));
	MTRY(bar_rd(io, r, MHI_CTRL, &r->ctrl));
	MTRY(bar_rd(io, r, MHI_STATUS, &r->status));
	MTRY(bar_rd(io, r, MHI_SOC_HW_VERSION, &r->soc_hw_version));
	if (r->ver == 0xffffffffu || r->bhioff >= r->bar_size || r->bhieoff >= r->bar_size ||
	    r->bhioff + BHI_ERRDBG3 + 4u > r->bar_size) {
		return LemonadepMhiBadRegs;
	}
	MTRY(bar_rd(io, r, r->bhioff + BHI_EXECENV, &r->ee));
	MTRY(bar_rd(io, r, r->bhioff + BHI_STATUS, &r->bhi_status0));

	CHECKPOINT("apps SMMU GR0 read");
	MTRY(rd(io, WlanWinSmmu, SMMU_sCR0, &r->scr0));
	MTRY(rd(io, WlanWinSmmu, SMMU_ID0, &r->id0));
	MTRY(rd(io, WlanWinSmmu, SMMU_sGFSR, &r->sgfsr));
	const bool exids = (r->scr0 & SMMU_sCR0_EXIDENABLE) != 0;
	for (unsigned n = 0; n < (r->id0 & 0xffu); ++n) {
		uint32_t smr, s2cr;
		MTRY(rd(io, WlanWinSmmu, SMMU_SMR(n), &smr));
		MTRY(rd(io, WlanWinSmmu, SMMU_S2CR(n), &s2cr));
		const bool used = exids ? (s2cr & SMMU_S2CR_EXIDVALID) != 0 : (smr & SMMU_SMR_VALID) != 0;
		if (!used && r->smr_free < 0) {
			r->smr_free = (int)n;
		}
		if (r->smr_index < 0 && smr_matches(smr, s2cr, exids, LEMONADEP_WLAN_SID)) {
			r->smr_index = (int)n;
			r->smr = smr;
			r->s2cr = s2cr;
		}
	}
	/* Bypass: client port disabled, or the matching S2CR says bypass, or no
	 * match while unmatched streams pass (USFCFG = 0). */
	r->smmu_bypass = (r->scr0 & SMMU_sCR0_CLIENTPD) ||
	    (r->smr_index >= 0 ? SMMU_S2CR_TYPE(r->s2cr) == SMMU_S2CR_BYPASS : !(r->scr0 & SMMU_sCR0_USFCFG));

	/* No entry for the stream and unmatched streams fault (what this phone's
	 * bootloader leaves: logs/lemonadep-wlan9.txt): give it a bypass entry the
	 * way arm-smmu.c arm_smmu_write_sme() does for a bypass domain -- S2CR
	 * (type bypass, privcfg default, SHCFG non-shareable) first, then the SMR
	 * (valid, ID, mask 0), each read back as arm_smmu_write_smr() does (the
	 * write can be refused). An entry that already matches is left alone. */
	MPHASE(LemonadepMhiPhaseRegs, LemonadepMhiPhaseSmmu);
	if (!io->dma_map && !r->smmu_bypass && r->smr_index < 0) {
		if (r->smr_free < 0) {
			return LemonadepMhiSmmuNoFreeEntry;
		}
		const uint32_t n = (uint32_t)r->smr_free;
		r->s2cr_want = SMMU_S2CR_BYPASS << 16 | SMMU_S2CR_SHCFG_NSH << 8 | (exids ? SMMU_S2CR_EXIDVALID : 0);
		r->smr_want = (exids ? 0 : SMMU_SMR_VALID) | LEMONADEP_WLAN_SID;
		CHECKPOINT("apps SMMU stream-map write (S2CR bypass, SMR 0x1c01)");
		MTRY(wr(io, WlanWinSmmu, SMMU_S2CR(n), r->s2cr_want));
		MTRY(wr(io, WlanWinSmmu, SMMU_SMR(n), r->smr_want));
		MTRY(rd(io, WlanWinSmmu, SMMU_S2CR(n), &r->s2cr_rb));
		MTRY(rd(io, WlanWinSmmu, SMMU_SMR(n), &r->smr_rb));
		r->smmu_written = true;
		if (r->smr_rb == r->smr_want && SMMU_S2CR_TYPE(r->s2cr_rb) == SMMU_S2CR_FAULT) {
			/* The quirk: emulate bypass through the last context bank, as
			 * mainline does -- after checking nobody uses that bank. */
			r->smmu_quirk = true;
			MTRY(rd(io, WlanWinSmmu, SMMU_ID1, &r->id1));
			const uint32_t ncb = r->id1 & 0xffu;
			const unsigned pgshift = (r->id1 >> 31) ? 16u : 12u;
			const uint32_t numpage = 1u << (((r->id1 >> 28) & 7u) + 1u);
			if (!ncb) {
				return LemonadepMhiSmmuCbInUse;
			}
			r->cb = ncb - 1u;
			const uint32_t gr1 = 1u << pgshift, cbpage = (numpage + r->cb) << pgshift;
			if (cbpage + (1u << pgshift) > SMMU_APPS_BYTES) {
				return LemonadepMhiSmmuCbInUse;
			}
			MTRY(rd(io, WlanWinSmmu, cbpage + SMMU_CB_SCTLR, &r->cb_sctlr0));
			MTRY(rd(io, WlanWinSmmu, gr1 + SMMU_GR1_CBAR(r->cb), &r->cbar0));
			if (r->cb_sctlr0 & 1u) {
				return LemonadepMhiSmmuCbInUse;
			}
			for (unsigned k = 0; k < (r->id0 & 0xffu); ++k) {
				uint32_t ksmr, ks2cr;
				MTRY(rd(io, WlanWinSmmu, SMMU_SMR(k), &ksmr));
				MTRY(rd(io, WlanWinSmmu, SMMU_S2CR(k), &ks2cr));
				const bool kvalid = exids ? (ks2cr & SMMU_S2CR_EXIDVALID) : (ksmr & SMMU_SMR_VALID);
				if (k != n && kvalid && SMMU_S2CR_TYPE(ks2cr) == 0 && (ks2cr & 0xffu) == r->cb) {
					return LemonadepMhiSmmuCbInUse;
				}
			}
			CHECKPOINT("apps SMMU bypass context bank + S2CR trans");
			MTRY(wr(io, WlanWinSmmu, cbpage + SMMU_CB_SCTLR, 0));
			MTRY(wr(io, WlanWinSmmu, gr1 + SMMU_GR1_CBAR(r->cb), SMMU_CBAR_S1_TRANS_S2_BYPASS));
			r->s2cr_want = r->cb | (exids ? SMMU_S2CR_EXIDVALID : 0);    /* type TRANS, privcfg default */
			MTRY(wr(io, WlanWinSmmu, SMMU_S2CR(n), r->s2cr_want));
			MTRY(rd(io, WlanWinSmmu, gr1 + SMMU_GR1_CBAR(r->cb), &r->cbar_rb));
			MTRY(rd(io, WlanWinSmmu, SMMU_S2CR(n), &r->s2cr_rb));
			if (((r->cbar_rb >> 16) & 3u) != 1u) {
				return LemonadepMhiSmmuWriteRefused;
			}
		}
		if (r->s2cr_rb != r->s2cr_want || r->smr_rb != r->smr_want) {
			return LemonadepMhiSmmuWriteRefused;
		}
		r->smr_index = (int)n;
		r->smr = r->smr_rb;
		r->s2cr = r->s2cr_rb;
		r->smmu_bypass = true;
	}

	/* mhi_async_power_up(): BHI_INTVEC = 0, the device must be in PBL (or
	 * already AMSS). mhi_fw_load_handler(): the image's first sbl_size bytes
	 * over BHI. */
	MPHASE(LemonadepMhiPhaseSmmu, LemonadepMhiPhaseBhi);
	if (r->ee != MHI_EE_PBL) {
		return LemonadepMhiBadEe;
	}
	const unsigned major = (r->soc_hw_version >> 8) & 0xfu;
	const lemonadep_mhi_fw *fw = major == LEMONADEP_MHI_FW_V2 ? &p->fw_v2 : &p->fw_v1;
	r->fw_name = major == LEMONADEP_MHI_FW_V2 ? "amss20.bin" : "amss.bin";
	r->fw_pa = fw->pa;
	if (io->dma_map) {
		/* Translated: the chip gets an SMMU address for the image. */
		if (!fw->bytes || !fw->data) {
			return LemonadepMhiNoFirmware;
		}
		r->sbl_bytes = fw->bytes < LEMONADEP_MHI_SBL_BYTES ? fw->bytes : LEMONADEP_MHI_SBL_BYTES;
		if (io->dma_map(io->cookie, fw->data, r->sbl_bytes, &r->fw_dma)) {
			return LemonadepMhiDmaMapFailed;
		}
		r->iommu = true;
	} else {
		if (!fw->bytes || !fw->pa || fw->pa + fw->bytes > LEMONADEP_MHI_DMA_LIMIT) {
			return LemonadepMhiNoFirmware;
		}
		if (!r->smmu_bypass) {
			return LemonadepMhiSmmuNotBypass;
		}
		r->sbl_bytes = fw->bytes < LEMONADEP_MHI_SBL_BYTES ? fw->bytes : LEMONADEP_MHI_SBL_BYTES;
		r->fw_dma = fw->pa;
	}
	CHECKPOINT("BHI doorbell (chip DMA starts)");
	MTRY(bar_wr(io, r, r->bhioff + BHI_INTVEC, 0));
	MTRY(bar_wr(io, r, r->bhioff + BHI_STATUS, 0));
	MTRY(bar_wr(io, r, r->bhioff + BHI_IMGADDR_HIGH, (uint32_t)(r->fw_dma >> 32)));
	MTRY(bar_wr(io, r, r->bhioff + BHI_IMGADDR_LOW, (uint32_t)r->fw_dma));
	MTRY(bar_wr(io, r, r->bhioff + BHI_IMGSIZE, r->sbl_bytes));
	/* Session id: Linux uses a random non-zero value; any non-zero one does. */
	MTRY(bar_wr(io, r, r->bhioff + BHI_IMGTXDB, 0x1c0ffee & BHI_TXDB_SEQ_MASK));
	/* Then 3 s with no BAR access at all: a crash inside this window is the
	 * chip's DMA (or the bus behind it), not our register reads. */
	if (io->trace) {
		io->trace(io->cookie, "doorbell rung, 3 s without BAR reads", 0, 0, 0, 0);
		io->delay_us(io->cookie, 3000000u);
		io->trace(io->cookie, "quiet window over, polling", 0, 0, 0, 0);
	}
	/* The first BHI run on the phone hard-crashed the SoC some time after this
	 * doorbell (logs/lemonadep-wlan11.txt), with nothing logged in between:
	 * every change of BHI status / EE / MHI status, and a heartbeat each
	 * second, now goes to the log. */
	uint32_t last_s = ~0u, last_ee = ~0u, last_st = ~0u;
	for (r->bhi_ms = 0;; ++r->bhi_ms) {
		MTRY(bar_rd(io, r, r->bhioff + BHI_STATUS, &r->bhi_status));
		const uint32_t s = r->bhi_status >> 30;
		if (io->trace) {
			uint32_t ee, st;
			MTRY(bar_rd(io, r, r->bhioff + BHI_EXECENV, &ee));
			MTRY(bar_rd(io, r, MHI_STATUS, &st));
			if (r->bhi_status != last_s || ee != last_ee || st != last_st || r->bhi_ms % 1000u == 0) {
				io->trace(io->cookie, "bhi", r->bhi_status, ee, st, r->bhi_ms);
				last_s = r->bhi_status;
				last_ee = ee;
				last_st = st;
			}
		}
		if (s == BHI_STATUS_SUCCESS) {
			break;
		}
		if (s == BHI_STATUS_ERROR) {
			MTRY(bar_rd(io, r, r->bhioff + BHI_ERRCODE, &r->bhi_errcode));
			for (unsigned i = 0; i < 3; ++i) {
				MTRY(bar_rd(io, r, r->bhioff + BHI_ERRDBG1 + 4u * i, &r->bhi_errdbg[i]));
			}
			return LemonadepMhiBhiError;
		}
		if (r->bhi_ms >= LEMONADEP_MHI_TIMEOUT_MS) {
			return LemonadepMhiBhiTimeout;
		}
		io->delay_us(io->cookie, 1000);
	}

	/* mhi_ready_state_transition(): wait for MHICTRL.RESET clear and
	 * MHISTATUS.READY set; SYSERR ends it early. */
	MPHASE(LemonadepMhiPhaseBhi, LemonadepMhiPhaseReady);
	for (r->ready_ms = 0;; ++r->ready_ms) {
		MTRY(bar_rd(io, r, MHI_CTRL, &r->ready_ctrl));
		MTRY(bar_rd(io, r, MHI_STATUS, &r->ready_status));
		if (io->trace && (r->ready_status != last_st || r->ready_ctrl != last_s || r->ready_ms % 1000u == 0)) {
			uint32_t ee;
			MTRY(bar_rd(io, r, r->bhioff + BHI_EXECENV, &ee));
			io->trace(io->cookie, "ready", r->ready_ctrl, ee, r->ready_status, r->ready_ms);
			last_st = r->ready_status;
			last_s = r->ready_ctrl;
		}
		if (r->ready_status & MHI_STATUS_SYSERR) {
			MTRY(bar_rd(io, r, r->bhioff + BHI_EXECENV, &r->ready_ee));
			return LemonadepMhiSysErr;
		}
		if (!(r->ready_ctrl & MHI_CTRL_RESET) && (r->ready_status & MHI_STATUS_READY)) {
			break;
		}
		if (r->ready_ms >= LEMONADEP_MHI_TIMEOUT_MS) {
			MTRY(bar_rd(io, r, r->bhioff + BHI_EXECENV, &r->ready_ee));
			return LemonadepMhiReadyTimeout;
		}
		io->delay_us(io->cookie, 1000);
	}
	MTRY(bar_rd(io, r, r->bhioff + BHI_EXECENV, &r->ready_ee));
	MPHASE(LemonadepMhiPhaseReady, LemonadepMhiPhaseDone);
	return LemonadepMhiOk;
}
