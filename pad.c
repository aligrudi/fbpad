#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "fbpad.h"

static int fbroff, fbcoff, fbrows, fbcols;
static int rows, cols;
static int fnrows, fncols;
static int bpp;
static struct font *fonts[3];

static int gc_init(int grows, int gcols);
static void gc_free(void);

static int pad_font(char *fr, char *fi, char *fb)
{
	struct font *r = fr ? font_open(fr) : NULL;
	if (!r || gc_init(font_rows(r), font_cols(r)))
		return 1;
	font_free(fonts[0]);
	font_free(fonts[1]);
	font_free(fonts[2]);
	fonts[0] = r;
	fonts[1] = fi ? font_open(fi) : NULL;
	fonts[2] = fb ? font_open(fb) : NULL;
	return 0;
}

int pad_init(char *fr, char *fi, char *fb)
{
	if (pad_font(fr, fi, fb))
		return 1;
	fnrows = font_rows(fonts[0]);
	fncols = font_cols(fonts[0]);
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
	gc_free();
	font_free(fonts[0]);
	font_free(fonts[1]);
	font_free(fonts[2]);
}

#define CR(a)		(((a) >> 16) & 0x0000ff)
#define CG(a)		(((a) >> 8) & 0x0000ff)
#define CB(a)		((a) & 0x0000ff)
#define COLORMERGE(f, b, c)		((b) + (((f) - (b)) * (c) >> 8u))

/* this can be optimised based on framebuffer pixel format */
static void fb_set(char *d, unsigned r, unsigned g, unsigned b)
{
	unsigned c = fb_val(r, g, b);
	int i;
	for (i = 0; i < bpp; i++)
		d[i] = (c >> (i << 3)) & 0xff;
}

static void fb_mixed(char *d, int fg, int bg, unsigned val)
{
	unsigned char r = COLORMERGE(CR(fg), CR(bg), val);
	unsigned char g = COLORMERGE(CG(fg), CG(bg), val);
	unsigned char b = COLORMERGE(CB(fg), CB(bg), val);
	fb_set(d, r, g, b);
}

/* glyph bitmap cache: use CGLCNT lists of size CGLLEN each */
#define GCLCNT		(1 << 7)		/* glyph cache list count */
#define GCLLEN		(1 << 4)		/* glyph cache list length */
#define GCN		(GCLCNT * GCLLEN)	/* total glpyhs */
#define GCGLEN(rs, cs)	((rs) * (cs) * 4)	/* bytes to store a glyph */
#define GCIDX(c)	((c) & (GCLCNT - 1))

static char *gc_mem;		/* cached glyph's memory */
static int gc_rows, gc_cols;	/* glyph size */
static int gc_next[GCLCNT];	/* the next slot to use in each list */
static int gc_glyph[GCN];	/* cached glyphs */
static int gc_bg[GCN];
static int gc_fg[GCN];

static int gc_init(int grows, int gcols)
{
	char *mem;
	memset(gc_next, 0, sizeof(gc_next));
	memset(gc_glyph, 0, sizeof(gc_glyph));
	if (gc_mem && grows == gc_rows && gcols == gc_cols)
		return 0;
	if ((mem = malloc(GCLCNT * GCLLEN * GCGLEN(grows, gcols)))) {
		free(gc_mem);
		gc_mem = mem;
		gc_rows = grows;
		gc_cols = gcols;
	}
	return !mem;
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
			return gc_mem + i * GCGLEN(gc_rows, gc_cols);
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
	return gc_mem + idx * GCGLEN(gc_rows, gc_cols);
}

static void bmp2fb(char *d, char *s, int fg, int bg, int nr, int nc)
{
	int i, j;
	for (i = 0; i < fnrows; i++) {
		char *p = d + i * fncols * bpp;
		for (j = 0; j < fncols; j++) {
			unsigned v = i < nr && j < nc ?
				(unsigned char) s[i * nc + j] : 0;
			fb_mixed(p + j * bpp, fg, bg, v);
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
	if (!fonts[fn] || font_bitmap(fonts[fn], bits, c))
		return NULL;
	fbbits = gc_put(c, fg, bg);
	bmp2fb(fbbits, bits, fg & FN_C, bg & FN_C,
		font_rows(fonts[fn]), font_cols(fonts[fn]));
	return fbbits;
}

static void fb_cpy(int r, int c, void *mem, int len)
{
	memcpy(fb_mem(fbroff + r) + (fbcoff + c) * bpp, mem, len * bpp);
}

static void fb_box(int sr, int er, int sc, int ec, int clr)
{
	static char row[32 * 1024];
	static int rowclr;
	static int rowwid;
	int i;
	if (rowclr != clr || rowwid < ec - sc) {
		fb_set(row, CR(clr), CG(clr), CB(clr));
		for (i = 1; i < ec - sc; i++)
			memcpy(row + i * bpp, row, bpp);
		rowclr = clr;
		rowwid = ec - sc;
	}
	for (i = sr; i < er; i++)
		fb_cpy(i, sc, row, ec - sc);
}

void pad_border(unsigned c, int wid)
{
	if (fbroff < wid || fbcoff < wid)
		return;
	fb_box(-wid, 0, -wid, fbcols + wid, c & FN_C);
	fb_box(fbrows, fbrows + wid, -wid, fbcols + wid, c & FN_C);
	fb_box(-wid, fbrows + wid, -wid, 0, c & FN_C);
	fb_box(-wid, fbrows + wid, fbcols, fbcols + wid, c & FN_C);
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
	if (r >= rows && c >= cols)
		return;
	bits = ch2fb(fnsel(fg, bg), ch, fg, bg);
	if (!bits)
		bits = ch2fb(0, ch, fg, bg);
	if (!bits)
		fb_box(sr, sr + fnrows, sc, sc + fncols, bg & FN_C);
	else
		for (i = 0; i < fnrows; i++)
			fb_cpy(sr + i, sc, bits + (i * fncols * bpp), fncols);
}

void pad_fill(int sr, int er, int sc, int ec, int c)
{
	int fber = er >= 0 ? er * fnrows : fbrows;
	int fbec = ec >= 0 ? ec * fncols : fbcols;
	fb_box(sr * fnrows, MIN(fber, fbrows),
		sc * fncols, MIN(fbec, fbcols), c & FN_C);
}

int pad_rows(void)
{
	return rows;
}

int pad_cols(void)
{
	return cols;
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
