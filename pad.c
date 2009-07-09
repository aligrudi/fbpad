#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "font.h"
#include "util.h"

#define MAXSQUARES	1 << 15
#define SQRADDR(r, c)		(&screen[(r) * cols + (c)])

static int rows, cols;
static int fg, bg;

static struct square {
	int c;
	int fg;
	int bg;
} screen[MAXSQUARES];

static unsigned int cd[] = {
	0x0a0a0a, 0xc04444, 0x339933, 0xcccc66,
	0x5566bc, 0xcd66af, 0xa166cd, 0xeeeeee,
	0x71a3b7, 0xc08888, 0x779977, 0xcccc99,
	0x8899bc, 0xcd99af, 0xa199cd, 0xdedede};

void pad_init(void)
{
	fb_init();
	font_init();
	rows = fb_rows() / font_rows();
	cols = fb_cols() / font_cols();
}

void pad_free(void)
{
	fb_free();
}

#define CR(a)		(((a) >> 16) & 0x0000ff)
#define CG(a)		(((a) >> 8) & 0x0000ff)
#define CB(a)		((a) & 0x0000ff)
#define COLORMERGE(f, b, c)		((b) + (((f) - (b)) * (c) >> 8u))

static u16_t mixed_color(int fg, int bg, u8_t val)
{
	unsigned int fore = cd[fg], back = cd[bg];
	u8_t r = COLORMERGE(CR(fore), CR(back), val);
	u8_t g = COLORMERGE(CG(fore), CG(back), val);
	u8_t b = COLORMERGE(CB(fore), CB(back), val);
	return fb_color(r, g, b);
}

static u16_t color2fb(int c)
{
	return mixed_color(fg, c, 0);
}

void pad_fg(int c)
{
	fg = c;
}

void pad_bg(int c)
{
	bg = c;
}

void pad_show(int r, int c, int reverse)
{
	int sr = font_rows() * r;
	int sc = font_cols() * c;
	struct square *sqr = SQRADDR(r, c);
	int fgcolor = sqr->c ? sqr->fg : fg;
	int bgcolor = sqr->c ? sqr->bg : bg;
	int i;
	char *bits;
	if (reverse) {
		int t = bgcolor;
		bgcolor = fgcolor;
		fgcolor = t;
	}
	fb_box(sr, sc, sr + font_rows(), sc + font_cols(), color2fb(bgcolor));
	if (!isprint(sqr->c))
		return;
	bits = font_bitmap(sqr->c, 0);
	for (i = 0; i < font_rows() * font_cols(); i++)
		if (bits[i])
			fb_put(sr + i / font_cols(), sc + i % font_cols(),
				mixed_color(fgcolor, bgcolor, bits[i]));
}

void pad_put(int ch, int r, int c)
{
	struct square *sqr = SQRADDR(r, c);
	if (!ch || !strchr("\a\b\f\n\r\v", ch)) {
		sqr->c = ch;
		sqr->fg = fg;
		sqr->bg = bg;
	}
	pad_show(r, c, 0);
}

static void pad_empty(int sr, int er)
{
	memset(SQRADDR(sr, 0), 0, (er - sr) * sizeof(screen[0]) * cols);
}

void pad_scroll(int sr, int nr, int n)
{
	fb_scroll(sr * font_rows(), nr * font_rows(),
		  n * font_rows(), color2fb(bg));
	memmove(SQRADDR(sr + n, 0), SQRADDR(sr, 0),
		nr * cols * sizeof(screen[0]));
	if (n > 0)
		pad_empty(sr, sr + n);
	else
		pad_empty(sr + nr + n, sr + nr);
}

void pad_blank(void)
{
	fb_box(0, 0, fb_rows(), fb_cols(), color2fb(bg));
	memset(screen, 0, sizeof(screen));
}

int pad_rows(void)
{
	return rows;
}

int pad_cols(void)
{
	return cols;
}
