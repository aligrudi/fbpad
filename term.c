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

/* encoding of term->scrfn entries */
#define FN_MK(fg, bg)		((fg) | ((bg) << 14))
#define FN_FG(c)		((c) & 0x3fff)
#define FN_BG(c)		(((c) >> 14) & 0x3fff)
#define FN_M(c)			((c) & (FN_B | FN_I))
/* fg or bg colours: colour index or 12-bit RGB */
#define XG_RGB			0x1000	/* 12-bit RGB */
#define XG_FG			0x0100	/* index of fg colour */
#define XG_BG			0x0101	/* index of bg colour */

#define LIMIT(n, a, b)		((n) < (a) ? (a) : ((n) > (b) ? (b) : (n)))
#define BIT_SET(i, b, val)	((val) ? ((i) | (b)) : ((i) & ~(b)))
#define OFFSET(r, c)		((r) * cols + (c))

struct term_state {
	int row, col;
	int fg, bg;
	int mode;
};

struct term {
	char recv[256];			/* receive buffer */
	char send[256];			/* send buffer */
	int recv_n;			/* number of buffered bytes in recv[] */
	int send_n;			/* number of buffered bytes in send[] */
	int *scrch;			/* screen characters */
	int *scrfn;			/* screen foreground/background colour */
	int *hist;			/* scrolling history */
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
static int lazy;
static int rows, cols;
static int row, col;
static int fg, bg;
static int top, bot;
static int mode;
static int visible;
static int clrfg = FGCOLOR;
static int clrbg = BGCOLOR;
static int cursorfg = -1;
static int cursorbg = -1;
static int borderfg = 0xff0000;
static int borderwd = 2;

static unsigned int clr16[16] = {
	COLOR0, COLOR1, COLOR2, COLOR3, COLOR4, COLOR5, COLOR6, COLOR7,
	COLOR8, COLOR9, COLORA, COLORB, COLORC, COLORD, COLORE, COLORF,
};

static int clrmap_rgb(int r, int g, int b)
{
	return XG_RGB | ((r & 0xf0) << 4 | (g & 0xf0) | (b >> 4));
}

static int clrmap_rgbdec(int c)
{
	int r = (c >> 4) & 0xf0;
	int g = c & 0xf0;
	int b = (c & 0x0f) << 4;
	return (r << 16) | (g << 8) | b;
}

static int clrmap(int c)
{
	int g = (c - 232) * 10 + 8;
	if (c < 16)
		return clr16[c];
	if (c == XG_FG)
		return clrfg;
	if (c == XG_BG)
		return clrbg;
	if (c & XG_RGB)
		return clrmap_rgbdec(c);
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

static int color(void)
{
	int c = mode & ATTR_REV ? FN_MK(bg, fg) : FN_MK(fg, bg);
	if (mode & ATTR_BOLD)
		c |= FN_B;
	if (mode & ATTR_ITALIC)
		c |= FN_I;
	return c;
}

/* assumes visible && !lazy */
static void _draw_pos(int r, int c, int cursor)
{
	int i = OFFSET(r, c);
	int fg = clrmap(FN_FG(term->scrfn[i]));
	int bg = clrmap(FN_BG(term->scrfn[i]));
	if (cursor && mode & MODE_CURSOR) {
		fg = cursorfg >= 0 ? cursorfg : clrmap(FN_BG(term->scrfn[i]));
		bg = cursorbg >= 0 ? cursorbg : clrmap(FN_FG(term->scrfn[i]));
	}
	pad_put(term->scrch[i], r, c, FN_M(term->scrfn[i]) | fg, bg);
}

/* assumes visible && !lazy */
static void _draw_row(int r)
{
	int cbg, cch;		/* current background and character */
	int fbg, fsc = -1;	/* filling background and start column */
	int *scrch = term->scrch;
	int i;
	/* call pad_fill() only once for blank columns with identical backgrounds */
	for (i = 0; i < cols; i++) {
		cbg = FN_BG(term->scrfn[OFFSET(r, i)]);
		cch = scrch[OFFSET(r, i)] ? scrch[OFFSET(r, i)] : ' ';
		if (fsc >= 0 && (cbg != fbg || cch != ' ')) {
			pad_fill(r, r + 1, fsc, i, clrmap(fbg));
			fsc = -1;
		}
		if (cch != ' ') {
			_draw_pos(r, i, 0);
		} else if (fsc < 0) {
			fsc = i;
			fbg = cbg;
		}
	}
	pad_fill(r, r + 1, fsc >= 0 ? fsc : cols, -1, clrmap(cbg));
}

static int candraw(int sr, int er)
{
	int i;
	if (lazy)
		for (i = sr; i < er; i++)
			term->dirty[i] = 1;
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
	term->scrch[i] = ch;
	term->scrfn[i] = color();
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
	memset(term->dirty, 0, rows * sizeof(term->dirty[0]));
	lazy = 1;
}

static void lazy_flush(void)
{
	int i;
	if (!visible || !lazy)
		return;
	for (i = 0; i < rows; i++)
		if (term->dirty[i])
			_draw_row(i);
	if (term->dirty[row])
		_draw_pos(row, col, 1);
	lazy = 0;
	term->hpos = 0;
}

static void screen_reset(int i, int n)
{
	int c;
	candraw(i / cols, (i + n) / cols);
	memset(term->scrch + i, 0, n * sizeof(*term->scrch));
	for (c = 0; c < n; c++)
		term->scrfn[i + c] = FN_MK(fg, bg);
}

static void screen_move(int dst, int src, int n)
{
	int srow = (MIN(src, dst) + (n > 0 ? 0 : n)) / cols;
	int drow = (MAX(src, dst) + (n > 0 ? n : 0)) / cols;
	candraw(srow, drow);
	memmove(term->scrch + dst, term->scrch + src, n * sizeof(*term->scrch));
	memmove(term->scrfn + dst, term->scrfn + src, n * sizeof(*term->scrfn));
}

/* terminal input buffering */

#define PTYLEN			(1 << 16)
#define pty_mark()		(ptyreq = ptycur)
#define pty_back()		(ptycur = ptyreq)
#define pty_left()		(ptylen - ptycur)

static char ptybuf[PTYLEN];		/* always emptied in term_read() */
static int ptylen;			/* buffer length */
static int ptycur;			/* current offset */
static int ptyreq;			/* the beginning of the last request */

static int pty_wait(int out, int us)
{
	struct pollfd ufds[1];
	ufds[0].fd = term->fd;
	ufds[0].events = out ? POLLOUT : POLLIN;
	return poll(ufds, 1, us) <= 0;
}

static int pty_read(void)
{
	int nr;
	if (ptycur < ptylen)
		return (unsigned char) ptybuf[ptycur++];
	if (!term->fd)
		return -1;
	if (ptyreq < ptylen)
		memmove(ptybuf, ptybuf + ptyreq, ptylen - ptyreq);
	ptycur = ptyreq < ptycur ? ptycur - ptyreq : 0;
	ptylen = ptyreq < ptylen ? ptylen - ptyreq : 0;
	ptyreq = 0;
	while ((nr = read(term->fd, ptybuf + ptylen, PTYLEN - ptylen)) > 0)
		ptylen += nr;
	return ptycur < ptylen ? (unsigned char) ptybuf[ptycur++] : -1;
}

static void pty_load(char *buf, int len)
{
	ptycur = 0;
	ptyreq = 0;
	ptylen = MIN(sizeof(ptybuf), len);
	memcpy(ptybuf, buf, ptylen);
}

static int pty_save(char *buf, int size)
{
	int len = MIN(size, ptylen - ptycur);
	memcpy(buf, ptybuf + ptycur, len);
	return len;
}

/* term interface functions */

static void term_zero(struct term *term)
{
	int r = term->rows, c = term->cols;
	memset(term->scrch, 0, r * c * sizeof(term->scrch[0]));
	memset(term->hist, 0, NHIST * c * sizeof(term->hist[0]));
	memset(term->scrfn, 0, r * c * sizeof(term->scrfn[0]));
	memset(term->dirty, 0, r * sizeof(term->dirty[0]));
	memset(&term->cur, 0, sizeof(term->cur));
	memset(&term->sav, 0, sizeof(term->sav));
	term->fd = 0;
	term->hrow = 0;
	term->hpos = 0;
	term->lazy = 0;
	term->pid = 0;
	term->top = 0;
	term->bot = 0;
	term->signal = 0;
}

static int term_resize(struct term *term, int r, int c)
{
	if (r > term->rows || c > term->cols) {
		int *scrch = malloc(r * c * sizeof(scrch[0]));
		int *scrfn = malloc(r * c * sizeof(scrfn[0]));
		int *hist = malloc(NHIST * c * sizeof(hist[0]));
		int *dirty = malloc(r * sizeof(dirty[0]));
		int rc = MIN(r * c, term->rows * term->cols);
		if (!scrch || !scrfn || !hist || !dirty) {
			free(scrch);
			free(scrfn);
			free(hist);
			free(dirty);
			return 1;
		}
		memcpy(scrch, term->scrch, rc * sizeof(scrch[0]));
		memcpy(scrfn, term->scrfn, rc * sizeof(scrfn[0]));
		memset(dirty, 0, r * sizeof(dirty[0]));
		memset(hist, 0, NHIST * c * sizeof(hist[0]));
		free(term->scrch);
		free(term->scrfn);
		free(term->hist);
		free(term->dirty);
		term->scrch = scrch;
		term->scrfn = scrfn;
		term->hist = hist;
		term->dirty = dirty;
	}
	term->rows = r;
	term->cols = c;
	return 0;
}

struct term *term_make(void)
{
	struct term *term = malloc(sizeof(*term));
	if (!term)
		return NULL;
	memset(term, 0, sizeof(*term));
	if (term_resize(term, pad_rows(), pad_cols())) {
		term_free(term);
		return NULL;
	}
	term_zero(term);
	return term;
}

void term_free(struct term *term)
{
	free(term->scrch);
	free(term->hist);
	free(term->scrfn);
	free(term->dirty);
	free(term);
}

int term_fd(struct term *term)
{
	return term->fd;
}

static int term_flush(void)
{
	int nr;
	if (term->send_n && (nr = write(term->fd, term->send, term->send_n)) > 0) {
		if (term->send_n > nr)
			memmove(term->send, term->send + nr, term->send_n - nr);
		term->send_n -= nr;
	}
	return term->send_n == sizeof(term->send);
}

void term_send(char *s, int n)
{
	int i;
	for (i = 0; term->fd && i < 4 && n > 0 && !pty_wait(1, 50); i++) {
		int cp = MIN(n, LEN(term->send) - term->send_n);
		memcpy(term->send + term->send_n, s, cp);
		term->send_n += cp;
		s += cp;
		n -= cp;
		term_flush();
	}
}

static void term_sendstr(char *s)
{
	term_send(s, strlen(s));
}

static void term_blank(void)
{
	screen_reset(0, rows * cols);
	if (visible)
		pad_fill(0, -1, 0, -1, clrmap(FN_BG(color())));
}

static int ctlseq(void);
void term_read(void)
{
	do {
		if (ctlseq()) {
			pty_back();
			break;
		}
		pty_mark();
		if (visible && !lazy && pty_left() > 15)
			lazy_start();
	} while (pty_left() > 0);
	lazy_flush();
}

static void term_reset(void)
{
	row = col = 0;
	top = 0;
	bot = rows;
	mode = MODE_CURSOR | MODE_WRAP | MODE_CLR8;
	fg = XG_FG;
	bg = XG_BG;
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
	winp.ws_col = cols;
	winp.ws_row = rows;
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
		if (memcmp(d[i], env, len) == 0)
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
	term->signal = swsig;
	fcntl(term->fd, F_SETFD, fcntl(term->fd, F_GETFD) | FD_CLOEXEC);
	fcntl(term->fd, F_SETFL, fcntl(term->fd, F_GETFL) | O_NONBLOCK);
	term_reset();
	memset(term->hist, 0, NHIST * cols * sizeof(term->hist[0]));
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
	term->recv_n = pty_save(term->recv, sizeof(term->recv));
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
		term->scrch[dst] = src >= 0 ? term->scrch[src] : 0;
		term->scrfn[dst] = src >= 0 ? term->scrfn[src] : color();
		dst = nc <= oc ? dst + 1 : dst - 1;
	}
}

/* redraw the screen; if all is zero, update changed lines only */
void term_redraw(int all)
{
	if (term->fd) {
		int r = term->rows, c = term->cols;
		if (r != pad_rows() || c != pad_cols()) {
			if (!term_resize(term, pad_rows(), pad_cols())) {
				rows = term->rows;
				cols = term->cols;
				resizeupdate(r, c, pad_rows(), pad_cols());
				tio_setsize(term->fd);
				if (bot == r)
					bot = term->rows;
				top = MIN(top, term->rows);
				bot = MIN(bot, term->rows);
				row = MIN(row, term->rows - 1);
				col = MIN(col, term->cols - 1);
			}
		}
		if (all) {
			pad_fill(rows, -1, 0, -1, clrbg);
			lazy_start();
			memset(term->dirty, 1, rows * sizeof(term->dirty[0]));
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
	visible = flags;
	top = term->top;
	bot = term->bot;
	lazy = term->lazy;
	rows = term->rows;
	cols = term->cols;
	pty_load(term->recv, term->recv_n);
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
	if (c < 0x80) {
		*d++ = c > 0 ? c : ' ';
		return 1;
	}
	if (c < 0x800) {
		*d++ = 0xc0 | (c >> 6);
		*d++ = 0x80 | (c & 0x3f);
		return 2;
	}
	if (c < 0xffff) {
		*d++ = 0xe0 | (c >> 12);
		*d++ = 0x80 | ((c >> 6) & 0x3f);
		*d++ = 0x80 | (c & 0x3f);
		return 3;
	}
	*d++ = 0xf0 | (c >> 18);
	*d++ = 0x80 | ((c >> 12) & 0x3f);
	*d++ = 0x80 | ((c >> 6) & 0x3f);
	*d++ = 0x80 | (c & 0x3f);
	return 4;
}

void term_screenshot(char *path)
{
	char buf[1 << 11];
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	int i, j;
	for (i = 0; i < rows; i++) {
		char *s = buf;
		char *r = s;
		for (j = 0; j < cols; j++) {
			int c = term->scrch[OFFSET(i, j)];
			if (~c & DWCHAR)
				s += writeutf8(s, c);
			if (c)
				r = s;
		}
		*r++ = '\n';
		write(fd, buf, r - buf);
	}
	close(fd);
}

int term_colors(char *path)
{
	char t[256];
	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return 1;
	while (fscanf(fp, "%31s", t) == 1) {
		if (!strcmp("color", t)) {
			fscanf(fp, "%x %x", &clrfg, &clrbg);
		} else if (!strcmp("color16", t)) {
			int i;
			for (i = 0; i < 16; i++)
				fscanf(fp, "%x", &clr16[i]);
		} else if (!strcmp("font", t)) {
			char fr[256], fi[256], fb[256];
			if (fscanf(fp, "%255s %255s %255s", fr, fi, fb) == 3)
				pad_init(fr, fi, fb);
		} else if (!strcmp("cursor", t)) {
			fscanf(fp, "%x %x", &cursorfg, &cursorbg);
		} else if (!strcmp("border", t)) {
			fscanf(fp, "%x %d", &borderfg, &borderwd);
		}
		fgets(t, sizeof(t), fp);
	}
	return 0;
}

int term_borderwd(void)
{
	return borderwd;
}

int term_borderfg(void)
{
	return borderfg;
}

/* high-level drawing functions */

static void empty_rows(int sr, int er)
{
	screen_reset(OFFSET(sr, 0), (er - sr) * cols);
}

static void blank_rows(int sr, int er)
{
	empty_rows(sr, er);
	draw_rows(sr, er);
	draw_cursor(1);
}

#define HISTROW(pos)	(term->hist + ((term->hrow + NHIST - (pos)) % NHIST) * cols)

static void scrl_rows(int nr)
{
	int i;
	for (i = 0; i < nr; i++) {
		memcpy(HISTROW(0), term->scrch + i * cols,
				cols * sizeof(term->scrch[0]));
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
	memset(term->dirty, 1, rows * sizeof(term->dirty[0]));
	for (i = 0; i < rows; i++) {
		int off = (i - hpos) * cols;
		int *_scr = i < hpos ? HISTROW(hpos - i) : term->scrch + off;
		int *_clr = i < hpos ? NULL : term->scrfn + off;
		for (j = 0; j < cols; j++) {
			int c = _clr ? _clr[j] : FN_MK(XG_BG, XG_FG);
			pad_put(_scr[j], i, j, FN_M(c) | clrmap(FN_FG(c)), clrmap(FN_BG(c)));
		}
	}
}

static void scroll_screen(int sr, int nr, int n)
{
	draw_cursor(0);
	if (sr + n == 0)
		scrl_rows(sr);
	screen_move(OFFSET(sr + n, 0), OFFSET(sr, 0), nr * cols);
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
	b = origin() ? bot : rows;
	row = LIMIT(r, t, b - 1);
	col = LIMIT(c, 0, cols - 1);
	draw_cursor(1);
	mode = BIT_SET(mode, MODE_WRAPREADY, 0);
}

static void set_region(int t, int b)
{
	top = LIMIT(t - 1, 0, rows - 1);
	bot = LIMIT(b ? b : rows, top + 1, rows);
	if (origin())
		move_cursor(top, 0);
}

static void setattr(int m)
{
	if (!m || (m / 10) == 3)
		mode |= MODE_CLR8;
	switch (m) {
	case 0:
		fg = XG_FG;
		bg = XG_BG;
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
			fg = m > 37 ? XG_FG : m - 30;
		if ((m / 10) == 4)
			bg = m > 47 ? XG_BG : m - 40;
		if ((m / 10) == 9)
			fg = 8 + m - 90;
		if ((m / 10) == 10)
			bg = 8 + m - 100;
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
		screen_reset(OFFSET(row, cols + n), -n);
	draw_cols(row, MIN(sc, sc + n), cols);
	draw_cursor(1);
}

static void delete_chars(int n)
{
	int sc = col + n;
	int nc = cols - sc;
	move_chars(sc, nc, -n);
}

static void insert_chars(int n)
{
	int nc = cols - col - n;
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
	c = LIMIT(c, 0, cols - 1);
	move_cursor(r, c);
}

static void insertchar(int c)
{
	if (mode & MODE_WRAPREADY)
		advance(1, -col, 1);
	if (mode & MODE_INSERT)
		insert_chars(1);
	draw_char(c, row, col);
	if (col == cols - 1)
		mode = BIT_SET(mode, MODE_WRAPREADY, 1);
	else
		advance(0, 1, 1);
}


/* partial vt102 implementation */

static int escseq(void);
static int escseq_cs(void);
static int escseq_g0(void);
static int escseq_g1(void);
static int escseq_g2(void);
static int escseq_g3(void);
static int escseq_osc(void);
static int csiseq(void);
static int csiseq_da(int c);
static int csiseq_dsr(int c);
static int modeseq(int c, int set);

/* comments taken from: http://www.ivarch.com/programs/termvt102.shtml */

static int readutf8(int c)
{
	int c1, c2, c3;
	if (~c & 0xc0)		/* ASCII or invalid */
		return c;
	c1 = pty_read();
	if (~c & 0x20)
		return ((c & 0x1f) << 6) | (c1 & 0x3f);
	c2 = pty_read();
	if (~c & 0x10)
		return ((c & 0x0f) << 12) | ((c1 & 0x3f) << 6) | (c2 & 0x3f);
	c3 = pty_read();
	if (~c & 0x08)
		return ((c & 0x07) << 18) | ((c1 & 0x3f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f);
	return c1 < 0 || c2 < 0 || c3 < 0 ? -1 : c;
}

#define unknown(ctl, c)

/* control sequences */
static int ctlseq(void)
{
	int c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case 0x09:	/* HT		horizontal tab to next tab stop */
		advance(0, 8 - col % 8, 0);
		return 0;
	case 0x0a:	/* LF		line feed */
	case 0x0b:	/* VT		line feed */
	case 0x0c:	/* FF		line feed */
		advance(1, (mode & MODE_AUTOCR) ? -col : 0, 1);
		return 0;
	case 0x08:	/* BS		backspace one column */
		advance(0, -1, 0);
		return 0;
	case 0x1b:	/* ESC		start escape sequence */
		return escseq();
	case 0x0d:	/* CR		carriage return */
		advance(0, -col, 0);
		return 0;
	case 0x9b:	/* CSI		equivalent to ESC [ */
		return csiseq();
	case 0x00:	/* NUL		ignored */
	case 0x07:	/* BEL		beep */
	case 0x7f:	/* DEL		ignored */
		return 0;
	case 0x05:	/* ENQ		trigger answerback message */
	case 0x0e:	/* SO		activate G1 character set & newline */
	case 0x0f:	/* SI		activate G0 character set */
	case 0x11:	/* XON		resume transmission */
	case 0x13:	/* XOFF		stop transmission, ignore characters */
	case 0x18:	/* CAN		interrupt escape sequence */
	case 0x1a:	/* SUB		interrupt escape sequence */
		unknown("ctlseq", c);
		return 0;
	default:
		if ((c = readutf8(c)) < 0)
			return 1;
		if (isdw(c) && col + 1 == cols && ~mode & MODE_WRAPREADY)
			insertchar(0);
		if (!iszw(c))
			insertchar(c);
		if (isdw(c))
			insertchar(c | DWCHAR);
	}
	return 0;
}

#define ESCM(c)		(((c) & 0xf0) == 0x20)
#define ESCF(c)		((c) > 0x30 && (c) < 0x7f)

/* escape sequences */
static int escseq(void)
{
	int c = pty_read();
	while (ESCM(c))
		c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case '[':	/* CSI		control sequence introducer */
		return csiseq();
	case '%':	/* CS...	escseq_cs table */
		return escseq_cs();
	case '(':	/* G0...	escseq_g0 table */
		return escseq_g0();
	case ')':	/* G1...	escseq_g1 table */
		return escseq_g1();
	case '*':	/* G2...	escseq_g2 table */
		return escseq_g2();
	case '+':	/* G3...	escseq_g3 table */
		return escseq_g3();
	case ']':	/* OSC		operating system command */
		return escseq_osc();
	case '7':	/* DECSC	save state (position, charset, attributes) */
		misc_save(&term->sav);
		return 0;
	case '8':	/* DECRC	restore most recently saved state */
		misc_load(&term->sav);
		return 0;
	case 'M':	/* RI		reverse line feed */
		advance(-1, 0, 1);
		return 0;
	case 'D':	/* IND		line feed */
		advance(1, 0, 1);
		return 0;
	case 'E':	/* NEL		newline */
		advance(1, -col, 1);
		return 0;
	case 'c':	/* RIS		reset */
		term_reset();
		return 0;
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
	case 'g':	/* BEL		alternate BEL */
	default:
		unknown("escseq", c);
	}
	return 0;
}

static int escseq_cs(void)
{
	int c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case '@':	/* CSDFL	select default charset (ISO646/8859-1) */
	case 'G':	/* CSUTF8	select UTF-8 */
	case '8':	/* CSUTF8	select UTF-8 (obsolete) */
	default:
		unknown("escseq_cs", c);
	}
	return 0;
}

static int escseq_g0(void)
{
	int c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case '8':	/* G0DFL	G0 charset = default mapping (ISO8859-1) */
	case '0':	/* G0GFX	G0 charset = VT100 graphics mapping */
	case 'U':	/* G0ROM	G0 charset = null mapping (straight to ROM) */
	case 'K':	/* G0USR	G0 charset = user defined mapping */
	case 'B':	/* G0TXT	G0 charset = ASCII mapping */
	default:
		unknown("escseq_g0", c);
	}
	return 0;
}

static int escseq_g1(void)
{
	int c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case '8':	/* G1DFL	G1 charset = default mapping (ISO8859-1) */
	case '0':	/* G1GFX	G1 charset = VT100 graphics mapping */
	case 'U':	/* G1ROM	G1 charset = null mapping (straight to ROM) */
	case 'K':	/* G1USR	G1 charset = user defined mapping */
	case 'B':	/* G1TXT	G1 charset = ASCII mapping */
	default:
		unknown("escseq_g1", c);
	}
	return 0;
}

static int escseq_g2(void)
{
	int c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case '8':	/* G2DFL	G2 charset = default mapping (ISO8859-1) */
	case '0':	/* G2GFX	G2 charset = VT100 graphics mapping */
	case 'U':	/* G2ROM	G2 charset = null mapping (straight to ROM) */
	case 'K':	/* G2USR	G2 charset = user defined mapping */
	default:
		unknown("escseq_g2", c);
	}
	return 0;
}

static int escseq_g3(void)
{
	int c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case '8':	/* G3DFL	G3 charset = default mapping (ISO8859-1) */
	case '0':	/* G3GFX	G3 charset = VT100 graphics mapping */
	case 'U':	/* G3ROM	G3 charset = null mapping (straight to ROM) */
	case 'K':	/* G3USR	G3 charset = user defined mapping */
	default:
		unknown("escseq_g3", c);
	}
	return 0;
}

static int escseq_osc(void)
{
	int c = pty_read();
	int osc = 0;
	int i;
	while (isdigit(c)) {
		osc = osc * 10 + (c - '0');
		c = pty_read();
	}
	if (c < 0)
		return 1;
	for (i = 0; i < 4096 && c >= 0; i++) {
		if (c == 0x07)
			break;
		if (c == 0x1b) {
			c = pty_read();
			if (c == '\\')
				break;
		}
		c = pty_read();
	}
	return c < 0;
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
static int csiseq(void)
{
	int args[MAXCSIARGS + 8] = {0};
	int i;
	int n = 0;
	int c = pty_read();
	int priv = 0;

	if (c >= 0 && strchr("<=>?", c)) {
		priv = c;
		c = pty_read();
	}
	while (CSIP(c)) {
		int arg = 0;
		while (isdigit(c)) {
			arg = arg * 10 + (c - '0');
			c = pty_read();
		}
		if (CSIP(c))
			c = pty_read();
		if (n < MAXCSIARGS)
			args[n++] = arg;
	}
	while (CSII(c))
		c = pty_read();
	if (c < 0)
		return 1;
	switch (c) {
	case 'H':	/* CUP		move cursor to row, column */
	case 'f':	/* HVP		move cursor to row, column */
		move_cursor(absrow(MAX(0, args[0] - 1)), MAX(0, args[1] - 1));
		return 0;
	case 'J':	/* ED		erase display */
		switch (args[0]) {
		case 0:
			kill_chars(col, cols);
			blank_rows(row + 1, rows);
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
			kill_chars(col, cols);
			break;
		case 1:
			kill_chars(0, col + 1);
			break;
		case 2:
			kill_chars(0, cols);
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
		scroll_screen(i, rows - i, -i);
		break;
	case 'T':	/* SD		scroll down */
		i = MAX(1, args[0]);
		scroll_screen(0, rows - i, i);
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
				fg = clrmap_rgb(args[i + 2], args[i + 3], args[i + 4]);
				i += 4;
				continue;
			}
			if (args[i] == 38) {
				mode &= ~MODE_CLR8;
				fg = args[i + 2];
				i += 2;
				continue;
			}
			if (args[i] == 48 && args[i + 1] == 2) {
				bg = clrmap_rgb(args[i + 2], args[i + 3], args[i + 4]);
				i += 4;
				continue;
			}
			if (args[i] == 48) {
				bg = args[i + 2];
				i += 2;
				continue;
			}
			setattr(args[i]);
		}
		if (mode & MODE_CLR8 && mode & ATTR_BOLD && BRIGHTEN)
			if (fg < 8)
				fg += 8;
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
		delete_chars(LIMIT(args[0], 1, cols - col));
		break;
	case '@':	/* ICH		insert blank characters */
		insert_chars(LIMIT(args[0], 1, cols - col));
		break;
	case 'n':	/* DSR		device status report */
		csiseq_dsr(args[0]);
		break;
	case 'G':	/* CHA		move cursor to column in current row */
		advance(0, MAX(0, args[0] - 1) - col, 0);
		break;
	case 'X':	/* ECH		erase characters on current line */
		kill_chars(col, MIN(col + MAX(1, args[0]), cols));
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
	}
	return 0;
}

static int csiseq_da(int c)
{
	switch (c) {
	case 0x00:
		term_sendstr("\x1b[?6c");
		break;
	default:	/* ignoring cursor shape requests */
		unknown("csiseq_da", c);
	}
	return 0;
}

static int csiseq_dsr(int c)
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
	}
	return 0;
}

/* ANSI/DEC specified modes for SM/RM ANSI Specified Modes */
static int modeseq(int c, int set)
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
	}
	return 0;
}
