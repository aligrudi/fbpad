#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "draw.h"
#include "fbpad.h"

#define NCACHE		(1 << 11)

static unsigned int cd[256] = {
	COLOR0, COLOR1, COLOR2, COLOR3,
	COLOR4, COLOR5, COLOR6, COLOR7,
	COLOR8, COLOR9, COLOR10, COLOR11,
	COLOR12, COLOR13, COLOR14, COLOR15};
static int rows, cols;
static int fnrows, fncols;
static struct font *fonts[3];

int pad_init(void)
{
	int r, g, b;
	if (fb_init())
		return 1;
	if (sizeof(fbval_t) != FBM_BPP(fb_mode())) {
		fprintf(stderr, "pad_init: fbval_t doesn't match fb depth\n");
		return 1;
	}
	if (pad_font(FR, FI, FB) < 0)
		return -1;
	fnrows = font_rows(fonts[0]);
	fncols = font_cols(fonts[0]);
	rows = fb_rows() / fnrows;
	cols = fb_cols() / fncols;
	for (r = 0; r < 6; r++) {
		for (g = 0; g < 6; g++) {
			for (b = 0; b < 6; b++) {
				int idx = 16 + r * 36 + g * 6 + b;
				cd[idx] = ((r * 40 + 55) << 16) |
						((g * 40 + 55) << 8) |
						(b * 40 + 55);
			}
		}
	}
	for (r = 0; r < 24; r++)
		cd[232 + r] = ((r * 10 + 8) << 16) |
				((r * 10 + 8) << 8) | (r * 10 + 8);
	return 0;
}

void pad_free(void)
{
	int i;
	for (i = 0; i < 3; i++)
		if (fonts[i])
			font_free(fonts[i]);
	fb_free();
}

#define CR(a)		(((a) >> 16) & 0x0000ff)
#define CG(a)		(((a) >> 8) & 0x0000ff)
#define CB(a)		((a) & 0x0000ff)
#define COLORMERGE(f, b, c)		((b) + (((f) - (b)) * (c) >> 8u))

static unsigned mixed_color(int fg, int bg, unsigned val)
{
	unsigned int fore = cd[fg], back = cd[bg];
	unsigned char r = COLORMERGE(CR(fore), CR(back), val);
	unsigned char g = COLORMERGE(CG(fore), CG(back), val);
	unsigned char b = COLORMERGE(CB(fore), CB(back), val);
	return FB_VAL(r, g, b);
}

static unsigned color2fb(int c)
{
	return FB_VAL(CR(cd[c]), CG(cd[c]), CB(cd[c]));
}

static fbval_t cache[NCACHE][NDOTS];
static struct glyph {
	int c;
	short fg, bg;
} glyphs[NCACHE];

static struct glyph *glyph_entry(int c, int fg, int bg)
{
	return &glyphs[((c - 32) ^ (fg << 6) ^ (bg << 5)) & (NCACHE - 1)];
}

static fbval_t *glyph_cache(int c, short fg, short bg)
{
	struct glyph *g = glyph_entry(c, fg, bg);
	if (g->c == c && g->fg == fg && g->bg == bg)
		return cache[g - glyphs];
	return NULL;
}

static fbval_t *glyph_add(int c, short fg, short bg)
{
	struct glyph *g = glyph_entry(c, fg, bg);
	g->c = c;
	g->fg = fg;
	g->bg = bg;
	return cache[g - glyphs];
}

static void bmp2fb(fbval_t *d, char *s, int fg, int bg, int nr, int nc)
{
	int i, j;
	for (i = 0; i < fnrows; i++) {
		for (j = 0; j < fncols; j++) {
			unsigned v = i < nr && j < nc ?
				(unsigned char) s[i * nc + j] : 0;
			d[i * fncols + j] = mixed_color(fg, bg, v);
		}
	}
}

static fbval_t *ch2fb(int fn, int c, short fg, short bg)
{
	char bits[NDOTS];
	fbval_t *fbbits;
	if (c < 0 || (c < 128 && (!isprint(c) || isspace(c))))
		return NULL;
	if ((fbbits = glyph_cache(c, fg, bg)))
		return fbbits;
	if (font_bitmap(fonts[fn], bits, c))
		return NULL;
	fbbits = glyph_add(c, fg, bg);
	bmp2fb(fbbits, bits, fg & FN_C, bg & FN_C,
		font_rows(fonts[fn]), font_cols(fonts[fn]));
	return fbbits;
}

static void fb_box(int sr, int er, int sc, int ec, fbval_t val)
{
	static fbval_t line[32 * NCOLS];
	int cn = ec - sc;
	int i;
	for (i = 0; i < cn; i++)
		line[i] = val;
	for (i = sr; i < er; i++)
		fb_set(i, sc, line, cn);
}

static int fnsel(int fg, int bg)
{
	if ((fg | bg) & FN_B)
		return fonts[2] ? 2 : 0;
	if ((fg | bg) & FN_I)
		return fonts[1] ? 1 : 0;
	return 0;
}

void pad_put(int ch, int r, int c, int fg, int bg)
{
	int sr = fnrows * r;
	int sc = fncols * c;
	fbval_t *bits;
	int i;
	if ((fg & 0xfff8) == FN_B && !fonts[2])
		fg |= 8;		/* increase intensity of no FB */
	bits = ch2fb(fnsel(fg, bg), ch, fg, bg);
	if (!bits)
		bits = ch2fb(0, ch, fg, bg);
	if (!bits)
		fb_box(sr, sr + fnrows, sc, sc + fncols, color2fb(bg & FN_C));
	else
		for (i = 0; i < fnrows; i++)
			fb_set(sr + i, sc, bits + (i * fncols), fncols);
}

void pad_fill(int sr, int er, int sc, int ec, int c)
{
	int fber = er >= 0 ? er * fnrows : fb_rows();
	int fbec = ec >= 0 ? ec * fncols : fb_cols();
	fb_box(sr * fnrows, fber, sc * fncols, fbec, color2fb(c & FN_C));
}

int pad_rows(void)
{
	return rows;
}

int pad_cols(void)
{
	return cols;
}

int pad_font(char *fr, char *fi, char *fb)
{
	char *fns[] = {fr, fi, fb};
	int i;
	for (i = 0; i < 3; i++) {
		if (fonts[i])
			font_free(fonts[i]);
		fonts[i] = fns[i] ? font_open(fns[i]) : NULL;
	}
	memset(glyphs, 0, sizeof(glyphs));
	if (!fonts[0])
		fprintf(stderr, "pad: bad font <%s>\n", fr);
	return fonts[0] ? 0 : -1;
}
