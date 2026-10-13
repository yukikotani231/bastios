/*
 * The compatibility suite's checks (test/compat). A program runs a list of
 * checks; each prints "@@ NAME" before it starts and then "ok NAME" or
 * "FAIL NAME: why". The harness (test/compat/run.py) reads these lines off
 * the serial console, together with the kernel's own
 * "[pid N: Linux system call X not implemented]" notes, which it charges to
 * the check that was running. A check that never ends (a crash or a hang)
 * shows up as a "@@" with no result.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *cur;
static int nfail, nth, skip = -1;
static const char *only;

/* Starts check NAME and says whether to run it. The harness reruns a
 * program that hung with COMPAT_SKIP=K, which skips its first K checks, and
 * one whose checks failed with COMPAT_ONLY=I,J,..., which runs just the
 * checks at those places in the list. */
static int begin(const char *name) {
    if (skip < 0) {
        const char *s = getenv("COMPAT_SKIP");
        skip = s ? atoi(s) : 0;
        only = getenv("COMPAT_ONLY");
    }
    cur = name;
    int at = nth++;
    if (only) {
        char want[16];
        snprintf(want, sizeof want, ",%d,", at);
        char *list = malloc(strlen(only) + 3);
        sprintf(list, ",%s,", only);
        int hit = strstr(list, want) != NULL;
        free(list);
        if (!hit) return 0;
    } else if (at < skip) {
        return 0;
    }
    printf("@@ %s\n", name);
    return 1;
}

static void pass(void) { printf("ok %s\n", cur); }

static void fail(const char *fmt, ...) {
    va_list ap;
    printf("FAIL %s: ", cur);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    nfail++;
}

/* The common case: a condition, and errno for the reason when it fails. */
#define EXPECT(name, cond) do { \
    if (begin(name)) { \
        errno = 0; \
        if (cond) pass(); else fail("%s (errno %d %s)", #cond, errno, strerror(errno)); \
    } \
} while (0)

static void setup(void) { setvbuf(stdout, NULL, _IONBF, 0); }
static int done(void) { return nfail ? 1 : 0; }
