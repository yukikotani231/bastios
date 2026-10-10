/* A dynamically linked musl program: the kernel loads musl's dynamic
 * linker (its PT_INTERP) beside it, and the linker resolves printf, malloc
 * and the rest from itself, as libc.so. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(int argc, char **argv)
{
    int *v = malloc(100 * sizeof *v);
    if (!v)
        return 1;
    for (int i = 0; i < 100; i++)
        v[i] = 100 - i;
    qsort(v, 100, sizeof *v, cmp);
    long sum = 0;
    for (int i = 0; i < 100; i++)
        sum += v[i] * (v[i] == i + 1);
    char *s = strdup(argv[0]);
    /* libc's code sits apart from the program's own. */
    long apart = (long)((char *)&printf - (char *)&main);
    printf("mdyn: linked dynamically, %s, sum %ld, argc %d, %s\n", s, sum, argc,
           apart > 0x100000 || apart < -0x100000 ? "libc apart" : "libc close");
    free(s);
    free(v);
    return 0;
}
