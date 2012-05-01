#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "draw.h"
#include "font.h"
#include "util.h"
#include "pad.h"

#define NCACHE		(1 << 11)
#define MAXFBWIDTH	(1 << 12)

static unsigned int cd[] = {
	COLOR0, COLOR1, COLOR2, COLOR3,
	COLOR4, COLOR5, COLOR6, COLOR7,
	COLOR8, COLOR9, COLOR10, COLOR11,
	COLOR12, COLOR13, COLOR14, COLOR15};
static int rows, cols;
static int fnrows, fncols;
static struct font *fonts[32];
static int nfonts;
static int font;

static int fnsets[16][3];
static char *fnpaths[][3] = {F0, F1, F2, F3, F4};

static int fontadd(char *path)
{
	fonts[nfonts] = font_open(path);
	if (!fonts[nfonts]) {
		fprintf(stderr, "pad: bad font <%s>\n", path);
		return -1;
	}
	return nfonts++;
}

int pad_init(void)
{
	int i, j;
	if (fb_init())
		return 1;
	if (sizeof(fbval_t) != FBM_BPP(fb_mode())) {
		fprintf(stderr, "pad_init: fbval_t doesn't match fb depth\n");
		return 1;
	}
	for (i = 0; i < ARRAY_SIZE(fnpaths); i++) {
		for (j = 0; j < 3; j++) {
			int fn = -1;
			if (fnpaths[i][j])
				if ((fn = fontadd(fnpaths[i][j])) < 0)
					return 1;
			if (fn < 0 && j > 0)
				fn = fnsets[i][0];
			if (fn < 0 && i > 0)
				fn = fnsets[0][j];
			fnsets[i][j] = fn;
		}
	}
	pad_font(0);
	return 0;
}

void pad_free(void)
{
	int i;
	for (i = 0; i < nfonts; i++)
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

static fbval_t cache[NCACHE][MAXDOTS];
static struct glyph {
	int c;
	int fn;
	short fg, bg;
} glyphs[NCACHE];

static struct glyph *glyph_entry(int c, int fn, int fg, int bg)
{
	return &glyphs[((c - 32) ^ (fg << 6) ^ (bg << 5) ^ (fn << 8)) & (NCACHE - 1)];
}

static fbval_t *glyph_cache(int c, int fn, short fg, short bg)
{
	struct glyph *g = glyph_entry(c, fn, fg, bg);
	if (g->c == c && g->fn == fn && g->fg == fg && g->bg == bg)
		return cache[g - glyphs];
	return NULL;
}

static fbval_t *glyph_add(int c, int fn, short fg, short bg)
{
	struct glyph *g = glyph_entry(c, fn, fg, bg);
	g->c = c;
	g->fg = fg;
	g->bg = bg;
	g->fn = fn;
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
	char bits[MAXDOTS];
	fbval_t *fbbits;
	if (c < 0 || (c < 256 && (!isprint(c) || isspace(c))))
		return NULL;
	if ((fbbits = glyph_cache(c, fn, fg, bg)))
		return fbbits;
	if (font_bitmap(fonts[fn], bits, c))
		return NULL;
	fbbits = glyph_add(c, fn, fg, bg);
	bmp2fb(fbbits, bits, FN_C(fg), FN_C(bg),
		font_rows(fonts[fn]), font_cols(fonts[fn]));
	return fbbits;
}

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

static int fnsel(int fg, int bg)
{
	if ((fg | bg) & FN_B)
		return fnsets[font][2];
	if ((fg | bg) & FN_I)
		return fnsets[font][1];
	return fnsets[font][0];
}

void pad_put(int ch, int r, int c, int fg, int bg)
{
	int sr = fnrows * r;
	int sc = fncols * c;
	int i;
	fbval_t *bits = ch2fb(fnsel(fg, bg), ch, fg, bg);
	if (!bits)
		bits = ch2fb(0, ch, fg, bg);
	if (!bits)
		fb_box(sr, sc, sr + fnrows, sc + fncols, color2fb(FN_C(bg)));
	else
		for (i = 0; i < fnrows; i++)
			fb_set(sr + i, sc, bits + (i * fncols), fncols);
}

void pad_blank(int c)
{
	fb_box(0, 0, fb_rows(), fb_cols(), color2fb(FN_C(c)));
}

void pad_blankrow(int r, int bg)
{
	int sr = r * fnrows;
	int er = r == rows - 1 ? fb_rows() : (r + 1) * fnrows;
	fb_box(sr, 0, er, fb_cols(), color2fb(FN_C(bg)));
}

int pad_rows(void)
{
	return rows;
}

int pad_cols(void)
{
	return cols;
}

void pad_font(int i)
{
	font = i < ARRAY_SIZE(fnpaths) ? i : 0;
	fnrows = font_rows(fonts[fnsets[0][0]]);
	fncols = font_cols(fonts[fnsets[0][0]]);
	rows = fb_rows() / fnrows;
	cols = fb_cols() / fncols;
}
