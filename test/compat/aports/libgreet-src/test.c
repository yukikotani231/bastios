#include <stdio.h>
#include <string.h>
#include "greet.h"

int main(void)
{
	char buf[32];
	int n = greet(buf, sizeof buf, "abuild");
	return n == 13 && strcmp(buf, "hello, abuild") == 0 ? 0 : 1;
}
