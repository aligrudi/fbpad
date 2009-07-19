#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "config.h"
#include "pad.h"
#include "util.h"
#include "term.h"

#define MODE_NOCURSOR		0x01
#define MODE_NOWRAP		0x02
#define MODE_ORIGIN		0x04
#define MODE_AUTOCR		0x08
#define BIT_SET(i, b, val)	((val) ? ((i) | (b)) : ((i) & ~(b)))
#define SQRADDR(r, c)		(screen + (r) * pad_cols() + (c))

static struct term *term;
static struct square *screen;
static int row, col;
static int fg, bg;
static int top, bot;
static int mode;
static int visible;

#define MAXLINES		(1 << 13)
static int dirty[MAXLINES];
static int lazy;

static void _term_show(int r, int c, int cursor)
{
	struct square *sqr = SQRADDR(r, c);
	int fgcolor = sqr->c ? sqr->fg : fg;
	int bgcolor = sqr->c ? sqr->bg : bg;
	if (cursor && !(mode & MODE_NOCURSOR)) {
		int t = fgcolor;
		fgcolor = bgcolor;
		bgcolor = t;
	}
	if (visible)
		pad_put(sqr->c, r, c, fgcolor, bgcolor);
}

static void _draw_row(int r)
{
	int i;
	for (i = 0; i < pad_cols(); i++)
		_term_show(r, i, 0);
}

static void lazy_draw(int sr, int er)
{
	int i;
	if (!visible)
		return;
	for (i = sr; i < er; i++) {
		if (lazy)
			dirty[i] = 1;
		else
			_draw_row(i);
	}
}

static void lazy_drawcols(int r, int sc, int ec)
{
	int i;
	if (!visible)
		return;
	if (lazy) {
		dirty[r] = 1;
	} else {
		for (i = sc; i < ec; i++)
			_term_show(r, i, 0);
	}
}

static void lazy_put(int ch, int r, int c)
{
	struct square *sqr = SQRADDR(r, c);
	sqr->c = ch;
	sqr->fg = fg;
	sqr->bg = bg;
	if (!visible)
		return;
	if (lazy)
		dirty[r] = 1;
	else
		_term_show(r, c, 0);
}

static void lazy_cursor(int put)
{
	if (!visible)
		return;
	if (lazy)
		dirty[row] = 1;
	else
		_term_show(row, col, put);
}

static void lazy_clean()
{
	if (visible)
		memset(dirty, 0, sizeof(*dirty) * MAXLINES);
}

static void lazy_flush()
{
	int i;
	if (!visible)
		return;
	_term_show(row, col, 0);
	for (i = 0; i < pad_rows(); i++)
		if (dirty[i])
			_draw_row(i);
	lazy_clean();
	_term_show(row, col, 1);
}

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
	ioctl(term->fd, TIOCSWINSZ, &size);
}

#define PTYBUFSIZE		(1 << 13)
static char ptybuf[PTYBUFSIZE];
static int ptylen;
static int ptycur;
static void waitpty(void)
{
	struct pollfd ufds[1];
	ufds[0].fd = term->fd;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static int readpty(void)
{
	if (ptycur < ptylen)
		return ptybuf[ptycur++];
	if (!term->fd)
		return -1;
	ptylen = read(term->fd, ptybuf, PTYBUFSIZE);
	if (ptylen == -1 && errno == EAGAIN) {
		waitpty();
		ptylen = read(term->fd, ptybuf, PTYBUFSIZE);
	}
	if (ptylen > 0) {
		ptycur = 1;
		return ptybuf[0];
	}
	return -1;
}

static void empty_rows(int sr, int er)
{
	memset(SQRADDR(sr, 0), 0, (er - sr) * sizeof(*screen) * pad_cols());
}

static void blank_rows(int sr, int er)
{
	empty_rows(sr, er);
	lazy_draw(sr, er);
	lazy_cursor(1);
}

static void scroll_screen(int sr, int nr, int n)
{
	lazy_cursor(0);
	memmove(SQRADDR(sr + n, 0), SQRADDR(sr, 0),
		nr * pad_cols() * sizeof(*screen));
	if (n > 0)
		empty_rows(sr, sr + n);
	else
		empty_rows(sr + nr + n, sr + nr);
	lazy_draw(MIN(sr, sr + n), MAX(sr + nr, sr + nr + n));
	lazy_cursor(1);
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
	int t, b;
	lazy_cursor(0);
	t = origin() ? top : 0;
	b = origin() ? bot : pad_rows();
	row = MAX(t, MIN(r, b - 1));
	col = MAX(0, MIN(c, pad_cols() - 1));
	lazy_cursor(1);
}

static void advance(int dr, int dc, int scrl)
{
	int r = row + dr;
	int c = col + dc;
	if (c >= pad_cols()) {
		if (!scrl || (mode & MODE_NOWRAP)) {
			c = pad_cols() - 1;
		} else {
			r++;
			c = 0;
		}
	}
	if (r >= bot && scrl) {
		int n = bot - r - 1;
		int nr = (bot - top) + n;
		scroll_screen(top + -n, nr, n);
	}
	if (r < top && scrl) {
		int n = top - r;
		int nr = (bot - top) - n;
		scroll_screen(top, nr, n);
	}
	r = MIN(bot - 1, MAX(top, r));
	move_cursor(r, MAX(0, c));
}

void term_send(int c)
{
	unsigned char b = (unsigned char) c;
	if (term->fd)
		write(term->fd, &b, 1);
}

void term_sendstr(char *s)
{
	if (term->fd)
		write(term->fd, s, strlen(s));
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
	for (i = sc; i < ec; i++)
		lazy_put(' ', row, i);
	lazy_cursor(1);
}

static void move_chars(int sc, int nc, int n)
{
	lazy_cursor(0);
	memmove(SQRADDR(row, sc + n), SQRADDR(row, sc), nc * sizeof(*screen));
	if (n > 0)
		memset(SQRADDR(row, sc), 0, n * sizeof(*screen));
	else
		memset(SQRADDR(row, pad_cols() + n), 0, -n * sizeof(*screen));
	lazy_drawcols(row, MIN(sc, sc + n), pad_cols());
	lazy_cursor(1);
}

static void delete_chars(int n)
{
	int sc = col + n;
	int nc = pad_cols() - sc;
	move_chars(sc, nc, -n);
}

static void insert_chars(int n)
{
	int nc = pad_cols() - col - n;
	move_chars(col, nc, n);
}

static void term_blank(void)
{
	memset(screen, 0, MAXCHARS * sizeof(*screen));
	if (visible) {
		pad_blank(bg);
		lazy_clean();
	}
}

static void ctlseq(void);
void term_read(void)
{
	ctlseq();
	lazy = 1;
	while (ptycur < ptylen)
		ctlseq();
	lazy_flush();
	lazy = 0;
}

void term_exec(char *cmd)
{
	memset(term, 0, sizeof(*term));
	term->cur.bot = term->sav.bot = pad_rows();
	term_load(term, visible);
	if ((term->pid = forkpty(&term->fd, NULL, NULL, NULL)) == -1) {
		perror("failed to fork");
		term->fd = 0;
		return;
	}
	if (!term->pid) {
		setenv("TERM", "linux", 1);
		execlp(cmd, cmd, NULL);
		exit(1);
	}
	fcntl(term->fd, F_SETFD, fcntl(term->fd, F_GETFD) | FD_CLOEXEC);
	fcntl(term->fd, F_SETFL, fcntl(term->fd, F_GETFL) | O_NONBLOCK);
	setsize();
	setmode(0);
	term_blank();
}

static void misc_save(struct term_state *state)
{
	state->row = row;
	state->col = col;
	state->fg = fg;
	state->bg = bg;
	state->top = top;
	state->bot = bot;
	state->mode = mode;
}

static void misc_load(struct term_state *state)
{
	row = state->row;
	col = state->col;
	fg = state->fg;
	bg = state->bg;
	top = state->top;
	bot = state->bot;
	mode = state->mode;
}

void term_save(struct term *term)
{
	visible = 0;
	misc_save(&term->cur);
}

void term_load(struct term *t, int flags)
{
	term = t;
	misc_load(&term->cur);
	screen = term->screen;
	visible = flags;
	if (flags == TERM_REDRAW) {
		if (term->fd) {
			lazy_draw(0, pad_rows());
			lazy_cursor(1);
		} else {
			pad_blank(0);
		}
	}
}

void term_end(void)
{
	if (term->fd)
		close(term->fd);
	term->fd = 0;
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
