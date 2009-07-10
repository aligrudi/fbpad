#define MAXCHARS	1 << 15

void pad_init(void);
void pad_free(void);
void pad_fg(int fg);
void pad_bg(int bg);
void pad_put(int ch, int r, int c);
int pad_row(void);
int pad_col(void);
int pad_rows(void);
int pad_cols(void);
void pad_scroll(int sr, int nr, int n);
void pad_show(int r, int c, int flags);
void pad_blank(void);

struct pad_state {
	int fg, bg;
	struct square {
		int c;
		int fg;
		int bg;
	} screen[MAXCHARS];
};
void pad_save(struct pad_state *state);
void pad_load(struct pad_state *state);
void pad_shown(void);
