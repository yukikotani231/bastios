#ifndef BSYS_H
#define BSYS_H
/* bsys, a System window for bwm: the kernel, uptime, memory, processors,
 * screen and root file system in a 60 x 12 grid drawn like bfiles (it shares
 * bfiles' pixel buffer and cell routines). r reads the figures again and
 * says "bsys: refreshed"; p swaps them for the list of processes (newest
 * first; j and k or the Up and Down keys move along it and say
 * "bsys: select PID NAME", x asks the selected one to end with SIGTERM and
 * says "bsys: terminated PID", p goes back); the close box ends it. Says "bsys: window ID" when
 * the window is open. It is part of the bterm program, which runs it when
 * started as "bsys", so it adds no image to the kernel. */
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>

#define BS_ROWS 12
#define BS_MAXPROC 64
#define BS_LIST 8                                 /* process rows shown, rows 2 .. 9 */

struct bs_proc { int pid; char state; char name[24]; };
static struct bs_proc bs_procs[BS_MAXPROC];
static int bs_nproc, bs_sel, bs_top, bs_tasks, bs_esc;

static int bs_cmp(const void *a, const void *b)
{
    return ((const struct bs_proc *)b)->pid - ((const struct bs_proc *)a)->pid;   /* newest first */
}

/* Reads /proc/PID/stat ("PID (NAME) STATE ...") of every process. */
static void bs_scan(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    bs_nproc = 0;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL && bs_nproc < BS_MAXPROC) {
        int pid = atoi(e->d_name);
        if (pid <= 0)
            continue;
        char path[48], line[256];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        size_t n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[n] = 0;
        char *o = strchr(line, '('), *c = strrchr(line, ')');
        if (!o || !c || c < o)
            continue;
        struct bs_proc *p = &bs_procs[bs_nproc++];
        p->pid = pid;
        p->state = c[1] == ' ' && c[2] ? c[2] : '?';
        size_t l = (size_t)(c - o - 1);
        if (l >= sizeof p->name)
            l = sizeof p->name - 1;
        memcpy(p->name, o + 1, l);
        p->name[l] = 0;
    }
    closedir(d);
    qsort(bs_procs, (size_t)bs_nproc, sizeof bs_procs[0], bs_cmp);
    if (bs_sel >= bs_nproc)
        bs_sel = bs_nproc ? bs_nproc - 1 : 0;
}

static void bs_move(int by)
{
    bs_sel += by;
    if (bs_sel >= bs_nproc)
        bs_sel = bs_nproc - 1;
    if (bs_sel < 0)
        bs_sel = 0;
    if (bs_sel < bs_top)
        bs_top = bs_sel;
    if (bs_sel >= bs_top + BS_LIST)
        bs_top = bs_sel - BS_LIST + 1;
    if (bs_nproc)
        printf("bsys: select %d %s\n", bs_procs[bs_sel].pid, bs_procs[bs_sel].name);
    fflush(stdout);
}

static void bs_paint_tasks(void)
{
    char t[BF_COLS + 1];
    snprintf(t, sizeof t, "Processes (%d)", bs_nproc);
    bf_put_row(0, t, strlen(t), 0x00FFFFFF, 0x00305090);
    bf_put_row(1, "  PID    S  Name", 16, 0x00808090, 0x00101018);
    for (int i = 0; i < BS_LIST; i++) {
        int k = bs_top + i;
        if (k >= bs_nproc) {
            bf_put_row(2 + i, "", 0, 0x00D8D8D8, 0x00101018);
            continue;
        }
        snprintf(t, sizeof t, "  %-6d %c  %s", bs_procs[k].pid, bs_procs[k].state, bs_procs[k].name);
        bf_put_row(2 + i, t, strlen(t), 0x00D8D8D8, k == bs_sel ? 0x00305090 : 0x00101018);
    }
    bf_put_row(10, "", 0, 0x00D8D8D8, 0x00101018);
    bf_put_row(11, "  j/k g/G: move  x: end  p: back", 32, 0x00808090, 0x00101018);
}

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
    bf_put_row(11, "  r: refresh  p: processes", 27, 0x00808090, 0x00101018);
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
            if (bs_tasks) {
                bs_scan();
                bs_paint_tasks();
            } else
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
        if (t != BWM_KEY)
            continue;
        int c = ((struct bwm_key *)buf)->ch;
        if (bs_esc == 0 && c == 27) {                         /* Up and Down arrive as ESC [ A and ESC [ B */
            bs_esc = 1;
            continue;
        }
        if (bs_esc == 1) {
            bs_esc = c == '[' ? 2 : 0;
            continue;
        }
        if (bs_esc == 2) {
            bs_esc = 0;
            c = c == 'A' ? 'k' : c == 'B' ? 'j' : c == 'H' ? 'g' : c == 'F' ? 'G' : 0;
        }
        if (c == 'p') {
            bs_tasks = !bs_tasks;
            if (bs_tasks) {
                bs_scan();
                bs_sel = bs_top = 0;
                bs_paint_tasks();
                printf("bsys: processes %d\n", bs_nproc);
            } else {
                bs_paint(sw, sh);
                puts("bsys: system");
            }
        } else if (bs_tasks && (c == 'j' || c == 'k')) {
            bs_move(c == 'j' ? 1 : -1);
            bs_paint_tasks();
        } else if (bs_tasks && (c == 'g' || c == 'G')) {            /* the first and the last of the list */
            bs_move(c == 'g' ? -bs_nproc : bs_nproc);
            bs_paint_tasks();
        } else if (bs_tasks && c == 'x' && bs_nproc) {
            int pid = bs_procs[bs_sel].pid;
            if (pid == 1 || pid == getpid())
                printf("bsys: not ending %d\n", pid);
            else if (kill(pid, SIGTERM) == 0)
                printf("bsys: terminated %d\n", pid);
            else
                printf("bsys: cannot end %d\n", pid);
            bs_scan();
            bs_paint_tasks();
        } else if (c == 'r') {
            if (bs_tasks) {
                bs_scan();
                bs_paint_tasks();
            } else
                bs_paint(sw, sh);
            puts("bsys: refreshed");
        } else
            continue;
        fflush(stdout);
        if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BS_ROWS * BF_CH) != 0)
            break;
    }
    return 0;
}
#endif
