#include <poll.h>
#include <pty.h>
#include <string.h>
#include <unistd.h>
#include "pad.h"
#include "term.h"
#include "util.h"

#define SHELL		"/bin/bash"
#define TAGS		8
#define CTRLKEY(x)	((x) - 96)

static struct term_state terms[TAGS * 2];
static int cterm;	/* current tag */
static int lterm;	/* last tag */
static int exitit;

static int readchar(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) > 0)
		return (int) b;
	return -1;
}

static void showterm(int n)
{
	if (lterm % TAGS != cterm % TAGS)
		lterm = cterm;
	term_save(&terms[cterm]);
	cterm = n;
	term_load(&terms[cterm]);
}

static void directkey(void)
{
	int c = readchar();
	if (c == ESC) {
		switch ((c = readchar())) {
		case 'c':
			if (!term_fd())
				term_exec(SHELL);
			return;
		case 'j':
		case 'k':
			showterm((cterm + TAGS) % ARRAY_SIZE(terms));
			return;
		case 'o':
			showterm(lterm);
			return;
		case CTRLKEY('q'):
			exitit = 1;
			return;
		default:
			term_send(ESC);
		}
	}
	term_send(c);
}

static void mainloop(void)
{
	struct pollfd ufds[2];
	int rv;
	struct termios oldtermios, termios;
	tcgetattr(STDIN_FILENO, &termios);
	oldtermios = termios;
	cfmakeraw(&termios);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios);
	memset(ufds, 0, sizeof(ufds));
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	ufds[1].fd = term_fd();
	ufds[1].events = POLLIN;
	while (!exitit && (rv = poll(ufds, term_fd() ? 2 : 1, 1000)) != -1) {
		if (ufds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
			break;
		if (term_fd() && ufds[1].revents & (POLLHUP | POLLERR | POLLNVAL))
			term_end();
		if (term_fd() && ufds[1].revents & POLLIN)
			term_read();
		if (ufds[0].revents & POLLIN)
			directkey();
		ufds[1].fd = term_fd();
	}
	tcsetattr(STDIN_FILENO, 0, &oldtermios);
}

int main(void)
{
	char *hide = "\x1b[?25l";
	char *clear = "\x1b[2J\x1b[H";
	char *show = "\x1b[?25h";
	write(STDOUT_FILENO, hide, strlen(hide));
	write(STDOUT_FILENO, clear, strlen(clear));
	pad_init();
	pad_blank();
	mainloop();
	pad_free();
	write(STDOUT_FILENO, clear, strlen(clear));
	write(STDOUT_FILENO, show, strlen(show));
	return 0;
}
