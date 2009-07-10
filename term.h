#define ESC		27

struct term_state {
	int row, col;
	int fd;
	int pid;
	int fg, bg;
	struct square {
		int c;
		int fg;
		int bg;
	} screen[MAXCHARS];
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
