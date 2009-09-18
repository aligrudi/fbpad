#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H
#include "config.h"
#include "font.h"
#include "util.h"

static FT_Library library;
static FT_Face face;
static int rows, cols;

static void xdie(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

void font_init(void)
{
	FT_Init_FreeType(&library);
	if (FT_New_Face(library, FONTFACE, 0, &face))
		xdie("failed to load font");
	FT_Set_Char_Size(face, 0, FONTSIZE << 6, DPI, DPI);
	rows = (face->size->metrics.height >> 6) + HEIGHTDIFF;
	cols = (face->size->metrics.max_advance >> 6) + WIDTHDIFF;
}

unsigned char *font_bitmap(int c)
{
	static unsigned char bits[MAXDOTS];
	int sr, sc, er, ec;
	int i;
	unsigned char *src;
	if (FT_Load_Char(face, c, FT_LOAD_RENDER))
		return NULL;
	sr = rows + (face->size->metrics.descender >> 6) -
		 face->glyph->bitmap_top;
	sc = face->glyph->bitmap_left;
	er = MIN(rows, sr + face->glyph->bitmap.rows);
	ec = MIN(cols, sc + face->glyph->bitmap.width);
	memset(bits, 0, rows * cols);
	src = face->glyph->bitmap.buffer - MIN(0, sc);
	sc = MAX(0, sc);
	for (i = MAX(0, sr); i < er; i++) {
		int w = face->glyph->bitmap.pitch;
		memcpy(&bits[i * cols + sc], src + (i - sr) * w, ec - sc);
	}
	return bits;
}

void font_free(void)
{
	FT_Done_Face(face);
	FT_Done_FreeType(library);
}

int font_rows(void)
{
	return rows;
}

int font_cols(void)
{
	return cols;
}
