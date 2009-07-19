#define MAXCHARS	(1 << 15)

void pad_init(void);
void pad_free(void);
void pad_put(int ch, int r, int c, int fg, int bg);
int pad_rows(void);
int pad_cols(void);
void pad_blank(int c);
void pad_shown(void);
