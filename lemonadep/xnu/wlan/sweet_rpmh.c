/*
 * SWEET PM6150L L11C bring-up; see report.md for DT and source evidence.
 * Sole XNU apps-DRV owner, called from thread context with deep idle disabled.
 * No sleep/wake programming, SPMI writes, GIC handler, or timeout recovery.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
int sweet_rpmh_vreg_enable(const char *res, unsigned mv);
int sweet_rpmh_vreg_vote(const char *res, unsigned mv, int hpm);

/* Live downstream DT has two positional reg entries, no reg-names. */
#define RH_RSC_PA       0x18220000u
#define RH_TCS_PA       0x18220d00u
#define RH_TCS_SIZE     0x3000u
#define RH_DICT_PA      0x0c3f000cu
#define RH_DRV          2u
#define RH_ACTIVE_N     2u       /* DT order: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N     8u       /* hidden CONTROL TCS is not in this bank */
#define RH_TCS_STRIDE   0x2a0u
#define RH_CMD_STRIDE   0x14u
#define RH_IRQ_EN       0x00u
#define RH_IRQ_STATUS   0x04u
#define RH_IRQ_CLEAR    0x08u
#define RH_WAIT         0x10u
#define RH_CONTROL      0x14u
#define RH_STATUS       0x18u
#define RH_ENABLE       0x1cu
#define RH_MSGID        0x30u
#define RH_ADDR         0x34u
#define RH_DATA         0x38u
#define RH_CMD_STATUS   0x3cu
#define RH_AMC_EN       (1u << 16)
#define RH_TRIGGER      (1u << 24)
#define RH_ISSUED       (1u << 8)
#define RH_COMPLETE     (1u << 16)
#define RH_WRITE_ACK    (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_DB_MAX       0x100000u /* policy bound, not a DT size */
#define RH_VRM          4u
#define RH_TIMEOUT_US   100000u

static vm_offset_t rh_rsc, rh_tcs, rh_dict, rh_db;
static uint32_t rh_db_size;
static unsigned rh_ncpt;
static unsigned rh_lock;
static bool rh_poisoned;

static uint32_t
rh_read(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
rh_write(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
rh_sync(uint32_t off, uint32_t value)
{
	rh_write(rh_tcs, off, value);
	for (unsigned i = 0; i < 1000; ++i) {
		if (rh_read(rh_tcs, off) == value) {
			return true;
		}
		delay(1);
	}
	printf("SWEET RPMH: readback timeout off=%#x want=%#x got=%#x\n",
	    off, value, rh_read(rh_tcs, off));
	return false;
}

/* DB is little endian; avoid packed structs and unaligned Device loads. */
static uint16_t
rh_db16(uint32_t off)
{
	uint32_t v = rh_read(rh_db, off & ~3u);
	return (uint16_t)(v >> (8u * (off & 3u)));
}

static bool
rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

static int
rh_map(void)
{
	if (!rh_rsc) {
		rh_rsc = ml_io_map(RH_RSC_PA, 0x100);
	}
	if (!rh_tcs) {
		rh_tcs = ml_io_map(RH_TCS_PA, RH_TCS_SIZE);
	}
	if (!rh_dict) {
		rh_dict = ml_io_map(RH_DICT_PA, 8);
	}
	if (!rh_rsc || !rh_tcs || !rh_dict) {
		printf("SWEET RPMH: RSC/locator mapping failed\n");
		return ENOMEM;
	}
	uint32_t id = rh_read(rh_rsc, 0);
	uint32_t solver = rh_read(rh_rsc, 4);
	uint32_t config = rh_read(rh_rsc, 0x0c);
	unsigned major = (id >> 16) & 0xffu;
	unsigned count = (config >> (6u * RH_DRV)) & 0x3fu;
	rh_ncpt = (config >> 27) & 0x1fu;
	printf("SWEET RPMH: rsc=%#x tcs=%#x DRV=%u id=%#x solver=%#x config=%#x TCS=%u ncpt=%u\n",
	    RH_RSC_PA, RH_TCS_PA, RH_DRV, id, solver, config, count, rh_ncpt);
	/* This client implements the pre-v3 layout only; do not guess v3 offsets. */
	if ((major != 1 && major != 2) || config == 0xffffffffu ||
	    solver == 0xffffffffu || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
		printf("SWEET RPMH: unsupported RSC geometry/revision\n");
		return ENODEV;
	}
	if (!rh_db) {
		uint32_t pa = rh_read(rh_dict, 0);
		uint32_t size = rh_read(rh_dict, 4);
		printf("SWEET RPMH: cmd-db locator=%#x pa=%#x size=%#x\n", RH_DICT_PA, pa, size);
		/* Firmware supplies the reserved RAM address; never scan physical RAM. */
		if (pa < 0x80000000u || (pa & 3u) || (size & 3u) ||
		    size < RH_DB_HEADER || size > RH_DB_MAX ||
		    (uint64_t)pa + size > 0x100000000ULL) {
			printf("SWEET RPMH: invalid cmd-db locator\n");
			return EINVAL;
		}
		rh_db = ml_io_map(pa, size);
		if (!rh_db) {
			return ENOMEM;
		}
		rh_db_size = size;
	}
	uint32_t db_version = rh_read(rh_db, 0);
	uint32_t magic = rh_read(rh_db, 4);
	uint32_t flags = rh_read(rh_db, 140);
	printf("SWEET RPMH: cmd-db version=%#x magic=%#x checksum=%#x flags=%#x\n",
	    db_version, magic, rh_read(rh_db, 136), flags);
	/* Linux only checks magic; version 1 is an intentionally stricter policy. */
	if (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
		printf("SWEET RPMH: unsupported/standalone cmd-db\n");
		return EINVAL;
	}
	return 0;
}

static int
rh_lookup(const char *res, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; ++i) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		uint32_t aux = RH_DB_HEADER + rh_db16(h + 4);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3u) ||
		    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
			printf("SWEET RPMH: malformed cmd-db slave header %u\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rh_read(rh_db, e) != key[0] || rh_read(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = rh_read(rh_db, e + 16);
			if (slave != RH_VRM || (addr >> 16) != RH_VRM || (addr & 15u) ||
			    !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20))) {
				printf("SWEET RPMH: invalid VRM entry for %s\n", res);
				return EINVAL;
			}
			/*
			 * This phone's cmd-db lists smpe2 twice with the same address
			 * (logs/lemonadep-wlan1.txt). Linux's cmd_db_get_header() takes
			 * the first match, so an identical repeat is harmless; refuse
			 * only entries that disagree.
			 */
			if (matches && addr != *address) {
				printf("SWEET RPMH: %s conflicting addresses %#x/%#x; refusing\n", res, *address, addr);
				return ENODEV;
			}
			*address = addr;
			++matches;
			printf("SWEET RPMH: %s addr=%#x slave=%u aux-version=%#x priority=%#x:%#x\n",
			    res, addr, slave, rh_db16(h + 8),
			    rh_read(rh_db, e + 8), rh_read(rh_db, e + 12));
		}
	}
	if (matches == 0) {
		printf("SWEET RPMH: %s not in cmd-db; refusing\n", res);
		return ENODEV;
	}
	return 0;
}

static bool
rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE;
	/* Idle alone is insufficient: a sleep/wake payload can be armed and idle. */
	return rh_read(rh_tcs, b + RH_STATUS) != 0 &&
	    rh_read(rh_tcs, b + RH_STATUS) != 0xffffffffu &&
	    /* 0, or AMC_EN alone: a finished active-mode request leaves it set (rpmh-rsc.c takes
	     * any idle slot it does not own; seen 0x10000 in TCS1 under UEFI, 2026-10-30) */
	    (rh_read(rh_tcs, b + RH_CONTROL) & ~RH_AMC_EN) == 0 &&
	    rh_read(rh_tcs, b + RH_ENABLE) == 0 &&
	    rh_read(rh_tcs, b + RH_WAIT) == 0 &&
	    !(rh_read(rh_tcs, RH_IRQ_STATUS) & (1u << t));
}

/*
 * One VRM vote: voltage (offset 0) and enable (offset 4), plus mode (offset
 * 8) = 7 (HPM) when the regulator's load calls for it -- Linux requests no
 * mode for a zero-load consumer, so neither does this. Enable comes last.
 */
static int
rh_send(const char *res, uint32_t address, unsigned mv, bool hpm)
{
	const unsigned n = hpm ? 3 : 2;
	const uint32_t offsets[3] = {0, hpm ? 8u : 4u, 4};
	const uint32_t values[3] = {mv, hpm ? 7u : 1u, 1};
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; ++t) {
		uint32_t b = t * RH_TCS_STRIDE;
		uint32_t enabled = rh_read(rh_tcs, b + RH_ENABLE);
		printf("SWEET RPMH: ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, rh_read(rh_tcs, b + RH_STATUS), rh_read(rh_tcs, b + RH_CONTROL),
		    enabled, rh_read(rh_tcs, b + RH_WAIT));
		if (enabled & ~((1u << rh_ncpt) - 1u)) {
			return EIO;
		}
		for (unsigned c = 0; c < rh_ncpt; ++c) {
			if ((enabled & (1u << c)) &&
			    (rh_read(rh_tcs, b + RH_ADDR + c * RH_CMD_STRIDE) & ~15u) == address) {
				printf("SWEET RPMH: conflicting inherited %s request in TCS%u\n", res, t);
				return EBUSY;
			}
		}
		if (chosen < 0 && rh_free(t)) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		printf("SWEET RPMH: no empty idle ACTIVE TCS; leaving inherited state intact\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
	if (irq_en == 0xffffffffu || !rh_free(t)) {
		return EBUSY;
	}
	/* Only this TCS's source is masked. Other sources/CPU IRQs remain intact. */
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit)) {
		rh_poisoned = true;
		return EIO;
	}
	if (!rh_free(t)) {
		/* No payload was touched. Do not acknowledge somebody else's IRQ. */
		if (!rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
			rh_poisoned = true;
		}
		return EBUSY;
	}
	/* From here an uncertain write/timeout quarantines our slot for this boot. */
	rh_poisoned = true;
	printf("SWEET RPMH: TCS%u ACTIVE %s %u mV%s enable=1 irq-enable=%#x\n", t, res, mv,
	    hpm ? " mode=7" : "", irq_en);
	for (unsigned c = 0; c < n; ++c) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) ||
		    !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c])) {
			return EIO;
		}
	}
	const uint32_t mask = (1u << n) - 1u;
	if (!rh_sync(b + RH_WAIT, mask) || !rh_sync(b + RH_ENABLE, mask) ||
	    !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_CONTROL, RH_AMC_EN)) {
		return EIO;
	}
	rh_write(rh_tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	uint32_t irq = 0, idle = 0, status[3] = {0, 0, 0};
	bool done = false, progress = false;
	for (unsigned us = 0; us < RH_TIMEOUT_US; ++us) {
		irq = rh_read(rh_tcs, RH_IRQ_STATUS);
		idle = rh_read(rh_tcs, b + RH_STATUS);
		bool complete = true;
		for (unsigned c = 0; c < n; ++c) {
			status[c] = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= status[c] != 0xffffffffu &&
			    (status[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		/* A fresh IRQ or observed execution transition excludes stale CMD_STATUS. */
		progress |= idle == 0 || !complete;
		if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
		    ((irq & bit) || progress) && complete) {
			done = true;
			break;
		}
		delay(1);
	}
	printf("SWEET RPMH: TCS%u done=%d irq=%#x idle=%#x control=%#x cmd-status=%#x/%#x/%#x\n",
	    t, done, irq, idle, rh_read(rh_tcs, b + RH_CONTROL), status[0], status[1], status[2]);
	if (!done) {
		printf("SWEET RPMH: timeout; TCS quarantined, IRQ remains masked; no reset/retry\n");
		return ETIMEDOUT;
	}
	/* Proven completion: clear trigger before mode, then free only our commands. */
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0)) {
		return EIO;
	}
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit); /* W1C: never clear other TCS bits */
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
		return EIO;
	}
	rh_poisoned = false;
	printf("SWEET RPMH: %s ACTIVE vote complete (rail voltage requires hardware verification)\n", res);
	return 0;
}

/*
 * Board-specific allow-list: rh_lookup()/rh_send() are generic (a real cmd-db
 * search plus the standard VRM write sequence), so this table is the only
 * thing that decides which rail may be voted, at which voltage, and whether
 * with an HPM mode vote. Every entry is the consumer's own DT values:
 *   ldoc11  sweet: PM6150L L11C, touch rail
 *   ldoc3   lemonadep: PM8350C L3, vdd_2v8-supply of the S3908 touch panel
 *           (qcom/lahaina-regulators.dtsi, HWMAP_lemonadep.md section 6)
 * lemonadep Wi-Fi (lemonadep/xnu/wlan/LemonadepWlanSource.h, from the live DT):
 *   ldob5   PCIe0 vreg-0p9, pm8350 L5, 880 mV, 47.9 mA load -> HPM
 *   ldob6   PCIe0 vreg-1p8, pm8350 L6, 1200 mV, 15 mA load -> HPM
 *   smpb10  QCA6490 vdd-wlan-io, pm8350 S10, 1.8 V
 *   ldoe7   QCA6490 wlan-ant-switch, pmr735a L7, 2.8 V
 *   smpe2   QCA6490 vdd-wlan-aon, pmr735a S2, 950..952 mV
 *   smpb11  QCA6490 vdd-wlan-dig, pm8350 S11, 950..952 mV
 *   smpc1   QCA6490 vdd-wlan-rfa1, pm8350c S1, 1880..2000 mV
 *   smpb12  QCA6490 vdd-wlan-rfa2, pm8350 S12, 1350 mV
 * The QCA6490 rails all have load 0 in the DT, so no mode vote (as Linux).
 */
static const struct {
	const char *res;
	unsigned min_mv, max_mv;
	bool hpm;
} rh_allow[] = {
	{"ldoc11", 2950, 3400, true},
	{"ldoc3", 2950, 3400, true},
	{"ldob5", 880, 880, true},
	{"ldob6", 1200, 1200, true},
	/* Wi-Fi rails: mode 7 (SMPS PWM / LDO HPM): with no mode vote the chip's boot ROM
	 * never handed over to its SBL (2026-10-30) */
	{"smpb10", 1800, 1800, true},
	{"ldoe7", 2800, 2800, true},
	{"smpe2", 950, 952, true},
	{"smpb11", 950, 952, true},
	{"smpc1", 1880, 2000, true},
	{"smpb12", 1350, 1350, true},
};

int
sweet_rpmh_vreg_enable(const char *res, unsigned mv)
{
	/* The original entry point: the touch rails, always with HPM. */
	return sweet_rpmh_vreg_vote(res, mv, 1);
}

int
sweet_rpmh_vreg_vote(const char *res, unsigned mv, int hpm)
{
	int allowed = -1;
	for (unsigned i = 0; res && i < sizeof(rh_allow) / sizeof(rh_allow[0]); ++i) {
		if (!strcmp(res, rh_allow[i].res)) {
			allowed = (int)i;
		}
	}
	if (allowed < 0 || mv < rh_allow[allowed].min_mv || mv > rh_allow[allowed].max_mv ||
	    (hpm != 0) != rh_allow[allowed].hpm) {
		printf("SWEET RPMH: %s %u mV hpm=%d is not on the allow-list\n", res ? res : "(null)", mv, hpm);
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	uint32_t address = 0;
	if (rh_poisoned) {
		printf("SWEET RPMH: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, &address);
	}
	if (!error) {
		error = rh_send(res, address, mv, rh_allow[allowed].hpm);
	}
out:
	if (error) {
		printf("SWEET RPMH: enable failed errno=%d\n", error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
