#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "draw.h"
#include "util.h"

#define FBDEV_PATH	"/dev/fb0"
#define BPP		sizeof(fbval_t)

static int fd;
static unsigned char *fb;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

static int fb_len()
{
	return vinfo.xres_virtual * vinfo.yres_virtual * BPP;
}

#define NLEVELS	1 << 8
void fb_cmap(void)
{
	unsigned short red[NLEVELS], green[NLEVELS], blue[NLEVELS];
	struct fb_cmap cmap;
	int mr = 1 << vinfo.red.length;
	int mg = 1 << vinfo.green.length;
	int mb = 1 << vinfo.blue.length;
	int i;
	if (finfo.visual == FB_VISUAL_TRUECOLOR)
		return;

	for (i = 0; i < mr; i++)
		red[i] = (i << 16) / (mr - 1);
	for (i = 0; i < mg; i++)
		green[i] = (i << 16) / (mg - 1);
	for (i = 0; i < mb; i++)
		blue[i] = (i << 16) / (mb - 1);

	cmap.start = 0;
	cmap.len = MAX(mr, MAX(mg, mb));
	cmap.red = red;
	cmap.green = green;
	cmap.blue = blue;
	cmap.transp = 0;

	ioctl(fd, FBIOPUTCMAP, &cmap);
}

void fb_init(void)
{
	fd = open(FBDEV_PATH, O_RDWR);
	if (fd == -1)
		xerror("can't open " FBDEV_PATH);
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == -1)
		xerror("ioctl failed");
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
		xerror("ioctl failed");
	fb = mmap(NULL, fb_len(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		xerror("can't map the framebuffer");
	fb_cmap();
}

void fb_put(int r, int c, fbval_t val)
{
	long loc = (c + vinfo.xoffset) * BPP +
		(r + vinfo.yoffset) * finfo.line_length;
	memcpy(fb + loc, &val, BPP);
}

void fb_free()
{
	munmap(fb, fb_len());
	close(fd);
}

static fbval_t color_bits(struct fb_bitfield *bf, fbval_t v)
{
	fbval_t moved = v >> (8 - bf->length);
	return moved << bf->offset;
}

fbval_t fb_color(unsigned char r, unsigned char g, unsigned char b)
{
	return color_bits(&vinfo.red, r) |
		color_bits(&vinfo.green, g) |
		color_bits(&vinfo.blue, b);
}

int fb_rows(void)
{
	return vinfo.yres_virtual;
}

int fb_cols(void)
{
	return vinfo.xres_virtual;
}

void fb_box(int sr, int sc, int er, int ec, fbval_t val)
{
	int r, c;
	for (r = sr; r < er; r++)
		for (c = sc; c < ec; c++)
			fb_put(r, c, val);
}

static unsigned char *rowaddr(int r)
{
	return fb + (r + vinfo.yoffset) * finfo.line_length;
}

void fb_scroll(int sr, int nr, int n, fbval_t val)
{
	memmove(rowaddr(sr + n), rowaddr(sr), nr * finfo.line_length);
	if (n > 0)
		fb_box(sr, 0, n, sr + n, val);
	else
		fb_box(sr + nr + n, 0, sr + nr, fb_cols(), val);
}
