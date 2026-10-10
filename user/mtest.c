/*
 * A musl C program that uses more of Linux: malloc (brk and mmap), a file
 * written with stdio and read back, and a line read from the console.
 * Prints one "mtest: ..." line per check and exits with the number that
 * failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    int bad = 0;

    /* Small blocks come from brk, a big one from mmap. */
    char *small = malloc(100);
    char *big = malloc(300 * 1024);
    if (!small || !big) { puts("mtest: malloc failed"); bad++; }
    else {
        memset(big, 0x5a, 300 * 1024);
        strcpy(small, "heap");
        int sum = 0;
        for (int i = 0; i < 300 * 1024; i += 4096) sum += big[i] == 0x5a;
        printf("mtest: malloc %s %d\n", small, sum);
        free(big);
        free(small);
    }

    FILE *f = fopen("/mtest.txt", "w");
    if (!f) { puts("mtest: fopen w failed"); bad++; }
    else {
        fprintf(f, "line %d\n", 42);
        fclose(f);
        char buf[32] = {0};
        f = fopen("mtest.txt", "r");
        if (!f || !fgets(buf, sizeof buf, f)) { puts("mtest: read back failed"); bad++; }
        else printf("mtest: file %s", buf);
        if (f) fclose(f);
        if (remove("mtest.txt") != 0) { puts("mtest: remove failed"); bad++; }
    }

    char line[64];
    printf("mtest: type a line\n");
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) { puts("mtest: no input"); bad++; }
    else printf("mtest: got %s", line);
    return bad;
}
