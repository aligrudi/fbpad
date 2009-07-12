#define ESC		27

struct miscterm_state {
	int row, col;
	int fg, bg;
	int top, bot;
	unsigned long mode;
};

struct term_state {
	int fd;
	int pid;
	struct square {
		int c;
		short fg;
		short bg;
	} screen[MAXCHARS];
	struct miscterm_state cur, sav;
};

void term_save(struct term_state *state);
void term_load(struct term_state *state);
int term_fd(void);
void term_read(void);
void term_send(int c);
void term_exec(char *cmd);
void term_end(void);
void term_blank(void);
void term_init(void);
void term_free(void);
