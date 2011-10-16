#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "draw.h"
#include "scrsnap.h"

#define NSCRS		((sizeof(saved) - 1) * 2)
#define SNAPSZ		(1 << 23)

static char saved[] = TAGS_SAVED;
static char scrs[NSCRS][SNAPSZ];
static void *owners[NSCRS];

static int scr_find(void *owner)
{
	int i;
	for (i = 0; i < NSCRS; i++)
		if (owners[i] == owner)
			return i;
	return -1;
}

static int scr_slot(void)
{
	int index = scr_find(NULL);
	return index > -1 ? index : 0;
}

void scr_snap(void *owner)
{
	int rowsz = sizeof(fbval_t) * fb_cols();
	int scr = scr_slot();
	int i;
	for (i = 0; i < fb_rows(); i++)
		memcpy(scrs[scr] + i * rowsz, fb_mem(i), rowsz);
	owners[scr] = owner;
}

void scr_free(void *owner)
{
	int scr = scr_find(owner);
	if (scr != -1)
		owners[scr] = NULL;
}

int scr_load(void *owner)
{
	int rowsz = sizeof(fbval_t) * fb_cols();
	int scr = scr_find(owner);
	int i;
	if (scr == -1)
		return -1;
	for (i = 0; i < fb_rows(); i++)
		memcpy(fb_mem(i), scrs[scr] + i * rowsz, rowsz);
	owners[scr] = NULL;
	return 0;
}
