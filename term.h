#define ESC		27

struct term_state {
	int row, col;
	int fg, bg;
	int mode;
};

struct term {
	int fd;
	int pid;
	int top, bot;
	struct term_state cur, sav;
	int screen[MAXCHARS];
	char fgs[MAXCHARS];
	char bgs[MAXCHARS];
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
