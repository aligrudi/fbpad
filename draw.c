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
#define BPP		2

static int fd;
static char *fb;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

static int fb_len()
{
	return vinfo.xres_virtual * vinfo.yres_virtual * BPP;
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
}

void fb_put(int r, int c, u16_t val)
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

static u16_t color_bits(struct fb_bitfield *bf, unsigned char v)
{
	unsigned char moved = v >> (8 - bf->length);
	return moved << bf->offset;
}

u16_t fb_color(u8_t r, u8_t g, u8_t b)
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

void fb_box(int sr, int sc, int er, int ec, u16_t val)
{
	int r, c;
	for (r = sr; r < er; r++)
		for (c = sc; c < ec; c++)
			fb_put(r, c, val);
}

static char *rowaddr(int r)
{
	return fb + (r + vinfo.yoffset) * finfo.line_length;
}

void fb_scroll(int n)
{
	int s = 0, e = fb_rows();
	int r;
	if (n < 0)
		s = -n;
	else
		e -= n;
	for (r = s; r < e; r++) {
		memcpy(rowaddr(r + n), rowaddr(r), finfo.line_length);
	}
}
