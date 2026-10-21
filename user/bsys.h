#ifndef BSYS_H
#define BSYS_H
/* bsys, a System window for bwm: the kernel, uptime, memory, processors,
 * screen and root file system in a 60 x 12 grid drawn like bfiles (it shares
 * bfiles' pixel buffer and cell routines). r reads the figures again and
 * says "bsys: refreshed"; the close box ends it. Says "bsys: window ID" when
 * the window is open. It is part of the bterm program, which runs it when
 * started as "bsys", so it adds no image to the kernel. */
#include <poll.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>

#define BS_ROWS 12

static void bs_row(int r, const char *k, const char *v)
{
    char t[BF_COLS + 1];
    snprintf(t, sizeof t, "  %-10s %s", k, v);
    bf_put_row(r, t, strlen(t), 0x00D8D8D8, 0x00101018);
}

static void bs_paint(unsigned sw, unsigned sh)
{
    char v[BF_COLS + 1];
    struct utsname u;
    struct sysinfo si;
    struct statvfs sv;
    bf_put_row(0, "System", 6, 0x00FFFFFF, 0x00305090);
    bf_put_row(1, "", 0, 0x00D8D8D8, 0x00101018);
    if (uname(&u) == 0)
        snprintf(v, sizeof v, "%s %s (%s)", u.sysname, u.release, u.machine);
    else
        snprintf(v, sizeof v, "unknown");
    bs_row(2, "Kernel", v);
    if (sysinfo(&si) != 0)
        memset(&si, 0, sizeof si);
    snprintf(v, sizeof v, "%ld days %ld:%02ld:%02ld", si.uptime / 86400, si.uptime / 3600 % 24, si.uptime / 60 % 60, si.uptime % 60);
    bs_row(3, "Uptime", v);
    snprintf(v, sizeof v, "%lu MiB free of %lu MiB", (unsigned long)(si.freeram * si.mem_unit >> 20), (unsigned long)(si.totalram * si.mem_unit >> 20));
    bs_row(4, "Memory", v);
    snprintf(v, sizeof v, "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    bs_row(5, "Processors", v);
    snprintf(v, sizeof v, "%u", (unsigned)si.procs);
    bs_row(6, "Processes", v);
    snprintf(v, sizeof v, "%u x %u", sw, sh);
    bs_row(7, "Screen", v);
    if (statvfs("/", &sv) == 0)
        snprintf(v, sizeof v, "%lu MiB free of %lu MiB", (unsigned long)((unsigned long long)sv.f_bavail * sv.f_frsize >> 20), (unsigned long)((unsigned long long)sv.f_blocks * sv.f_frsize >> 20));
    else
        snprintf(v, sizeof v, "unknown");
    bs_row(8, "Root disk", v);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(v, sizeof v, "%Y-%m-%d %H:%M:%S UTC", &tm);
    bs_row(9, "Time", v);
    bf_put_row(10, "", 0, 0x00D8D8D8, 0x00101018);
    bf_put_row(11, "  r: refresh", 13, 0x00808090, 0x00101018);
}

static int bsys_main(void)
{
    unsigned sw, sh;
    bwm_screen(&sw, &sh);
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bsys: no window server");
        return 2;
    }
    int id = bwm_create(fd, BF_WIDTH, BS_ROWS * BF_CH, "System");
    if (id < 0) {
        puts("bsys: cannot open a window");
        return 1;
    }
    bs_paint(sw, sh);
    if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BS_ROWS * BF_CH) != 0)
        return 1;
    printf("bsys: window %d\n", id);
    fflush(stdout);
    static unsigned char buf[BWM_MAX_PAYLOAD];
    int live = 0;
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 2000) == 0) {                       /* quiet for 2 s: read the numbers again */
            bs_paint(sw, sh);
            if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BS_ROWS * BF_CH) != 0)
                break;
            if (!live) {
                live = 1;
                puts("bsys: live");
                fflush(stdout);
            }
            continue;
        }
        int t = bwm_event(fd, buf);
        if (t < 0 || t == BWM_CLOSE)
            break;
        if (t == BWM_KEY && ((struct bwm_key *)buf)->ch == 'r') {
            bs_paint(sw, sh);
            if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BS_ROWS * BF_CH) != 0)
                break;
            puts("bsys: refreshed");
            fflush(stdout);
        }
    }
    return 0;
}
#endif
