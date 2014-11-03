#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "fbpad.h"

struct font {
	int rows, cols;	/* glyph bitmap rows and columns */
	int n;		/* number of font glyphs */
	int *glyphs;	/* glyph unicode character codes */
	char *data;	/* glyph bitmaps */
};

/*
 * This tinyfont header is followed by:
 *
 * glyphs[n]	unicode character codes (int)
 * bitmaps[n]	character bitmaps (char[rows * cols])
 */
struct tinyfont {
	char sig[8];	/* tinyfont signature; "tinyfont" */
	int ver;	/* version; 0 */
	int n;		/* number of glyphs */
	int rows, cols;	/* glyph dimensions */
};

static void *xread(int fd, int len)
{
	void *buf = malloc(len);
	if (buf && read(fd, buf, len) == len)
		return buf;
	free(buf);
	return NULL;
}

struct font *font_open(char *path)
{
	struct font *font;
	struct tinyfont head;
	int fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, &head, sizeof(head)) != sizeof(head)) {
		close(fd);
		return NULL;
	}
	font = malloc(sizeof(*font));
	font->n = head.n;
	font->rows = head.rows;
	font->cols = head.cols;
	font->glyphs = xread(fd, font->n * sizeof(int));
	font->data = xread(fd, font->n * font->rows * font->cols);
	close(fd);
	if (!font->glyphs || !font->data) {
		font_free(font);
		return NULL;
	}
	return font;
}

static int find_glyph(struct font *font, int c)
{
	int l = 0;
	int h = font->n;
	while (l < h) {
		int m = (l + h) / 2;
		if (font->glyphs[m] == c)
			return m;
		if (c < font->glyphs[m])
			h = m;
		else
			l = m + 1;
	}
	return -1;
}

int font_bitmap(struct font *font, void *dst, int c)
{
	int i = find_glyph(font, c);
	int len = font->rows * font->cols;
	if (i < 0)
		return 1;
	memcpy(dst, font->data + i * len, len);
	return 0;
}

void font_free(struct font *font)
{
	if (font->data)
		free(font->data);
	if (font->glyphs)
		free(font->glyphs);
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
