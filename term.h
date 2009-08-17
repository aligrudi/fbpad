#define ESC		27

struct term_state {
	int row, col;
	int fg, bg;
	unsigned int mode;
};

struct term {
	int fd;
	int pid;
	int top, bot;
	struct term_state cur, sav;
	struct square {
		int c;
		short fg;
		short bg;
	} screen[MAXCHARS];
};

#define TERM_HIDDEN		0
#define TERM_VISIBLE		1
#define TERM_REDRAW		2

void term_load(struct term *term, int visible);
void term_save(struct term *term);

void term_read(void);
void term_send(int c);
void term_exec(char *cmd);
void term_end(void);
void term_screenshot(void);
