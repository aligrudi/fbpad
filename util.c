#include <stdio.h>
#include <stdlib.h>

void xerror(char *msg)
{
	perror(msg);
	exit(1);
}
