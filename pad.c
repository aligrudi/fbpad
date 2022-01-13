#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "conf.h"
#include "draw.h"
#include "fbpad.h"

static int fbroff, fbcoff, fbrows, fbcols;
static int rows, cols;
static int fnrows, fncols;
static int bpp;
static struct font *fonts[3];

static int gc_init(void);
static void gc_free(void);

int pad_init(void)
{
	if (pad_font(FR, FI, FB))
		return 1;
	if (gc_init())
		return 1;
	rows = fb_rows() / fnrows;
	cols = fb_cols() / fncols;
	bpp = FBM_BPP(fb_mode());
	pad_conf(0, 0, fb_rows(), fb_cols());
	return 0;
}

void pad_conf(int roff, int coff, int _rows, int _cols)
{
	fbroff = roff;
	fbcoff = coff;
	fbrows = _rows;
	fbcols = _cols;
	rows = fbrows / fnrows;
	cols = fbcols / fncols;
}

void pad_free(void)
{
	int i;
	gc_free();
	for (i = 0; i < 3; i++)
		if (fonts[i])
			font_free(fonts[i]);
}

#define CR(a)		(((a) >> 16) & 0x0000ff)
#define CG(a)		(((a) >> 8) & 0x0000ff)
#define CB(a)		((a) & 0x0000ff)
#define COLORMERGE(f, b, c)		((b) + (((f) - (b)) * (c) >> 8u))

static unsigned mixed_color(int fg, int bg, unsigned val)
{
	unsigned char r = COLORMERGE(CR(fg), CR(bg), val);
	unsigned char g = COLORMERGE(CG(fg), CG(bg), val);
	unsigned char b = COLORMERGE(CB(fg), CB(bg), val);
	return FB_VAL(r, g, b);
}

static unsigned color2fb(int c)
{
	return FB_VAL(CR(c), CG(c), CB(c));
}

/* glyph bitmap cache: use CGLCNT lists of size CGLLEN each */
#define GCLCNT		(1 << 7)		/* glyph cache list count */
#define GCLLEN		(1 << 4)		/* glyph cache list length */
#define GCGLEN		(fnrows * fncols * 4)	/* bytes to store a glyph */
#define GCN		(GCLCNT * GCLLEN)	/* total glpyhs */
#define GCIDX(c)	((c) & (GCLCNT - 1))

static char *gc_mem;		/* cached glyph's memory */
static int gc_next[GCLCNT];	/* the next slot to use in each list */
static int gc_glyph[GCN];	/* cached glyphs */
static int gc_bg[GCN];
static int gc_fg[GCN];

static int gc_init(void)
{
	gc_mem = malloc(GCLCNT * GCLLEN * GCGLEN);
	return !gc_mem;
}

static void gc_free(void)
{
	free(gc_mem);
}

static char *gc_get(int c, int fg, int bg)
{
	int idx = GCIDX(c) * GCLLEN;
	int i;
	for (i = idx; i < idx + GCLLEN; i++)
		if (gc_glyph[i] == c && gc_fg[i] == fg && gc_bg[i] == bg)
			return gc_mem + i * GCGLEN;
	return NULL;
}

static char *gc_put(int c, int fg, int bg)
{
	int lst = GCIDX(c);
	int pos = gc_next[lst]++;
	int idx = lst * GCLLEN + pos;
	if (gc_next[lst] >= GCLLEN)
		gc_next[lst] = 0;
	gc_glyph[idx] = c;
	gc_fg[idx] = fg;
	gc_bg[idx] = bg;
	return gc_mem + idx * GCGLEN;
}

static void bmp2fb(char *d, char *s, int fg, int bg, int nr, int nc)
{
	int i, j, k;
	for (i = 0; i < fnrows; i++) {
		char *p = d + i * fncols * bpp;
		for (j = 0; j < fncols; j++) {
			unsigned v = i < nr && j < nc ?
				(unsigned char) s[i * nc + j] : 0;
			unsigned c = mixed_color(fg, bg, v);
			for (k = 0; k < bpp; k++)	/* little-endian */
				*p++ = (c >> (k * 8)) & 0xff;
		}
	}
}

static char *ch2fb(int fn, int c, int fg, int bg)
{
	static char bits[1024 * 4];
	char *fbbits;
	if (c < 0 || (c < 128 && (!isprint(c) || isspace(c))))
		return NULL;
	if ((fbbits = gc_get(c, fg, bg)))
		return fbbits;
	if (font_bitmap(fonts[fn], bits, c))
		return NULL;
	fbbits = gc_put(c, fg, bg);
	bmp2fb(fbbits, bits, fg & FN_C, bg & FN_C,
		font_rows(fonts[fn]), font_cols(fonts[fn]));
	return fbbits;
}

static void fb_set(int r, int c, void *mem, int len)
{
	memcpy(fb_mem(fbroff + r) + (fbcoff + c) * bpp, mem, len * bpp);
}

static char *rowbuf(unsigned c, int len)
{
	static char row[32 * 1024];
	char *p = row;
	int i, k;
	for (i = 0; i < len; i++)
		for (k = 0; k < bpp; k++)	/* little-endian */
			*p++ = (c >> (k * 8)) & 0xff;
	return row;
}

static void fb_box(int sr, int er, int sc, int ec, unsigned val)
{
	char *row = rowbuf(val, ec - sc);
	int i;
	for (i = sr; i < er; i++)
		fb_set(i, sc, row, ec - sc);
}

void pad_border(unsigned c, int wid)
{
	int v = color2fb(c & FN_C);
	if (fbroff < wid || fbcoff < wid)
		return;
	fb_box(-wid, 0, -wid, fbcols + wid, v);
	fb_box(fbrows, fbrows + wid, -wid, fbcols + wid, v);
	fb_box(-wid, fbrows + wid, -wid, 0, v);
	fb_box(-wid, fbrows + wid, fbcols, fbcols + wid, v);
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
	char *bits;
	int i;
	bits = ch2fb(fnsel(fg, bg), ch, fg, bg);
	if (!bits)
		bits = ch2fb(0, ch, fg, bg);
	if (!bits)
		fb_box(sr, sr + fnrows, sc, sc + fncols, color2fb(bg & FN_C));
	else
		for (i = 0; i < fnrows; i++)
			fb_set(sr + i, sc, bits + (i * fncols * bpp), fncols);
}

void pad_fill(int sr, int er, int sc, int ec, int c)
{
	int fber = er >= 0 ? er * fnrows : fbrows;
	int fbec = ec >= 0 ? ec * fncols : fbcols;
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
	struct font *r = fr ? font_open(fr) : NULL;
	if (!r)
		return 1;
	fonts[0] = r;
	fonts[1] = fi ? font_open(fi) : NULL;
	fonts[2] = fb ? font_open(fb) : NULL;
	fnrows = font_rows(fonts[0]);
	fncols = font_cols(fonts[0]);
	return 0;
}

char *pad_fbdev(void)
{
	static char fbdev[1024];
	snprintf(fbdev, sizeof(fbdev), "FBDEV=%s:%dx%d%+d%+d",
		fb_dev(), fbcols, fbrows, fbcoff, fbroff);
	return fbdev;
}

/* character height */
int pad_crows(void)
{
	return fnrows;
}

/* character width */
int pad_ccols(void)
{
	return fncols;
}
