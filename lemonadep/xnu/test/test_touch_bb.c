/*
 * End-to-end host test of overlay 1005's lemonadep touch path: the real
 * bsd/dev/sweet_touch.c (only touch_rd/touch_wr redirected, gen_touch_bb.py)
 * drives simulated TLMM pins. An open-drain I2C bus is modelled on gpio20
 * (SDA) / gpio21 (SCL) with a Synaptics TCM device at 0x4B behind it:
 *   - reset (gpio22 rising) -> unsolicited REPORT_IDENTIFY, mode=application
 *   - commands are [cmd, len_lo, len_hi, payload], answered STATUS_OK
 *   - messages are {0xA5, code, len_lo, len_hi, payload, 0x5A}; a read that
 *     stops early continues on the next read behind {0xA5, 0x03}
 *   - idle reads return {0xA5, 0x00, 0, 0, 0x5A}
 * Checks: bring-up succeeds, the report config and enable-report payload the
 * IC receives, and a two-finger touch report followed by a lift becomes the
 * right sweet_hid_touch down/up events.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int kern_return_t;
typedef void *thread_t;
typedef int wait_result_t;
typedef void *DTEntry;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define ARM64_BOARD_CONFIG_SWEET 1
#define KERN_SUCCESS 0
#define kSuccess 0
#define NSEC_PER_MSEC 1000000ull
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL ((thread_continue_t)0)

static uint64_t fake_clock;
void delay(int us) { fake_clock += (uint64_t)us * 1000u; }
int assert_wait_deadline(void *e, int i, uint64_t d) { (void)e; (void)i; (void)d; return 0; }
int thread_block(thread_continue_t c) { (void)c; return 0; }
void clock_interval_to_deadline(uint32_t i, uint32_t s, uint64_t *d) { *d = (uint64_t)i * s; }
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs) { *abs = ns; }
uint64_t mach_absolute_time(void) { return fake_clock += 100; }
int PE_parse_boot_argn(const char *n, void *v, int s) { (void)n; (void)v; (void)s; return 0; }
int SecureDTLookupEntry(void *p, const char *n, DTEntry *e) { (void)p; (void)n; (void)e; return 1; }
int SecureDTGetProperty(DTEntry e, const char *n, void const **p, unsigned int *s) { (void)e; (void)n; (void)p; (void)s; return 1; }
kern_return_t kernel_thread_start(thread_continue_t c, void *a, thread_t *t) { (void)c; (void)a; (void)t; return 1; }
void thread_deallocate(thread_t t) { (void)t; }
thread_t current_thread(void) { return NULL; }
kern_return_t thread_terminate(thread_t t) { (void)t; return 0; }
vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t sz) { (void)pa; (void)sz; return 0; }
int sweet_rpmh_vreg_enable(const char *r, unsigned mv) { (void)r; (void)mv; return 0; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- recorded HID events ---- */
static struct { int finger, down, x, y; } ev[32];
static unsigned nev;
static int ext_w, ext_h;
void sweet_hid_touch_extents(int w, int h) { ext_w = w; ext_h = h; }

void sweet_hid_touch(int finger, int down, int x, int y)
{
	if (nev < 32) {
		ev[nev].finger = finger; ev[nev].down = down; ev[nev].x = x; ev[nev].y = y;
		nev++;
	}
}

/* ---- TLMM pins ---- */
#define GPIO_BASE ((vm_offset_t)0x40000000u)
#define PIN_SDA 20
#define PIN_SCL 21
#define PIN_RST 22
static uint32_t cfg[256], io[256];
static int slave_low;	/* the IC pulls SDA low */
/*
 * eGPIO model, as the hardware behaves (pinctrl-sm8350 .egpio_present = 11,
 * .egpio_enable = 12; pinctrl-msm claims the pad by setting bit 12): a pad
 * with EGPIO_PRESENT set is owned by an island subsystem and ignores the
 * TLMM output driver entirely until the apps processor claims it. PIN_PWR
 * models lemonadep's gpio197 (touch 1.8 V enable), which is such a pad.
 */
#define EGPIO_PRESENT (1u << 11)
#define EGPIO_ENABLE  (1u << 12)
#define PIN_PWR 197
static int egpio_pin_claimed;

/* An unclaimed eGPIO pad is not driven by the TLMM at all. */
static int
pad_is_ours(unsigned p)
{
	return !(cfg[p] & EGPIO_PRESENT) || (cfg[p] & EGPIO_ENABLE);
}

static int
master_level(unsigned p, int idle)
{
	if (!pad_is_ours(p)) {
		return 0;	/* island holds it low, as gpio197 did on hw */
	}
	if (cfg[p] & 0x200u) {
		return (io[p] & 2u) ? 1 : 0;
	}
	return idle;	/* input: pull-up (or floating high) */
}

/* The IC runs off the 1.8 V enable pad; unpowered it clamps SDA low, which is
 * what an unclaimed gpio197 produced on the device (reads came back 0x00, so
 * the TCM marker check failed with ENXIO). */
static int
ic_powered(void)
{
	return pad_is_ours(PIN_PWR) && (cfg[PIN_PWR] & 0x200u) && (io[PIN_PWR] & 2u);
}

static int
line(unsigned p)
{
	if (p == PIN_SDA && !ic_powered()) {
		return 0;
	}
	int lvl = master_level(p, 1);
	if (p == PIN_SDA && slave_low) {
		lvl = 0;
	}
	if (p == PIN_SDA && (cfg[p] & 0x200u) && !(io[p] & 2u)) {
		lvl = 0;
	}
	return lvl;
}

/* ---- TCM device model ---- */
#define QMAX 16
static uint8_t q[QMAX][128];
static unsigned qlen[QMAX], qh, qt;
static uint8_t cur[128];
static unsigned curlen, curoff;
static bool cont_pending, txn_cont;
static unsigned txn_prefix;	/* bytes of {A5,03} still to send */
static uint8_t got_cfg[64];
static unsigned got_cfg_len, got_enable_len, cmds_seen;
static uint8_t got_enable[8];
static uint8_t pend[128];
static unsigned pend_len;
static uint8_t pend_cmd;

static void
queue_msg(uint8_t code, const uint8_t *p, unsigned n)
{
	uint8_t *m = q[qt % QMAX];
	m[0] = 0xA5; m[1] = code; m[2] = (uint8_t)n; m[3] = (uint8_t)(n >> 8);
	if (n) {
		memcpy(m + 4, p, n);
	}
	m[4 + n] = 0x5A;
	qlen[qt % QMAX] = n + 5;
	qt++;
}

static void
queue_identify(uint8_t code)
{
	uint8_t id[24] = {0x02, 0x01 /* MODE_APPLICATION */};
	memcpy(id + 2, "s3908-test-part", 15);
	queue_msg(code, id, sizeof(id));
}

static void
device_reset(void)
{
	qh = qt = 0;
	curlen = curoff = 0;
	cont_pending = false;
	queue_identify(0x10);	/* REPORT_IDENTIFY */
}

static void
device_command(uint8_t c, const uint8_t *p, unsigned n)
{
	cmds_seen++;
	switch (c) {
	case 0x02: queue_identify(0x01); break;
	case 0x26:
		memcpy(got_cfg, p, n < sizeof(got_cfg) ? n : sizeof(got_cfg));
		got_cfg_len = n;
		queue_msg(0x01, NULL, 0);
		break;
	case 0x05:
		memcpy(got_enable, p, n < sizeof(got_enable) ? n : sizeof(got_enable));
		got_enable_len = n;
		queue_msg(0x01, NULL, 0);
		break;
	default: queue_msg(0x0f, NULL, 0); break;
	}
}

static void
device_write(const uint8_t *b, unsigned n)
{
	if (n == 0) {
		return;
	}
	if (b[0] == 0x01) {		/* CMD_CONTINUE_WRITE */
		memcpy(pend + pend_len, b + 1, n - 1);
		pend_len += n - 1;
	} else {
		pend_cmd = b[0];
		memcpy(pend, b + 1, n - 1);
		pend_len = n - 1;
	}
	if (pend_len >= 2) {
		unsigned want = pend[0] | (unsigned)pend[1] << 8;
		if (pend_len - 2 >= want) {
			device_command(pend_cmd, pend + 2, want);
			pend_len = 0;
		}
	}
}

static void
read_begin(void)
{
	txn_prefix = 0;
	txn_cont = false;
	if (cont_pending) {
		txn_prefix = 2;
		txn_cont = true;
		return;
	}
	if (qh != qt) {
		memcpy(cur, q[qh % QMAX], qlen[qh % QMAX]);
		curlen = qlen[qh % QMAX];
		qh++;
	} else {
		static const uint8_t idle[5] = {0xA5, 0x00, 0, 0, 0x5A};
		memcpy(cur, idle, 5);
		curlen = 5;
	}
	curoff = 0;
}

static uint8_t
read_byte(void)
{
	if (txn_prefix) {
		return txn_prefix-- == 2 ? 0xA5 : 0x03;
	}
	return curoff < curlen ? cur[curoff++] : 0x5A;
}

static void
read_end(void)
{
	cont_pending = curoff < curlen;
}

/* ---- I2C slave state machine on the simulated lines ---- */
enum { B_IDLE, B_ADDR, B_WRITE, B_READ };
static int bus = B_IDLE, bitn, acking, match, rw, rd_ack_phase, master_ack, rbitn;
static uint8_t cbyte, rbyte;
static uint8_t wbuf[128];
static unsigned wlen;
static int sda_prev = 1, scl_prev = 1;

static void
drive_bit(void)
{
	slave_low = !((rbyte >> (7 - rbitn)) & 1);
}

static void
bus_stop_or_restart(void)
{
	if (bus == B_WRITE) {
		device_write(wbuf, wlen);
	} else if (bus == B_READ) {
		read_end();
	}
}

static void
bus_eval(void)
{
	int scl = line(PIN_SCL), sda = line(PIN_SDA);
	if (scl && scl_prev && sda != sda_prev) {
		bus_stop_or_restart();
		slave_low = 0;
		if (!sda) {		/* START */
			bus = B_ADDR; bitn = 0; cbyte = 0; acking = 0; wlen = 0;
		} else {		/* STOP */
			bus = B_IDLE;
		}
	} else if (scl && !scl_prev) {	/* rising: sample */
		if ((bus == B_ADDR || bus == B_WRITE) && !acking) {
			cbyte = (uint8_t)(cbyte << 1 | sda);
			bitn++;
		} else if (bus == B_READ) {
			if (rd_ack_phase) {
				master_ack = !sda;
			} else {
				rbitn++;
			}
		}
	} else if (!scl && scl_prev) {	/* falling: drive */
		if (bus == B_ADDR || bus == B_WRITE) {
			if (acking) {
				acking = 0; slave_low = 0; bitn = 0; cbyte = 0;
				if (bus == B_ADDR) {
					if (!match) {
						bus = B_IDLE;
					} else if (rw) {
						bus = B_READ; read_begin();
						rbyte = read_byte(); rbitn = 0; rd_ack_phase = 0;
						drive_bit();
					} else {
						bus = B_WRITE;
					}
				}
			} else if (bitn == 8) {
				if (bus == B_ADDR) {
					match = (cbyte >> 1) == 0x4B; rw = cbyte & 1;
					slave_low = match;
				} else {
					wbuf[wlen++] = cbyte;
					slave_low = 1;
				}
				acking = 1;
			}
		} else if (bus == B_READ) {
			if (rd_ack_phase) {
				rd_ack_phase = 0;
				if (master_ack) {
					rbyte = read_byte(); rbitn = 0; drive_bit();
				} else {
					slave_low = 0;
				}
			} else if (rbitn == 8) {
				slave_low = 0; rd_ack_phase = 1;
			} else {
				drive_bit();
			}
		}
	}
	scl_prev = line(PIN_SCL);
	sda_prev = line(PIN_SDA);
}

/* ---- MMIO hooks used by touch_rd/touch_wr ---- */
static int rst_prev = 1;

uint32_t
mock_rd(vm_offset_t base, uint32_t off)
{
	if (base != GPIO_BASE) {
		return 0;
	}
	unsigned p = off / 0x1000u;
	if ((off & 0xfffu) == 0) {
		return cfg[p];
	}
	if ((off & 0xfffu) == 4) {
		return (io[p] & ~1u) | (uint32_t)line(p);
	}
	return 0;
}

void
mock_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	if (base != GPIO_BASE) {
		return;
	}
	unsigned p = off / 0x1000u;
	if ((off & 0xfffu) == 0) {
		cfg[p] = val;
	} else if ((off & 0xfffu) == 4) {
		io[p] = val;
	}
	if (p == PIN_SDA || p == PIN_SCL) {
		bus_eval();
	}
	if (p == PIN_RST) {
		int lvl = master_level(PIN_RST, 1);
		if (lvl && !rst_prev && ic_powered()) {
			device_reset();
		}
		rst_prev = lvl;
	}
	if (p == PIN_PWR && (cfg[p] & EGPIO_ENABLE)) {
		egpio_pin_claimed = 1;
	}
}

#include "sweet_touch_host.c"

int
main(void)
{
	st.lmn = true;
	st.gpio = GPIO_BASE;
	st.i2c_addr = 0x4B;
	st.sda_gpio = PIN_SDA;
	st.scl_gpio = PIN_SCL;
	st.reset_gpio = PIN_RST;
	st.irq_gpio = 23;
	st.pwr_gpio_1v8 = PIN_PWR;
	st.pwr_gpio_2v8 = 31;
	st.panel_x = 1440;
	st.panel_y = 3216;
	/* gpio197 arrives island-owned, exactly as on the device */
	cfg[PIN_PWR] = EGPIO_PRESENT;
	/* as touch_thread's lemonadep branch */
	touch_gpio(st.pwr_gpio_2v8, 0, 0, 0, true, true);
	touch_gpio(st.pwr_gpio_1v8, 0, 0, 0, true, true);
	CHECK(egpio_pin_claimed);
	CHECK(ic_powered());
	printf("egpio: gpio%d cfg=%#x claimed=%d powered=%d\n",
	    PIN_PWR, cfg[PIN_PWR], egpio_pin_claimed, ic_powered());
	touch_gpio(st.reset_gpio, 0, 3, 3, true, false);
	touch_gpio(st.sda_gpio, 0, 3, 1, false, false);
	touch_gpio(st.scl_gpio, 0, 3, 1, false, false);
	st.bitbang = true;
	bb_selftest();

	int err = touch_tcm_bringup();
	CHECK(err == 0);
	CHECK(got_cfg_len == sizeof(tcm_report_cfg) &&
	    memcmp(got_cfg, tcm_report_cfg, sizeof(tcm_report_cfg)) == 0);
	CHECK(got_enable_len == 1 && got_enable[0] == 0x11);
	printf("bring-up: err=%d, IC got report config (%u bytes) and enable-report [%#x], %u commands\n",
	    err, got_cfg_len, got_enable[0], cmds_seen);

	/* two fingers: id0 (720,1600), id1 (100,3000), our 5-byte records */
	const uint8_t two[10] = {0, 0xD0, 0x02, 0x40, 0x06, 1, 100, 0, 0xB8, 0x0B};
	queue_msg(0x11, two, sizeof(two));
	nev = 0;
	CHECK(touch_tcm_poll() == 0);
	CHECK(nev == 2);
	CHECK(ev[0].finger == 0 && ev[0].down == 1 && ev[0].x == 720 && ev[0].y == 1600);
	CHECK(ev[1].finger == 1 && ev[1].down == 1 && ev[1].x == 100 && ev[1].y == 3000);
	printf("touch: %u events, finger0 (%d,%d) finger1 (%d,%d)\n",
	    nev, ev[0].x, ev[0].y, ev[1].x, ev[1].y);

	/* idle read in between: nothing changes */
	nev = 0;
	CHECK(touch_tcm_poll() == 0);
	CHECK(nev == 0);

	/*
	 * The firmware's own layout (op9pro-mainline-poc patch 0002): 7-byte
	 * prefix, then 12 bytes per object. One finger at (1439,3215) -- the
	 * far corner, which the old 1079/2399 clamp in SweetHID would have
	 * thrown away. Classification nibble 1 = finger, index 3.
	 */
	const uint8_t nat[7 + 12] = {
		0, 0, 0, 0, 0, 0, 0,
		0x13, 0x9F, 0x05, 0x8F, 0x0C, 40, 10, 0, 8, 0, 0, 0,
	};
	queue_msg(0x11, nat, sizeof(nat));
	nev = 0;
	CHECK(touch_tcm_poll() == 0);
	CHECK(nev == 3); /* finger 3 down, plus the two earlier fingers lifting */
	int seen = 0;
	for (unsigned i = 0; i < nev; ++i) {
		if (ev[i].finger == 3) {
			seen = 1;
			CHECK(ev[i].down == 1 && ev[i].x == 1439 && ev[i].y == 3215);
		}
	}
	CHECK(seen);
	printf("native layout: finger 3 at (1439,3215), %u events\n", nev);

	/* lift: a touch report with no active objects */
	queue_msg(0x11, NULL, 0);
	nev = 0;
	CHECK(touch_tcm_poll() == 0);
	/* only finger 3 is still down: the native frame above lifted 0 and 1 */
	CHECK(nev == 1 && ev[0].finger == 3 && ev[0].down == 0);
	printf("lift: %u up event(s)\n", nev);

	printf("test_touch_bb %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
