#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "config.h"
#include "font.h"
#include "util.h"

static int fd;
static int rows;
static int cols;
static int n;
static int *glyphs;

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

int font_init(void)
{
	struct tinyfont head;
	fd = open(TINYFONT, O_RDONLY);
	if (fd == -1)
		return 1;
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	if (read(fd, &head, sizeof(head)) != sizeof(head))
		return 1;
	n = head.n;
	rows = head.rows;
	cols = head.cols;
	glyphs = malloc(n * sizeof(int));
	if (read(fd, glyphs, n * sizeof(int)) != n * sizeof(int))
		return 1;
	return 0;
}

static int find_glyph(int c)
{
	int l = 0;
	int h = n;
	while (l < h) {
		int m = (l + h) / 2;
		if (glyphs[m] == c)
			return m;
		if (c < glyphs[m])
			h = m;
		else
			l = m + 1;
	}
	return -1;
}

int font_bitmap(void *dst, int c)
{
	int i = find_glyph(c);
	if (i < 0)
		return 1;
	lseek(fd, sizeof(struct tinyfont) + n * sizeof(int) + i * rows * cols, 0);
	read(fd, dst, rows * cols);
	return 0;
}

void font_free(void)
{
	free(glyphs);
	close(fd);
}

int font_rows(void)
{
	return rows;
}

int font_cols(void)
{
	return cols;
}
