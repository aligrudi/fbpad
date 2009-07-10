#include <ctype.h>
#include <pty.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "pad.h"
#include "util.h"
#include "term.h"

#define MAXESCARGS	32
#define FGCOLOR		0
#define BGCOLOR		7
#define SQRADDR(r, c)		(&screen[(r) * pad_cols() + (c)])

static pid_t pid;
static int fd;
static int row, col;
static int fg, bg;
static struct square screen[MAXCHARS];

static void setsize(void)
{
	struct winsize size;
	size.ws_col = pad_cols();
	size.ws_row = pad_rows();
	size.ws_xpixel = 0;
	size.ws_ypixel = 0;
	ioctl(fd, TIOCSWINSZ, &size);
}

static int readpty(void)
{
	char b;
	if (read(fd, &b, 1) > 0)
		return (int) b;
	return -1;
}

static void term_show(int r, int c, int cursor)
{
	struct square *sqr = SQRADDR(r, c);
	int fgcolor = sqr->c ? sqr->fg : fg;
	int bgcolor = sqr->c ? sqr->bg : bg;
	if (cursor) {
		int t = fgcolor;
		fgcolor = bgcolor;
		bgcolor = t;
	}
	pad_put(sqr->c, r, c, fgcolor, bgcolor);
}

void term_put(int ch, int r, int c)
{
	struct square *sqr = SQRADDR(r, c);
	if (!ch || !strchr("\a\b\f\n\r\v", ch)) {
		sqr->c = ch;
		sqr->fg = fg;
		sqr->bg = bg;
	}
	term_show(r, c, 0);
}

static void move_cursor(int r, int c)
{
	term_show(row, col, 0);
	row = MAX(0, MIN(r, pad_rows() - 1));
	col = MAX(0, MIN(c, pad_cols() - 1));
	term_show(row, col, 1);
}

static void empty_rows(int sr, int er)
{
	memset(SQRADDR(sr, 0), 0, (er - sr) * sizeof(screen[0]) * pad_cols());
}

static void scroll_screen(int sr, int nr, int n)
{
	term_show(row, col, 0);
	memmove(SQRADDR(sr + n, 0), SQRADDR(sr, 0),
		nr * pad_cols() * sizeof(screen[0]));
	if (n > 0)
		empty_rows(sr, sr + n);
	else
		empty_rows(sr + nr + n, sr + nr);
	pad_scroll(sr, nr, n, bg);
	term_show(row, col, 1);
}

static void advance(int ch)
{
	int r = row;
	int c = col;
	switch (ch) {
	case '\n':
		r++;
		c = 0;
		break;
	case '\t':
		c = (c / 8 + 1) * 8;
		break;
	case '\b':
		if (c)
			c--;
		break;
	case '\r':
		c = 0;
		break;
	case '\a':
	case '\f':
	case '\v':
		break;
	default:
		c++;
	}
	if (c >= pad_cols()) {
		r++;
		c = 0;
	}
	if (r >= pad_rows()) {
		int n = pad_rows() - r - 1;
		int nr = r + n;
		r = pad_rows() - 1;
		scroll_screen(-n, nr, n);
	}
	move_cursor(r, c);
}

void term_send(int c)
{
	unsigned char b = (unsigned char) c;
	if (fd)
		write(fd, &b, 1);
}

static void writepty(int c)
{
	term_put(c, row, col);
	advance(c);
}

static void setmode(int m)
{
	if (m == 0) {
		fg = FGCOLOR;
		bg = BGCOLOR;
	}
	if (m == 1)
		fg = fg | 0x08;
	if (m == 7) {
		int t = fg;
		fg = bg;
		bg = t;
	}
	if (m >= 30 && m <= 37)
		fg = m - 30;
	if (m >= 40 && m <= 47)
		bg = m - 40;
}

static void kill_line(void)
{
	int i;
	for (i = col; i < pad_cols(); i++)
		term_put('\0', row, i);
	move_cursor(row, col);
}

static void delete_lines(int n)
{
	int sr = row + n;
	int nr = pad_rows() - row - n;
	scroll_screen(sr, nr, -n);
}

void term_blank(void)
{
	pad_blank(bg);
	memset(screen, 0, sizeof(screen));
}

static void insert_lines(int n)
{
	int nr = pad_rows() - row - n;
	scroll_screen(row, nr, n);
}

static void escape_bracket(void)
{
	int args[MAXESCARGS] = {0};
	int i;
	int n = 0;
	int c = 0;
	for (i = 0; i < ARRAY_SIZE(args) && !isalpha(c); i++) {
		int arg = 0;
		while (isdigit((c = readpty())))
			arg = arg * 10 + (c - '0');
		args[n++] = arg;
	}
	switch (c) {
	case 'H':
	case 'f':
		move_cursor(MAX(0, args[0] - 1), MAX(0, args[1] - 1));
		break;
	case 'J':
		term_blank();
		move_cursor(0, 0);
		break;
	case 'A':
		move_cursor(row - MAX(1, args[0]), col);
		break;
	case 'B':
		move_cursor(row + MAX(1, args[0]), col);
		break;
	case 'C':
		move_cursor(row, col + MAX(1, args[0]));
		break;
	case 'D':
		move_cursor(row, col - MAX(1, args[0]));
		break;
	case 'K':
		kill_line();
		break;
	case 'L':
		insert_lines(MAX(1, args[0]));
		break;
	case 'M':
		delete_lines(MAX(1, args[0]));
		break;
	case 'c':
		break;
	case 'h':
		break;
	case 'l':
		break;
	case 'm':
		setmode(0);
		for (i = 0; i < n; i++)
			setmode(args[i]);
		break;
	case 'r':
		break;
	default:
		printf("unknown escape bracket char <%c>\n", c);
	}
}

static void reverse_index()
{
	scroll_screen(0, pad_rows() - 1, 1);
}

static int escape_alone(int c)
{
	switch (c) {
	case 'M':
		reverse_index();
		return 0;
	default:
		printf("unknown escape char <%c>\n", c);
	}
	return 1;
}

static void escape(void)
{
	int c = readpty();
	if (c == '[') {
		escape_bracket();
	} else if (escape_alone(c)) {
		writepty(ESC);
		writepty(c);
		return;
	}
}

void term_read(void)
{
	int c = readpty();
	if (c == ESC)
		escape();
	else
		writepty(c);
}

void term_exec(char *cmd)
{
	if ((pid = forkpty(&fd, NULL, NULL, NULL)) == -1)
		xerror("failed to create a pty");
	if (!pid) {
		setenv("TERM", "linux", 1);
		execl(cmd, cmd, NULL);
		exit(1);
	}
	setsize();
	setmode(0);
	term_blank();
}

void term_save(struct term_state *state)
{
	state->row = row;
	state->col = col;
	state->fd = fd;
	state->pid = pid;
	state->fg = fg;
	state->bg = bg;
	memcpy(state->screen, screen,
		pad_rows() * pad_cols() * sizeof(screen[0]));
}

void term_load(struct term_state *state)
{
	int i;
	row = state->row;
	col = state->col;
	fd = state->fd;
	pid = state->pid;
	fg = state->fg;
	bg = state->bg;
	memcpy(screen, state->screen,
		pad_rows() * pad_cols() * sizeof(screen[0]));
	for (i = 0; i < pad_rows() * pad_cols(); i++)
		term_show(i / pad_cols(), i % pad_cols(), 0);
	term_show(row, col, 1);
}

int term_fd(void)
{
	return fd;
}

void term_init(void)
{
	pad_init();
	term_blank();
}

void term_free(void)
{
	pad_free();
}

void term_end(void)
{
	fd = 0;
	row = col = 0;
	fg = 0;
	bg = 0;
	term_blank();
}
