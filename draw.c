#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "draw.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define NLEVELS		(1 << 8)

static struct fb_var_screeninfo vinfo;	/* linux-specific FB structure */
static struct fb_fix_screeninfo finfo;	/* linux-specific FB structure */
static char fbdev[1024];		/* FB device */
static int fd;				/* FB device file descriptor */
static void *fb;			/* mmap()ed FB memory */
static int bpp;				/* bytes per pixel */
static int nr, ng, nb;			/* color levels */
static int rl, rr, gl, gr, bl, br;	/* shifts per color */
static int xres, yres, xoff, yoff;	/* drawing region */

static int fb_len(void)
{
	return finfo.line_length * vinfo.yres_virtual;
}

static void fb_cmap(int save)
{
	static unsigned short red[NLEVELS], green[NLEVELS], blue[NLEVELS];
	struct fb_cmap cmap;
	cmap.start = 0;
	cmap.len = MAX(nr, MAX(ng, nb));
	cmap.red = red;
	cmap.green = green;
	cmap.blue = blue;
	cmap.transp = NULL;
	ioctl(fd, save ? FBIOGETCMAP : FBIOPUTCMAP, &cmap);
}

void fb_enter(void)
{
	unsigned short red[NLEVELS], green[NLEVELS], blue[NLEVELS];
	struct fb_cmap cmap;
	int i;
	ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo);
	if (finfo.visual == FB_VISUAL_TRUECOLOR)
		return;
	fb_cmap(1);
	for (i = 0; i < nr; i++)
		red[i] = (65535 / (nr - 1)) * i;
	for (i = 0; i < ng; i++)
		green[i] = (65535 / (ng - 1)) * i;
	for (i = 0; i < nb; i++)
		blue[i] = (65535 / (nb - 1)) * i;
	cmap.start = 0;
	cmap.len = MAX(nr, MAX(ng, nb));
	cmap.red = red;
	cmap.green = green;
	cmap.blue = blue;
	cmap.transp = NULL;
	ioctl(fd, FBIOPUTCMAP, &cmap);
}

void fb_leave(void)
{
	if (finfo.visual != FB_VISUAL_TRUECOLOR)
		fb_cmap(0);
}

unsigned fb_mode(void)
{
	return ((rl < gl) << 22) | ((rl < bl) << 21) | ((gl < bl) << 20) |
		(bpp << 16) | (vinfo.red.length << 8) |
		(vinfo.green.length << 4) | (vinfo.blue.length);
}

static void init_colors(void)
{
	nr = 1 << vinfo.red.length;
	ng = 1 << vinfo.blue.length;
	nb = 1 << vinfo.green.length;
	rr = 8 - vinfo.red.length;
	rl = vinfo.red.offset;
	gr = 8 - vinfo.green.length;
	gl = vinfo.green.offset;
	br = 8 - vinfo.blue.length;
	bl = vinfo.blue.offset;
}

int fb_init(char *dev)
{
	char *path = dev ? dev : FBDEV;
	char *geom = dev ? strchr(dev, ':') : NULL;
	if (geom) {
		*geom = '\0';
		sscanf(geom + 1, "%dx%d%d%d", &xres, &yres, &xoff, &yoff);
	}
	snprintf(fbdev, sizeof(fbdev), "%s", path);
	fd = open(path, O_RDWR);
	if (fd < 0)
		goto failed;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
		goto failed;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
		goto failed;
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	bpp = (vinfo.bits_per_pixel + 7) >> 3;
	fb = mmap(NULL, fb_len(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		goto failed;
	init_colors();
	fb_enter();
	return 0;
failed:
	perror("fb_init()");
	close(fd);
	return 1;
}

void fb_free(void)
{
	fb_leave();
	munmap(fb, fb_len());
	close(fd);
}

int fb_rows(void)
{
	return yres ? yres : vinfo.yres;
}

int fb_cols(void)
{
	return xres ? xres : vinfo.xres;
}

void *fb_mem(int r)
{
	return fb + (r + vinfo.yoffset + yoff) * finfo.line_length + (vinfo.xoffset + xoff) * bpp;
}

unsigned fb_val(int r, int g, int b)
{
	return ((r >> rr) << rl) | ((g >> gr) << gl) | ((b >> br) << bl);
}

char *fb_dev(void)
{
	return fbdev;
}
