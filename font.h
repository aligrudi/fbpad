#define MAXDOTS		(1 << 10)

int font_init(void);
void font_free(void);
int font_rows(void);
int font_cols(void);
int font_bitmap(void *dst, int c);
