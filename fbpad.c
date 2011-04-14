/*
 * fbpad - a small framebuffer virtual terminal
 *
 * Copyright (C) 2009-2011 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * This program is released under GNU GPL version 2.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <linux/vt.h>
#include "config.h"
#include "pad.h"
#include "term.h"
#include "util.h"

#define CTRLKEY(x)	((x) - 96)
#define BADPOLLFLAGS	(POLLHUP | POLLERR | POLLNVAL)
#define NTAGS		sizeof(tags)
#define NTERMS		(NTAGS * 2)

static char tags[] = TAGS;
static struct term terms[NTERMS];
static int tops[NTAGS];	/* top terms of tags */
static int ctag;	/* current tag */
static int ltag;	/* the last tag */
static int exitit;
static int hidden;

static int readchar(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) > 0)
		return (int) b;
	return -1;
}

static int cterm(void)
{
	return tops[ctag] * NTAGS + ctag;
}

static void showterm(int n)
{
	if (cterm() == n)
		return;
	if (ctag != n % NTAGS)
		ltag = ctag;
	term_save(&terms[cterm()]);
	ctag = n % NTAGS;
	tops[ctag] = n / NTAGS;
	term_load(&terms[n], hidden ? TERM_HIDDEN : TERM_REDRAW);
}

static void showtag(int n)
{
	showterm(tops[n] * NTAGS + n);
}

static struct term *mainterm(void)
{
	if (terms[cterm()].fd)
		return &terms[cterm()];
	return NULL;
}

static void exec_cmd(char *file)
{
	if (!mainterm())
		term_exec(file);
}

static int altterm(int n)
{
	return n < NTAGS ? n + NTAGS : n - NTAGS;
}

static void nextterm(void)
{
	int n = (cterm() + 1) % NTERMS;
	while (n != cterm()) {
		if (terms[n].fd) {
			showterm(n);
			break;
		}
		n = (n + 1) % NTERMS;
	}
}

static void showtags(void)
{
	int colors[] = {15, 4, 2};
	int c = 0;
	int r = pad_rows() - 1;
	int i;
	pad_put('T', r, c++, FGCOLOR, BGCOLOR);
	pad_put('A', r, c++, FGCOLOR, BGCOLOR);
	pad_put('G', r, c++, FGCOLOR, BGCOLOR);
	pad_put('S', r, c++, FGCOLOR, BGCOLOR);
	pad_put(':', r, c++, FGCOLOR, BGCOLOR);
	pad_put(' ', r, c++, FGCOLOR, BGCOLOR);
	for (i = 0; i < NTAGS; i++) {
		int nt = 0;
		if (terms[i].fd)
			nt++;
		if (terms[altterm(i)].fd)
			nt++;
		pad_put(i == ctag ? '(' : ' ', r, c++, FGCOLOR, BGCOLOR);
		pad_put(tags[i], r, c++, colors[nt], 7);
		pad_put(i == ctag ? ')' : ' ', r, c++, FGCOLOR, BGCOLOR);
	}
}

static void directkey(void)
{
	int c = readchar();
	if (c == ESC) {
		switch ((c = readchar())) {
		case 'c':
			exec_cmd(SHELL);
			return;
		case 'm':
			exec_cmd(MAIL);
			return;
		case 'e':
			exec_cmd(EDITOR);
			return;
		case 'j':
		case 'k':
			showterm(altterm(cterm()));
			return;
		case 'o':
			showtag(ltag);
			return;
		case 'p':
			showtags();
			return;
		case '\t':
			nextterm();
			return;
		case CTRLKEY('q'):
			exitit = 1;
			return;
		case 's':
			term_screenshot();
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

static void temp_switch(int termid)
{
	if (termid != cterm()) {
		term_save(&terms[cterm()]);
		term_load(&terms[termid], TERM_HIDDEN);
	}
}

static void switch_back(int termid)
{
	if (termid != cterm()) {
		term_save(&terms[termid]);
		term_load(&terms[cterm()], hidden ? TERM_HIDDEN : TERM_VISIBLE);
	}
}

static int poll_all(void)
{
	struct pollfd ufds[NTERMS + 1];
	int term_idx[NTERMS + 1];
	int i;
	int n = 1;
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	for (i = 0; i < NTERMS; i++) {
		if (terms[i].fd) {
			ufds[n].fd = terms[i].fd;
			ufds[n].events = POLLIN;
			term_idx[n++] = i;
		}
	}
	if (poll(ufds, n, 1000) < 1)
		return 0;
	if (ufds[0].revents & BADPOLLFLAGS)
		return 1;
	if (ufds[0].revents & POLLIN)
		directkey();
	for (i = 1; i < n; i++) {
		temp_switch(term_idx[i]);
		if (ufds[i].revents & POLLIN)
			term_read();
		if (ufds[i].revents & BADPOLLFLAGS)
			term_end();
		switch_back(term_idx[i]);
	}
	return 0;
}

static void mainloop(void)
{
	struct termios oldtermios, termios;
	tcgetattr(STDIN_FILENO, &termios);
	oldtermios = termios;
	cfmakeraw(&termios);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios);
	term_load(&terms[cterm()], TERM_REDRAW);
	while (!exitit)
		if (poll_all())
			break;
	tcsetattr(STDIN_FILENO, 0, &oldtermios);
}

static void signalreceived(int n)
{
	if (exitit)
		return;
	switch (n) {
	case SIGUSR1:
		hidden = 1;
		term_save(&terms[cterm()]);
		term_load(&terms[cterm()], TERM_HIDDEN);
		ioctl(STDIN_FILENO, VT_RELDISP, 1);
		break;
	case SIGUSR2:
		hidden = 0;
		pad_shown();
		term_save(&terms[cterm()]);
		term_load(&terms[cterm()], TERM_REDRAW);
		break;
	case SIGCHLD:
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		break;
	}
}

static void setupsignals(void)
{
	struct vt_mode vtm;
	vtm.mode = VT_PROCESS;
	vtm.waitv = 0;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR2;
	vtm.frsig = 0;
	ioctl(STDIN_FILENO, VT_SETMODE, &vtm);

	signal(SIGUSR1, signalreceived);
	signal(SIGUSR2, signalreceived);
	signal(SIGCHLD, signalreceived);
}

int main(void)
{
	char *hide = "\x1b[?25l";
	char *clear = "\x1b[2J\x1b[H";
	char *show = "\x1b[?25h";
	write(STDOUT_FILENO, clear, strlen(clear));
	write(STDIN_FILENO, hide, strlen(hide));
	if (pad_init())
		goto failed;
	setupsignals();
	fcntl(STDIN_FILENO, F_SETFL,
		fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
	mainloop();
	pad_free();
failed:
	write(STDIN_FILENO, show, strlen(show));
	return 0;
}
