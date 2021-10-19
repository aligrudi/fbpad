/* fbpad header file */

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define LEN(a)		(sizeof(a) / sizeof((a)[0]))

#define ESC		27		/* escape code */
#define NCOLS		256		/* maximum number of screen columns */
#define NROWS		128		/* maximum number of screen rows */
#define NDOTS		1024		/* maximum pixels in glyphs */
#define NHIST		128		/* scrolling history lines */

/* isdw.c */
#define DWCHAR		0x40000000u	/* 2nd half of a fullwidth char */

int isdw(int c);
int iszw(int c);

/* term.c */
struct term_state {
	int row, col;
	int fg, bg;
	int mode;
};

struct term {
	int screen[NROWS * NCOLS];	/* screen content */
	int hist[NHIST * NCOLS];	/* scrolling history */
	int fgs[NROWS * NCOLS];		/* foreground color */
	int bgs[NROWS * NCOLS];		/* background color */
	int dirty[NROWS];		/* changed rows in lazy mode */
	struct term_state cur, sav;	/* terminal saved state */
	int fd;				/* terminal file descriptor */
	int hrow;			/* the next history row in hist[] */
	int hpos;			/* scrolling history; position */
	int lazy;			/* lazy mode */
	int pid;			/* pid of the terminal program */
	int top, bot;			/* terminal scrolling region */
	int rows, cols;
};

void term_load(struct term *term, int visible);
void term_save(struct term *term);

void term_read(void);
void term_send(int c);
void term_exec(char **args);
void term_end(void);
void term_screenshot(void);
void term_scrl(int pos);
void term_redraw(int all);

/* pad.c */
#define FN_I		0x01000000	/* italic font */
#define FN_B		0x02000000	/* bold font */
#define FN_C		0x00ffffff	/* font color mask */

int pad_init(void);
void pad_free(void);
void pad_conf(int row, int col, int rows, int cols);
int pad_font(char *fr, char *fi, char *fb);
void pad_put(int ch, int r, int c, int fg, int bg);
int pad_rows(void);
int pad_cols(void);
void pad_fill(int sr, int er, int sc, int ec, int c);
void pad_border(unsigned c);

/* font.c */
struct font *font_open(char *path);
void font_free(struct font *font);
int font_rows(struct font *font);
int font_cols(struct font *font);
int font_bitmap(struct font *font, void *dst, int c);

/* scrsnap.c */
void scr_snap(int idx);
int scr_load(int idx);
void scr_free(int idx);
void scr_done(void);
