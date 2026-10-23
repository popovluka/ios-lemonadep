/*
 * lemonadep: the real A660 CP ring attach/submit/fence-wait. New code, not
 * adapted from NativeAdrenoRing.cpp -- that file's fence write is
 * genuinely the A7XX CP_EVENT_WRITE7 payload (event 0x31/CACHE_CLEAN plus
 * an A7XX-only enable bit), not A660's classic CP_EVENT_WRITE/CACHE_FLUSH_TS
 * one, confirmed from the real a6xx_submit() (a6xx_gpu.c:416-430). See
 * lemonadep/HANDOFF.md "GPU, Phase 5" for the full citation.
 *
 * Deliberately simplified for this first bring-up, same "simplest correct
 * path first" choice already used for ACD/IFPC: no RPTR-shadow setup
 * (legitimate -- A660 has ADRENO_QUIRK_HAS_HW_APRIV, confirmed in the real
 * a6xx_catalog.c, and the real a6xx_hw_init()'s own comment says
 * APRIV-capable targets "can use the RPTR shadow from hardware" instead),
 * no AHB-fence-status write robustness layer (fenced_write() in the real
 * source; a real thing, just deferred), and completion is CPU-polled on
 * the fence memory location rather than IRQ-driven -- the same pattern
 * already used for GMU cold-start, and it means NativeAdrenoIrq.cpp's own
 * A7XX-specific interrupt decode doesn't need fixing for this milestone
 * at all.
 *
 * CP_RB_CNTL = 0x20c: BUFSZ=ilog2(32KiB/8)=12, BLKSZ=ilog2(32/8)=2,
 * (0,0x0,0,0)-free wire: value = 12 | (2<<8). Real source:
 * MSM_GPU_RB_CNTL_DEFAULT in msm_gpu.h ("all targets use the same
 * ringbuffer size"), confirmed A660 takes the APRIV branch (not
 * |NO_UPDATE) in the real a6xx_hw_init() since ADRENO_QUIRK_HAS_HW_APRIV
 * is set. CP_EVENT_WRITE's IRQ bit (pos 31) and EVENT field (bits 0-7) are
 * from the real adreno_pm4.xml domain, same opcode number for both
 * CP_EVENT_WRITE (A2XX-A6XX) and CP_EVENT_WRITE7 (A7XX-): 0x46.
 */
#include <stdbool.h>
#include <stdint.h>

#include "AdrenoPM4.h"

#define LEMONADEP_CP_RB_CNTL_VALUE 0x0000020cu
#define LEMONADEP_CP_EVENT_WRITE_OPCODE 0x46u
#define LEMONADEP_CACHE_FLUSH_TS 0x04u
#define LEMONADEP_CP_EVENT_WRITE_IRQ (1u << 31)
#define LEMONADEP_CP_SCRATCH2_DWORD 0x0885u /* CP_SCRATCH array base 0x883 + index 2 */

/* GPU-window dword register offsets, LemonadepA660HwInit.h's `defines`
 * section (gpu_write() convention -- dword offsets, not byte): */
#define LEMONADEP_CP_RB_BASE_DWORD 0x0800u
#define LEMONADEP_CP_RB_CNTL_DWORD 0x0802u
#define LEMONADEP_CP_RB_WPTR_DWORD 0x0807u

typedef struct {
	void *cookie;
	int (*write32)(void *cookie, uint32_t dword_reg, uint32_t value);
	int (*barrier)(void *cookie);
	int (*now_us)(void *cookie, uint64_t *out);
	int (*wait_us)(void *cookie, unsigned us);
} lemonadep_ring_io;

typedef enum { LemonadepRingOk, LemonadepRingBadArg, LemonadepRingIoError,
	LemonadepRingTimeout } lemonadep_ring_status;

/*
 * `ring`: caller-owned, cache-coherent CPU pointer to `ring_dwords`
 * dwords, already mapped at `ring_iova` in the active GPU SMMU context.
 * `fence_word`: a single coherent dword at `fence_iova`, zeroed by the
 * caller before the first submit -- this is where the CP writes the
 * completed sequence number; wait() polls it directly (no IRQ).
 */
typedef struct {
	volatile uint32_t *ring;
	uint32_t ring_dwords; /* power of two; 32 KiB / 4 = 8192 for the real default size */
	uint64_t ring_iova;
	volatile uint32_t *fence_word;
	uint64_t fence_iova;
	uint32_t wptr;
	uint32_t seqno;
} lemonadep_ring;

lemonadep_ring_status
lemonadep_ring_attach(const lemonadep_ring_io *io, lemonadep_ring *ring, volatile uint32_t *ring_mem,
    uint32_t ring_dwords, uint64_t ring_iova, volatile uint32_t *fence_word, uint64_t fence_iova)
{
	if (!io || !io->write32 || !io->barrier || !ring || !ring_mem || !ring_dwords ||
	    (ring_dwords & (ring_dwords - 1)) || !fence_word || (fence_iova & 3)) {
		return LemonadepRingBadArg;
	}
	ring->ring = ring_mem;
	ring->ring_dwords = ring_dwords;
	ring->ring_iova = ring_iova;
	ring->fence_word = fence_word;
	ring->fence_iova = fence_iova;
	ring->wptr = 0;
	ring->seqno = 0;
	*fence_word = 0;
	if (io->write32(io->cookie, LEMONADEP_CP_RB_BASE_DWORD, (uint32_t)ring_iova) ||
	    io->write32(io->cookie, LEMONADEP_CP_RB_BASE_DWORD + 1, (uint32_t)(ring_iova >> 32)) ||
	    io->write32(io->cookie, LEMONADEP_CP_RB_CNTL_DWORD, LEMONADEP_CP_RB_CNTL_VALUE)) {
		return LemonadepRingIoError;
	}
	return io->barrier(io->cookie) ? LemonadepRingIoError : LemonadepRingOk;
}

/*
 * Copies `word_count` dwords into the ring (wrapping), appends the real
 * A660 fence sequence (CP_SCRATCH(2)=seqno, then CP_EVENT_WRITE/
 * CACHE_FLUSH_TS/IRQ with the fence address and seqno), publishes to PoC,
 * advances WPTR. Returns the new fence (sequence) number.
 */
lemonadep_ring_status
lemonadep_ring_submit(const lemonadep_ring_io *io, lemonadep_ring *ring, const uint32_t *words,
    uint32_t word_count, uint32_t *fence_out)
{
	if (!io || !ring || (!words && word_count) || !fence_out) {
		return LemonadepRingBadArg;
	}
	const uint32_t mask = ring->ring_dwords - 1;
	uint32_t at = ring->wptr;
	for (uint32_t i = 0; i < word_count; ++i) {
		ring->ring[(at + i) & mask] = words[i];
	}
	at = (at + word_count) & mask;
	const uint32_t seqno = ring->seqno + 1;
	/* CP_SCRATCH(2) = seqno (a diagnostic copy; not what wait() reads).
	 * Real PM4 type-4 header encoding (AdrenoPM4Type4, not hand-rolled --
	 * it includes two odd-parity bits this file got wrong on a first
	 * draft; see lemonadep/HANDOFF.md "GPU, Phase 5"). */
	{
		uint32_t pkt[2];
		if (AdrenoPM4Type4(LEMONADEP_CP_SCRATCH2_DWORD, 1, &pkt[0]) != AdrenoPM4Success) {
			return LemonadepRingBadArg;
		}
		pkt[1] = seqno;
		for (uint32_t i = 0; i < 2; ++i) {
			ring->ring[(at + i) & mask] = pkt[i];
		}
		at = (at + 2) & mask;
	}
	/* CP_EVENT_WRITE(CACHE_FLUSH_TS, IRQ), 4-word payload: event|irq,
	 * fence addr lo/hi, seqno -- the real a6xx_submit() layout. */
	{
		uint32_t pkt[5];
		if (AdrenoPM4Type7(LEMONADEP_CP_EVENT_WRITE_OPCODE, 4, &pkt[0]) != AdrenoPM4Success) {
			return LemonadepRingBadArg;
		}
		pkt[1] = LEMONADEP_CACHE_FLUSH_TS | LEMONADEP_CP_EVENT_WRITE_IRQ;
		pkt[2] = (uint32_t)ring->fence_iova;
		pkt[3] = (uint32_t)(ring->fence_iova >> 32);
		pkt[4] = seqno;
		for (uint32_t i = 0; i < 5; ++i) {
			ring->ring[(at + i) & mask] = pkt[i];
		}
		at = (at + 5) & mask;
	}
	if (io->barrier(io->cookie)) {
		return LemonadepRingIoError;
	}
	if (io->write32(io->cookie, LEMONADEP_CP_RB_WPTR_DWORD, at)) {
		return LemonadepRingIoError;
	}
	ring->wptr = at;
	ring->seqno = seqno;
	*fence_out = seqno;
	return LemonadepRingOk;
}

lemonadep_ring_status
lemonadep_ring_wait(const lemonadep_ring_io *io, const lemonadep_ring *ring, uint32_t fence,
    uint32_t timeout_us)
{
	uint64_t start, now;
	if (!io || !io->now_us || !io->wait_us || !ring) {
		return LemonadepRingBadArg;
	}
	if (io->now_us(io->cookie, &start)) {
		return LemonadepRingIoError;
	}
	for (;;) {
		/* Standard wrapping sequence-number compare: observed >= fence,
		 * tolerant of the counter wrapping past UINT32_MAX. */
		uint32_t observed = *ring->fence_word;
		if ((int32_t)(observed - fence) >= 0) {
			return LemonadepRingOk;
		}
		if (io->now_us(io->cookie, &now) || now < start) {
			return LemonadepRingIoError;
		}
		if (now - start >= timeout_us) {
			return LemonadepRingTimeout;
		}
		if (io->wait_us(io->cookie, 50)) {
			return LemonadepRingIoError;
		}
	}
}
