/* SPDX-License-Identifier: MIT */
#ifndef LEMONADEP_GPU_SMMU_MAP_H
#define LEMONADEP_GPU_SMMU_MAP_H
#include "vendor/ArmSmmu4K/ArmSmmu4KLive.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
	uint64_t iova, physical, bytes;
} lemonadep_gmu_region;
/* See lemonadep_gpu_smmu_map.c for the real contract (ArmSmmu4KLiveUpdate,
 * one call per region, the "before" list growing as each one is mapped). */
ArmSmmu4KStatus lemonadep_gmu_map_regions(const ArmSmmu4KLiveArena *, ArmSmmu4KLiveState *,
    const ArmSmmu4KIo *, unsigned bank, const ArmSmmu4KContextConfig *,
    const ArmSmmu4KLiveVisibility *, const lemonadep_gmu_region *, size_t count,
    ArmSmmu4KRange *scratch, unsigned poll_budget);
#ifdef __cplusplus
}
#endif
#endif
