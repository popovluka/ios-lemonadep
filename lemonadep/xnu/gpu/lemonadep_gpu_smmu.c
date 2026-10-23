/*
 * lemonadep: SMMU context bank discovery for the Adreno SMMU
 * (qcom,sm8350-smmu-500, confirmed `arm,mmu-500` compatible -- generic
 * ARM SMMUv2, not a Qualcomm-specific protocol here). The device tree's
 * `iommus = <&adreno_smmu SID MASK>` cells are literal, confirmed from the
 * real arm_smmu_of_xlate() (not guessed): GPU uses stream IDs 0 and 1
 * (one context, two routed streams), GMU uses stream ID 5 (its own
 * context). See lemonadep/HANDOFF.md "GPU, Phase 4" for the full citation
 * and the SMR mask semantics (bit=1 means "don't care" for that ID bit,
 * architectural ARM SMMUv2, not Qualcomm-specific).
 *
 * The context BANK number for each is not a device-tree constant -- on
 * Linux it is chosen by the IOMMU core's own runtime allocator, not a
 * fixed hardware wiring. Since this driver exclusively owns the SMMU
 * instance (no Linux allocator sharing it), it reads the real ID1.NUMCB
 * capability register and picks its own banks, rather than hardcoding a
 * guessed number.
 */
#include <stdbool.h>
#include <stdint.h>

/* ARM_SMMU_GR0_ID1 offset and the NUMCB field, confirmed in the real
 * pinned drivers/iommu/arm/arm-smmu/arm-smmu.h (ARM_SMMU_GR0_ID1 = 0x24,
 * ARM_SMMU_ID1_NUMCB = GENMASK(7,0)) -- standard ARM SMMUv2 architecture,
 * not Qualcomm-specific. */
#define LEMONADEP_SMMU_GR0_ID1_OFFSET 0x24u
#define LEMONADEP_SMMU_ID1_NUMCB_MASK 0xffu

#define LEMONADEP_SMMU_SID_GPU_0 0u
#define LEMONADEP_SMMU_SID_GPU_1 1u
#define LEMONADEP_SMMU_SID_GMU 5u
#define LEMONADEP_SMMU_SID_MASK 0x400u

typedef enum { LemonadepSmmuOk, LemonadepSmmuBadArg, LemonadepSmmuIoError,
	LemonadepSmmuNoBanks } lemonadep_smmu_status;

typedef struct {
	void *cookie;
	int (*read32)(void *cookie, uint32_t byte_offset, uint32_t *out);
} lemonadep_smmu_discover_io;

/*
 * Picks the lowest two free banks: bank_gpu for streams {0,1}, bank_gmu
 * for stream {5} -- any two distinct, in-range banks work equally well
 * (nothing in the real source ties a specific bank index to these
 * streams), this just needs NUMCB >= 2 and takes the simplest valid
 * choice. Does not touch any SMMU register beyond the one ID1 read.
 */
lemonadep_smmu_status
lemonadep_smmu_discover_banks(const lemonadep_smmu_discover_io *io, unsigned *bank_gpu,
    unsigned *bank_gmu, unsigned *numcb_out)
{
	uint32_t id1;
	if (!io || !io->read32 || !bank_gpu || !bank_gmu) {
		return LemonadepSmmuBadArg;
	}
	if (io->read32(io->cookie, LEMONADEP_SMMU_GR0_ID1_OFFSET, &id1)) {
		return LemonadepSmmuIoError;
	}
	unsigned numcb = id1 & LEMONADEP_SMMU_ID1_NUMCB_MASK;
	if (numcb_out) {
		*numcb_out = numcb;
	}
	if (numcb < 2) {
		return LemonadepSmmuNoBanks;
	}
	*bank_gpu = 0;
	*bank_gmu = 1;
	return LemonadepSmmuOk;
}
