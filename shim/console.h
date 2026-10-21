#pragma once
#include <stdint.h>
#include "board.h"

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define BLACK	0xff000000U

/*
 * Framebuffer the console draws into. Starts as the board's compiled-in
 * splash (sweet: fully known); a board whose geometry is only known at run
 * time (lemonadep) has width 0 here and nothing is drawn until con_setup().
 * Colours are a8r8g8b8; on other formats only black and white are exact.
 */
extern volatile uint8_t *con_fb;
extern uint32_t con_w, con_h, con_stride, con_bpp;
/* Nonzero: MMIO register to write 1 after every update (command-mode panel kick). */
extern uintptr_t con_kick;

void con_setup(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp);
void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
