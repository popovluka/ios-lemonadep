/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint8_t *con_fb = (volatile uint8_t *)FB_BASE;
uint32_t con_w = FB_W, con_h = FB_H, con_stride = FB_STRIDE, con_bpp = FB_BPP;
uintptr_t con_kick;
static uint32_t con_bg;
static int row;

void con_setup(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp)
{
	con_fb = (volatile uint8_t *)base;
	con_w = w;
	con_h = h;
	con_stride = stride;
	con_bpp = bpp;
}

static void kick(void)
{
	if (con_kick) {
#if defined(__aarch64__)
		__asm__ volatile("dsb sy" ::: "memory");
#endif
		*(volatile uint32_t *)con_kick = 1;
	}
}

static void put_px(volatile uint8_t *line, uint32_t x, uint32_t c)
{
	switch (con_bpp) {
	case 4:
		((volatile uint32_t *)line)[x] = c;
		break;
	case 3:
		line[3 * x] = c;
		line[3 * x + 1] = c >> 8;
		line[3 * x + 2] = c >> 16;
		break;
	case 2:	/* r5g6b5 */
		((volatile uint16_t *)line)[x] = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f);
		break;
	}
}

void con_clear(uint32_t bg)
{
	for (uint32_t y = 0; y < con_h; y++) {
		volatile uint8_t *line = con_fb + y * con_stride;
		if (con_bpp == 4) {
			volatile uint32_t *l32 = (volatile uint32_t *)line;
			for (uint32_t x = 0; x < con_w; x++)
				l32[x] = bg;
		} else {
			for (uint32_t x = 0; x < con_w; x++)
				put_px(line, x, bg);
		}
	}
	con_bg = bg;
	row = 0;
	kick();
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > con_w || y0 + CH_H > con_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint8_t *line = con_fb + (y0 + gy * SCALE + sy) * con_stride;
				for (int sx = 0; sx < SCALE; sx++)
					put_px(line, x0 + gx * SCALE + sx, c);
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
	kick();
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
	kick();
}
