#define MAXDOTS		(1 << 10)

void font_init(void);
void font_free(void);
int font_rows(void);
int font_cols(void);
unsigned char *font_bitmap(int c, int flags);
