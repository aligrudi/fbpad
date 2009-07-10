#include <ctype.h>
#include <pty.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include "pad.h"
#include "util.h"
#include "term.h"

#define MAXESCARGS	32
#define FGCOLOR		0
#define BGCOLOR		7

static pid_t pid;
static int fd;
static int row, col;

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

static void move_cursor(int r, int c)
{
	pad_show(row, col, 0);
	row = MIN(r, pad_rows() - 1);
	col = MIN(c, pad_cols() - 1);
	pad_show(row, col, 1);
}

static void scroll_screen(int sr, int nr, int n)
{
	pad_show(row, col, 0);
	pad_scroll(sr, nr, n);
	pad_show(row, col, 1);
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
	pad_put(c, row, col);
	advance(c);
}

static void setmode(int m)
{
	if (m == 0) {
		pad_fg(FGCOLOR);
		pad_bg(BGCOLOR);
	}
	if (m == 1)
		pad_fg(pad_getfg() + 8);
	if (m == 7) {
		pad_fg(pad_getbg());
		pad_bg(pad_getfg());
	}
	if (m >= 30 && m <= 37)
		pad_fg(m - 30);
	if (m >= 40 && m <= 47)
		pad_bg(m - 40);
}

static void kill_line(void)
{
	int i;
	for (i = col; i < pad_cols(); i++)
		pad_put('\0', row, i);
	move_cursor(row, col);
}

static void delete_lines(int n)
{
	int sr = row + n;
	int nr = pad_rows() - row - n;
	scroll_screen(sr, nr, -n);
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
		pad_blank();
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
	pad_blank();
}

void term_save(struct term_state *state)
{
	state->row = row;
	state->col = col;
	state->fd = fd;
	state->pid = pid;
	pad_save(&state->pad);
}

void term_load(struct term_state *state)
{
	row = state->row;
	col = state->col;
	fd = state->fd;
	pid = state->pid;
	pad_load(&state->pad);
	move_cursor(row, col);
}

int term_fd(void)
{
	return fd;
}

void term_end(void)
{
	fd = 0;
	row = col = 0;
	pad_fg(0);
	pad_bg(0);
	pad_blank();
}
