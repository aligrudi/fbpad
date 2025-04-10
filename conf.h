/* list of tags */
#define TAGS		"xnlhtr01uiva "
#define TAGS_SAVED	""

/* programs mapped to m-c, m-m, m-e */
#define SHELL		{"sh"}
#define EDITOR		{"vi"}
#define MAIL		{"mailx", "-f", "+inbox"}

/* TERM variable for launched programs */
#define TERM		"linux"

/* tinyfont files for regular, italic, and bold fonts */
#define FR		"/path/to/font.tf"
#define FI		NULL
#define FB		NULL

/* where to write the screen shot */
#define SCRSHOT		"/tmp/scr"

/* file from which to read terminal font and colour options */
#define CLRFILE		"/tmp/clr"

/* lock command password; NULL disables locking */
#define PASS		NULL

/* brighten colors 0-7 for bold text */
#define BRIGHTEN	1

/* foreground and background colors */
#define FGCOLOR		COLOR0
#define BGCOLOR		COLORF

/* black */
#define COLOR0		0x000000
#define COLOR8		0x555555
/* red */
#define COLOR1		0xaa0000
#define COLOR9		0xff5555
/* green */
#define COLOR2		0x00aa00
#define COLORA		0x55ff55
/* yellow */
#define COLOR3		0xaa5500
#define COLORB		0xffff55
/* blue */
#define COLOR4		0x0000aa
#define COLORC		0x5555ff
/* magenta */
#define COLOR5		0xaa00aa
#define COLORD		0xff55ff
/* cyan */
#define COLOR6		0x00aaaa
#define COLORE		0x55ffff
/* white */
#define COLOR7		0xaaaaaa
#define COLORF		0xffffff
