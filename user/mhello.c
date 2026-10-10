/*
 * An ordinary C program built against musl libc as a static PIE: no
 * BastiOS headers, nothing but what Linux offers. Its startup sets the
 * thread pointer (arch_prctl), printf buffers and flushes through writev,
 * and exit ends in exit_group.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {
    errno = 0;                      /* errno lives in thread-local storage */
    printf("hello from musl: %s, argc %d, %zu\n", argv[0], argc, strlen(argv[0]));
    return 3;
}
