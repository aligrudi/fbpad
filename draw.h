typedef unsigned short u16_t;
typedef unsigned char u8_t;

void fb_init(void);
void fb_free(void);
u16_t fb_color(u8_t r, u8_t g, u8_t b);
void fb_put(int r, int c, u16_t val);
int fb_rows(void);
int fb_cols(void);
void fb_box(int sr, int sc, int er, int ec, u16_t val);
void fb_scroll(int n, u16_t val);
