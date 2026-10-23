/* SPDX-License-Identifier: MIT */
/*
 * lemonadep: the GMU's real memory -- what lets the GMU actually run its
 * firmware once released from reset. Plain C, no IOKit, host-testable
 * (test_gmu_mem.c).
 *
 * Why this exists: a660_gmu.bin's main code block (54476 bytes) lives at
 * GMU address 0x4000, the "icache" region the GMU reaches through the
 * Adreno SMMU, not over MMIO. Until this file, nothing put it there and
 * the GMU's SMMU context was never programmed, so releasing the GMU from
 * reset sent it fetching from an unmapped address -- the most likely
 * cause of the earlier XBL ramdump (lemonadep/HANDOFF.md "GPU, Phase 9").
 *
 * Memory comes from one physically contiguous region the shim reserves
 * below topOfKernelData (tools/mkpayload.py --extra-region gpumem:SIZE),
 * never handed to the VM. Its layout is fixed below.
 *
 * Every SMMU step uses the real vendored ArmSmmu4K kit (already exercised
 * end to end against a mock SMMU-500 in test_gpu_memory.c). Real facts
 * from mainline sm8350.dtsi: the Adreno SMMU is `dma-coherent` (so
 * coherent walks and write-back mappings, as Linux uses), and the GMU is
 * `iommus = <&adreno_smmu 5 0x400>` -- SID 5 with SMR mask bit 10, i.e.
 * SIDs 5 and 0x405. The kit only installs exact stream matches, so both
 * SIDs get their own exact SMR, which is equivalent.
 */
#include <stdint.h>
#include <string.h>

#include "LemonadepGmuSource.h"
#include "lemonadep_gpu_smmu_map.h"
#include "vendor/ArmSmmu4K/ArmSmmu4KStreams.h"

/* ---- the reserved gpumem region's layout (byte offsets) ---- */
#define LEMONADEP_GPUMEM_TABLES_OFF 0x0000000u
#define LEMONADEP_GPUMEM_ICACHE_OFF 0x0400000u
#define LEMONADEP_GPUMEM_DUMMY_OFF  0x1400000u
#define LEMONADEP_GPUMEM_DEBUG_OFF  0x1404000u
#define LEMONADEP_GPUMEM_LOG_OFF    0x140c000u
#define LEMONADEP_GPUMEM_HFI_OFF    0x1410000u
#define LEMONADEP_GPUMEM_BYTES      0x1414000u
_Static_assert(LEMONADEP_GMU_TABLE_BUDGET_PAGES * 4096u <= LEMONADEP_GPUMEM_ICACHE_OFF, "tables overlap icache");
_Static_assert(LEMONADEP_GPUMEM_ICACHE_OFF + LEMONADEP_GMU_ICACHE_BYTES <= LEMONADEP_GPUMEM_DUMMY_OFF, "icache overlaps dummy");
_Static_assert(LEMONADEP_GPUMEM_DUMMY_OFF + LEMONADEP_GMU_DUMMY_BYTES <= LEMONADEP_GPUMEM_DEBUG_OFF, "dummy overlaps debug");
_Static_assert(LEMONADEP_GPUMEM_DEBUG_OFF + LEMONADEP_GMU_DEBUG_BYTES <= LEMONADEP_GPUMEM_LOG_OFF, "debug overlaps log");
_Static_assert(LEMONADEP_GPUMEM_LOG_OFF + LEMONADEP_GMU_LOG_BYTES <= LEMONADEP_GPUMEM_HFI_OFF, "log overlaps hfi");
_Static_assert(LEMONADEP_GPUMEM_HFI_OFF + LEMONADEP_GMU_HFI_BYTES <= LEMONADEP_GPUMEM_BYTES, "hfi past end");

/* log/hfi have no fixed IOVA in the real source; placed right after debug
 * (same choice lemonadep_gpu_memory.cpp and the cold-start glue make). */
/* sm8350.dtsi gmu node: iommus = <&adreno_smmu 5 0x400>. */
#define LEMONADEP_GMU_SMMU_SID      5u
#define LEMONADEP_GMU_SMMU_SID_MASK 0x400u

#define LEMONADEP_GMU_LOG_IOVA (LEMONADEP_GMU_DEBUG_IOVA + LEMONADEP_GMU_DEBUG_BYTES)
#define LEMONADEP_GMU_HFI_IOVA (LEMONADEP_GMU_LOG_IOVA + LEMONADEP_GMU_LOG_BYTES)

/*
 * a6xx_hfi_init() + a6xx_hfi_queue_init() (a6xx_hfi.c), word for word: a
 * table header, then HFI_MAX_QUEUES (3) queue headers, then the queues
 * themselves at hfi+4K, +8K, +12K, each 4 KiB. A660 is not gmu->legacy, so
 * the queue ids are 0 (command), 1 (response), 2 (debug). `table->size`
 * is assigned the byte count in the real code despite its "dwords"
 * comment; copied as the code does it, not as the comment says.
 */
void
lemonadep_hfi_table_init(volatile uint32_t *hfi, uint32_t hfi_iova)
{
	enum { TableWords = 6, HeaderWords = 12, Queues = 3 };
	for (unsigned i = 0; i < LEMONADEP_GMU_HFI_BYTES / 4u; ++i) {
		hfi[i] = 0;
	}
	hfi[0] = 0;                                       /* version */
	hfi[1] = (TableWords + Queues * HeaderWords) * 4; /* size: 168 bytes */
	hfi[2] = TableWords;                              /* qhdr0_offset, dwords */
	hfi[3] = HeaderWords;                             /* qhdr_size, dwords */
	hfi[4] = Queues;                                  /* num_queues */
	hfi[5] = Queues;                                  /* active_queues */
	for (unsigned q = 0; q < Queues; ++q) {
		volatile uint32_t *h = hfi + TableWords + q * HeaderWords;
		h[0] = 1;                                 /* status */
		h[1] = hfi_iova + 4096u * (q + 1u);       /* iova */
		h[2] = (10u << 8) | q;                    /* type */
		h[3] = 4096u >> 2;                        /* size, dwords */
		h[4] = 0;                                 /* msg_size */
		h[5] = 0;                                 /* dropped */
		h[6] = 1;                                 /* rx_watermark */
		h[7] = 1;                                 /* tx_watermark */
		h[8] = 1;                                 /* rx_request */
		h[9] = 0;                                 /* tx_request */
		h[10] = 0;                                /* read_index */
		h[11] = 0;                                /* write_index */
	}
}

/*
 * The firmware blocks lemonadep_gmu_coldstart.c's load_firmware() leaves
 * to the board glue: real a6xx_gmu_fw_load() memcpys them to
 * icache.virt + (addr - icache.iova). Returns the bytes copied, 0 on a
 * bounds failure.
 */
uint32_t
lemonadep_gmu_icache_load(uint8_t *icache, const uint8_t *fw, uint32_t fw_len)
{
	uint32_t copied = 0;
	for (unsigned i = 0; i < sizeof(LemonadepGmuFwBlocks) / sizeof(LemonadepGmuFwBlocks[0]); ++i) {
		const struct LemonadepGmuFwBlock *b = &LemonadepGmuFwBlocks[i];
		if (b->target != LemonadepGmuFwTargetIcache) {
			continue;
		}
		if (b->addr < LEMONADEP_GMU_ICACHE_IOVA ||
		    (uint64_t)(b->addr - LEMONADEP_GMU_ICACHE_IOVA) + b->bytes > LEMONADEP_GMU_ICACHE_BYTES ||
		    (uint64_t)b->file_offset + b->bytes > fw_len) {
			return 0;
		}
		memcpy(icache + (b->addr - LEMONADEP_GMU_ICACHE_IOVA), fw + b->file_offset, b->bytes);
		copied += b->bytes;
	}
	return copied;
}

/* ---- read-only probe of the live Adreno SMMU-500 (SMMUv2 GR0 layout,
 * the same offsets the vendored ArmSmmu4K kit reads). Nothing is written. */
typedef struct {
	uint32_t scr0, id0, id1, id2, sgfsr;
	unsigned numcb, nums2cb, numsmrg, sid_bits, exids, output_bits;
	size_t pages, stride;
	unsigned smr_valid, s2cr_fault, s2cr_bypass, s2cr_trans, banks_enabled;
	unsigned bank;       /* first usable bank, or ~0u */
	unsigned slots[2];   /* first two attachable stream slots */
	unsigned free_slots; /* how many of slots[] are valid (0..2) */
	unsigned global_ok;  /* sCR0 as ArmSmmu4KAttachStreams requires */
} lemonadep_smmu_probe_info;

static unsigned
oas_bits(unsigned index)
{
	static const unsigned table[] = {32, 36, 40, 42, 44, 48};
	return index < 6 ? table[index] : 48;
}

/* 0 on success, 1 on a register read failure or an ID1 layout that doesn't
 * fit the mapped window. */
int
lemonadep_smmu_probe(const ArmSmmu4KIo *io, lemonadep_smmu_probe_info *p)
{
	memset(p, 0, sizeof(*p));
	p->bank = ~0u;
	if (io->read32(io->cookie, 0x0, &p->scr0) || io->read32(io->cookie, 0x20, &p->id0) ||
	    io->read32(io->cookie, 0x24, &p->id1) || io->read32(io->cookie, 0x28, &p->id2) ||
	    io->read32(io->cookie, 0x48, &p->sgfsr)) {
		return 1;
	}
	p->numsmrg = p->id0 & 0xffu;
	p->exids = (p->id0 >> 8) & 1u;
	p->sid_bits = p->exids ? 16u : ((p->id0 >> 9) & 15u);
	p->numcb = p->id1 & 0xffu;
	p->nums2cb = (p->id1 >> 16) & 0xffu;
	p->pages = (size_t)1 << (((p->id1 >> 28) & 7u) + 1u);
	p->stride = (p->id1 >> 31) ? 0x10000u : 0x1000u;
	p->output_bits = oas_bits((p->id2 >> 4) & 15u);
	/* ArmSmmu4KStreams' own global() check, mirrored for logging. */
	p->global_ok = (p->scr0 & 0x407u) == 0x406u && !!(p->scr0 & 8u) == p->exids;
	if (0xc00u + 4u * p->numsmrg > io->register_bytes ||
	    (p->pages + p->numcb) * p->stride > io->register_bytes) {
		return 1;
	}

	uint64_t used_banks = 0; /* banks some valid stream translates through */
	for (unsigned i = 0; i < p->numsmrg; ++i) {
		uint32_t smr, s2cr;
		if (io->read32(io->cookie, 0x800u + 4u * i, &smr) || io->read32(io->cookie, 0xc00u + 4u * i, &s2cr)) {
			return 1;
		}
		unsigned valid = p->exids ? (s2cr >> 10) & 1u : smr >> 31;
		unsigned type = (s2cr >> 16) & 3u;
		p->smr_valid += valid;
		p->s2cr_fault += type == 2;
		p->s2cr_bypass += type == 1;
		p->s2cr_trans += type == 0;
		if (valid && type == 0 && (s2cr & 0xffu) < 64) {
			used_banks |= (uint64_t)1 << (s2cr & 0xffu);
		}
		/* Attachable exactly as ArmSmmu4KAttachStreams' preflight wants:
		 * not a live match, and S2CR already FAULT. */
		uint32_t exids_valid = p->exids ? 1u << 10 : 0;
		if (!valid && (s2cr & ~exids_valid) == (2u << 16) && p->free_slots < 2) {
			p->slots[p->free_slots++] = i;
		}
	}
	/* Stage-1 banks start at NUMS2CB, the same place Linux's arm-smmu
	 * allocator starts them; take the first disabled, unreferenced one. */
	for (unsigned b = p->nums2cb; b < p->numcb && b < 64; ++b) {
		uint32_t sctlr;
		if (io->read32(io->cookie, (p->pages + b) * p->stride, &sctlr)) {
			return 1;
		}
		if (sctlr & 1u) {
			++p->banks_enabled;
			continue;
		}
		if (p->bank == ~0u && !(used_banks & ((uint64_t)1 << b))) {
			p->bank = b;
		}
	}
	return 0;
}

/* ---- GMU SMMU context: prepare, program, attach, map ---- */
typedef struct {
	ArmSmmu4KLiveArena arena;
	ArmSmmu4KLiveState state;
	ArmSmmu4KContextConfig context;
	ArmSmmu4KProgramResult program;
	ArmSmmu4KStreamState streams;
	ArmSmmu4KStreamResult routes;
	ArmSmmu4KRange mapped[5];
	unsigned step;      /* last step attempted, for the log */
	unsigned sid_count; /* 2 (5 and 0x405), or 1 if SIDSIZE can't hold 0x405 */
} lemonadep_gmu_smmu;

enum {
	LemonadepGmuSmmuStepPrepare = 1,
	LemonadepGmuSmmuStepAudit,
	LemonadepGmuSmmuStepPublish,
	LemonadepGmuSmmuStepProgram,
	LemonadepGmuSmmuStepAttach,
	LemonadepGmuSmmuStepMap,
	LemonadepGmuSmmuStepDone,
};

/*
 * gpumem_va/gpumem_pa: the reserved region (CPU mapping / physical).
 * `vis` publishes table pages for device walks (the caller cleans them to
 * PoC; the node is dma-coherent, so this is belt and braces).
 */
ArmSmmu4KStatus
lemonadep_gmu_smmu_setup(lemonadep_gmu_smmu *s, const ArmSmmu4KIo *io, const lemonadep_smmu_probe_info *p,
    uint8_t *gpumem_va, uint64_t gpumem_pa, const ArmSmmu4KLiveVisibility *vis)
{
	ArmSmmu4KStatus status;
	memset(s, 0, sizeof(*s));
	if (p->bank == ~0u || p->free_slots < 1 || !p->global_ok) {
		return ArmSmmu4KUnsupported;
	}

	s->step = LemonadepGmuSmmuStepPrepare;
	s->arena = (ArmSmmu4KLiveArena){
		.config = {.input_bits = 32, .output_bits = p->output_bits,
		    .table_physical = gpumem_pa + LEMONADEP_GPUMEM_TABLES_OFF, .leaf_budget = LEMONADEP_GMU_LEAF_BUDGET},
		.iova = LEMONADEP_GMU_APERTURE_IOVA, .bytes = LEMONADEP_GMU_APERTURE_BYTES, .upper_region = 0,
		.mapping = (volatile uint64_t *)(gpumem_va + LEMONADEP_GPUMEM_TABLES_OFF),
		.capacity_pages = LEMONADEP_GMU_TABLE_BUDGET_PAGES,
	};
	if ((status = ArmSmmu4KLivePrepare(&s->arena, &s->state)) != ArmSmmu4KSuccess) {
		return status;
	}
	s->step = LemonadepGmuSmmuStepAudit;
	ArmSmmu4KLiveResult audit;
	if ((status = ArmSmmu4KLiveAudit(&s->arena, &s->state, NULL, 0, &audit)) != ArmSmmu4KSuccess) {
		return status;
	}
	s->step = LemonadepGmuSmmuStepPublish;
	for (size_t page = 0; page < s->state.tables.table_pages; ++page) {
		if (vis->publish_table_page(vis->cookie, s->arena.mapping + page * 512u, 4096)) {
			return ArmSmmu4KIoError;
		}
	}

	s->step = LemonadepGmuSmmuStepProgram;
	s->context = (ArmSmmu4KContextConfig){
		.lower = &s->arena.config, .lower_tables = &s->state.tables,
		/* Linux's arm-smmu uses the bank index as the ASID: unique per bank. */
		.asid = p->bank, .coherent_walk = 1, .stall_on_fault = 0, .continue_after_fault = 1,
	};
	if ((status = ArmSmmu4KProgramDisabledContext(io, p->bank, &s->context, 1000, &s->program)) != ArmSmmu4KSuccess) {
		return status;
	}

	s->step = LemonadepGmuSmmuStepAttach;
	ArmSmmu4KStream streams[2] = {{p->slots[0], LEMONADEP_GMU_SMMU_SID},
	    {p->slots[1], LEMONADEP_GMU_SMMU_SID | LEMONADEP_GMU_SMMU_SID_MASK}};
	s->sid_count = (p->free_slots >= 2 && p->sid_bits >= 11) ? 2 : 1;
	if ((status = ArmSmmu4KAttachStreams(io, &s->streams, streams, s->sid_count, p->bank, &s->context,
	    &s->routes)) != ArmSmmu4KSuccess) {
		return status;
	}

	s->step = LemonadepGmuSmmuStepMap;
	const lemonadep_gmu_region regions[5] = {
		{LEMONADEP_GMU_ICACHE_IOVA, gpumem_pa + LEMONADEP_GPUMEM_ICACHE_OFF, LEMONADEP_GMU_ICACHE_BYTES},
		{LEMONADEP_GMU_DUMMY_IOVA, gpumem_pa + LEMONADEP_GPUMEM_DUMMY_OFF, LEMONADEP_GMU_DUMMY_BYTES},
		{LEMONADEP_GMU_DEBUG_IOVA, gpumem_pa + LEMONADEP_GPUMEM_DEBUG_OFF, LEMONADEP_GMU_DEBUG_BYTES},
		{LEMONADEP_GMU_LOG_IOVA, gpumem_pa + LEMONADEP_GPUMEM_LOG_OFF, LEMONADEP_GMU_LOG_BYTES},
		{LEMONADEP_GMU_HFI_IOVA, gpumem_pa + LEMONADEP_GPUMEM_HFI_OFF, LEMONADEP_GMU_HFI_BYTES},
	};
	if ((status = lemonadep_gmu_map_regions(&s->arena, &s->state, io, p->bank, &s->context, vis,
	    regions, 5, s->mapped, 1000)) != ArmSmmu4KSuccess) {
		return status;
	}
	s->step = LemonadepGmuSmmuStepDone;
	return ArmSmmu4KSuccess;
}
