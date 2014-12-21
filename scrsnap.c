#include <stdlib.h>
#include <string.h>
#include "draw.h"

#define NSCRS		128

static void *scrs[NSCRS];

void scr_snap(int idx)
{
	int rowsz = FBM_BPP(fb_mode()) * fb_cols();
	int i;
	if (idx < NSCRS && !scrs[idx])
		scrs[idx] = malloc(fb_rows() * rowsz);
	if (idx < NSCRS && scrs[idx])
		for (i = 0; i < fb_rows(); i++)
			memcpy(scrs[idx] + i * rowsz, fb_mem(i), rowsz);
}

void scr_free(int idx)
{
	if (idx < NSCRS) {
		free(scrs[idx]);
		scrs[idx] = NULL;
	}
}

int scr_load(int idx)
{
	int rowsz = FBM_BPP(fb_mode()) * fb_cols();
	int i;
	if (idx < NSCRS && scrs[idx])
		for (i = 0; i < fb_rows(); i++)
			memcpy(fb_mem(i), scrs[idx] + i * rowsz, rowsz);
	return 0;
}

void scr_done(void)
{
	int i;
	for (i = 0; i < NSCRS; i++)
		free(scrs[i]);
}
