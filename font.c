#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "fbpad.h"

struct font {
	int rows, cols;	/* glyph bitmap rows and columns */
	int n;		/* number of font glyphs */
	void *raw;	/* raw allocated data */
	int *gmap;	/* glyph unicode character codes */
	char *gdat;	/* glyph pixels (cols * rows bytes per glyph) */
	int glen;	/* glyph data size per pixel (PSF fonts) */
	int *gidx;	/* glyph index (PSF fonts) */
};

/*
 * This tinyfont header is followed by:
 *
 * glyphs[n]	unicode character codes (int)
 * bitmaps[n]	character bitmaps (char[rows * cols])
 */
struct tinyfont {
	char sig[8];		/* tinyfont signature; "tinyfont" */
	unsigned ver;		/* version; 0 */
	unsigned n;		/* number of glyphs */
	unsigned rows, cols;	/* glyph dimensions */
};

struct psf2 {
	uint32_t magic;
	uint32_t version;
	uint32_t headersize;
	uint32_t flags;
	uint32_t glyphcount;
	uint32_t glyphsize;
	uint32_t height;
	uint32_t width;
};

static void *readfully(int fd, long *len)
{
	char buf[16 << 10];
	void *out = NULL;
	long out_n = 0;
	long out_sz = 0;
	long nr;
	while ((nr = read(fd, buf, sizeof(buf))) > 0) {
		if (out_n + nr > out_sz) {
			char *new;
			out_sz = out_sz + (128 << 10);
			if (!(new = malloc(out_sz * sizeof(new)))) {
				free(out);
				return NULL;
			}
			memcpy(new, out, out_n);
			free(out);
			out = new;
		}
		memcpy(out + out_n, buf, nr);
		out_n += nr;
	}
	*len = out_n;
	return out;
}

static int uc_code(char *s, int len)
{
	int c = (unsigned char) s[0];
	if (~c & 0xc0)
		return c;
	if (~c & 0x20 && len > 1)
		return ((c & 0x1f) << 6) | (s[1] & 0x3f);
	if (~c & 0x10 && len > 2)
		return ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
	if (~c & 0x08 && len > 3)
		return ((c & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
	return c;
}

/* for sorting glyphs in PSF fonts */
struct ginfo {
	int idx;
	int uc;
};
static int ginfo_cmp(const void *g1, const void *g2)
{
	return ((struct ginfo *) g1)->uc - ((struct ginfo *) g2)->uc;
}

struct font *font_open(char *path)
{
	struct font *font;
	char *tf_sig = "tinyfont";
	char *psf_sig = "\x72\xb5\x4a\x86";
	int fd = open(path, O_RDONLY);
	long len;
	char *raw = readfully(fd, &len);
	struct tinyfont *tf = (void *) raw;
	struct psf2 *psf = (void *) raw;
	close(fd);
	if (len > sizeof(*tf) && !memcmp(tf_sig, raw, 8)) {
		int fsize = sizeof(*tf) + (tf->rows * tf->cols + sizeof(int)) * tf->n;
		if (len < fsize || !(font = malloc(sizeof(*font))))
			goto nofont;
		memset(font, 0, sizeof(*font));
		font->raw = raw;
		font->n = tf->n;
		font->rows = tf->rows;
		font->cols = tf->cols;
		font->gmap = (void *) raw + sizeof(*tf);
		font->gdat = (void *) raw + sizeof(*tf) + tf->n * sizeof(int);
		return font;
	}
	if (len > sizeof(*psf) && !memcmp(psf_sig, raw, 4)) {
		int fsize = sizeof(*tf) + (psf->glyphcount * psf->glyphsize) * tf->n;
		int i;
		if (len < fsize || !(font = malloc(sizeof(*font))))
			goto nofont;
		memset(font, 0, sizeof(*font));
		font->raw = raw;
		font->n = psf->glyphcount;
		font->rows = psf->height;
		font->cols = psf->width;
		font->glen = psf->glyphsize;
		font->gdat = raw + psf->headersize;
		font->gmap = malloc(font->n * sizeof(font->gmap[0]));
		font->gidx = malloc(font->n * sizeof(font->gmap[0]));
		if (!font->gmap || !font->gidx) {
			font_free(font);
			return NULL;
		}
		for (i = 0; i < font->n; i++)
			font->gmap[i] = i;
		for (i = 0; i < font->n; i++)
			font->gidx[i] = i;
		if (psf->flags & 0x01) {
			char *pos = raw + fsize;
			char *end = raw + len;
			struct ginfo *gi = malloc(font->n * sizeof(gi[0]));
			if (!gi) {
				font_free(font);
				return NULL;
			}
			for (i = 0; i < font->n; i++) {
				char *eoc = memchr(pos, 0xff, end - pos);
				gi[i].idx = i;
				gi[i].uc = i;
				if (eoc && pos < eoc)
					gi[i].uc = uc_code(pos, eoc - pos);
				pos = eoc + 1;
			}
			qsort(gi, font->n, sizeof(gi[0]), ginfo_cmp);
			for (i = 0; i < font->n; i++) {
				font->gidx[i] = gi[i].idx;
				font->gmap[i] = gi[i].uc;
			}
			free(gi);
		}
		return font;
	}
nofont:
	free(raw);
	return NULL;
}

static int find_glyph(struct font *font, int c)
{
	int l = 0;
	int h = font->n;
	while (l < h) {
		int m = (l + h) / 2;
		if (font->gmap[m] == c)
			return m;
		if (c < font->gmap[m])
			h = m;
		else
			l = m + 1;
	}
	return -1;
}

int font_bitmap(struct font *font, void *dst, int c)
{
	int i = find_glyph(font, c);
	if (i < 0)
		return 1;
	if (font->glen) {
		char *beg = font->gdat + font->gidx[i] * font->glen;
		int rlen = (font->cols + 7) >> 3;
		int i, j;
		for (i = 0; i < font->rows; i++) {
			unsigned char *row = (unsigned char *) beg + i * rlen;
			for (j = 0; j < font->cols; j++) {
				int val = row[j >> 3] & (1 << (j & 0x7));
				((unsigned char *) dst)[i * font->cols + 7 - j] = val ? 255 : 0;
			}
		}
	} else {
		int len = font->rows * font->cols;
		memcpy(dst, font->gdat + i * len, len);
	}
	return 0;
}

void font_free(struct font *font)
{
	if (font)
		free(font->raw);
	if (font && font->glen)
		free(font->gmap);
	if (font && font->glen)
		free(font->gidx);
	free(font);
}

int font_rows(struct font *font)
{
	return font->rows;
}

int font_cols(struct font *font)
{
	return font->cols;
}
