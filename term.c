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

#define FGCOLOR		0
#define BGCOLOR		7
#define SQRADDR(r, c)		(&screen[(r) * pad_cols() + (c)])

#define MODE_NOCURSOR		0x01
#define MODE_NOWRAP		0x02
#define MODE_ORIGIN		0x04
#define MODE_NOAUTOCR		0x08
#define BIT_SET(i, b, val)	((val) ? ((i) | (b)) : ((i) & ~(b)))

static pid_t pid;
static int fd;
static int row, col;
static int fg, bg;
static struct square screen[MAXCHARS];
static int top, bot;
static int mode;
static struct miscterm_state saved;

static int origin(void)
{
	return mode & MODE_ORIGIN;
}

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
	if (cursor && !(mode & MODE_NOCURSOR)) {
		int t = fgcolor;
		fgcolor = bgcolor;
		bgcolor = t;
	}
	pad_put(sqr->c, r, c, fgcolor, bgcolor);
}

void term_put(int ch, int r, int c)
{
	struct square *sqr = SQRADDR(r, c);
	sqr->c = ch;
	sqr->fg = fg;
	sqr->bg = bg;
	term_show(r, c, 0);
}

static void empty_rows(int sr, int er)
{
	memset(SQRADDR(sr, 0), 0, (er - sr) * sizeof(screen[0]) * pad_cols());
}

static void draw_rows(int sr, int er)
{
	int i = 0;
	for (i = sr * pad_cols(); i < er * pad_cols(); i++)
		term_show(i / pad_cols(), i % pad_cols(), 0);
}

static void blank_rows(int sr, int er)
{
	empty_rows(sr, er);
	draw_rows(sr, er);
	term_show(row, col, 1);
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

static void insert_lines(int n)
{
	int sr = MAX(top, row);
	int nr = bot - row - n;
	if (nr > 0)
		scroll_screen(sr, nr, n);
}

static void delete_lines(int n)
{
	int r = MAX(top, row);
	int sr = r + n;
	int nr = bot - r - n;
	if (nr > 0)
		scroll_screen(sr, nr, -n);
}

static void move_cursor(int r, int c)
{
	term_show(row, col, 0);
	row = MAX(origin() ? top : 0, MIN(r, (origin() ? bot : pad_rows()) - 1));
	col = MAX(0, MIN(c, pad_cols() - 1));
	term_show(row, col, 1);
}

static void advance(int dr, int dc, int scrl)
{
	int r = row + dr;
	int c = col + dc;
	int t = origin() ? top : 0;
	int b = origin() ? bot : pad_rows();
	if (c >= pad_cols()) {
		if (!scrl || (mode & MODE_NOWRAP)) {
			c = pad_cols() - 1;
		} else {
			r++;
			c = 0;
		}
	}
	if (r >= b && scrl) {
		int n = b - r - 1;
		int nr = (b - t) + n;
		scroll_screen(-n, nr, n);
	}
	if (r < t && scrl) {
		int n = t - r;
		int nr = (b - t) - n;
		scroll_screen(t, nr, n);
	}
	r = MIN(b - 1, MAX(t, r));
	move_cursor(r, MAX(0, c));
}

void term_send(int c)
{
	unsigned char b = (unsigned char) c;
	if (fd)
		write(fd, &b, 1);
}

void term_sendstr(char *s)
{
	if (fd)
		write(fd, s, strlen(s));
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

static void kill_chars(int sc, int ec)
{
	int i;
	memset(SQRADDR(row, sc), 0, (ec - sc) * sizeof(screen[0]));
	for (i = sc; i < ec; i++)
		term_show(row, i, 0);
	move_cursor(row, col);
}

static void move_chars(int sc, int nc, int n)
{
	int i;
	term_show(row, col, 0);
	memmove(SQRADDR(row, sc + n), SQRADDR(row, sc),
		nc * sizeof(screen[0]));
	if (n > 0)
		memset(SQRADDR(row, sc), 0, n * sizeof(screen[0]));
	else
		memset(SQRADDR(row, pad_rows() + n), 0, -n * sizeof(screen[0]));
	for (i = MIN(sc, sc + n); i < pad_cols(); i++)
		term_show(row, i, 0);
	term_show(row, col, 1);
}

static void delete_chars(int n)
{
	move_chars(col + n, pad_cols(), -n);
}

static void insert_chars(int n)
{
	int nc = pad_cols() - col - n;
	move_chars(col, nc, n);
}

void term_blank(void)
{
	pad_blank(bg);
	memset(screen, 0, sizeof(screen));
}

static void ctlseq(void);
void term_read(void)
{
	ctlseq();
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

static void misc_save(struct miscterm_state *state)
{
	state->row = row;
	state->col = col;
	state->fg = fg;
	state->bg = bg;
	state->top = top;
	state->bot = bot;
	state->mode = mode;
}

static void misc_load(struct miscterm_state *state)
{
	row = state->row;
	col = state->col;
	fg = state->fg;
	bg = state->bg;
	top = state->top;
	bot = state->bot;
	mode = state->mode;
}

void term_save(struct term_state *state)
{
	state->fd = fd;
	state->pid = pid;
	memcpy(state->screen, screen,
		pad_rows() * pad_cols() * sizeof(screen[0]));
	state->sav = saved;
	misc_save(&state->cur);
}

void term_load(struct term_state *state)
{
	fd = state->fd;
	pid = state->pid;
	misc_load(&state->cur);
	saved = state->sav;
	memcpy(screen, state->screen,
		pad_rows() * pad_cols() * sizeof(screen[0]));
	draw_rows(0, pad_rows());
	term_show(row, col, 1);
}

int term_fd(void)
{
	return fd;
}

void term_init(void)
{
	pad_init();
	bot = pad_rows();
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

void set_region(int t, int b)
{
	top = MIN(pad_rows(), MAX(0, t - 1));
	bot = MIN(pad_rows(), MAX(0, b ? b : pad_rows()));
	if (origin())
		move_cursor(top, 0);
}

#include "vt102.c"
