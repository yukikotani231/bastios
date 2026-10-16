#include <stdio.h>
#include <greet.h>

int main(int argc, char **argv)
{
	char buf[64];
	greet(buf, sizeof buf, argc > 1 ? argv[1] : "world");
	puts(buf);
	return 0;
}
