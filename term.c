#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "conf.h"
#include "fbpad.h"

#define MODE_CURSOR		0x01
#define MODE_WRAP		0x02
#define MODE_ORIGIN		0x04
#define MODE_AUTOCR		0x08
#define ATTR_BOLD		0x10
#define ATTR_ITALIC		0x20
#define ATTR_REV		0x40
#define ATTR_ALL		(ATTR_BOLD | ATTR_ITALIC | ATTR_REV)
#define MODE_INSERT		0x100
#define MODE_WRAPREADY		0x200
#define MODE_CLR8		0x400	/* colours 0-7 */

#define LIMIT(n, a, b)		((n) < (a) ? (a) : ((n) > (b) ? (b) : (n)))
#define BIT_SET(i, b, val)	((val) ? ((i) | (b)) : ((i) & ~(b)))
#define OFFSET(r, c)		((r) * pad_cols() + (c))

struct term_state {
	int row, col;
	int fg, bg;
	int mode;
};

struct term {
	int *screen;			/* screen content */
	int *hist;			/* scrolling history */
	int *fgs;			/* foreground color */
	int *bgs;			/* background color */
	int *dirty;			/* changed rows in lazy mode */
	struct term_state cur, sav;	/* terminal saved state */
	int fd;				/* terminal file descriptor */
	int hrow;			/* the next history row in hist[] */
	int hpos;			/* scrolling history; position */
	int lazy;			/* lazy mode */
	int pid;			/* pid of the terminal program */
	int top, bot;			/* terminal scrolling region */
	int rows, cols;
	int signal;			/* send SIGUSR1 and SIGUSR2 */
};

static struct term *term;
static int *screen;
static int *fgs, *bgs;
static int *dirty;
static int lazy;
static int row, col;
static int fg, bg;
static int top, bot;
static int mode;
static int visible;

static unsigned int clr16[16] = {
	COLOR0, COLOR1, COLOR2, COLOR3, COLOR4, COLOR5, COLOR6, COLOR7,
	COLOR8, COLOR9, COLORA, COLORB, COLORC, COLORD, COLORE, COLORF,
};

static int clrmap(int c)
{
	int g = (c - 232) * 10 + 8;
	if (c < 16) {
		return clr16[c];
	}
	if (c < 232) {
		int ri = (c - 16) / 36 ;
		int gi = (c - 16) % 36 / 6;
		int bi = (c - 16) % 6;
		int rc = ri ? (ri * 40 + 55) : 0;
		int gc = gi ? (gi * 40 + 55) : 0;
		int bc = bi ? (bi * 40 + 55) : 0;
		return (rc << 16) | (gc << 8) | bc;
	}
	return (g << 16) | (g << 8) | g;
}

/* low level drawing and lazy updating */

static int fgcolor(void)
{
	int c = mode & ATTR_REV ? bg : fg;
	if (mode & ATTR_BOLD)
		c |= FN_B;
	if (mode & ATTR_ITALIC)
		c |= FN_I;
	return c;
}

static int bgcolor(void)
{
	return mode & ATTR_REV ? fg : bg;
}

/* assumes visible && !lazy */
static void _draw_pos(int r, int c, int cursor)
{
	int rev = cursor && mode & MODE_CURSOR;
	int i = OFFSET(r, c);
	int fg = rev ? bgs[i] : fgs[i];
	int bg = rev ? fgs[i] : bgs[i];
	pad_put(screen[i], r, c, fg, bg);
}

/* assumes visible && !lazy */
static void _draw_row(int r)
{
	int cbg, cch;		/* current background and character */
	int fbg, fsc = -1;	/* filling background and start column */
	int i;
	/* call pad_fill() only once for blank columns with identical backgrounds */
	for (i = 0; i < pad_cols(); i++) {
		cbg = bgs[OFFSET(r, i)];
		cch = screen[OFFSET(r, i)] ? screen[OFFSET(r, i)] : ' ';
		if (fsc >= 0 && (cbg != fbg || cch != ' ')) {
			pad_fill(r, r + 1, fsc, i, fbg & FN_C);
			fsc = -1;
		}
		if (cch != ' ') {
			_draw_pos(r, i, 0);
		} else if (fsc < 0) {
			fsc = i;
			fbg = cbg;
		}
	}
	pad_fill(r, r + 1, fsc >= 0 ? fsc : pad_cols(), -1, cbg & FN_C);
}

static int candraw(int sr, int er)
{
	int i;
	if (lazy)
		for (i = sr; i < er; i++)
			dirty[i] = 1;
	return visible && !lazy;
}

static void draw_rows(int sr, int er)
{
	int i;
	if (candraw(sr, er))
		for (i = sr; i < er; i++)
			_draw_row(i);
}

static void draw_cols(int r, int sc, int ec)
{
	int i;
	if (candraw(r, r + 1))
		for (i = sc; i < ec; i++)
			_draw_pos(r, i, 0);
}

static void draw_char(int ch, int r, int c)
{
	int i = OFFSET(r, c);
	screen[i] = ch;
	fgs[i] = fgcolor();
	bgs[i] = bgcolor();
	if (candraw(r, r + 1))
		_draw_pos(r, c, 0);
}

static void draw_cursor(int put)
{
	if (candraw(row, row + 1))
		_draw_pos(row, col, put);
}

static void lazy_start(void)
{
	memset(dirty, 0, pad_rows() * sizeof(*dirty));
	lazy = 1;
}

static void lazy_flush(void)
{
	int i;
	if (!visible || !lazy)
		return;
	for (i = 0; i < pad_rows(); i++)
		if (dirty[i])
			_draw_row(i);
	if (dirty[row])
		_draw_pos(row, col, 1);
	lazy = 0;
	term->hpos = 0;
}

static void screen_reset(int i, int n)
{
	int c;
	candraw(i / pad_cols(), (i + n) / pad_cols());
	memset(screen + i, 0, n * sizeof(*screen));
	for (c = 0; c < n; c++)
		fgs[i + c] = fg;
	for (c = 0; c < n; c++)
		bgs[i + c] = bg;
}

static void screen_move(int dst, int src, int n)
{
	int srow = (MIN(src, dst) + (n > 0 ? 0 : n)) / pad_cols();
	int drow = (MAX(src, dst) + (n > 0 ? n : 0)) / pad_cols();
	candraw(srow, drow);
	memmove(screen + dst, screen + src, n * sizeof(*screen));
	memmove(fgs + dst, fgs + src, n * sizeof(*fgs));
	memmove(bgs + dst, bgs + src, n * sizeof(*bgs));
}

/* terminal input buffering */

#define PTYLEN			(1 << 16)

static char ptybuf[PTYLEN];		/* always emptied in term_read() */
static int ptylen;			/* buffer length */
static int ptycur;			/* current offset */

static int waitpty(int us)
{
	struct pollfd ufds[1];
	ufds[0].fd = term->fd;
	ufds[0].events = POLLIN;
	return poll(ufds, 1, us) <= 0;
}

static int readpty(void)
{
	int nr;
	if (ptycur < ptylen)
		return (unsigned char) ptybuf[ptycur++];
	if (!term->fd)
		return -1;
	ptylen = 0;
	while ((nr = read(term->fd, ptybuf + ptylen, PTYLEN - ptylen)) > 0)
		ptylen += nr;
	if (!ptylen && errno == EAGAIN && !waitpty(100))
		ptylen = read(term->fd, ptybuf, PTYLEN);
	ptycur = 1;
	return ptylen > 0 ? (unsigned char) ptybuf[0] : -1;
}

/* term interface functions */

static void term_zero(struct term *term)
{
	memset(term->screen, 0, pad_rows() * pad_cols() * sizeof(term->screen[0]));
	memset(term->hist, 0, NHIST * pad_cols() * sizeof(term->hist[0]));
	memset(term->fgs, 0, pad_rows() * pad_cols() * sizeof(term->fgs[0]));
	memset(term->bgs, 0, pad_rows() * pad_cols() * sizeof(term->bgs[0]));
	memset(term->dirty, 0, pad_rows() * sizeof(term->dirty[0]));
	memset(&term->cur, 0, sizeof(term->cur));
	memset(&term->sav, 0, sizeof(term->sav));
	term->fd = 0;
	term->hrow = 0;
	term->hpos = 0;
	term->lazy = 0;
	term->pid = 0;
	term->top = 0;
	term->bot = 0;
	term->rows = 0;
	term->cols = 0;
	term->signal = 0;
}

struct term *term_make(void)
{
	struct term *term = malloc(sizeof(*term));
	term->screen = malloc(pad_rows() * pad_cols() * sizeof(term->screen[0]));
	term->hist = malloc(NHIST * pad_cols() * sizeof(term->hist[0]));
	term->fgs = malloc(pad_rows() * pad_cols() * sizeof(term->fgs[0]));
	term->bgs = malloc(pad_rows() * pad_cols() * sizeof(term->bgs[0]));
	term->dirty = malloc(pad_rows() * sizeof(term->dirty[0]));
	term_zero(term);
	return term;
}

void term_free(struct term *term)
{
	free(term->screen);
	free(term->hist);
	free(term->fgs);
	free(term->bgs);
	free(term->dirty);
	free(term);
}

int term_fd(struct term *term)
{
	return term->fd;
}

void term_send(int c)
{
	char b = c;
	if (term->fd)
		write(term->fd, &b, 1);
}

static void term_sendstr(char *s)
{
	if (term->fd)
		write(term->fd, s, strlen(s));
}

static void term_blank(void)
{
	screen_reset(0, pad_rows() * pad_cols());
	if (visible)
		pad_fill(0, -1, 0, -1, bgcolor() & FN_C);
}

static void ctlseq(void);
void term_read(void)
{
	ctlseq();
	while (ptycur < ptylen) {
		if (visible && !lazy && ptylen - ptycur > 15)
			lazy_start();
		ctlseq();
	}
	lazy_flush();
}

static void term_reset(void)
{
	row = col = 0;
	top = 0;
	bot = pad_rows();
	mode = MODE_CURSOR | MODE_WRAP | MODE_CLR8;
	fg = FGCOLOR;
	bg = BGCOLOR;
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

static void tio_setsize(int fd)
{
	struct winsize winp;
	winp.ws_col = pad_cols();
	winp.ws_row = pad_rows();
	winp.ws_xpixel = 0;
	winp.ws_ypixel = 0;
	ioctl(fd, TIOCSWINSZ, &winp);
}

static void tio_login(int fd)
{
	setsid();
	ioctl(fd, TIOCSCTTY, NULL);
	tio_setsize(fd);
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
}

static void execvep(char *cmd, char **argv, char **envp)
{
	char path[512];
	char *p = getenv("PATH");
	execve(cmd, argv, envp);
	while (*p) {
		char *s = path;
		while (*p && *p != ':')
			*s++ = *p++;
		*s++ = '/';
		strcpy(s, cmd);
		execve(path, argv, envp);
		while (*p == ':')
			p++;
	}
}

static void envcpy(char **d, char **s, int len)
{
	int i = 0;
	for (i = 0; i < len - 1 && s[i]; i++)
		d[i] = s[i];
	d[i] = NULL;
}

static void envset(char **d, char *env)
{
	int i;
	int len = strchr(env, '=') - env;
	for (i = 0; d[i]; i++) {
		if (memcmp(d[i], env, len))
			break;
	}
	d[i] = env;
}

extern char **environ;
void term_exec(char **args, int swsig)
{
	int master, slave;
	term_zero(term);
	if (_openpty(&master, &slave) == -1)
		return;
	if ((term->pid = fork()) == -1)
		return;
	if (!term->pid) {
		char *envp[256] = {NULL};
		char pgid[32];
		snprintf(pgid, sizeof(pgid), "TERM_PGID=%d", getpid());
		envcpy(envp, environ, LEN(envp) - 3);
		envset(envp, "TERM=" TERM);
		envset(envp, pad_fbdev());
		if (swsig)
			envset(envp, pgid);
		tio_login(slave);
		close(master);
		execvep(args[0], args, envp);
		exit(1);
	}
	close(slave);
	term->fd = master;
	term->rows = pad_rows();
	term->cols = pad_cols();
	term->signal = swsig;
	fcntl(term->fd, F_SETFD, fcntl(term->fd, F_GETFD) | FD_CLOEXEC);
	fcntl(term->fd, F_SETFL, fcntl(term->fd, F_GETFL) | O_NONBLOCK);
	term_reset();
	memset(term->hist, 0, NHIST * pad_cols() * sizeof(term->hist[0]));
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
	if (!lazy)
		lazy_start();
	misc_save(&term->cur);
	term->top = top;
	term->bot = bot;
	term->lazy = lazy;
}

void term_hide(struct term *term)
{
	if (term->pid > 0 && term->signal)
		kill(-term->pid, SIGUSR1);
}

void term_show(struct term *term)
{
	if (term->pid > 0 && term->signal)
		kill(-term->pid, SIGUSR2);
}

void term_signal(struct term *term)
{
	term->signal = 1;
}

static void resizeupdate(int or, int oc, int nr,  int nc)
{
	int dr = row >= nr ? row - nr + 1 : 0;
	int dst = nc <= oc ? 0 : nr * nc - 1;
	while (dst >= 0 && dst < nr * nc) {
		int r = dst / nc;
		int c = dst % nc;
		int src = dr + r < or && c < oc ? (dr + r) * oc + c : -1;
		term->screen[dst] = src >= 0 ? term->screen[src] : 0;
		term->fgs[dst] = src >= 0 ? term->fgs[src] : fgcolor();
		term->bgs[dst] = src >= 0 ? term->bgs[src] : bgcolor();
		dst = nc <= oc ? dst + 1 : dst - 1;
	}
}

/* redraw the screen; if all is zero, update changed lines only */
void term_redraw(int all)
{
	if (term->fd) {
		if (term->rows != pad_rows() || term->cols != pad_cols()) {
			tio_setsize(term->fd);
			resizeupdate(term->rows, term->cols, pad_rows(), pad_cols());
			if (bot == term->rows)
				bot = pad_rows();
			term->rows = pad_rows();
			term->cols = pad_cols();
			top = MIN(top, term->rows);
			bot = MIN(bot, term->rows);
			row = MIN(row, term->rows - 1);
			col = MIN(col, term->cols - 1);
		}
		if (all) {
			pad_fill(pad_rows(), -1, 0, -1, BGCOLOR);
			lazy_start();
			memset(dirty, 1, pad_rows() * sizeof(*dirty));
		}
		if (all || !term->hpos)
			lazy_flush();
	} else {
		if (all)
			pad_fill(0, -1, 0, -1, 0);
	}
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
	lazy = term->lazy;
	dirty = term->dirty;
}

void term_end(void)
{
	if (term->fd)
		close(term->fd);
	term_zero(term);
	term_load(term, visible);
	if (visible)
		term_redraw(1);
}

static int writeutf8(char *dst, int c)
{
	char *d = dst;
	int l;
	if (c > 0xffff) {
		*d++ = 0xf0 | (c >> 18);
		l = 3;
	} else if (c > 0x7ff) {
		*d++ = 0xe0 | (c >> 12);
		l = 2;
	} else if (c > 0x7f) {
		*d++ = 0xc0 | (c >> 6);
		l = 1;
	} else {
		*d++ = c > 0 ? c : ' ';
		l = 0;
	}
	while (l--)
		*d++ = 0x80 | ((c >> (l * 6)) & 0x3f);
	return d - dst;
}

void term_screenshot(void)
{
	char buf[1 << 11];
	int fd = open(SCRSHOT, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	int i, j;
	for (i = 0; i < pad_rows(); i++) {
		char *s = buf;
		for (j = 0; j < pad_cols(); j++)
			if (~screen[OFFSET(i, j)] & DWCHAR)
				s += writeutf8(s, screen[OFFSET(i, j)]);
		*s++ = '\n';
		write(fd, buf, s - buf);
	}
	close(fd);
}

/* high-level drawing functions */

static void empty_rows(int sr, int er)
{
	screen_reset(OFFSET(sr, 0), (er - sr) * pad_cols());
}

static void blank_rows(int sr, int er)
{
	empty_rows(sr, er);
	draw_rows(sr, er);
	draw_cursor(1);
}

#define HISTROW(pos)	(term->hist + ((term->hrow + NHIST - (pos)) % NHIST) * pad_cols())

static void scrl_rows(int nr)
{
	int i;
	for (i = 0; i < nr; i++) {
		memcpy(HISTROW(0), screen + i * pad_cols(),
				pad_cols() * sizeof(screen[0]));
		term->hrow = (term->hrow + 1) % NHIST;
	}
}

void term_scrl(int scrl)
{
	int i, j;
	int hpos = LIMIT(term->hpos + scrl, 0, NHIST);
	term->hpos = hpos;
	if (!hpos) {
		lazy_flush();
		return;
	}
	lazy_start();
	memset(dirty, 1, pad_rows() * sizeof(*dirty));
	for (i = 0; i < pad_rows(); i++) {
		int off = (i - hpos) * pad_cols();
		int *_scr = i < hpos ? HISTROW(hpos - i) : term->screen + off;
		int *_fgs = i < hpos ? NULL : term->fgs + off;
		int *_bgs = i < hpos ? NULL : term->bgs + off;
		for (j = 0; j < pad_cols(); j++)
			pad_put(_scr[j], i, j, _fgs ? _fgs[j] : BGCOLOR,
						_bgs ? _bgs[j] : FGCOLOR);
	}
}

static void scroll_screen(int sr, int nr, int n)
{
	draw_cursor(0);
	if (sr + n == 0)
		scrl_rows(sr);
	screen_move(OFFSET(sr + n, 0), OFFSET(sr, 0), nr * pad_cols());
	if (n > 0)
		empty_rows(sr, sr + n);
	else
		empty_rows(sr + nr + n, sr + nr);
	draw_rows(MIN(sr, sr + n), MAX(sr + nr, sr + nr + n));
	draw_cursor(1);
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

static int origin(void)
{
	return mode & MODE_ORIGIN;
}

static void move_cursor(int r, int c)
{
	int t, b;
	draw_cursor(0);
	t = origin() ? top : 0;
	b = origin() ? bot : pad_rows();
	row = LIMIT(r, t, b - 1);
	col = LIMIT(c, 0, pad_cols() - 1);
	draw_cursor(1);
	mode = BIT_SET(mode, MODE_WRAPREADY, 0);
}

static void set_region(int t, int b)
{
	top = LIMIT(t - 1, 0, pad_rows() - 1);
	bot = LIMIT(b ? b : pad_rows(), top + 1, pad_rows());
	if (origin())
		move_cursor(top, 0);
}

static void setattr(int m)
{
	if (!m || (m / 10) == 3)
		mode |= MODE_CLR8;
	switch (m) {
	case 0:
		fg = FGCOLOR;
		bg = BGCOLOR;
		mode &= ~ATTR_ALL;
		break;
	case 1:
		mode |= ATTR_BOLD;
		break;
	case 3:
		mode |= ATTR_ITALIC;
		break;
	case 7:
		mode |= ATTR_REV;
		break;
	case 22:
		mode &= ~ATTR_BOLD;
		break;
	case 23:
		mode &= ~ATTR_ITALIC;
		break;
	case 27:
		mode &= ~ATTR_REV;
		break;
	default:
		if ((m / 10) == 3)
			fg = m > 37 ? FGCOLOR : clrmap(m - 30);
		if ((m / 10) == 4)
			bg = m > 47 ? BGCOLOR : clrmap(m - 40);
		if ((m / 10) == 9)
			fg = clrmap(8 + m - 90);
		if ((m / 10) == 10)
			bg = clrmap(8 + m - 100);
	}
}

static void kill_chars(int sc, int ec)
{
	int i;
	for (i = sc; i < ec; i++)
		draw_char(0, row, i);
	draw_cursor(1);
}

static void move_chars(int sc, int nc, int n)
{
	draw_cursor(0);
	screen_move(OFFSET(row, sc + n), OFFSET(row, sc), nc);
	if (n > 0)
		screen_reset(OFFSET(row, sc), n);
	else
		screen_reset(OFFSET(row, pad_cols() + n), -n);
	draw_cols(row, MIN(sc, sc + n), pad_cols());
	draw_cursor(1);
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

static void advance(int dr, int dc, int scrl)
{
	int r = row + dr;
	int c = col + dc;
	if (dr && r >= bot && scrl) {
		int n = bot - r - 1;
		int nr = (bot - top) + n;
		if (nr > 0)
			scroll_screen(top + -n, nr, n);
	}
	if (dr && r < top && scrl) {
		int n = top - r;
		int nr = (bot - top) - n;
		if (nr > 0)
			scroll_screen(top, nr, n);
	}
	r = dr ? LIMIT(r, top, bot - 1) : r;
	c = LIMIT(c, 0, pad_cols() - 1);
	move_cursor(r, c);
}

static void insertchar(int c)
{
	if (mode & MODE_WRAPREADY)
		advance(1, -col, 1);
	if (mode & MODE_INSERT)
		insert_chars(1);
	draw_char(c, row, col);
	if (col == pad_cols() - 1)
		mode = BIT_SET(mode, MODE_WRAPREADY, 1);
	else
		advance(0, 1, 1);
}


/* partial vt102 implementation */

static void escseq(void);
static void escseq_cs(void);
static void escseq_g0(void);
static void escseq_g1(void);
static void escseq_g2(void);
static void escseq_g3(void);
static void csiseq(void);
static void csiseq_da(int c);
static void csiseq_dsr(int c);
static void modeseq(int c, int set);

/* comments taken from: http://www.ivarch.com/programs/termvt102.shtml */

static int readutf8(int c)
{
	int result;
	int l = 1;
	if (~c & 0xc0)
		return c;
	while (l < 6 && c & (0x40 >> l))
		l++;
	result = (0x3f >> l) & c;
	while (l--)
		result = (result << 6) | (readpty() & 0x3f);
	return result;
}

#define unknown(ctl, c)

/* control sequences */
static void ctlseq(void)
{
	int c = readpty();
	switch (c) {
	case 0x09:	/* HT		horizontal tab to next tab stop */
		advance(0, 8 - col % 8, 0);
		break;
	case 0x0a:	/* LF		line feed */
	case 0x0b:	/* VT		line feed */
	case 0x0c:	/* FF		line feed */
		advance(1, (mode & MODE_AUTOCR) ? -col : 0, 1);
		break;
	case 0x08:	/* BS		backspace one column */
		advance(0, -1, 0);
		break;
	case 0x1b:	/* ESC		start escape sequence */
		escseq();
		break;
	case 0x0d:	/* CR		carriage return */
		advance(0, -col, 0);
		break;
	case 0x9b:	/* CSI		equivalent to ESC [ */
		csiseq();
		break;
	case 0x00:	/* NUL		ignored */
	case 0x07:	/* BEL		beep */
	case 0x7f:	/* DEL		ignored */
		break;
	case 0x05:	/* ENQ		trigger answerback message */
	case 0x0e:	/* SO		activate G1 character set & newline */
	case 0x0f:	/* SI		activate G0 character set */
	case 0x11:	/* XON		resume transmission */
	case 0x13:	/* XOFF		stop transmission, ignore characters */
	case 0x18:	/* CAN		interrupt escape sequence */
	case 0x1a:	/* SUB		interrupt escape sequence */
		unknown("ctlseq", c);
		break;
	default:
		c = readutf8(c);
		if (isdw(c) && col + 1 == pad_cols() && ~mode & MODE_WRAPREADY)
			insertchar(0);
		if (!iszw(c))
			insertchar(c);
		if (isdw(c))
			insertchar(c | DWCHAR);
		break;
	}
}

#define ESCM(c)		(((c) & 0xf0) == 0x20)
#define ESCF(c)		((c) > 0x30 && (c) < 0x7f)

/* escape sequences */
static void escseq(void)
{
	int c = readpty();
	while (ESCM(c))
		c = readpty();
	switch (c) {
	case '[':	/* CSI		control sequence introducer */
		csiseq();
		break;
	case '%':	/* CS...	escseq_cs table */
		escseq_cs();
		break;
	case '(':	/* G0...	escseq_g0 table */
		escseq_g0();
		break;
	case ')':	/* G1...	escseq_g1 table */
		escseq_g1();
		break;
	case '*':	/* G2...	escseq_g2 table */
		escseq_g2();
		break;
	case '+':	/* G3...	escseq_g3 table */
		escseq_g3();
		break;
	case '7':	/* DECSC	save state (position, charset, attributes) */
		misc_save(&term->sav);
		break;
	case '8':	/* DECRC	restore most recently saved state */
		misc_load(&term->sav);
		break;
	case 'M':	/* RI		reverse line feed */
		advance(-1, 0, 1);
		break;
	case 'D':	/* IND		line feed */
		advance(1, 0, 1);
		break;
	case 'E':	/* NEL		newline */
		advance(1, -col, 1);
		break;
	case 'c':	/* RIS		reset */
		term_reset();
		break;
	case 'H':	/* HTS		set tab stop at current column */
	case 'Z':	/* DECID	DEC private ID; return ESC [ ? 6 c (VT102) */
	case '#':	/* DECALN	("#8") DEC alignment test - fill screen with E's */
	case '>':	/* DECPNM	set numeric keypad mode */
	case '=':	/* DECPAM	set application keypad mode */
	case 'N':	/* SS2		select G2 charset for next char only */
	case 'O':	/* SS3		select G3 charset for next char only */
	case 'P':	/* DCS		device control string (ended by ST) */
	case 'X':	/* SOS		start of string */
	case '^':	/* PM		privacy message (ended by ST) */
	case '_':	/* APC		application program command (ended by ST) */
	case '\\':	/* ST		string terminator */
	case 'n':	/* LS2		invoke G2 charset */
	case 'o':	/* LS3		invoke G3 charset */
	case '|':	/* LS3R		invoke G3 charset as GR */
	case '}':	/* LS2R		invoke G2 charset as GR */
	case '~':	/* LS1R		invoke G1 charset as GR */
	case ']':	/* OSC		operating system command */
	case 'g':	/* BEL		alternate BEL */
	default:
		unknown("escseq", c);
		break;
	}
}

static void escseq_cs(void)
{
	int c = readpty();
	switch (c) {
	case '@':	/* CSDFL	select default charset (ISO646/8859-1) */
	case 'G':	/* CSUTF8	select UTF-8 */
	case '8':	/* CSUTF8	select UTF-8 (obsolete) */
	default:
		unknown("escseq_cs", c);
		break;
	}
}

static void escseq_g0(void)
{
	int c = readpty();
	switch (c) {
	case '8':	/* G0DFL	G0 charset = default mapping (ISO8859-1) */
	case '0':	/* G0GFX	G0 charset = VT100 graphics mapping */
	case 'U':	/* G0ROM	G0 charset = null mapping (straight to ROM) */
	case 'K':	/* G0USR	G0 charset = user defined mapping */
	case 'B':	/* G0TXT	G0 charset = ASCII mapping */
	default:
		unknown("escseq_g0", c);
		break;
	}
}

static void escseq_g1(void)
{
	int c = readpty();
	switch (c) {
	case '8':	/* G1DFL	G1 charset = default mapping (ISO8859-1) */
	case '0':	/* G1GFX	G1 charset = VT100 graphics mapping */
	case 'U':	/* G1ROM	G1 charset = null mapping (straight to ROM) */
	case 'K':	/* G1USR	G1 charset = user defined mapping */
	case 'B':	/* G1TXT	G1 charset = ASCII mapping */
	default:
		unknown("escseq_g1", c);
		break;
	}
}

static void escseq_g2(void)
{
	int c = readpty();
	switch (c) {
	case '8':	/* G2DFL	G2 charset = default mapping (ISO8859-1) */
	case '0':	/* G2GFX	G2 charset = VT100 graphics mapping */
	case 'U':	/* G2ROM	G2 charset = null mapping (straight to ROM) */
	case 'K':	/* G2USR	G2 charset = user defined mapping */
	default:
		unknown("escseq_g2", c);
		break;
	}
}

static void escseq_g3(void)
{
	int c = readpty();
	switch (c) {
	case '8':	/* G3DFL	G3 charset = default mapping (ISO8859-1) */
	case '0':	/* G3GFX	G3 charset = VT100 graphics mapping */
	case 'U':	/* G3ROM	G3 charset = null mapping (straight to ROM) */
	case 'K':	/* G3USR	G3 charset = user defined mapping */
	default:
		unknown("escseq_g3", c);
		break;
	}
}

static int absrow(int r)
{
	return origin() ? top + r : r;
}

#define CSIP(c)			(((c) & 0xf0) == 0x30)
#define CSII(c)			(((c) & 0xf0) == 0x20)
#define CSIF(c)			((c) >= 0x40 && (c) < 0x80)

#define MAXCSIARGS	32
/* ECMA-48 CSI sequences */
static void csiseq(void)
{
	int args[MAXCSIARGS + 8] = {0};
	int i;
	int n = 0;
	int c = readpty();
	int priv = 0;

	if (strchr("<=>?", c)) {
		priv = c;
		c = readpty();
	}
	while (CSIP(c)) {
		int arg = 0;
		while (isdigit(c)) {
			arg = arg * 10 + (c - '0');
			c = readpty();
		}
		if (CSIP(c))
			c = readpty();
		if (n < MAXCSIARGS)
			args[n++] = arg;
	}
	while (CSII(c))
		c = readpty();
	switch (c) {
	case 'H':	/* CUP		move cursor to row, column */
	case 'f':	/* HVP		move cursor to row, column */
		move_cursor(absrow(MAX(0, args[0] - 1)), MAX(0, args[1] - 1));
		break;
	case 'J':	/* ED		erase display */
		switch (args[0]) {
		case 0:
			kill_chars(col, pad_cols());
			blank_rows(row + 1, pad_rows());
			break;
		case 1:
			kill_chars(0, col + 1);
			blank_rows(0, row - 1);
			break;
		case 2:
			term_blank();
			break;
		}
		break;
	case 'A':	/* CUU		move cursor up */
		advance(-MAX(1, args[0]), 0, 0);
		break;
	case 'e':	/* VPR		move cursor down */
	case 'B':	/* CUD		move cursor down */
		advance(MAX(1, args[0]), 0, 0);
		break;
	case 'a':	/* HPR		move cursor right */
	case 'C':	/* CUF		move cursor right */
		advance(0, MAX(1, args[0]), 0);
		break;
	case 'D':	/* CUB		move cursor left */
		advance(0, -MAX(1, args[0]), 0);
		break;
	case 'K':	/* EL		erase line */
		switch (args[0]) {
		case 0:
			kill_chars(col, pad_cols());
			break;
		case 1:
			kill_chars(0, col + 1);
			break;
		case 2:
			kill_chars(0, pad_cols());
			break;
		}
		break;
	case 'L':	/* IL		insert blank lines */
		if (row >= top && row < bot)
			insert_lines(MAX(1, args[0]));
		break;
	case 'M':	/* DL		delete lines */
		if (row >= top && row < bot)
			delete_lines(MAX(1, args[0]));
		break;
	case 'S':	/* SU		scroll up */
		i = MAX(1, args[0]);
		scroll_screen(i, pad_rows() - i, -i);
		break;
	case 'T':	/* SD		scroll down */
		i = MAX(1, args[0]);
		scroll_screen(0, pad_rows() - i, i);
		break;
	case 'd':	/* VPA		move to row (current column) */
		move_cursor(absrow(MAX(1, args[0]) - 1), col);
		break;
	case 'm':	/* SGR		set graphic rendition */
		if (!n)
			setattr(0);
		for (i = 0; i < n; i++) {
			if (args[i] == 38 && args[i + 1] == 2) {
				mode &= ~MODE_CLR8;
				fg = (args[i + 2] << 16) |
					(args[i + 3] << 8) | args[i + 4];
				i += 5;
				continue;
			}
			if (args[i] == 38) {
				mode &= ~MODE_CLR8;
				fg = clrmap(args[i + 2]);
				i += 2;
				continue;
			}
			if (args[i] == 48 && args[i + 1] == 2) {
				bg = (args[i + 2] << 16) |
					(args[i + 3] << 8) | args[i + 4];
				i += 5;
				continue;
			}
			if (args[i] == 48) {
				bg = clrmap(args[i + 2]);
				i += 2;
				continue;
			}
			setattr(args[i]);
		}
		if (mode & MODE_CLR8 && mode & ATTR_BOLD && BRIGHTEN)
			for (i = 0; i < 8; i++)
				if (clr16[i] == fg)
					fg = clr16[8 + i];
		break;
	case 'r':	/* DECSTBM	set scrolling region to (top, bottom) rows */
		set_region(args[0], args[1]);
		break;
	case 'c':	/* DA		return ESC [ ? 6 c (VT102) */
		csiseq_da(priv == '?' ? args[0] | 0x80 : args[0]);
		break;
	case 'h':	/* SM		set mode */
		for (i = 0; i < n; i++)
			modeseq(priv == '?' ? args[i] | 0x80 : args[i], 1);
		draw_cursor(1);
		break;
	case 'l':	/* RM		reset mode */
		for (i = 0; i < n; i++)
			modeseq(priv == '?' ? args[i] | 0x80 : args[i], 0);
		draw_cursor(1);
		break;
	case 'P':	/* DCH		delete characters on current line */
		delete_chars(LIMIT(args[0], 1, pad_cols() - col));
		break;
	case '@':	/* ICH		insert blank characters */
		insert_chars(LIMIT(args[0], 1, pad_cols() - col));
		break;
	case 'n':	/* DSR		device status report */
		csiseq_dsr(args[0]);
		break;
	case 'G':	/* CHA		move cursor to column in current row */
		advance(0, MAX(0, args[0] - 1) - col, 0);
		break;
	case 'X':	/* ECH		erase characters on current line */
		kill_chars(col, MIN(col + MAX(1, args[0]), pad_cols()));
		break;
	case '[':	/* IGN		ignored control sequence */
	case 'E':	/* CNL		move cursor down and to column 1 */
	case 'F':	/* CPL		move cursor up and to column 1 */
	case 'g':	/* TBC		clear tab stop (CSI 3 g = clear all stops) */
	case 'q':	/* DECLL	set keyboard LEDs */
	case 's':	/* CUPSV	save cursor position */
	case 'u':	/* CUPRS	restore cursor position */
	case '`':	/* HPA		move cursor to column in current row */
	default:
		unknown("csiseq", c);
		break;
	}
}

static void csiseq_da(int c)
{
	switch (c) {
	case 0x00:
		term_sendstr("\x1b[?6c");
		break;
	default:
		/* we don't care much about cursor shape */
		/* printf("csiseq_da <0x%x>\n", c); */
		break;
	}
}

static void csiseq_dsr(int c)
{
	char status[1 << 5];
	switch (c) {
	case 0x05:
		term_sendstr("\x1b[0n");
		break;
	case 0x06:
		sprintf(status, "\x1b[%d;%dR",
			 (origin() ? row - top : row) + 1, col + 1);
		term_sendstr(status);
		break;
	default:
		unknown("csiseq_dsr", c);
		break;
	}
}

/* ANSI/DEC specified modes for SM/RM ANSI Specified Modes */
static void modeseq(int c, int set)
{
	switch (c) {
	case 0x87:	/* DECAWM	Auto Wrap */
		mode = BIT_SET(mode, MODE_WRAP, set);
		break;
	case 0x99:	/* DECTCEM	Cursor on (set); Cursor off (reset) */
		mode = BIT_SET(mode, MODE_CURSOR, set);
		break;
	case 0x86:	/* DECOM	Sets relative coordinates (set); Sets absolute coordinates (reset) */
		mode = BIT_SET(mode, MODE_ORIGIN, set);
		break;
	case 0x14:	/* LNM		Line Feed / New Line Mode */
		mode = BIT_SET(mode, MODE_AUTOCR, set);
		break;
	case 0x04:	/* IRM		insertion/replacement mode (always reset) */
		mode = BIT_SET(mode, MODE_INSERT, set);
		break;
	case 0x00:	/* IGN		error (ignored) */
	case 0x01:	/* GATM		guarded-area transfer mode (ignored) */
	case 0x02:	/* KAM		keyboard action mode (always reset) */
	case 0x03:	/* CRM		control representation mode (always reset) */
	case 0x05:	/* SRTM		status-reporting transfer mode */
	case 0x06:	/* ERM		erasure mode (always set) */
	case 0x07:	/* VEM		vertical editing mode (ignored) */
	case 0x08:	/* BDSM		bi-directional support mode */
	case 0x0a:	/* HEM		horizontal editing mode */
	case 0x0b:	/* PUM		positioning unit mode */
	case 0x0c:	/* SRM		send/receive mode (echo on/off) */
	case 0x0d:	/* FEAM		format effector action mode */
	case 0x0e:	/* FETM		format effector transfer mode */
	case 0x0f:	/* MATM		multiple area transfer mode */
	case 0x10:	/* TTM		transfer termination mode */
	case 0x11:	/* SATM		selected area transfer mode */
	case 0x12:	/* TSM		tabulation stop mode */
	case 0x13:	/* EBM		editing boundary mode */
	/* DEC Private Modes: "?NUM" -> (NUM | 0x80) */
	case 0x80:	/* IGN		error (ignored) */
	case 0x81:	/* DECCKM	cursorkeys application (set); cursorkeys normal (reset) */
	case 0x82:	/* DECANM	ANSI (set); VT52 (reset) */
	case 0x83:	/* DECCOLM	132 columns (set); 80 columns (reset) */
	case 0x84:	/* DECSCLM	jump scroll (set); smooth scroll (reset) */
	case 0x85:	/* DECSCNM	reverse screen (set); normal screen (reset) */
	case 0x88:	/* DECARM	auto repeat */
	case 0x89:	/* DECINLM	interlace */
	case 0x92:	/* DECPFF	send FF to printer after print screen (set); no char after PS (reset) */
	case 0x93:	/* DECPEX	print screen: prints full screen (set); prints scroll region (reset) */
	default:
		unknown("modeseq", c);
		break;
	}
}
