#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
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
static int *screen;
static char *fgs, *bgs;
static int row, col;
static char fg, bg;
static int top, bot;
static int mode;
static int visible;

#define MAXLINES		(1 << 8)
static int dirty[MAXLINES];
static int lazy;

static char fgcolor(void)
{
	int c = mode & ATTR_REV ? bg : fg;
	return mode & ATTR_BOLD ? c | 0x08 : c;
}

static char bgcolor(void)
{
	return mode & ATTR_REV ? fg : bg;
}

static void _draw_pos(int r, int c, int cursor)
{
	int rev = cursor && mode & MODE_CURSOR;
	int i = OFFSET(r, c);
	char fg = rev ? bgs[i] : fgs[i];
	char bg = rev ? fgs[i] : bgs[i];
	if (visible)
		pad_put(screen[i], r, c, fg, bg);
}

static void _draw_row(int r)
{
	int i;
	pad_blankrow(r, bgcolor());
	for (i = 0; i < pad_cols(); i++) {
		int c = screen[OFFSET(r, i)];
		if ((c && c != ' ') || bgs[OFFSET(r, i)] != bgcolor())
			_draw_pos(r, i, 0);
	}
}

static void draw_rows(int sr, int er)
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

static void draw_cols(int r, int sc, int ec)
{
	int i;
	if (!visible)
		return;
	if (lazy)
		dirty[r] = 1;
	else
		for (i = sc; i < ec; i++)
			_draw_pos(r, i, 0);
}

static void draw_char(int ch, int r, int c)
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
		_draw_pos(r, c, 0);
}

static void draw_cursor(int put)
{
	if (!visible)
		return;
	if (lazy)
		dirty[row] = 1;
	else
		_draw_pos(row, col, put);
}

static void lazy_clean(void)
{
	if (visible)
		memset(dirty, 0, sizeof(dirty));
}

static void lazy_flush(void)
{
	int i;
	if (!visible)
		return;
	_draw_pos(row, col, 0);
	for (i = 0; i < pad_rows(); i++)
		if (dirty[i])
			_draw_row(i);
	lazy_clean();
	_draw_pos(row, col, 1);
}

static void term_redraw(void)
{
	int i;
	for (i = 0; i < pad_rows(); i++)
		_draw_row(i);
	_draw_pos(row, col, 1);
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
	poll(ufds, 1, 100);
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
	draw_rows(sr, er);
	draw_cursor(1);
}

static void scroll_screen(int sr, int nr, int n)
{
	draw_cursor(0);
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

static void move_cursor(int r, int c)
{
	int t, b;
	draw_cursor(0);
	t = origin() ? top : 0;
	b = origin() ? bot : pad_rows();
	row = MAX(t, MIN(r, b - 1));
	col = MAX(0, MIN(c, pad_cols() - 1));
	draw_cursor(1);
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
	draw_char(c, row, col);
	wrapready = col == pad_cols() - 1;
	advance(0, 1, 1);
	if (wrapready)
		mode = BIT_SET(mode, MODE_WRAPREADY, 1);
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
		draw_char(' ', row, i);
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

static int writeutf8(char *dst, int c)
{
	char *d = dst;
	int l;
	if (!(c & ~0x7f)) {
		*d++ = c ? c : ' ';
		return 1;
	}
	if (c > 0xffff) {
		*d++ = 0xf0 | (c >> 18);
		l = 3;
	} else if (c > 0x7ff) {
		*d++ = 0xe0 | (c >> 12);
		l = 2;
	} else if (c > 0x7f) {
		*d++ = 0xc0 | (c >> 6);
		l = 1;
	}
	while (l--)
		*d++ = 0x80 | ((c >> (l * 6)) & 0x3f);
	return d - dst;
}

void term_screenshot(void)
{
	char buf[1 << 11];
	int fd = open(SCREENSHOT, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	int i, j;
	for (i = 0; i < pad_rows(); i++) {
		char *s = buf;
		for (j = 0; j < pad_cols(); j++)
			s += writeutf8(s, screen[OFFSET(i, j)]);
		*s++ = '\n';
		write(fd, buf, s - buf);
	}
	close(fd);
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
	case 0x9b:	/* CSI		equivalent to ESC [ */
		unknown("ctlseq", c);
		break;
	default:
		insertchar(readutf8(c));
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
	int args[MAXCSIARGS] = {0};
	int i;
	int n = 0;
	int c = readpty();
	int inter = 0;
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
		if (c == ';')
			c = readpty();
		args[n] = arg;
		n = n < ARRAY_SIZE(args) ? n + 1 : 0;
	}
	while (CSII(c)) {
		inter = c;
		c = readpty();
	}
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
	case 'd':	/* VPA		move to row (current column) */
		move_cursor(absrow(MAX(1, args[0]) - 1), col);
		break;
	case 'm':	/* SGR		set graphic rendition */
		if (!n)
			setattr(0);
		for (i = 0; i < n; i++)
			setattr(args[i]);
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
		break;
	case 'l':	/* RM		reset mode */
		for (i = 0; i < n; i++)
			modeseq(priv == '?' ? args[i] | 0x80 : args[i], 0);
		break;
	case 'P':	/* DCH		delete characters on current line */
		delete_chars(MIN(MAX(1, args[0]), pad_cols() - col));
		break;
	case '@':	/* ICH		insert blank characters */
		insert_chars(MAX(1, args[0]));
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
		snprintf(status, sizeof(status), "\x1b[%d;%dR",
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
		break;
	case 0x00:	/* IGN		Error (Ignored) */
	case 0x01:	/* GATM		guarded-area transfer mode (ignored) */
	case 0x02:	/* KAM		keyboard action mode (always reset) */
	case 0x03:	/* CRM		control representation mode (always reset) */
	case 0x05:	/* SRTM		status-reporting transfer mode */
	case 0x06:	/* ERM		erasure mode (always set) */
	case 0x07:	/* VEM		vertical editing mode (ignored) */
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
	case 0x80:	/* IGN		Error (Ignored) */
	case 0x81:	/* DECCKM	Cursorkeys application (set); Cursorkeys normal (reset) */
	case 0x82:	/* DECANM	ANSI (set); VT52 (reset) */
	case 0x83:	/* DECCOLM	132 columns (set); 80 columns (reset) */
	case 0x84:	/* DECSCLM	Jump scroll (set); Smooth scroll (reset) */
	case 0x85:	/* DECSCNM	Reverse screen (set); Normal screen (reset) */
	case 0x88:	/* DECARM	Auto Repeat */
	case 0x89:	/* DECINLM	Interlace */
	case 0x92:	/* DECPFF	Send FF to printer after print screen (set); No char after PS (reset) */
	case 0x93:	/* DECPEX	Print screen: prints full screen (set); prints scroll region (reset) */
	default:
		unknown("modeseq", c);
		break;
	}
}
