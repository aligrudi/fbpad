typedef unsigned short fbval_t;

void fb_init(void);
void fb_free(void);
fbval_t fb_color(unsigned char r, unsigned char g, unsigned char b);
void fb_put(int r, int c, fbval_t val);
int fb_rows(void);
int fb_cols(void);
void fb_box(int sr, int sc, int er, int ec, fbval_t val);
void fb_scroll(int sr, int nr, int n, fbval_t val);
void fb_cmap(void);
