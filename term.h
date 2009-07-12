#define ESC		27

struct term_state {
	int row, col;
	int fg, bg;
	int top, bot;
	unsigned long mode;
};

struct term {
	int fd;
	int pid;
	struct square {
		int c;
		short fg;
		short bg;
	} screen[MAXCHARS];
	struct term_state cur, sav;
};

void term_load(struct term *term);
void term_save(struct term *term);

int term_fd(void);
void term_read(void);
void term_send(int c);
void term_exec(char *cmd);
void term_end(void);
void term_blank(void);
void term_init(void);
void term_free(void);
