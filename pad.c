#include <stdio.h>
#include <stdlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H
#include "draw.h"

#define FONTFACE	"/home/ali/.fonts/monaco.ttf"
#define FONTSIZE	10
#define DPI		192
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

static FT_Library library;
static FT_Face face;
static int rows, cols;
static int row, col;
static int char_height, char_width;
static int fg, bg;

static unsigned int cd[] = {
	0x0a0a0a, 0xc04444, 0x339933, 0xcccc66,
	0x5566bc, 0xcd66af, 0xa166cd, 0xeeeeee,
	0x71a3b7, 0xc08888, 0x779977, 0xcccc99,
	0x8899bc, 0xcd99af, 0xa199cd, 0xdedede};

static void init_size(void)
{
	char_height = face->size->metrics.height >> 6;
	char_width = face->size->metrics.max_advance >> 6;
	rows = fb_rows() / char_height;
	cols = fb_cols() / char_width;
}

void pad_init(void)
{
	FT_Init_FreeType(&library);
	FT_New_Face(library, FONTFACE, 0, &face);
	FT_Set_Char_Size(face, 0, FONTSIZE << 6, DPI, DPI);
	fb_init();
	init_size();
}

void pad_free(void)
{
	fb_free();
}

#define CR(a)		(((a) >> 16) & 0x0000ff)
#define CG(a)		(((a) >> 8) & 0x0000ff)
#define CB(a)		((a) & 0x0000ff)
#define COLORMERGE(f, b, c)		((b) + (((f) - (b)) * (c) >> 8u))

static u16_t mixed_color(u8_t val)
{
	unsigned int fore = cd[fg], back = cd[bg];
	u8_t r = COLORMERGE(CR(fore), CR(back), val);
	u8_t g = COLORMERGE(CG(fore), CG(back), val);
	u8_t b = COLORMERGE(CB(fore), CB(back), val);
	return fb_color(r, g, b);
}

static void draw_bitmap(FT_Bitmap *bitmap, int r, int c)
{
	int i, j;
	for (i = 0; i < bitmap->rows; i++) {
		for (j = 0; j < bitmap->width; j++) {
			u8_t val = bitmap->buffer[i * bitmap->width + j];
			fb_put(r + i, c + j, mixed_color(val));
		}
	}
}

void pad_fg(int c)
{
	fg = c;
}

void pad_bg(int c)
{
	bg = c;
}

void pad_put(int ch, int r, int c)
{
	int sr = char_height * r;
	int sc = char_width * c;
	FT_Load_Char(face, ch, FT_LOAD_RENDER);
	if (fg >= 8) {
		int FT_GlyphSlot_Own_Bitmap(FT_GlyphSlot);
		FT_GlyphSlot_Own_Bitmap(face->glyph);
		FT_Bitmap_Embolden(library, &face->glyph->bitmap, 32, 32);
	}
	fb_box(sr, sc, sr + char_height, sc + char_width, mixed_color(0));
	sr -= face->glyph->bitmap_top - char_height;
	sc += face->glyph->bitmap_left / 2;
	draw_bitmap(&face->glyph->bitmap, sr, sc);
}

static int advance(int c)
{
	int printable = 0;
	switch (c) {
	case '\n':
		row++;
		col = 0;
		break;
	case '\t':
		col = (col / 8 + 1) * 8;
		break;
	case '\b':
		if (col)
			col--;
		break;
	case '\r':
		col = 0;
		break;
	case '\a':
	case '\f':
	case '\v':
		break;
	default:
		printable = 1;
		col++;
	}
	if (col >= cols) {
		row++;
		col = 0;
	}
	if (row >= rows)
		row = rows - 1;
	return printable;
}

void pad_add(int c)
{
	if (advance(c))
		pad_put(c, row, col);
}

void pad_blank(void)
{
	fb_box(0, 0, fb_rows(), fb_cols(), mixed_color(0));
}

void pad_move(int r, int c)
{
	row = MIN(r, rows - 1);
	col = MIN(c, cols - 1);
}

int pad_row(void)
{
	return row;
}

int pad_col(void)
{
	return col;
}

int pad_rows(void)
{
	return rows;
}

int pad_cols(void)
{
	return cols;
}
