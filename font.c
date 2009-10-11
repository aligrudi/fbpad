#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "config.h"
#include "font.h"
#include "util.h"

static int fd;
static char *tf;
static int rows;
static int cols;
static int n;
static int *glyphs;
static unsigned char *data;

static void xerror(char *msg)
{
	perror(msg);
	exit(1);
}

static size_t file_size(int fd)
{
	struct stat st;
	if (!fstat(fd, &st))
		return st.st_size;
	return 0;
}

struct tf_header {
	char sig[8];
	int ver;
	int n;
	int rows, cols;
};

void font_init(void)
{
	struct tf_header *head;
	fd = open(TINYFONT, O_RDONLY);
	if (fd == -1)
		xerror("can't open font");
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	tf = mmap(NULL, file_size(fd), PROT_READ, MAP_SHARED, fd, 0);
	if (tf == MAP_FAILED)
		xerror("can't mmap font file");
	head = (struct tf_header *) tf;
	n = head->n;
	rows = head->rows;
	cols = head->cols;
	glyphs = (int *) (tf + sizeof(*head));
	data = (unsigned char *) (glyphs + n);
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

unsigned char *font_bitmap(int c)
{
	int i = find_glyph(c);
	return i >= 0 ? &data[i * rows * cols] : NULL;
}

void font_free(void)
{
	munmap(tf, file_size(fd));
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
