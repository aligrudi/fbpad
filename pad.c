#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "draw.h"
#include "font.h"
#include "util.h"
#include "pad.h"

static unsigned int cd[] = {
	COLOR0, COLOR1, COLOR2, COLOR3,
	COLOR4, COLOR5, COLOR6, COLOR7,
	COLOR8, COLOR9, COLOR10, COLOR11,
	COLOR12, COLOR13, COLOR14, COLOR15};
static int rows, cols;

int pad_init(void)
{
	if (fb_init())
		return 1;
	if (sizeof(fbval_t) != FBM_BPP(fb_mode())) {
		fprintf(stderr, "pad_init: fbval_t doesn't match fb depth\n");
		return 1;
	}
	font_init();
	rows = fb_rows() / font_rows();
	cols = fb_cols() / font_cols();
	return 0;
}

void pad_free(void)
{
	fb_free();
}

#define CR(a)		(((a) >> 16) & 0x0000ff)
#define CG(a)		(((a) >> 8) & 0x0000ff)
#define CB(a)		((a) & 0x0000ff)
#define COLORMERGE(f, b, c)		((b) + (((f) - (b)) * (c) >> 8u))

static unsigned mixed_color(int fg, int bg, unsigned char val)
{
	unsigned int fore = cd[fg], back = cd[bg];
	unsigned char r = COLORMERGE(CR(fore), CR(back), val);
	unsigned char g = COLORMERGE(CG(fore), CG(back), val);
	unsigned char b = COLORMERGE(CB(fore), CB(back), val);
	return fb_val(r, g, b);
}

static unsigned color2fb(int c)
{
	return fb_val(CR(cd[c]), CG(cd[c]), CB(cd[c]));
}

#define NCACHE		(1 << 11)
static fbval_t cache[NCACHE * MAXDOTS];
static struct glyph {
	int c;
	short fg, bg;
} cacheid[NCACHE];

static int glyph_hash(struct glyph *g)
{
	return (g->c ^ (g->fg << 7) ^ (g->bg << 6)) & (NCACHE - 1);
}

static fbval_t *bitmap(int c, short fg, short bg)
{
	unsigned char *bits;
	fbval_t *fbbits;
	struct glyph glyph = {0};
	int hash;
	int i;
	int nbits = font_rows() * font_cols();
	if (c < 0 || (c < 256 && (!isprint(c) || isspace(c))))
		return NULL;
	glyph.c = c;
	glyph.fg = fg;
	glyph.bg = bg;
	hash = glyph_hash(&glyph);
	fbbits = &cache[hash * MAXDOTS];
	if (!memcmp(&glyph, &cacheid[hash], sizeof(glyph)))
		return fbbits;
	bits = font_bitmap(c);
	if (!bits)
		return NULL;
	memcpy(&cacheid[hash], &glyph, sizeof(glyph));
	for (i = 0; i < nbits; i++)
		fbbits[i] = mixed_color(fg, bg, bits[i]);
	return fbbits;
}

#define MAXFBWIDTH	(1 << 12)
static void fb_box(int sr, int sc, int er, int ec, fbval_t val)
{
	static fbval_t line[MAXFBWIDTH];
	int cn = ec - sc;
	int i;
	for (i = 0; i < cn; i++)
		line[i] = val;
	for (i = sr; i < er; i++)
		fb_set(i, sc, line, cn);
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

void pad_blank(int c)
{
	fb_box(0, 0, fb_rows(), fb_cols(), color2fb(c));
}

void pad_blankrow(int r, int bg)
{
	int sr = r * font_rows();
	int er = r == rows - 1 ? fb_rows() : (r + 1) * font_rows();
	fb_box(sr, 0, er, fb_cols(), color2fb(bg));
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
