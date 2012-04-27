#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "config.h"
#include "font.h"
#include "util.h"

struct font {
	int fd;
	int rows;
	int cols;
	int n;
	int *glyphs;
};

/*
 * tinyfont format:
 *
 * sig[8]	"tinyfont"
 * ver		0
 * n		number of glyphs
 * rows		glyph rows
 * cols		glyph cols
 *
 * glyphs[n]	unicode character numbers (int)
 * bitmaps[n]	character bitmaps (char[rows * cols])
 */
struct tinyfont {
	char sig[8];
	int ver;
	int n;
	int rows, cols;
};

struct font *font_open(char *path)
{
	struct font *font;
	struct tinyfont head;
	font = malloc(sizeof(*font));
	font->fd = open(path, O_RDONLY);
	if (font->fd == -1)
		return NULL;
	fcntl(font->fd, F_SETFD, fcntl(font->fd, F_GETFD) | FD_CLOEXEC);
	if (read(font->fd, &head, sizeof(head)) != sizeof(head))
		return NULL;
	font->n = head.n;
	font->rows = head.rows;
	font->cols = head.cols;
	font->glyphs = malloc(font->n * sizeof(int));
	if (read(font->fd, font->glyphs, font->n * sizeof(int)) != font->n * sizeof(int))
		return NULL;
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
	if (i < 0)
		return 1;
	lseek(font->fd, sizeof(struct tinyfont) + font->n * sizeof(int) +
					i * font->rows * font->cols, 0);
	read(font->fd, dst, font->rows * font->cols);
	return 0;
}

void font_free(struct font *font)
{
	free(font->glyphs);
	close(font->fd);
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
