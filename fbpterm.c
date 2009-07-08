#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <poll.h>
#include <unistd.h>
#include <ctype.h>
#include <pty.h>
#include "pad.h"
#include "util.h"

#define SHELL		"/bin/bash"
#define ESC		27
#define MAXESCARGS	32
#define FGCOLOR		0
#define BGCOLOR		7

static pid_t pid;
static int fd;

static void execshell(void)
{
	if ((pid = forkpty(&fd, NULL, NULL, NULL)) == -1)
		xerror("failed to create a pty");
	if (!pid) {
		setenv("TERM", "linux", 1);
		execl(SHELL, SHELL, NULL);
		exit(1);
	}
}

static void setsize(void)
{
	struct winsize size;
	size.ws_col = pad_cols();
	size.ws_row = pad_rows();
	size.ws_xpixel = 0;
	size.ws_ypixel = 0;
	ioctl(fd, TIOCSWINSZ, &size);
}

static int readpty(void)
{
	char b;
	if (read(fd, &b, 1) > 0)
		return (int) b;
	return -1;
}

static int readchar(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) > 0)
		return (int) b;
	return -1;
}

static void writechar(int c)
{
	unsigned char b = (unsigned char) c;
	write(fd, &b, 1);
}

static void writepty(int c)
{
	pad_add(c);
}

static void setmode(int m)
{
	if (m == 0) {
		pad_fg(FGCOLOR);
		pad_bg(BGCOLOR);
	}
	if (m >= 30 && m <= 37)
		pad_fg(m - 30);
	if (m >= 40 && m <= 47)
		pad_bg(m - 40);
}

static void kill_line(void)
{
	int i;
	for (i = pad_col(); i < pad_cols(); i++)
		pad_put(' ', pad_row(), i);
}

static void escape(void)
{
	int args[MAXESCARGS] = {0};
	int i;
	int n = -1;
	int c = readpty();
	if (c != '[') {
		writepty(ESC);
		writepty(c);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(args) && !isalpha(c); i++) {
		int arg = 0;
		while (isdigit((c = readpty())))
			arg = arg * 10 + (c - '0');
		args[n++] = arg;
	}
	switch (c) {
	case 'H':
	case 'f':
		pad_move(args[0], args[1]);
		break;
	case 'J':
		pad_blank();
		pad_move(0, 0);
		break;
	case 'A':
		pad_move(pad_row() - args[0], pad_col());
		break;
	case 'B':
		pad_move(pad_row() + args[0], pad_col());
		break;
	case 'C':
		pad_move(pad_row(), pad_col() + args[0]);
		break;
	case 'D':
		pad_move(pad_row(), pad_col() - args[0]);
		break;
	case 'K':
		kill_line();
		break;
	case 'm':
		for (i = 0; i < n; i++)
			setmode(args[i]);
		if (!n)
			setmode(0);
		break;
	default:
		printf("unknown escapse <%c>\n", c);
	}
}

static void shcmds(void)
{
	int c = readpty();
	if (c == ESC)
		escape();
	else
		writepty(c);
}

static void directkey(void)
{
	int c = readchar();
	writechar(c);
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
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	ufds[1].fd = fd;
	ufds[1].events = POLLIN;
	while ((rv = poll(ufds, 2, 1000)) != -1) {
		if ((ufds[0].revents | ufds[1].revents) &
			(POLLHUP | POLLERR | POLLNVAL))
			break;
		if (ufds[0].revents & POLLIN)
			directkey();
		if (ufds[1].revents & POLLIN)
			shcmds();
	}
	tcsetattr(STDIN_FILENO, 0, &oldtermios);
}

int main(void)
{
	execshell();
	pad_init();
	setsize();
	setmode(0);
	pad_blank();
	mainloop();
	pad_free();
	return 0;
}
