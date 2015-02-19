/*
 * fbpad - a small framebuffer virtual terminal
 *
 * Copyright (C) 2009-2015 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * This program is released under the Modified BSD license.
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
#include "config.h"
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
static int ctag;		/* current tag */
static int ltag;		/* the last tag */
static int exitit;
static int hidden;		/* do not touch the framebuffer */
static int locked;
static int taglock;		/* disable tag switching */
static char pass[1024];
static int passlen;
static int cmdmode;		/* execute a command and exit */
static int altfont;		/* using alternative font set */

static int readchar(void)
{
	char b;
	if (read(0, &b, 1) > 0)
		return (unsigned char) b;
	return -1;
}

static int cterm(void)
{
	return tops[ctag] * NTAGS + ctag;
}

static int altterm(int n)
{
	return n < NTAGS ? n + NTAGS : n - NTAGS;
}

static int nextterm(void)
{
	int n = (cterm() + 1) % NTERMS;
	while (n != cterm()) {
		if (TERMOPEN(n))
			break;
		n = (n + 1) % NTERMS;
	}
	return n;
}

static struct term *mainterm(void)
{
	return TERMOPEN(cterm()) ? &terms[cterm()] : NULL;
}

static void switchterm(int oidx, int nidx, int show, int save, int load)
{
	if (save && TERMOPEN(oidx) && TERMSNAP(oidx))
		scr_snap(oidx);
	term_save(&terms[oidx]);
	term_load(&terms[nidx], show);
	if (show)
		term_redraw(load && (!TERMOPEN(nidx) || !TERMSNAP(nidx) ||
					scr_load(nidx)));
}

static void showterm(int n)
{
	if (cterm() == n || cmdmode)
		return;
	if (taglock && ctag != n % NTAGS)
		return;
	if (ctag != n % NTAGS)
		ltag = ctag;
	switchterm(cterm(), n, !hidden, !hidden, !hidden);
	ctag = n % NTAGS;
	tops[ctag] = n / NTAGS;
}

static void showtag(int n)
{
	showterm(tops[n] * NTAGS + n);
}

static void execterm(char **args)
{
	if (!mainterm())
		term_exec(args);
}

static void listtags(void)
{
	int c = pad_cols() - 1;
	int r = 1;
	int i;
	for (i = 0; i < NTAGS; i++) {
		int fg = 8, fglow = TERMSNAP(i) ? 218 : 150;
		int bg = i == ctag ? 193 : 225;
		int t1 = tops[i] * NTAGS + i;
		int t2 = (1 - tops[i]) * NTAGS + i;
		pad_put(tags[i], r + i, c - 1, (TERMOPEN(t1) ? fg : fglow) | FN_B, bg);
		pad_put(tags[i], r + i, c, (TERMOPEN(t2) ? fg : bg) | FN_B, bg);
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
			execterm(shell);
			return;
		case 'm':
			execterm(mail);
			return;
		case 'e':
			execterm(editor);
			return;
		case 'j':
		case 'k':
			showterm(altterm(cterm()));
			return;
		case 'o':
			showtag(ltag);
			return;
		case 'p':
			listtags();
			return;
		case '\t':
			if (nextterm() != cterm())
				showterm(nextterm());
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
		case 'f':
			altfont = 1 - altfont;
			if (altfont)
				pad_font(FR0, FI0, FB0);
			else
				pad_font(FR, FI, FB);
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
		default:
			if (strchr(tags, c)) {
				showtag(strchr(tags, c) - tags);
				return;
			}
			if (mainterm())
				term_send(ESC);
		}
	}
	if (c != -1 && mainterm())
		term_send(c);
}

static void peepterm(int termid)
{
	if (termid != cterm())
		switchterm(cterm(), termid, 0, 0, 0);
}

static void peepback(int termid)
{
	if (termid != cterm())
		switchterm(termid, cterm(), !hidden, 0, 0);
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
		execterm(args);
	}
	while (!exitit)
		if (pollterms())
			break;
	tcsetattr(0, 0, &oldtermios);
}

static void signalreceived(int n);
static void signalregister(void)
{
	signal(SIGUSR1, signalreceived);
	signal(SIGUSR2, signalreceived);
	signal(SIGCHLD, signalreceived);
}

static void signalreceived(int n)
{
	if (exitit)
		return;
	/* racy, new signals may arrive before re-registeration */
	signalregister();
	switch (n) {
	case SIGUSR1:
		hidden = 1;
		switchterm(cterm(), cterm(), 0, 1, 0);
		ioctl(0, VT_RELDISP, 1);
		break;
	case SIGUSR2:
		hidden = 0;
		fb_cmap();
		switchterm(cterm(), cterm(), 1, 0, 1);
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
	signalregister();
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
	if (sizeof(fbval_t) != FBM_BPP(fb_mode())) {
		fprintf(stderr, "fbpad: fbval_t does not match framebuffer depth\n");
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
