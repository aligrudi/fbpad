#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "font.h"
#include "util.h"
#include "pad.h"

static int rows, cols;
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

static fbval_t mixed_color(int fg, int bg, unsigned char val)
{
	unsigned int fore = cd[fg], back = cd[bg];
	unsigned char r = COLORMERGE(CR(fore), CR(back), val);
	unsigned char g = COLORMERGE(CG(fore), CG(back), val);
	unsigned char b = COLORMERGE(CB(fore), CB(back), val);
	return fb_color(r, g, b);
}

static fbval_t color2fb(int c)
{
	return fb_color(CR(cd[c]), CG(cd[c]), CB(cd[c]));
}

#define NCACHE		((1 << 11) - 1)
static fbval_t cache[NCACHE * MAXDOTS];
static struct glyph {
	int c;
	short fg, bg;
} cacheid[NCACHE];

static int glyph_hash(struct glyph *g)
{
	return (g->c | (((g->fg + 1) ^ g->bg) << 7)) % NCACHE;
}

static fbval_t *bitmap(int c, short fg, short bg)
{
	unsigned char *bits;
	fbval_t *fbbits;
	struct glyph glyph = {0};
	int hash;
	int i;
	int nbits = font_rows() * font_cols();
	if (!isprint(c) || isspace(c))
		return NULL;
	glyph.c = c;
	glyph.fg = fg;
	glyph.bg = bg;
	hash = glyph_hash(&glyph);
	fbbits = &cache[hash * MAXDOTS];
	if (!memcmp(&glyph, &cacheid[hash], sizeof(glyph)))
		return fbbits;
	cacheid[hash] = glyph;
	bits = font_bitmap(c, fg >= 8);
	for (i = 0; i < nbits; i++)
		fbbits[i] = mixed_color(fg, bg, bits[i]);
	return fbbits;
}

void pad_put(int ch, int r, int c, int fg, int bg)
{
	int sr = font_rows() * r;
	int sc = font_cols() * c;
	int frows = font_rows(), fcols = font_cols();
	int i;
	fbval_t *bits = bitmap(ch, fg, bg);
	if (!bits)
		fb_box(sr, sc, sr + frows, sc + fcols, color2fb(bg));
	else
		for (i = 0; i < frows; i++)
			fb_set(sr + i, sc, bits + (i * fcols), fcols);
}

void pad_scroll(int sr, int nr, int n, int c)
{
	fb_scroll(sr * font_rows(), nr * font_rows(),
		  n * font_rows(), color2fb(c));
}

void pad_blank(int c)
{
	fb_box(0, 0, fb_rows(), fb_cols(), color2fb(c));
}

int pad_rows(void)
{
	return rows;
}

int pad_cols(void)
{
	return cols;
}

void pad_shown(void)
{
	fb_cmap();
}
