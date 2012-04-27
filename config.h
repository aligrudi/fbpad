#define TAGS		"xnlhtr01uiva-"
#define TAGS_SAVED	""
#define SHELL		"ksh"
#define MAIL		"mailx"
#define EDITOR		"vi"

/* fontsets; tinyfont files for regular, italic, and bold fonts */
#define F0		{"/path/to/font.tf", NULL, NULL}
#define F1		{}
#define F2		{}
#define F3		{}
#define F4		{}

#define FGCOLOR		0
#define BGCOLOR		7

/* black */
#define COLOR0		0x000000
#define COLOR8		0x407080
/* red */
#define COLOR1		0xa02020
#define COLOR9		0xb05050
/* green */
#define COLOR2		0x156015
#define COLOR10		0x307030
/* yellow */
#define COLOR3		0x707030
#define COLOR11		0x909060
/* blue */
#define COLOR4		0x202070
#define COLOR12		0x303080
/* magenta */
#define COLOR5		0x903070
#define COLOR13		0xa05080
/* cyan */
#define COLOR6		0x602080
#define COLOR14		0x704090
/* white */
#define COLOR7		0xf0f0f0
#define COLOR15		0xdedede

/* where to write the screen shot */
#define SCRSHOT		"/tmp/scr"

/* framebuffer depth */
typedef unsigned int fbval_t;

/* optimized version of fb_val() */
#define FB_VAL(r, g, b)	fb_val((r), (g), (b))
