#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "config.h"
#include "pad.h"
#include "util.h"
#include "term.h"

#define MODE_CURSOR		0x01
#define MODE_WRAP		0x02
#define MODE_ORIGIN		0x04
#define MODE_AUTOCR		0x08
#define MODE_DEFAULT		(MODE_CURSOR | MODE_WRAP)
#define ATTR_BOLD		0x10
#define ATTR_REV		0x20
#define ATTR_ALL		(ATTR_BOLD | ATTR_REV)
#define MODE_WRAPREADY		0x80

#define BIT_SET(i, b, val)	((val) ? ((i) | (b)) : ((i) & ~(b)))
#define OFFSET(r, c)		((r) * pad_cols() + (c))

static struct term *term;
static unsigned int *screen;
static unsigned char *fgs, *bgs;
static int row, col;
static unsigned char fg, bg;
static int top, bot;
static unsigned int mode;
static int visible;

#define MAXLINES		(1 << 13)
static int dirty[MAXLINES];
static int lazy;

static unsigned char fgcolor(void)
{
	int c = mode & ATTR_REV ? bg : fg;
	return mode & ATTR_BOLD ? c | 0x08 : c;
}

static unsigned char bgcolor(void)
{
	return mode & ATTR_REV ? fg : bg;
}

static void _term_show(int r, int c, int cursor)
{
	int i = OFFSET(r, c);
	unsigned char fg = screen[i] ? fgs[i] : fgcolor();
	unsigned char bg = screen[i] ? bgs[i] : bgcolor();
	if (cursor && mode & MODE_CURSOR) {
		int t = fg;
		fg = bg;
		bg = t;
	}
	if (visible)
		pad_put(screen[i], r, c, fg, bg);
}

static void _draw_row(int r)
{
	int i;
	pad_blankrow(r, bgcolor());
	for (i = 0; i < pad_cols(); i++) {
		unsigned int c = screen[OFFSET(r, i)];
		if (c && (c != ' ' || bgs[OFFSET(r, i)] != bgcolor()))
			_term_show(r, i, 0);
	}
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
	int i = OFFSET(r, c);
	screen[i] = ch;
	fgs[i] = fgcolor();
	bgs[i] = bgcolor();
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

static void lazy_clean(void)
{
	if (visible)
		memset(dirty, 0, sizeof(*dirty) * MAXLINES);
}

static void lazy_flush(void)
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

static void term_redraw(void)
{
	int i;
	for (i = 0; i < pad_rows(); i++)
		_draw_row(i);
	_term_show(row, col, 1);
}

static int origin(void)
{
	return mode & MODE_ORIGIN;
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

static void screen_move(int dst, int src, int n)
{
	memmove(screen + dst, screen + src, n * sizeof(*screen));
	memmove(fgs + dst, fgs + src, n);
	memmove(bgs + dst, bgs + src, n);
}

static void screen_reset(int i, int n)
{
	memset(screen + i, 0, n * sizeof(*screen));
	memset(fgs + i, fg, n);
	memset(bgs + i, bg, n);
}

static void empty_rows(int sr, int er)
{
	screen_reset(OFFSET(sr, 0), (er - sr) * pad_cols());
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
	screen_move(OFFSET(sr + n, 0), OFFSET(sr, 0), nr * pad_cols());
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
	mode = BIT_SET(mode, MODE_WRAPREADY, 0);
}

static void advance(int dr, int dc, int scrl)
{
	int r = row + dr;
	int c = col + dc;
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
	c = MIN(pad_cols() - 1, MAX(0, c));
	move_cursor(r, c);
}

static void insertchar(int c)
{
	int wrapready;
	if (mode & MODE_WRAPREADY)
		advance(1, -col, 1);
	lazy_put(c, row, col);
	wrapready = col == pad_cols() - 1;
	advance(0, 1, 1);
	if (wrapready)
		mode = BIT_SET(mode, MODE_WRAPREADY, 1);
}

void term_send(int c)
{
	unsigned char b = (unsigned char) c;
	if (term->fd)
		write(term->fd, &b, 1);
}

static void term_sendstr(char *s)
{
	if (term->fd)
		write(term->fd, s, strlen(s));
}

static void setattr(int m)
{
	switch (m) {
	case 0:
		fg = FGCOLOR;
		bg = BGCOLOR;
		mode &= ~ATTR_ALL;
		break;
	case 1:
		mode |= ATTR_BOLD;
		break;
	case 7:
		mode |= ATTR_REV;
		break;
	case 22:
		mode &= ~ATTR_BOLD;
		break;
	case 27:
		mode &= ~ATTR_REV;
		break;
	default:
		if ((m / 10) == 3)
			fg = m > 37 ? FGCOLOR : m - 30;
		if ((m / 10) == 4)
			bg = m > 47 ? BGCOLOR : m - 40;
	}
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
	screen_move(OFFSET(row, sc + n), OFFSET(row, sc), nc);
	if (n > 0)
		screen_reset(OFFSET(row, sc), n);
	else
		screen_reset(OFFSET(row, pad_cols() + n), -n);
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
	screen_reset(0, pad_rows() * pad_cols());
	if (visible) {
		pad_blank(bgcolor());
		lazy_clean();
	}
}

static void ctlseq(void);
void term_read(void)
{
	ctlseq();
	while (ptycur < ptylen) {
		if (ptylen - ptycur > 15)
			lazy = 1;
		ctlseq();
	}
	if (lazy)
		lazy_flush();
	lazy = 0;
}

static void term_reset(void)
{
	row = col = 0;
	top = 0;
	bot = pad_rows();
	mode = MODE_DEFAULT;
	setattr(0);
	term_blank();
}

static int _openpty(int *master, int *slave)
{
	int unlock = 0;
	int ptyno = 0;
	char name[20];
	if ((*master = open("/dev/ptmx", O_RDWR)) == -1)
		return -1;
	if (ioctl(*master, TIOCSPTLCK, &unlock) == -1)
		return -1;
	if (ioctl(*master, TIOCGPTN, &ptyno) == -1)
		return -1;
	sprintf(name, "/dev/pts/%d", ptyno);
	*slave = open(name, O_RDWR | O_NOCTTY);
	return 0;
}

static void _login(int fd)
{
	static char env[] = "TERM=linux";
	struct winsize winp;
	winp.ws_col = pad_cols();
	winp.ws_row = pad_rows();
	winp.ws_xpixel = 0;
	winp.ws_ypixel = 0;
	setsid();
	ioctl(fd, TIOCSCTTY, NULL);
	ioctl(fd, TIOCSWINSZ, &winp);
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
	putenv(env);
}

void term_exec(char *cmd)
{
	int master, slave;
	memset(term, 0, sizeof(*term));
	if (_openpty(&master, &slave) == -1)
		return;
	if ((term->pid = fork()) == -1)
		return;
	if (!term->pid) {
		_login(slave);
		close(master);
		execlp(cmd, cmd, NULL);
		exit(1);
	}
	close(slave);
	term->fd = master;
	fcntl(term->fd, F_SETFD, fcntl(term->fd, F_GETFD) | FD_CLOEXEC);
	fcntl(term->fd, F_SETFL, fcntl(term->fd, F_GETFL) | O_NONBLOCK);
	term_reset();
}

static void misc_save(struct term_state *state)
{
	state->row = row;
	state->col = col;
	state->fg = fg;
	state->bg = bg;
	state->mode = mode;
}

static void misc_load(struct term_state *state)
{
	row = state->row;
	col = state->col;
	fg = state->fg;
	bg = state->bg;
	mode = state->mode;
}

void term_save(struct term *term)
{
	visible = 0;
	misc_save(&term->cur);
	term->top = top;
	term->bot = bot;
}

void term_load(struct term *t, int flags)
{
	term = t;
	misc_load(&term->cur);
	screen = term->screen;
	fgs = term->fgs;
	bgs = term->bgs;
	visible = flags;
	top = term->top;
	bot = term->bot;
	if (flags == TERM_REDRAW) {
		if (term->fd)
			term_redraw();
		else
			pad_blank(0);
	}
}

void term_end(void)
{
	if (term->fd)
		close(term->fd);
	memset(term, 0, sizeof(*term));
	term_load(term, visible ? TERM_REDRAW : TERM_HIDDEN);
}

static void set_region(int t, int b)
{
	top = MIN(pad_rows(), MAX(0, t - 1));
	bot = MIN(pad_rows(), MAX(0, b ? b : pad_rows()));
	if (origin())
		move_cursor(top, 0);
}

void term_screenshot(void)
{
	FILE *fp = fopen(SCREENSHOT, "w");
	int i, j;
	for (i = 0; i < pad_rows(); i++) {
		for (j = 0; j < pad_cols(); j++) {
			int c = screen[OFFSET(i, j)];
			fputc(c ? c : ' ', fp);
		}
		fputc('\n', fp);
	}
	fclose(fp);
}

#include "vt102.c"
