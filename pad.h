#define MAXCHARS	(1 << 15)
#define FN_I		0x10
#define FN_B		0x20
#define FN_C(fg)	(((fg) & FN_B ? (fg) + 8 : (fg)) & 0x0f)

int pad_init(void);
void pad_free(void);
void pad_put(int ch, int r, int c, int fg, int bg);
int pad_rows(void);
int pad_cols(void);
void pad_blank(int c);
void pad_blankrow(int r, int bg);
