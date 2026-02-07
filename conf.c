#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fbpad.h"

static char tags[32] = "xnlhtr01234uiva ";
static char saved[32];
static char fonts[3][512];
static int clrfg = 0x000000;
static int clrbg = 0xffffff;
static int cursorfg = -1;
static int cursorbg = -1;
static int borderfg = 0xff0000;
static int borderwd = 2;
static char term[128] = "linux";
static char shell[128];
static char editor[128];
static char mail[128];
static char pass[128];
static char scrshot[128] = "/tmp/scr";
static char quitkey;
static int brighten = 1;

static unsigned clr16[16] = {
	0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
	0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

/* load .fbpad; returns -1 for failure, 1 if fonts are updated */
int conf_read(void)
{
	char path[512];
	char t[256];
	int fnup = 0;
	FILE *fp;
	snprintf(path, sizeof(path), "%s/.fbpad", getenv("HOME") ? getenv("HOME") : "");
	if (!(fp = fopen(path, "r")))
		return -1;
	while (fscanf(fp, "%31s", t) == 1) {
		if (!strcmp("tags", t)) {
			fscanf(fp, "%30s", tags);
			strcpy(strchr(tags, '\0'), " ");
		} else if (!strcmp("saved", t)) {
			fscanf(fp, "%31s", saved);
		} else if (!strcmp("color", t)) {
			fscanf(fp, "%x %x", &clrfg, &clrbg);
		} else if (!strcmp("color16", t)) {
			int i;
			for (i = 0; i < 16; i++)
				fscanf(fp, "%x", &clr16[i]);
		} else if (!strcmp("font", t)) {
			char fr[256], fi[256], fb[256];
			if (fscanf(fp, "%511s %511s %511s", fonts[0], fonts[1], fonts[2]) == 3)
				pad_init(fr, fi, fb);
			fnup = 1;
		} else if (!strcmp("cursor", t)) {
			fscanf(fp, "%x %x", &cursorfg, &cursorbg);
		} else if (!strcmp("border", t)) {
			fscanf(fp, "%x %d", &borderfg, &borderwd);
		} else if (!strcmp("term", t)) {
			fscanf(fp, "%127s", term);
		} else if (!strcmp("shell", t)) {
			fscanf(fp, "%127s", shell);
		} else if (!strcmp("editor", t)) {
			fscanf(fp, "%127s", editor);
		} else if (!strcmp("mail", t)) {
			fscanf(fp, "%127s", mail);
		} else if (!strcmp("scrshot", t)) {
			fscanf(fp, "%127s", scrshot);
		} else if (!strcmp("pass", t)) {
			fscanf(fp, "%127s", pass);
		} else if (!strcmp("quitkey", t)) {
			fscanf(fp, " %c", &quitkey);
		} else if (!strcmp("brighten", t)) {
			fscanf(fp, "%d", &brighten);
		}
		fgets(t, sizeof(t), fp);
	}
	return fnup;
}

char *conf_tags(void)
{
	return tags;
}

char *conf_saved(void)
{
	return saved;
}

int conf_fg(void)
{
	return clrfg;
}

int conf_bg(void)
{
	return clrbg;
}

int conf_cursorfg(void)
{
	return cursorfg;
}

int conf_cursorbg(void)
{
	return cursorbg;
}

int conf_borderwd(void)
{
	return borderwd;
}

int conf_borderfg(void)
{
	return borderfg;
}

unsigned *conf_clr16(void)
{
	return clr16;
}

char *conf_scrshot(void)
{
	return scrshot;
}

char *conf_term(void)
{
	return term;
}

char *conf_font(int i)
{
	return i >= 0 && i < 3 ? fonts[i] : "";
}

char *conf_shell(void)
{
	char *sh = shell[0] ? shell : getenv("SHELL");
	return sh ? sh : "/bin/sh";
}

char *conf_editor(void)
{
	char *ed = editor[0] ? editor : getenv("VISUAL");
	return ed ? ed : "vi";
}

char *conf_mail(void)
{
	return mail;
}

char *conf_pass(void)
{
	return pass;
}

int conf_quitkey(void)
{
	return quitkey;
}

int conf_brighten(void)
{
	return brighten;
}
