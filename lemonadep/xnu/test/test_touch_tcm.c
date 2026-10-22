/*
 * Host (ASan/UBSan) test of overlay 1005's TCM message-framing logic
 * (touch_tcm_write / touch_tcm_read), extracted verbatim from the patched
 * bsd/dev/sweet_touch.c by gen_touch.py. touch_msg() below is a mock of the
 * GENI FIFO transport: it records every TX transaction and serves RX
 * transactions from a scripted queue, so these two functions run exactly as
 * they would against real silicon, with no kernel headers and no MMIO.
 *
 * Usage: test_touch_tcm   (each case asserts and aborts on the first
 * mismatch; a clean exit means every case in main() passed)
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { uint8_t i2c_addr; bool bitbang; } st = {0x4B, false};

/* ---- mock GENI transport: records TX, replays scripted RX ---- */
#define MOCK_MAX 32
static struct {
	uint8_t tx[72];
	unsigned nt;
} tx_log[MOCK_MAX];
static unsigned tx_n;
static uint8_t rx_script[MOCK_MAX][96];
static unsigned rx_len[MOCK_MAX];
static unsigned rx_n, rx_i;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void
mock_reset(void)
{
	tx_n = rx_n = rx_i = 0;
}

static void
mock_queue_rx(const uint8_t *bytes, unsigned len)
{
	memcpy(rx_script[rx_n], bytes, len);
	rx_len[rx_n] = len;
	rx_n++;
}

static int
touch_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, bool stretch)
{
	(void)addr;
	(void)stretch;
	if (nt) {
		CHECK(tx_n < MOCK_MAX);
		memcpy(tx_log[tx_n].tx, tx, nt);
		tx_log[tx_n].nt = nt;
		tx_n++;
		return 0;
	}
	CHECK(rx_i < rx_n);
	if (nr != rx_len[rx_i]) {
		printf("FAIL: touch_tcm_read requested %u bytes, script has %u\n", nr, rx_len[rx_i]);
		fails++;
		return EIO;
	}
	memcpy(rx, rx_script[rx_i], nr);
	rx_i++;
	return 0;
}

#include "touch_tcm_frag.inc"

/* ---- cases ---- */

/* Every command carries a 2-byte little-endian length after the command byte
 * (syna_tcm_write_message: "adding two length bytes as part of payload"). The
 * first draft omitted it. FIFO path: at most 2 stream bytes per transfer, the
 * rest continue with CMD_CONTINUE_WRITE (0x01). */
static void
case_write_zero_len(void)
{
	mock_reset();
	st.bitbang = false;
	CHECK(touch_tcm_write(0x02, NULL, 0) == 0);
	CHECK(tx_n == 1);
	CHECK(tx_log[0].nt == 3 && tx_log[0].tx[0] == 0x02 && tx_log[0].tx[1] == 0 && tx_log[0].tx[2] == 0);
	printf("write_zero_len: 1 transfer, [0x02 00 00]\n");
}

static void
case_write_chunked(void)
{
	mock_reset();
	st.bitbang = false;
	uint8_t payload[8] = {1, 6, 8, 8, 16, 9, 16, 3};
	CHECK(touch_tcm_write(0x26, payload, sizeof(payload)) == 0);
	CHECK(tx_n == 5); /* [cmd,8,0] then 4x [0x01,p,p] */
	CHECK(tx_log[0].nt == 3 && tx_log[0].tx[0] == 0x26 && tx_log[0].tx[1] == 8 && tx_log[0].tx[2] == 0);
	CHECK(tx_log[1].nt == 3 && tx_log[1].tx[0] == 0x01 && tx_log[1].tx[1] == 1 && tx_log[1].tx[2] == 6);
	CHECK(tx_log[4].nt == 3 && tx_log[4].tx[0] == 0x01 && tx_log[4].tx[1] == 16 && tx_log[4].tx[2] == 3);
	printf("write_chunked: 5 transfers, [cmd len] then CONTINUE_WRITE\n");
}

static void
case_write_chunked_odd(void)
{
	mock_reset();
	st.bitbang = false;
	uint8_t payload[5] = {1, 2, 3, 4, 5};
	CHECK(touch_tcm_write(0x26, payload, sizeof(payload)) == 0);
	CHECK(tx_n == 4); /* [cmd,5,0] [1,1,2] [1,3,4] [1,5] */
	CHECK(tx_log[3].nt == 2 && tx_log[3].tx[0] == 0x01 && tx_log[3].tx[1] == 5);
	printf("write_chunked_odd: 4 transfers, last carries 1 byte\n");
}

/* Bit-banged bus: the whole packet in one transfer, as the vendor does. */
static void
case_write_bitbang(void)
{
	mock_reset();
	st.bitbang = true;
	uint8_t payload[9] = {1, 6, 8, 8, 16, 9, 16, 3, 0};
	CHECK(touch_tcm_write(0x26, payload, sizeof(payload)) == 0);
	CHECK(tx_n == 1 && tx_log[0].nt == 12);
	CHECK(tx_log[0].tx[0] == 0x26 && tx_log[0].tx[1] == 9 && tx_log[0].tx[2] == 0 &&
	    tx_log[0].tx[3] == 1 && tx_log[0].tx[11] == 0);
	st.bitbang = false;
	printf("write_bitbang: 1 transfer, [0x26 09 00 + 9 bytes]\n");
}

/* STATUS_OK with a zero-length payload: just the 5-byte header+padding read,
 * no continuation (this is the common "nothing new" poll response). */
static void
case_read_empty(void)
{
	mock_reset();
	uint8_t hdr[5] = {0xA5, 0x01 /* STATUS_OK */, 0, 0, 0x5A /* padding */};
	mock_queue_rx(hdr, sizeof(hdr));
	uint8_t code, payload[32];
	unsigned got = 99;
	CHECK(touch_tcm_read(&code, payload, sizeof(payload), &got) == 0);
	CHECK(code == 0x01 && got == 0);
	CHECK(rx_i == 1); /* exactly one read transaction, no continuation */
	printf("read_empty: STATUS_OK, got_len=0, 1 transfer\n");
}

/* payload_length == 1: the whole payload already sits in hdr[4], but the
 * message's trailing padding byte still needs its own continuation read
 * (this exact case was the off-by-one bug the first draft had: it returned
 * early and left the padding byte unread, desyncing the next transaction). */
static void
case_read_one_byte(void)
{
	mock_reset();
	uint8_t hdr[5] = {0xA5, 0x01, 1, 0, 0x42 /* payload[0] */};
	mock_queue_rx(hdr, sizeof(hdr));
	uint8_t cont[3] = {0xA5, 0x03 /* STATUS_CONTINUED_READ */, 0x5A /* padding */};
	mock_queue_rx(cont, sizeof(cont));
	uint8_t code, payload[32];
	unsigned got = 99;
	CHECK(touch_tcm_read(&code, payload, sizeof(payload), &got) == 0);
	CHECK(got == 1 && payload[0] == 0x42);
	CHECK(rx_i == 2); /* header read + the padding-only continuation */
	printf("read_one_byte: got_len=1, 2 transfers (continuation fetches padding)\n");
}

/* A 10-byte payload (two of our 5-byte touch records): 1 byte comes from
 * hdr[4], the other 9 plus the padding byte come from one continuation. */
static void
case_read_multi_byte(void)
{
	mock_reset();
	uint8_t hdr[5] = {0xA5, 0x11 /* REPORT_TOUCH */, 10, 0, 0 /* record0.index */};
	mock_queue_rx(hdr, sizeof(hdr));
	/* record0 = {id=0,x=100,y=200}, record1 = {id=1,x=300,y=400} */
	uint8_t cont[2 + 9 + 1] = {
		0xA5, 0x03,
		100 & 0xff, 100 >> 8, 200 & 0xff, 200 >> 8, /* rest of record0 */
		1, 300 & 0xff, 300 >> 8, 400 & 0xff, 400 >> 8, /* record1 */
		0x5A,
	};
	mock_queue_rx(cont, sizeof(cont));
	uint8_t code, payload[32];
	unsigned got = 99;
	CHECK(touch_tcm_read(&code, payload, sizeof(payload), &got) == 0);
	CHECK(code == 0x11 && got == 10);
	CHECK(payload[0] == 0 && payload[1] == (100 & 0xff) && payload[2] == (100 >> 8));
	CHECK(payload[5] == 1 && payload[7] == (300 >> 8) && payload[8] == (400 & 0xff));
	printf("read_multi_byte: got_len=10, both records assembled in order\n");
}

/* STATUS_BUSY must be treated as zero-payload even if the length field is
 * garbage (an out-of-sync device can send anything there); no continuation
 * read must be attempted on a length the device isn't actually honoring. */
static void
case_read_busy_ignores_length(void)
{
	mock_reset();
	uint8_t hdr[5] = {0xA5, 0x02 /* STATUS_BUSY */, 0xff, 0xff, 0x5A};
	mock_queue_rx(hdr, sizeof(hdr));
	uint8_t code, payload[32];
	unsigned got = 99;
	CHECK(touch_tcm_read(&code, payload, sizeof(payload), &got) == 0);
	CHECK(code == 0x02 && got == 0);
	CHECK(rx_i == 1);
	printf("read_busy_ignores_length: got_len=0 despite length=0xffff\n");
}

/* A bad marker byte must be rejected outright, not parsed as a message. */
static void
case_read_bad_marker(void)
{
	mock_reset();
	uint8_t hdr[5] = {0x00, 0x01, 0, 0, 0};
	mock_queue_rx(hdr, sizeof(hdr));
	uint8_t code, payload[32];
	unsigned got = 99;
	CHECK(touch_tcm_read(&code, payload, sizeof(payload), &got) == ENXIO);
	printf("read_bad_marker: rejected\n");
}

/* A payload_length over max_payload must be rejected, not overrun the
 * caller's buffer. */
static void
case_read_too_long(void)
{
	mock_reset();
	uint8_t hdr[5] = {0xA5, 0x11, 50, 0, 0};
	mock_queue_rx(hdr, sizeof(hdr));
	uint8_t code, payload[8];
	unsigned got = 99;
	CHECK(touch_tcm_read(&code, payload, sizeof(payload), &got) == EIO);
	printf("read_too_long: rejected before touching payload[]\n");
}

int
main(void)
{
	case_write_zero_len();
	case_write_chunked();
	case_write_chunked_odd();
	case_write_bitbang();
	case_read_empty();
	case_read_one_byte();
	case_read_multi_byte();
	case_read_busy_ignores_length();
	case_read_bad_marker();
	case_read_too_long();
	printf("test_touch_tcm %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
