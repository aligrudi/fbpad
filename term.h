#define ESC		27

struct term_state {
	int row, col;
	int fd;
	int pid;
	struct pad_state pad;
};
void term_save(struct term_state *state);
void term_load(struct term_state *state);
int term_fd(void);
void term_read(void);
void term_send(int c);
void term_exec(char *cmd);
void term_end(void);
