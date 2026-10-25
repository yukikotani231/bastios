#include <stdio.h>
#include "greet.h"

int greet(char *buf, int size, const char *name)
{
	return snprintf(buf, size, "hello, %s", name);
}
