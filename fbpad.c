/*
 * FBPAD FRAMEBUFFER VIRTUAL TERMINAL
 *
 * Copyright (C) 2009-2026 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <linux/vt.h>
#include "fbpad.h"
#include "draw.h"

#define CTRLKEY(x)	((x) - 96)
#define POLLFLAGS	(POLLIN | POLLHUP | POLLERR | POLLNVAL)
#define NTAGS		32
#define NTERMS		(NTAGS * 2)
#define TERMOPEN(i)	(terms[i] && term_fd(terms[i]))

static struct term *terms[NTERMS];
static int tops[NTAGS];		/* top terms of tags */
static int split[NTAGS];	/* terms are shown together */
static int saved[NTAGS];	/* saved tags */
static int ctag;		/* current tag */
static int ltag;		/* last tag */
static int exitit;		/* exit fbpad if set */
static int confirm;		/* wait for exit confirmation */
static int hidden;		/* do not touch the framebuffer */
static int locked;		/* fbpad is locked; wait for PASS */
static int taglock;		/* disable tag switching */
static char pass[1024];
static int passlen;
static int cmdmode;		/* execute a command and exit */

/* the current terminal */
static int cterm(void)
{
	return tops[ctag] ? NTAGS + ctag : ctag;
}

/* tag's active terminal */
static int tterm(int n)
{
	return tops[n] ? NTAGS + n : n;
}

/* the other terminal in the same tag */
static int aterm(int n)
{
	return n < NTAGS ? n + NTAGS : n - NTAGS;
}

/* the next terminal */
static int nterm(void)
{
	int n = (cterm() + 1) % NTERMS;
	while (n != cterm()) {
		if (TERMOPEN(n))
			break;
		n = (n + 1) % NTERMS;
	}
	return n;
}

static void t_conf(int idx)
{
	int brwid = conf_borderwd();
	int h1 = fb_rows() / 2 / pad_crows() * pad_crows();
	int h2 = fb_rows() - h1 - 4 * brwid;
	int w1 = fb_cols() / 2 / pad_ccols() * pad_ccols();
	int w2 = fb_cols() - w1 - 4 * brwid;
	int tag = idx % NTAGS;
	int top = idx < NTAGS;
	if (split[tag] == 0)
		pad_conf(0, 0, fb_rows(), fb_cols());
	if (split[tag] == 1)
		pad_conf(top ? brwid : h1 + 3 * brwid, brwid,
			top ? h1 : h2, fb_cols() - 2 * brwid);
	if (split[tag] == 2)
		pad_conf(brwid, top ? brwid : w1 + 3 * brwid,
			fb_rows() - 2 * brwid, top ? w1 : w2);
}

static void t_hide(int idx, int save)
{
	if (save && TERMOPEN(idx))
		term_hide(terms[idx]);
	if (save && saved[idx % NTAGS] && TERMOPEN(idx))
		scr_snap(idx);
	if (terms[idx])
		term_save(terms[idx]);
}

/* show=0 (hidden), show=1 (visible), show=2 (load), show=3 (redraw) */
static int t_show(int idx, int show)
{
	t_conf(idx);
	term_load(terms[idx], show > 0);
	if (show == 2)	/* redraw if scr_load() fails */
		show += !TERMOPEN(idx) || !saved[idx % NTAGS] || scr_load(idx);
	if (show > 0)
		term_redraw(show == 3);
	if ((show == 2 || show == 3) && TERMOPEN(idx))
		term_show(terms[idx]);
	return show;
}

/* switch active terminal; hide oidx and show nidx */
/* + save: whether to save terminal contents for oidx */
/* + show: same as in t_show() */
/* + perm: whether it is a permanent switch */
static int t_hideshow(int oidx, int save, int nidx, int show, int perm)
{
	int otag = oidx % NTAGS;
	int ntag = nidx % NTAGS;
	int ret;
	t_hide(oidx, save);
	if (show && split[otag] && otag == ntag && perm)
		pad_border(0, conf_borderwd());
	ret = t_show(nidx, show);
	if (show && split[ntag] && perm)
		pad_border(conf_borderfg(), conf_borderwd());
	return ret;
}

/* set cterm() */
static void t_set(int n)
{
	if (cterm() == n || cmdmode)
		return;
	if (taglock && ctag != n % NTAGS)
		return;
	if (ctag != n % NTAGS)
		ltag = ctag;
	if (ctag == n % NTAGS) {
		if (split[n % NTAGS])
			t_hideshow(cterm(), 0, n, 1, 1);
		else
			t_hideshow(cterm(), 1, n, 2, 1);
	} else {
		int draw = t_hideshow(cterm(), 1, n, 2, 1);
		if (split[n % NTAGS]) {
			t_hideshow(n, 0, aterm(n), draw == 2 ? 1 : 2, 0);
			t_hideshow(aterm(n), 0, n, 1, 1);
		}
	}
	ctag = n >= NTAGS ? n - NTAGS : n;
	tops[ctag] = n >= NTAGS;
}

static void t_split(int n)
{
	split[ctag] = n;
	t_hideshow(cterm(), 0, aterm(cterm()), 3, 0);
	t_hideshow(aterm(cterm()), 1, cterm(), 3, 1);
}

static void t_exec(char **args, int swsig)
{
	if (!terms[cterm()]) {
		terms[cterm()] = term_make();
		term_load(terms[cterm()], 1);
	}
	term_exec(args, swsig);
}

static void listtags(void)
{
	/* colors for tags based on their number of terminals */
	char *tags = conf_tags();
	int fg = 0x96cb5c, bg = 0x516f7b;
	int colors[] = {0x173f4f, fg, 0x68cbc0 | FN_B};
	int c = 0;
	int r = pad_rows() - 1;
	int i;
	pad_put('T', r, c++, fg | FN_B, bg);
	pad_put('A', r, c++, fg | FN_B, bg);
	pad_put('G', r, c++, fg | FN_B, bg);
	pad_put('S', r, c++, fg | FN_B, bg);
	pad_put(':', r, c++, fg | FN_B, bg);
	pad_put(' ', r, c++, fg | FN_B, bg);
	for (i = 0; i < NTAGS && c + 2 < pad_cols(); i++) {
		int nt = 0;
		if (TERMOPEN(i))
			nt++;
		if (TERMOPEN(aterm(i)))
			nt++;
		pad_put(i == ctag ? '(' : ' ', r, c++, fg, bg);
		if (saved[i])
			pad_put(tags[i], r, c++, !nt ? bg : colors[nt], colors[0]);
		else
			pad_put(tags[i], r, c++, colors[nt], bg);
		pad_put(i == ctag ? ')' : ' ', r, c++, fg, bg);
	}
	for (; c < pad_cols(); c++)
		pad_put(' ', r, c, fg, bg);
}

static void directkey(void)
{
	char *tags = conf_tags();
	char *shell[4] = {conf_shell()};
	char *mail[4] = {conf_mail()};
	char *editor[4] = {conf_editor()};
	char user[16];
	int n = read(0, user, sizeof(user));
	int c = (unsigned char) user[0];
	if (n <= 0)
		return;
	if (conf_pass()[0] && locked) {
		if (c == '\r') {
			pass[passlen] = '\0';
			if (!strcmp(conf_pass(), pass))
				locked = 0;
			passlen = 0;
			return;
		}
		if (isprint(c) && passlen + 1 < sizeof(pass))
			pass[passlen++] = c;
		return;
	}
	if (confirm) {
		exitit = c == conf_quitkey();
		confirm = 0;
		return;
	}
	if (c == ESC && n > 1) {
		switch ((c = (unsigned char) user[1])) {
		case 'c':
			t_exec(shell, 0);
			return;
		case ';':
			t_exec(shell, 1);
			return;
		case 'm':
			if (TERMOPEN(cterm()))
				saved[ctag] = 1;
			t_exec(mail, 0);
			return;
		case 'e':
			if (TERMOPEN(cterm())) {
				saved[ctag] = 0;
				scr_free(ctag);
				scr_free(aterm(ctag));
			}
			t_exec(editor, 0);
			return;
		case 'j':
		case 'k':
			t_set(aterm(cterm()));
			return;
		case 'o':
			t_set(tterm(ltag));
			return;
		case 'p':
			listtags();
			return;
		case '\t':
			if (nterm() != cterm())
				t_set(nterm());
			return;
		case CTRLKEY('q'):
			if (conf_quitkey())
				confirm = 1;
			else
				exitit = 1;
			return;
		case 's':
			if (terms[cterm()])
				term_screenshot(terms[cterm()], conf_scrshot());
			return;
		case 'y':
			term_redraw(1);
			return;
		case CTRLKEY('e'):
			if (conf_read() > 0)
				pad_init(conf_font(0), conf_font(1), conf_font(2));
			term_redraw(1);
			return;
		case CTRLKEY('l'):
			locked = 1;
			passlen = 0;
			return;
		case CTRLKEY('o'):
			taglock = 1 - taglock;
			return;
		case ',':
			term_scrl(pad_rows() / 2);
			return;
		case '.':
			term_scrl(-pad_rows() / 2);
			return;
		case '=':
			t_split(split[ctag] == 1 ? 2 : 1);
			return;
		case '-':
			t_split(0);
			return;
		default:
			if (strchr(tags, c)) {
				t_set(tterm(strchr(tags, c) - tags));
				return;
			}
		}
	}
	term_send(user, n);
}

static void peepterm(int termid)
{
	int visible = !hidden && ctag == (termid % NTAGS) && split[ctag];
	if (termid != cterm())
		t_hideshow(cterm(), 0, termid, visible, 0);
}

static void peepback(int termid)
{
	if (termid != cterm())
		t_hideshow(termid, 0, cterm(), !hidden, 1);
}

static int pollterms(void)
{
	struct pollfd ufds[NTERMS + 1];
	int term_idx[NTERMS + 1];
	int i;
	int n = 1;
	ufds[0].fd = 0;
	ufds[0].events = POLLIN;
	for (i = 0; i < NTERMS; i++) {
		if (TERMOPEN(i)) {
			ufds[n].fd = term_fd(terms[i]);
			ufds[n].events = POLLIN;
			term_idx[n++] = i;
		}
	}
	if (poll(ufds, n, 1000) < 1)
		return 0;
	if (ufds[0].revents & (POLLFLAGS & ~POLLIN))
		return 1;
	if (ufds[0].revents & POLLIN)
		directkey();
	for (i = 1; i < n; i++) {
		if (!(ufds[i].revents & POLLFLAGS))
			continue;
		peepterm(term_idx[i]);
		if (ufds[i].revents & POLLIN) {
			term_read();
		} else {
			scr_free(term_idx[i]);
			term_end();
			if (cmdmode)
				exitit = 1;
		}
		peepback(term_idx[i]);
	}
	return 0;
}

static void mainloop(char **args)
{
	struct termios oldtermios, termios;
	tcgetattr(0, &termios);
	oldtermios = termios;
	cfmakeraw(&termios);
	tcsetattr(0, TCSAFLUSH, &termios);
	term_load(terms[cterm()], 1);
	term_redraw(1);
	if (args) {
		cmdmode = 1;
		t_exec(args, 0);
	}
	while (!exitit)
		if (pollterms())
			break;
	tcsetattr(0, 0, &oldtermios);
}

static void signalreceived(int n)
{
	if (exitit)
		return;
	switch (n) {
	case SIGUSR1:
		hidden = 1;
		t_hide(cterm(), 1);
		fb_leave();
		ioctl(0, VT_RELDISP, 1);
		break;
	case SIGUSR2:
		hidden = 0;
		fb_enter();
		if (t_show(cterm(), 2) == 3 && split[ctag]) {
			t_hideshow(cterm(), 0, aterm(cterm()), 3, 0);
			t_hideshow(aterm(cterm()), 0, cterm(), 1, 1);
		}
		break;
	case SIGCHLD:
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		break;
	}
}

static void signalsetup(void)
{
	struct vt_mode vtm;
	vtm.mode = VT_PROCESS;
	vtm.waitv = 0;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR2;
	vtm.frsig = 0;
	signal(SIGUSR1, signalreceived);
	signal(SIGUSR2, signalreceived);
	signal(SIGCHLD, signalreceived);
	signal(SIGPIPE, SIG_IGN);
	ioctl(0, VT_SETMODE, &vtm);
}

int main(int argc, char **argv)
{
	char *hide = "\x1b[2J\x1b[H\x1b[?25l";
	char *show = "\x1b[?25h";
	char **args = argv + 1;
	int i;
	conf_read();
	if (fb_init(getenv("FBDEV"))) {
		fprintf(stderr, "fbpad: failed to initialize the framebuffer\n");
		return 1;
	}
	if (pad_init(conf_font(0), conf_font(1), conf_font(2))) {
		fprintf(stderr, "fbpad: cannot find fonts\n");
		return 1;
	}
	write(1, hide, strlen(hide));
	signalsetup();
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
	while (args[0] && args[0][0] == '-')
		args++;
	for (i = 0; conf_tags()[i]; i++)
		saved[i] = strchr(conf_saved(), conf_tags()[i % NTAGS]) != NULL;
	mainloop(args[0] ? args : NULL);
	write(1, show, strlen(show));
	for (i = 0; i < NTERMS; i++)
		if (terms[i])
			term_free(terms[i]);
	pad_free();
	scr_done();
	fb_free();
	return 0;
}
