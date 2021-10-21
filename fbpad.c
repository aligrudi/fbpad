/*
 * FBPAD FRAMEBUFFER VIRTUAL TERMINAL
 *
 * Copyright (C) 2009-2021 Ali Gholami Rudi <ali at rudi dot ir>
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
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <linux/vt.h>
#include "conf.h"
#include "fbpad.h"
#include "draw.h"

#define CTRLKEY(x)	((x) - 96)
#define POLLFLAGS	(POLLIN | POLLHUP | POLLERR | POLLNVAL)
#define NTAGS		(sizeof(tags) - 1)
#define NTERMS		(NTAGS * 2)
#define TERMOPEN(i)	(terms[i].fd)
#define TERMSNAP(i)	(strchr(TAGS_SAVED, tags[(i) % NTAGS]))

static char tags[] = TAGS;
static struct term terms[NTERMS];
static int tops[NTAGS];		/* top terms of tags */
static int split[NTAGS];	/* terms are shown together */
static int ctag;		/* current tag */
static int ltag;		/* the last tag */
static int exitit;
static int hidden;		/* do not touch the framebuffer */
static int locked;
static int taglock;		/* disable tag switching */
static char pass[1024];
static int passlen;
static int cmdmode;		/* execute a command and exit */

static int readchar(void)
{
	char b;
	if (read(0, &b, 1) > 0)
		return (unsigned char) b;
	return -1;
}

/* the current terminal */
static int cterm(void)
{
	return tops[ctag] * NTAGS + ctag;
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

static struct term *fterm_main(void)
{
	return TERMOPEN(cterm()) ? &terms[cterm()] : NULL;
}

static void fterm_conf(int idx)
{
	int hrows = (fb_rows() - 4) / 2;
	int hcols = (fb_cols() - 4) / 2;
	int tag = idx % NTAGS;
	int top = idx < NTAGS;
	if (split[tag] == 0)
		pad_conf(0, 0, fb_rows(), fb_cols());
	if (split[tag] == 1)
		pad_conf(top ? 1 : hrows + 3, 1, hrows, fb_cols() - 2);
	if (split[tag] == 2)
		pad_conf(1, top ? 1 : hcols + 3, fb_rows() - 2, hcols);
}

static void fterm_switch(int oidx, int nidx, int show, int save, int load)
{
	int otag = oidx % NTAGS;
	int ntag = nidx % NTAGS;
	int bothvisible = otag == ntag && split[otag];
	if (save && TERMOPEN(oidx) && TERMSNAP(oidx) && !bothvisible)
		scr_snap(split[otag] ? otag : oidx);
	term_save(&terms[oidx]);
	if (show && split[otag] && otag == ntag)
		pad_border(0);
	fterm_conf(nidx);
	term_load(&terms[nidx], show);
	if (show)
		term_redraw(load && (load < 0 || !TERMOPEN(nidx) || !TERMSNAP(nidx) ||
				(!bothvisible && scr_load(split[ntag] ? ntag : nidx))));
	if (show && split[ntag])
		pad_border(0xff0000);
}

static void fterm_show(int n)
{
	if (cterm() == n || cmdmode)
		return;
	if (taglock && ctag != n % NTAGS)
		return;
	if (ctag != n % NTAGS)
		ltag = ctag;
	if (ctag == n % NTAGS) {
		if (split[n % NTAGS])
			fterm_switch(cterm(), n, !hidden, 0, 0);
		else
			fterm_switch(cterm(), n, !hidden, !hidden, !hidden);
	} else {
		fterm_switch(cterm(), n, !hidden, !hidden, !hidden);
		if (split[n % NTAGS]) {
			fterm_switch(n, aterm(n), !hidden, 0, !hidden);
			fterm_switch(aterm(n), n, !hidden, 0, 0);
		}
	}
	ctag = n % NTAGS;
	tops[ctag] = n / NTAGS;
}

static void tag_split(int n)
{
	split[ctag] = n;
	fterm_switch(cterm(), aterm(cterm()), !hidden, 0, -!hidden);
	fterm_switch(aterm(cterm()), cterm(), !hidden, !hidden, -!hidden);
}

static void tag_show(int n)
{
	fterm_show(tops[n] * NTAGS + n);
}

static void fterm_exec(char **args)
{
	if (!fterm_main())
		term_exec(args);
}

static void tag_list(void)
{
	/* colors for tags based on their number of terminals */
	int colors[] = {COLOR7, FGCOLOR, FGCOLOR | FN_B};
	int c = 0;
	int r = pad_rows() - 1;
	int i;
	pad_put('T', r, c++, FGCOLOR, BGCOLOR);
	pad_put('A', r, c++, FGCOLOR, BGCOLOR);
	pad_put('G', r, c++, FGCOLOR, BGCOLOR);
	pad_put('S', r, c++, FGCOLOR, BGCOLOR);
	pad_put(':', r, c++, FGCOLOR, BGCOLOR);
	pad_put(' ', r, c++, FGCOLOR, BGCOLOR);
	for (i = 0; i < NTAGS && c + 2 < pad_cols(); i++) {
		int nt = 0;
		if (TERMOPEN(i))
			nt++;
		if (TERMOPEN(aterm(i)))
			nt++;
		pad_put(i == ctag ? '(' : ' ', r, c++, FGCOLOR, BGCOLOR);
		if (TERMSNAP(i))
			pad_put(tags[i], r, c++, !nt ? BGCOLOR : colors[nt], colors[0]);
		else
			pad_put(tags[i], r, c++, colors[nt], BGCOLOR);
		pad_put(i == ctag ? ')' : ' ', r, c++, FGCOLOR, BGCOLOR);
	}
}

static void directkey(void)
{
	char *shell[32] = SHELL;
	char *mail[32] = MAIL;
	char *editor[32] = EDITOR;
	int c = readchar();
	if (PASS && locked) {
		if (c == '\r') {
			pass[passlen] = '\0';
			if (!strcmp(PASS, pass))
				locked = 0;
			passlen = 0;
			return;
		}
		if (isprint(c) && passlen + 1 < sizeof(pass))
			pass[passlen++] = c;
		return;
	}
	if (c == ESC) {
		switch ((c = readchar())) {
		case 'c':
			fterm_exec(shell);
			return;
		case 'm':
			fterm_exec(mail);
			return;
		case 'e':
			fterm_exec(editor);
			return;
		case 'j':
		case 'k':
			fterm_show(aterm(cterm()));
			return;
		case 'o':
			tag_show(ltag);
			return;
		case 'p':
			tag_list();
			return;
		case '\t':
			if (nterm() != cterm())
				fterm_show(nterm());
			return;
		case CTRLKEY('q'):
			exitit = 1;
			return;
		case 's':
			term_screenshot();
			return;
		case 'y':
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
			tag_split(split[ctag] == 1 ? 2 : 1);
			return;
		case '-':
			tag_split(0);
			return;
		default:
			if (strchr(tags, c)) {
				tag_show(strchr(tags, c) - tags);
				return;
			}
			if (fterm_main())
				term_send(ESC);
		}
	}
	if (c != -1 && fterm_main())
		term_send(c);
}

static void peepterm(int termid)
{
	int visible = !hidden && ctag == (termid % NTAGS) && split[ctag];
	if (termid != cterm())
		fterm_switch(cterm(), termid, visible, 0, 0);
}

static void peepback(int termid)
{
	if (termid != cterm())
		fterm_switch(termid, cterm(), !hidden, 0, 0);
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
			ufds[n].fd = terms[i].fd;
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
	term_load(&terms[cterm()], 1);
	term_redraw(1);
	if (args) {
		cmdmode = 1;
		fterm_exec(args);
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
		fterm_switch(cterm(), cterm(), 0, 1, 0);
		ioctl(0, VT_RELDISP, 1);
		break;
	case SIGUSR2:
		hidden = 0;
		fb_cmap();
		fterm_switch(cterm(), cterm(), 1, 0, 1);
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
	ioctl(0, VT_SETMODE, &vtm);
}

int main(int argc, char **argv)
{
	char *hide = "\x1b[2J\x1b[H\x1b[?25l";
	char *show = "\x1b[?25h";
	char **args = argv + 1;
	if (fb_init(FBDEV)) {
		fprintf(stderr, "fbpad: failed to initialize the framebuffer\n");
		return 1;
	}
	if (pad_init()) {
		fprintf(stderr, "fbpad: cannot find fonts\n");
		return 1;
	}
	write(1, hide, strlen(hide));
	signalsetup();
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
	while (args[0] && args[0][0] == '-')
		args++;
	mainloop(args[0] ? args : NULL);
	write(1, show, strlen(show));
	pad_free();
	scr_done();
	fb_free();
	return 0;
}
