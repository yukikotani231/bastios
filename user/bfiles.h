#ifndef BFILES_H
#define BFILES_H
/* bfiles, a file browser and text viewer for bwm (user/bwmproto.h): a grid
 * of 8 x 16 cells, 60 x 24, drawn with the console's font. The first row
 * shows the directory; below it come ".." and the entries, directories
 * first. A click on a directory goes into it, a click on a file shows its
 * first 64 KiB as text (non-printing bytes as dots) and a click anywhere in
 * the viewer, or Backspace, goes back. j and k (or Space and b) scroll by
 * a line (a page), also in the viewer. In the viewer, e edits a file smaller
 * than 64 KiB in place: typing inserts, Enter, Backspace, Delete, the arrow,
 * Home, End and Page keys and a click move and change the text, Ctrl-S saves
 * ("bfiles: saved PATH") and Ctrl-X goes back to the viewer, which shows the
 * file as it is on disk. In the editor Ctrl-K cuts the rest of the line and Ctrl-U puts it back,
 * Ctrl-F finds text (Enter on an empty prompt repeats the last search; "bfiles: found at N" or
 * "bfiles: not found"), Ctrl-R replaces every occurrence of a text with another ("bfiles: replaced N",
 * one undo step) and Ctrl-G goes to a line number ("bfiles: line N"); the header shows
 * the line the cursor is on. "bfiles /path/to/file" opens the file in the viewer.
 * In the list n makes an empty file (untitled, untitled2, ...) and opens it
 * in the editor; c or x marks the entry under the cursor to copy (a file) or move (a file or
 * directory) and v puts it into the directory shown ("bfiles: copy PATH", "bfiles: move PATH",
 * then "bfiles: copied SRC to DST" or "bfiles: moved SRC to DST"). The mouse wheel scrolls three lines a step (and says "bfiles: top N"). Says "bfiles: window ID" when the
 * window is open and "bfiles: dir PATH" or "bfiles: view PATH" as it
 * moves.
 * It is part of the bterm program, which runs it when started as "bfiles"
 * (/bin/bterm with that argv[0], so it adds no image to the kernel). */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define BF_COLS 60
static int bf_rows = 24;
#define BF_ROWS bf_rows
#define BF_MAXROWS 24
#define BF_CW 8
#define BF_CH 16
#define BF_WIDTH (BF_COLS * BF_CW)
#define BF_HEIGHT (BF_ROWS * BF_CH)
#define BF_MAXENT 1024
#define BF_VIEWMAX 65536
#define BF_MAXLINES 8192

struct bf_ent { char name[NAME_MAX + 1]; int dir; off_t size; };

static uint32_t bf_pix[BF_WIDTH * BF_MAXROWS * BF_CH];
static struct bf_ent bf_ents[BF_MAXENT];
static int bf_nent, bf_top;                     /* the entries (".." is the first) and the first one shown */
static char bf_cwd[PATH_MAX] = "/";
static int bf_viewing;
static int bf_confirm;                    /* D was pressed once in the viewer: a second D deletes the file */
static int bf_renaming;                   /* r was pressed: the new name is being typed */
static char bf_tpath[PATH_MAX];           /* what r and D act on: the file in the viewer, or the entry under the cursor in the list */
static char bf_rname[NAME_MAX + 1];
static int bf_rlen;
static int bf_hidden = 1;                 /* names starting with a dot are left out of the list (h toggles) */
static int bf_stale;                      /* a file was made: the list must be read again */
static char bf_vpath[PATH_MAX];
static char bf_vtext[BF_VIEWMAX];
static const char *bf_vline[BF_MAXLINES];       /* each line of the file, cut at COLS characters */
static int bf_nvline, bf_vtop;
static size_t bf_vlen;                          /* bytes in bf_vtext */
static int bf_editing, bf_modified;
static size_t bf_cur;                           /* the cursor, as an offset in bf_vtext */
static int bf_etop;                             /* the first display row shown while editing */
#define BF_CLIP_FILE "/tmp/bwm.clip"            /* the text a bterm window selected (and the editor's cuts), for any window to paste */
static int bf_esc, bf_escnum, bf_escmod;        /* where we are in an ESC [ ... key sequence (state 4: after the ';', bf_escmod is xterm's modifier number) */
static size_t bf_erow[BF_MAXLINES + 1];         /* where each display row starts while editing */
static int bf_nerow;
static char bf_kill[512];                       /* the line Ctrl-K cut last, for Ctrl-U */
static size_t bf_killn;
static int bf_eprompt;                          /* 'f', 'g', 'r' or 'w' while a find, goto-line, replace or replace-with prompt reads a line, else 0 */
static char bf_ebuf[40], bf_efind[40];          /* what was typed, and what was last searched for */
static size_t bf_ebn;

static int bf_sel;                              /* the entry the keyboard cursor is on (arrows move it, Enter opens it) */
static int bf_by_size;                          /* 1: files largest first, 0: by name */

static int bf_cmp(const void *a, const void *b)
{
    const struct bf_ent *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    if (bf_by_size && x->size != y->size)
        return x->size < y->size ? 1 : -1;
    return strcmp(x->name, y->name);
}

static void bf_glyph(int r, int c, unsigned char ch, uint32_t f, uint32_t b)
{
    if (ch < 32 || ch >= 127)
        ch = ' ';
    for (int y = 0; y < BF_CH; y++) {
        unsigned bits = font8x8[ch * 8 + y / 2];
        for (int x = 0; x < BF_CW; x++, bits >>= 1)
            bf_pix[(r * BF_CH + y) * BF_WIDTH + c * BF_CW + x] = bits & 1 ? f : b;
    }
}

static void bf_put_row(int r, const char *s, size_t n, uint32_t f, uint32_t b)
{
    for (int c = 0; c < BF_COLS; c++)
        bf_glyph(r, c, (size_t)c < n ? (unsigned char)s[c] : ' ', f, b);
}

static void bf_load_dir(void)
{
    bf_nent = 0;
    bf_top = 0;
    bf_sel = 0;
    snprintf(bf_ents[bf_nent].name, sizeof bf_ents[bf_nent].name, "..");
    bf_ents[bf_nent].dir = 1;
    bf_ents[bf_nent++].size = 0;
    DIR *d = opendir(bf_cwd);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && bf_nent < BF_MAXENT) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (bf_hidden && e->d_name[0] == '.')
            continue;
        char p[PATH_MAX + NAME_MAX + 2];
        struct stat st;
        snprintf(p, sizeof p, "%s%s%s", bf_cwd, strcmp(bf_cwd, "/") == 0 ? "" : "/", e->d_name);
        snprintf(bf_ents[bf_nent].name, sizeof bf_ents[bf_nent].name, "%s", e->d_name);
        bf_ents[bf_nent].dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        bf_ents[bf_nent].size = stat(p, &st) == 0 ? st.st_size : 0;
        bf_nent++;
    }
    closedir(d);
    qsort(&bf_ents[1], bf_nent - 1, sizeof bf_ents[0], bf_cmp);
    printf("bfiles: dir %s\n", bf_cwd);
    fflush(stdout);
}

static void bf_path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s", bf_cwd, strcmp(bf_cwd, "/") == 0 ? "" : "/", name);
}

static void bf_go_up(void)
{
    char *s = strrchr(bf_cwd, '/');
    if (s == bf_cwd)
        s[1] = 0;
    else if (s)
        *s = 0;
}

static void bf_load_file(const char *path)
{
    int f = open(path, O_RDONLY);
    size_t n = 0;
    if (f >= 0) {
        ssize_t r;
        while (n < BF_VIEWMAX - 1 && (r = read(f, bf_vtext + n, BF_VIEWMAX - 1 - n)) > 0)
            n += r;
        close(f);
    }
    bf_vtext[n] = 0;
    bf_vlen = n;
    bf_nvline = 0;
    bf_vtop = 0;
    size_t i = 0;
    while (i <= n && bf_nvline < BF_MAXLINES) {
        bf_vline[bf_nvline++] = bf_vtext + i;
        size_t k = 0;
        while (i < n && bf_vtext[i] != '\n') {
            if (++k > BF_COLS) {                    /* a long line goes on in the next row */
                bf_vline[bf_nvline++] = bf_vtext + i;
                k = 1;
                if (bf_nvline >= BF_MAXLINES)
                    break;
            }
            i++;
        }
        if (i >= n)
            break;
        i++;
    }
    snprintf(bf_vpath, sizeof bf_vpath, "%s", path);
    bf_viewing = 1;
    printf("bfiles: view %s\n", path);
    fflush(stdout);
}

static void be_paint(void);

static void bf_paint(void)
{
    if (bf_editing) {
        be_paint();
        return;
    }
    uint32_t bg = 0x00101018, fg = 0x00D8D8D8, hd = 0x00305090;
    if (bf_viewing) {
        char h[BF_COLS + 1];
        if (bf_renaming)
            snprintf(h, sizeof h, "Rename to: %.40s_", bf_rname);
        else if (bf_confirm)
            snprintf(h, sizeof h, "Delete %s? D again to confirm", bf_tpath);
        else
            snprintf(h, sizeof h, "%s", bf_vpath);
        bf_put_row(0, h, strlen(h), 0x00FFFFFF, bf_confirm || bf_renaming ? 0x00A03030 : hd);
        for (int r = 1; r < BF_ROWS; r++) {
            int i = bf_vtop + r - 1;
            char t[BF_COLS + 1];
            size_t n = 0;
            if (i < bf_nvline) {
                const char *s = bf_vline[i];
                while (n < BF_COLS && *s && *s != '\n') {
                    t[n++] = (unsigned char)*s >= 32 && (unsigned char)*s < 127 ? *s : (*s == '\t' ? ' ' : '.');
                    s++;
                }
            }
            bf_put_row(r, t, n, fg, bg);
        }
        return;
    }
    char h[BF_COLS + 1];
    if (bf_renaming)
        snprintf(h, sizeof h, "Rename to: %.40s_", bf_rname);
    else if (bf_confirm)
        snprintf(h, sizeof h, "Delete %s? D again to confirm", bf_tpath);
    else
        snprintf(h, sizeof h, "%s", bf_cwd);
    bf_put_row(0, h, strlen(h), 0x00FFFFFF, bf_confirm || bf_renaming ? 0x00A03030 : hd);
    for (int r = 1; r < BF_ROWS; r++) {
        int i = bf_top + r - 1;
        if (i >= bf_nent) {
            bf_put_row(r, "", 0, fg, bg);
            continue;
        }
        char t[BF_COLS + 32];
        if (bf_ents[i].dir)
            snprintf(t, sizeof t, "%s/", bf_ents[i].name);
        else
            snprintf(t, sizeof t, "%-44.44s %10lld", bf_ents[i].name, (long long)bf_ents[i].size);
        bf_put_row(r, t, strlen(t), bf_ents[i].dir ? 0x0080C0FF : fg, bf_sel == i ? 0x00203050 : bg);
    }
}

/* The editor. Display rows are cut at COLS characters, like the viewer's. */
static void be_layout(void)
{
    size_t col = 0;
    bf_nerow = 0;
    bf_erow[bf_nerow++] = 0;
    for (size_t i = 0; i < bf_vlen && bf_nerow < BF_MAXLINES; i++) {
        if (bf_vtext[i] == '\n') {
            bf_erow[bf_nerow++] = i + 1;
            col = 0;
        } else {
            if (col == BF_COLS) {
                bf_erow[bf_nerow++] = i;
                col = 0;
            }
            col++;
        }
    }
}

static int be_row_of(size_t off)                /* the display row an offset is on */
{
    int r = 0;
    while (r + 1 < bf_nerow && bf_erow[r + 1] <= off)
        r++;
    return r;
}

static size_t be_row_len(int r)                 /* characters on a row, without its newline */
{
    size_t end = r + 1 < bf_nerow ? bf_erow[r + 1] : bf_vlen;
    size_t n = end - bf_erow[r];
    if (n > 0 && bf_vtext[end - 1] == '\n')
        n--;
    return n;
}

static int be_isword(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

/* Ctrl-Left and Ctrl-Right: to the start of the word before the cursor, to the end of the word after it. */
static void be_word_move(int dir)
{
    if (dir < 0) {
        while (bf_cur > 0 && !be_isword(bf_vtext[bf_cur - 1])) bf_cur--;
        while (bf_cur > 0 && be_isword(bf_vtext[bf_cur - 1])) bf_cur--;
    } else {
        while (bf_cur < bf_vlen && !be_isword(bf_vtext[bf_cur])) bf_cur++;
        while (bf_cur < bf_vlen && be_isword(bf_vtext[bf_cur])) bf_cur++;
    }
}

static void be_move_rows(int by)
{
    int r = be_row_of(bf_cur);
    size_t col = bf_cur - bf_erow[r];
    r += by;
    if (r < 0) r = 0;
    if (r >= bf_nerow) r = bf_nerow - 1;
    size_t n = be_row_len(r);
    bf_cur = bf_erow[r] + (col < n ? col : n);
}

/* Undo (Ctrl-Z): before an edit the text and cursor are kept, but a run of the same kind of edit (typing,
 * backspacing) with no cursor move between is kept once, so one Ctrl-Z undoes the run. The last
 * BE_UNDO_MAX are kept. */
#define BE_UNDO_MAX 16
static struct { char *text; size_t len, cur; } be_undo[BE_UNDO_MAX];
static int be_nundo, be_ulast;
/* Redo (Ctrl-Y): what Ctrl-Z took off is kept here until the next edit. */
static struct { char *text; size_t len, cur; } be_redo[BE_UNDO_MAX];
static int be_nredo;

static void be_redo_clear(void)
{
    while (be_nredo > 0)
        free(be_redo[--be_nredo].text);
}

static void be_undo_clear(void)
{
    while (be_nundo > 0)
        free(be_undo[--be_nundo].text);
    be_redo_clear();
    be_ulast = 0;
}

/* Called before an edit of KIND (1 typing, 2 backspace, 3 anything else, which is always kept). */
static void be_undo_push(int kind)
{
    be_redo_clear();
    if (kind == be_ulast && kind != 3)
        return;
    be_ulast = kind;
    char *t = malloc(bf_vlen + 1);
    if (!t)
        return;
    memcpy(t, bf_vtext, bf_vlen + 1);
    if (be_nundo == BE_UNDO_MAX) {
        free(be_undo[0].text);
        memmove(&be_undo[0], &be_undo[1], sizeof be_undo[0] * (BE_UNDO_MAX - 1));
        be_nundo--;
    }
    be_undo[be_nundo].text = t;
    be_undo[be_nundo].len = bf_vlen;
    be_undo[be_nundo].cur = bf_cur;
    be_nundo++;
}

/* Ctrl-Z: puts back the text as it was before the last edit (or run of edits). */
static void be_undo_pop(void)
{
    be_ulast = 0;
    if (be_nundo == 0) {
        puts("bfiles: nothing to undo");
        fflush(stdout);
        return;
    }
    char *t = malloc(bf_vlen + 1);
    if (t && be_nredo < BE_UNDO_MAX) {
        memcpy(t, bf_vtext, bf_vlen + 1);
        be_redo[be_nredo].text = t;
        be_redo[be_nredo].len = bf_vlen;
        be_redo[be_nredo].cur = bf_cur;
        be_nredo++;
    } else {
        free(t);
    }
    be_nundo--;
    memcpy(bf_vtext, be_undo[be_nundo].text, be_undo[be_nundo].len + 1);
    bf_vlen = be_undo[be_nundo].len;
    bf_cur = be_undo[be_nundo].cur;
    free(be_undo[be_nundo].text);
    bf_modified = 1;
    puts("bfiles: undo");
    fflush(stdout);
}

/* Ctrl-Y: puts back what the last Ctrl-Z took off. */
static void be_redo_pop(void)
{
    be_ulast = 0;
    if (be_nredo == 0) {
        puts("bfiles: nothing to redo");
        fflush(stdout);
        return;
    }
    char *t = malloc(bf_vlen + 1);
    if (t && be_nundo < BE_UNDO_MAX) {
        memcpy(t, bf_vtext, bf_vlen + 1);
        be_undo[be_nundo].text = t;
        be_undo[be_nundo].len = bf_vlen;
        be_undo[be_nundo].cur = bf_cur;
        be_nundo++;
    } else {
        free(t);
    }
    be_nredo--;
    memcpy(bf_vtext, be_redo[be_nredo].text, be_redo[be_nredo].len + 1);
    bf_vlen = be_redo[be_nredo].len;
    bf_cur = be_redo[be_nredo].cur;
    free(be_redo[be_nredo].text);
    bf_modified = 1;
    puts("bfiles: redo");
    fflush(stdout);
}

static void be_insert(unsigned char c)
{
    if (bf_vlen + 1 >= BF_VIEWMAX)
        return;
    memmove(bf_vtext + bf_cur + 1, bf_vtext + bf_cur, bf_vlen - bf_cur);
    bf_vtext[bf_cur++] = c;
    bf_vlen++;
    bf_vtext[bf_vlen] = 0;
    bf_modified = 1;
}

static void be_delete(size_t at)
{
    if (at >= bf_vlen)
        return;
    memmove(bf_vtext + at, bf_vtext + at + 1, bf_vlen - at - 1);
    bf_vlen--;
    bf_vtext[bf_vlen] = 0;
    bf_modified = 1;
}

/* Ctrl-K: cuts the rest of the line (the whole newline if the line is empty) into the kill buffer. */
static void be_cut_line(void)
{
    size_t end = bf_cur;
    while (end < bf_vlen && bf_vtext[end] != '\n')
        end++;
    if (end == bf_cur && end < bf_vlen)
        end++;
    size_t n = end - bf_cur;
    if (n == 0)
        return;
    if (n > sizeof bf_kill)
        n = sizeof bf_kill;
    memcpy(bf_kill, bf_vtext + bf_cur, n);
    bf_killn = n;
    int cf = open(BF_CLIP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (cf >= 0) {
        if (write(cf, bf_kill, n) < 0)
            puts("bfiles: cannot write the clipboard");
        close(cf);
    }
    for (size_t i = 0; i < n; i++)
        be_delete(bf_cur);
    printf("bfiles: cut %zu\n", n);
    fflush(stdout);
}

/* Ctrl-V: inserts the clipboard at the cursor (up to what fits; control characters other than newline
 * and tab are left out). */
static void be_paste_clip(void)
{
    int f = open(BF_CLIP_FILE, O_RDONLY);
    if (f < 0) {
        puts("bfiles: the clipboard is empty");
        fflush(stdout);
        return;
    }
    char buf[1024];
    ssize_t n;
    size_t put = 0;
    be_undo_push(3);
    while ((n = read(f, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)buf[i];
            if (ch == '\r')
                continue;
            if (ch == '\n' || ch == '\t' || (ch >= 32 && ch < 127)) {
                be_insert(ch);
                put++;
            }
        }
    close(f);
    printf("bfiles: pasted %zu\n", put);
    fflush(stdout);
}

/* Ctrl-U: inserts the kill buffer at the cursor. */
static void be_paste_kill(void)
{
    for (size_t i = 0; i < bf_killn; i++)
        be_insert((unsigned char)bf_kill[i]);
}

/* Ctrl-F, then Enter: moves to the next place the text typed occurs, after the cursor and wrapping
 * round; Enter on an empty prompt searches for the last text again. */
static void be_find(void)
{
    if (bf_ebn > 0)
        snprintf(bf_efind, sizeof bf_efind, "%s", bf_ebuf);
    size_t n = strlen(bf_efind);
    if (n == 0 || n > bf_vlen)
        return;
    for (size_t k = 1; k <= bf_vlen; k++) {
        size_t at = (bf_cur + k) % (bf_vlen + 1);
        if (at + n <= bf_vlen && memcmp(bf_vtext + at, bf_efind, n) == 0) {
            bf_cur = at;
            printf("bfiles: found at %zu\n", at);
            fflush(stdout);
            return;
        }
    }
    printf("bfiles: not found\n");
    fflush(stdout);
}

/* Ctrl-G, then a number and Enter: moves to the start of that line of the file. */
static void be_goto_line(void)
{
    long want = atol(bf_ebuf);
    size_t at = 0;
    for (long l = 1; l < want && at < bf_vlen; l++) {
        while (at < bf_vlen && bf_vtext[at] != '\n')
            at++;
        if (at < bf_vlen)
            at++;
    }
    bf_cur = at;
    printf("bfiles: line %ld\n", want);
    fflush(stdout);
}

/* Ctrl-R, the text to find, Enter (empty: the last text found), the text to put in its place, Enter:
 * replaces every occurrence in the file, as one undo step. */
static void be_replace_all(void)
{
    size_t n = strlen(bf_efind), m = strlen(bf_ebuf), hits = 0;
    if (n == 0)
        return;
    for (size_t i = 0; i + n <= bf_vlen; ) {
        if (memcmp(bf_vtext + i, bf_efind, n) == 0) {
            hits++;
            i += n;
        } else {
            i++;
        }
    }
    if (hits == 0) {
        printf("bfiles: not found\n");
        fflush(stdout);
        return;
    }
    if (bf_vlen - hits * n + hits * m + 1 >= BF_VIEWMAX) {
        printf("bfiles: replace would make the file too large\n");
        fflush(stdout);
        return;
    }
    char *out = malloc(BF_VIEWMAX);
    if (!out)
        return;
    be_undo_push(3);
    size_t o = 0, newcur = bf_cur;
    for (size_t i = 0; i < bf_vlen; ) {
        if (i + n <= bf_vlen && memcmp(bf_vtext + i, bf_efind, n) == 0) {
            if (bf_cur >= i + n)
                newcur = newcur - n + m;
            else if (bf_cur > i)
                newcur = o;
            memcpy(out + o, bf_ebuf, m);
            o += m;
            i += n;
        } else {
            out[o++] = bf_vtext[i++];
        }
    }
    memcpy(bf_vtext, out, o);
    free(out);
    bf_vlen = o;
    bf_vtext[o] = 0;
    bf_cur = newcur < bf_vlen ? newcur : bf_vlen;
    bf_modified = 1;
    printf("bfiles: replaced %zu\n", hits);
    fflush(stdout);
}

/* A key typed at the find, goto-line or replace prompt. */
static void be_prompt_key(int c)
{
    be_ulast = 0;
    if (c == 27) {
        bf_eprompt = 0;
        bf_esc = 1;
    } else if (c == '\r' || c == '\n') {
        int which = bf_eprompt;
        bf_eprompt = 0;
        if (which == 'f') {
            be_find();
        } else if (which == 'r') {
            if (bf_ebn > 0)
                snprintf(bf_efind, sizeof bf_efind, "%s", bf_ebuf);
            if (bf_efind[0]) {
                bf_eprompt = 'w';
                bf_ebuf[0] = 0;
                bf_ebn = 0;
            }
        } else if (which == 'w') {
            be_replace_all();
        } else {
            be_goto_line();
        }
    } else if (c == 127 || c == 8) {
        if (bf_ebn > 0)
            bf_ebuf[--bf_ebn] = 0;
    } else if (c >= 32 && c < 127 && bf_ebn + 1 < sizeof bf_ebuf) {
        bf_ebuf[bf_ebn++] = (char)c;
        bf_ebuf[bf_ebn] = 0;
    }
}

static void be_save(void)
{
    int f = open(bf_vpath, O_WRONLY | O_TRUNC);
    size_t n = 0;
    if (f >= 0) {
        ssize_t r;
        while (n < bf_vlen && (r = write(f, bf_vtext + n, bf_vlen - n)) > 0)
            n += r;
        close(f);
    }
    if (n == bf_vlen) {
        bf_modified = 0;
        printf("bfiles: saved %s\n", bf_vpath);
    } else {
        printf("bfiles: cannot save %s\n", bf_vpath);
    }
    fflush(stdout);
}

static void be_start(void)
{
    struct stat st;
    if (stat(bf_vpath, &st) != 0 || st.st_size >= BF_VIEWMAX - 1) {
        printf("bfiles: cannot edit %s\n", bf_vpath);
        fflush(stdout);
        return;
    }
    bf_editing = 1;
    bf_modified = 0;
    be_undo_clear();
    bf_eprompt = 0;
    bf_cur = 0;
    bf_etop = 0;
    bf_esc = 0;
    printf("bfiles: edit %s\n", bf_vpath);
    fflush(stdout);
}

/* n in the directory list: makes an empty file ("untitled", "untitled2",
 * ...) here and opens it in the editor. */
static void bf_new_file(void)
{
    char path[PATH_MAX];
    for (int i = 1; i < 100; i++) {
        char name[32];
        if (i == 1)
            snprintf(name, sizeof name, "untitled");
        else
            snprintf(name, sizeof name, "untitled%d", i);
        bf_path_of(name, path, sizeof path);
        int f = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (f < 0)
            continue;
        close(f);
        printf("bfiles: new %s\n", path);
        fflush(stdout);
        bf_load_file(path);
        be_start();
        bf_stale = 1;
        return;
    }
}

/* N in the directory list: makes an empty directory ("newdir", "newdir2",
 * ...) here and shows it in the list. */
static void bf_new_dir(void)
{
    char path[PATH_MAX];
    for (int i = 1; i < 100; i++) {
        char name[32];
        if (i == 1)
            snprintf(name, sizeof name, "newdir");
        else
            snprintf(name, sizeof name, "newdir%d", i);
        bf_path_of(name, path, sizeof path);
        if (mkdir(path, 0755) != 0)
            continue;
        printf("bfiles: mkdir %s\n", path);
        fflush(stdout);
        bf_load_dir();
        return;
    }
}

/* Sets bf_tpath to what r and D act on: the file shown in the viewer, or in the list the entry under the
 * cursor (never ".."). Returns whether there is one. */
static int bf_target(void)
{
    if (bf_viewing) {
        snprintf(bf_tpath, sizeof bf_tpath, "%s", bf_vpath);
        return 1;
    }
    if (bf_sel <= 0 || bf_sel >= bf_nent)
        return 0;
    bf_path_of(bf_ents[bf_sel].name, bf_tpath, sizeof bf_tpath);
    return 1;
}

/* r in the viewer or the list: type a new name (in the same directory), Enter renames,
 * Escape cancels (a name that is taken is refused). */
static void bf_rename_key(int c)
{
    if (c == 27) {
        bf_renaming = 0;
    } else if (c == '\r' || c == '\n') {
        char *sl = strrchr(bf_tpath, '/');
        char to[PATH_MAX];
        snprintf(to, sizeof to, "%.*s%s", (int)(sl - bf_tpath + 1), bf_tpath, bf_rname);
        bf_renaming = 0;
        struct stat st;
        if (bf_rlen > 0 && strchr(bf_rname, '/') == NULL && lstat(to, &st) != 0 && rename(bf_tpath, to) == 0) {
            printf("bfiles: renamed %s to %s\n", bf_tpath, to);
            if (bf_viewing) {
                snprintf(bf_vpath, sizeof bf_vpath, "%s", to);
                bf_stale = 1;
            } else {
                bf_load_dir();
            }
        } else {
            printf("bfiles: cannot rename %s\n", bf_tpath);
        }
        fflush(stdout);
    } else if (c == 127 || c == 8) {
        if (bf_rlen > 0)
            bf_rname[--bf_rlen] = 0;
    } else if (c >= 32 && c < 127 && bf_rlen < NAME_MAX) {
        bf_rname[bf_rlen++] = (char)c;
        bf_rname[bf_rlen] = 0;
    }
}

/* Copies SRC into the directory DIR under its own name, or "NAME.copy" ("NAME.copy2", ...) when
 * that is taken. Says "bfiles: copied SRC to DST"; returns 0 if it did. */
static int bf_copy_into(const char *src, const char *dir)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    const char *sep = strcmp(dir, "/") == 0 ? "" : "/";
    char to[PATH_MAX];
    int out = -1;
    for (int i = 0; i < 100 && out < 0; i++) {
        if (i == 0)
            snprintf(to, sizeof to, "%s%s%s", dir, sep, base);
        else if (i == 1)
            snprintf(to, sizeof to, "%s%s%s.copy", dir, sep, base);
        else
            snprintf(to, sizeof to, "%s%s%s.copy%d", dir, sep, base, i);
        out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0644);
    }
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[4096];
    ssize_t n;
    int bad = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) {
            bad = 1;
            break;
        }
    close(in);
    close(out);
    if (bad || n < 0) {
        unlink(to);
        printf("bfiles: cannot copy %s\n", src);
        fflush(stdout);
        return -1;
    }
    printf("bfiles: copied %s to %s\n", src, to);
    fflush(stdout);
    bf_stale = 1;
    return 0;
}

/* Copies the file SRC to the new file DST (mode kept). Returns 0 if it did. */
static int bf_copy_data(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode & 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[4096];
    ssize_t n;
    int bad = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) {
            bad = 1;
            break;
        }
    close(in);
    close(out);
    if (bad || n < 0) {
        unlink(dst);
        return -1;
    }
    return 0;
}

#define BF_TREE_DEPTH 8

/* Copies the directory SRC and everything under it to the new directory DST: regular files and
 * directories only (anything else is skipped), at most BF_TREE_DEPTH levels. Returns 0 if all of it
 * was copied. */
static int bf_copy_tree(const char *src, const char *dst, int depth)
{
    struct stat st;
    if (depth > BF_TREE_DEPTH || stat(src, &st) != 0 || mkdir(dst, st.st_mode & 0777) != 0)
        return -1;
    DIR *d = opendir(src);
    if (!d)
        return -1;
    int bad = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", src, e->d_name) >= (int)sizeof a
            || snprintf(b, sizeof b, "%s/%s", dst, e->d_name) >= (int)sizeof b
            || lstat(a, &st) != 0) {
            bad = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode))
            bad |= bf_copy_tree(a, b, depth + 1);
        else if (S_ISREG(st.st_mode))
            bad |= bf_copy_data(a, b, st.st_mode);
    }
    closedir(d);
    return bad ? -1 : 0;
}

/* Copies the directory SRC into the directory DIR under its own name, or "NAME.copy" ("NAME.copy2",
 * ...) when that is taken; refuses to copy a directory into itself. Says "bfiles: copied SRC to DST". */
static int bf_copy_dir_into(const char *src, const char *dir)
{
    size_t n = strlen(src);
    if (strncmp(dir, src, n) == 0 && (dir[n] == 0 || dir[n] == '/')) {
        printf("bfiles: cannot copy %s into itself\n", src);
        fflush(stdout);
        return -1;
    }
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    const char *sep = strcmp(dir, "/") == 0 ? "" : "/";
    char to[PATH_MAX];
    for (int i = 0; i < 100; i++) {
        struct stat st;
        if (i == 0)
            snprintf(to, sizeof to, "%s%s%s", dir, sep, base);
        else if (i == 1)
            snprintf(to, sizeof to, "%s%s%s.copy", dir, sep, base);
        else
            snprintf(to, sizeof to, "%s%s%s.copy%d", dir, sep, base, i);
        if (lstat(to, &st) != 0)
            break;
        if (i == 99) {
            printf("bfiles: cannot copy %s\n", src);
            fflush(stdout);
            return -1;
        }
    }
    if (bf_copy_tree(src, to, 0) != 0) {
        printf("bfiles: cannot copy %s\n", src);
        fflush(stdout);
        return -1;
    }
    printf("bfiles: copied %s to %s\n", src, to);
    fflush(stdout);
    bf_stale = 1;
    return 0;
}

/* c in the viewer: copies the file shown to "NAME.copy" ("NAME.copy2", ...)
 * next to it. */
static void bf_copy_file(void)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", bf_vpath);
    char *sl = strrchr(dir, '/');
    if (sl == NULL)
        return;
    if (sl == dir)
        sl[1] = 0;
    else
        *sl = 0;
    bf_copy_into(bf_vpath, dir);
}

/* c or x in the list: marks the entry under the cursor to be copied (c) or moved (x) by the next v. */
static char bf_clip[PATH_MAX];
static int bf_clip_move;

static void bf_mark(int move)
{
    if (bf_sel <= 0 || bf_sel >= bf_nent)
        return;
    bf_path_of(bf_ents[bf_sel].name, bf_clip, sizeof bf_clip);
    bf_clip_move = move;
    printf("bfiles: %s %s\n", move ? "move" : "copy", bf_clip);
    fflush(stdout);
}

/* v in the list: copies the marked file here, or moves the marked file or directory here (never
 * over a name that is taken). */
static void bf_paste(void)
{
    if (bf_clip[0] == 0)
        return;
    if (!bf_clip_move) {
        struct stat st;
        if (stat(bf_clip, &st) != 0) {
            printf("bfiles: cannot copy %s\n", bf_clip);
            fflush(stdout);
            return;
        }
        if ((S_ISDIR(st.st_mode) ? bf_copy_dir_into(bf_clip, bf_cwd) : bf_copy_into(bf_clip, bf_cwd)) == 0)
            bf_load_dir();
        return;
    }
    const char *base = strrchr(bf_clip, '/');
    char to[PATH_MAX];
    bf_path_of(base ? base + 1 : bf_clip, to, sizeof to);
    struct stat st;
    if (lstat(to, &st) == 0 || rename(bf_clip, to) != 0) {
        printf("bfiles: cannot move %s\n", bf_clip);
    } else {
        printf("bfiles: moved %s to %s\n", bf_clip, to);
        bf_clip[0] = 0;
        bf_load_dir();
    }
    fflush(stdout);
}

/* t in the directory list: opens a terminal window in the directory shown.
 * The terminal is a grandchild, so nothing is left to be waited for. */
/* Starts /bin/bterm in the directory shown under the name NAME with ARG, from a double
 * fork so that it needs no wait; says "bfiles: MSG". */
static void bf_spawn(const char *name, const char *arg, const char *msg)
{
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            char *argv[] = { (char *)name, (char *)arg, NULL };
            for (int fd = 3; fd < 64; fd++)
                close(fd);
            if (chdir(bf_cwd) == 0)
                execv("/bin/bterm", argv);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) {
        int st;
        waitpid(p, &st, 0);
        printf("bfiles: %s %s\n", msg, arg ? arg : bf_cwd);
        fflush(stdout);
    }
}

static void be_key(int c)
{
    int page = BF_ROWS - 2;
    if (bf_eprompt) {
        be_prompt_key(c);
        return;
    }
    if (bf_esc == 1) {
        bf_esc = c == '[' ? 2 : 0;
        if (bf_esc)
            return;
    } else if (bf_esc >= 2 && bf_esc <= 4) {
        if (c == ';') {
            if (bf_esc == 2) bf_escnum = 0;
            bf_esc = 4;
            bf_escmod = 0;
            return;
        }
        if (c >= '0' && c <= '9') {
            if (bf_esc == 4) {
                bf_escmod = bf_escmod * 10 + c - '0';
            } else {
                bf_escnum = bf_esc == 2 ? c - '0' : bf_escnum * 10 + c - '0';
                bf_esc = 3;
            }
            return;
        }
        int num = bf_esc >= 3 ? bf_escnum : 0;
        int ctrl = bf_esc == 4 && bf_escmod >= 5 && ((bf_escmod - 1) & 4);
        bf_esc = 0;
        be_ulast = 0;
        if (c == '~' && num == 3) be_undo_push(3);
        if (c == 'A') be_move_rows(-1);
        else if (c == 'B') be_move_rows(1);
        else if (c == 'C') { if (ctrl) be_word_move(1); else if (bf_cur < bf_vlen) bf_cur++; }
        else if (c == 'D') { if (ctrl) be_word_move(-1); else if (bf_cur > 0) bf_cur--; }
        else if (c == 'H' || (c == '~' && (num == 1 || num == 7))) bf_cur = bf_erow[be_row_of(bf_cur)];
        else if (c == 'F' || (c == '~' && (num == 4 || num == 8))) {
            int r = be_row_of(bf_cur);
            bf_cur = bf_erow[r] + be_row_len(r);
        } else if (c == '~' && num == 3) be_delete(bf_cur);
        else if (c == '~' && num == 5) be_move_rows(-page);
        else if (c == '~' && num == 6) be_move_rows(page);
        return;
    }
    if (c == 27) {
        bf_esc = 1;
    } else if (c == 19) {
        be_save();
    } else if (c == 24) {
        bf_editing = 0;
        be_undo_clear();
        bf_load_file(bf_vpath);
    } else if (c == 6 || c == 7) {
        bf_eprompt = c == 6 ? 'f' : 'g';
        bf_ebuf[0] = 0;
        bf_ebn = 0;
    } else if (c == 18) {
        bf_eprompt = 'r';
        bf_ebuf[0] = 0;
        bf_ebn = 0;
    } else if (c == 25) {
        be_redo_pop();
    } else if (c == 26) {
        be_undo_pop();
    } else if (c == 1) {
        bf_cur = bf_erow[be_row_of(bf_cur)];
    } else if (c == 5) {
        int r = be_row_of(bf_cur);
        bf_cur = bf_erow[r] + be_row_len(r);
    } else if (c == 22) {
        be_paste_clip();
    } else if (c == 11) {
        be_undo_push(3);
        be_cut_line();
    } else if (c == 21) {
        be_undo_push(3);
        be_paste_kill();
    } else if (c == 127 || c == 8) {
        if (bf_cur > 0) {
            be_undo_push(2);
            be_delete(--bf_cur);
        }
    } else if (c == '\n' || c == '\r') {
        be_undo_push(3);
        /* The new line starts with the blanks the one being left starts with (up to the cursor). */
        size_t ls = bf_cur, ind = 0;
        while (ls > 0 && bf_vtext[ls - 1] != '\n')
            ls--;
        while (ls + ind < bf_cur && (bf_vtext[ls + ind] == ' ' || bf_vtext[ls + ind] == '\t'))
            ind++;
        char blanks[BF_COLS];
        if (ind > sizeof blanks)
            ind = sizeof blanks;
        memcpy(blanks, bf_vtext + ls, ind);
        be_insert('\n');
        for (size_t i = 0; i < ind; i++)
            be_insert((unsigned char)blanks[i]);
    } else if (c == '\t') {
        be_undo_push(3);
        for (int i = 0; i < 4; i++)
            be_insert(' ');
    } else if (c >= 32 && c < 127) {
        be_undo_push(1);
        be_insert(c);
    }
}

static void be_click(int row, int x)
{
    int r = bf_etop + row - 1;
    if (row < 1)
        return;
    be_ulast = 0;
    if (r >= bf_nerow) r = bf_nerow - 1;
    size_t col = x / BF_CW, n = be_row_len(r);
    bf_cur = bf_erow[r] + (col < n ? col : n);
}

static void be_paint(void)
{
    uint32_t bg = 0x00101018, fg = 0x00D8D8D8, hd = 0x00305090;
    be_layout();
    if (bf_cur > bf_vlen)
        bf_cur = bf_vlen;
    int cr = be_row_of(bf_cur);
    if (cr < bf_etop) bf_etop = cr;
    if (cr > bf_etop + BF_ROWS - 2) bf_etop = cr - (BF_ROWS - 2);
    char h[BF_COLS + 80];
    if (bf_eprompt)
        snprintf(h, sizeof h, "%s: %.40s_", bf_eprompt == 'f' ? "Find" : bf_eprompt == 'r' ? "Replace" :
                 bf_eprompt == 'w' ? "With" : "Go to line", bf_ebuf);
    else
        snprintf(h, sizeof h, "%.12s%s ^S save ^X done ^F find ^G line ^K ^U %d", bf_vpath, bf_modified ? " *" : "", cr + 1);
    bf_put_row(0, h, strlen(h), 0x00FFFFFF, bf_eprompt ? 0x00A03030 : hd);
    for (int r = 1; r < BF_ROWS; r++) {
        int i = bf_etop + r - 1;
        char t[BF_COLS + 1];
        size_t n = 0;
        if (i < bf_nerow) {
            n = be_row_len(i);
            for (size_t k = 0; k < n; k++) {
                unsigned char ch = bf_vtext[bf_erow[i] + k];
                t[k] = ch >= 32 && ch < 127 ? ch : '.';
            }
        }
        bf_put_row(r, t, n, fg, bg);
    }
    int row = cr - bf_etop + 1, col = (int)(bf_cur - bf_erow[cr]);
    if (col >= BF_COLS) col = BF_COLS - 1;
    unsigned char under = ' ';
    if (bf_cur < bf_vlen && bf_vtext[bf_cur] != '\n')
        under = (unsigned char)bf_vtext[bf_cur];
    bf_glyph(row, col, under, bg, fg);                /* the cursor: the cell with its colours swapped */
}

static void bf_scroll(int by)
{
    if (bf_viewing) {
        bf_vtop += by;
        if (bf_vtop > bf_nvline - 1) bf_vtop = bf_nvline - 1;
        if (bf_vtop < 0) bf_vtop = 0;
    } else {
        bf_top += by;
        if (bf_top > bf_nent - 1) bf_top = bf_nent - 1;
        if (bf_top < 0) bf_top = 0;
    }
}

/* Goes into the directory, or shows the file, that entry I of the list is. */
static void bf_open(int i)
{
    if (i < 0 || i >= bf_nent)
        return;
    if (i == 0) {
        bf_go_up();
        bf_load_dir();
    } else if (bf_ents[i].dir) {
        char p[PATH_MAX];
        bf_path_of(bf_ents[i].name, p, sizeof p);
        snprintf(bf_cwd, sizeof bf_cwd, "%s", p);
        bf_load_dir();
    } else {
        char p[PATH_MAX];
        bf_path_of(bf_ents[i].name, p, sizeof p);
        size_t n = strlen(bf_ents[i].name);
        if (n > 4 && (strcasecmp(bf_ents[i].name + n - 4, ".ppm") == 0 ||
                      strcasecmp(bf_ents[i].name + n - 4, ".bmp") == 0))
            bf_spawn("bview", p, "image");
        else
            bf_load_file(p);
    }
}

static void bf_click(int row)
{
    if (bf_viewing) {
        bf_viewing = 0;
        if (bf_stale) {
            bf_stale = 0;
            bf_load_dir();
        }
        return;
    }
    if (row < 1)
        return;
    bf_sel = bf_top + row - 1;
    bf_open(bf_sel);
}

/* Moves the keyboard cursor by BY entries and scrolls it into view. */
static void bf_move_sel(int by)
{
    bf_sel += by;
    if (bf_sel > bf_nent - 1) bf_sel = bf_nent - 1;
    if (bf_sel < 0) bf_sel = 0;
    if (bf_sel < bf_top) bf_top = bf_sel;
    if (bf_sel > bf_top + BF_ROWS - 2) bf_top = bf_sel - (BF_ROWS - 2);
    printf("bfiles: cursor %d\n", bf_sel);
    fflush(stdout);
}

static int bfiles_main(int argc, char **argv)
{
    char first[PATH_MAX] = "";                         /* a file given: show it (e edits it) */
    if (argc > 1 && argv[1][0] == '/') {
        struct stat st;
        snprintf(bf_cwd, sizeof bf_cwd, "%s", argv[1]);
        if (stat(argv[1], &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(first, sizeof first, "%s", argv[1]);
            bf_go_up();
        }
    }
    unsigned sw, sh;
    bwm_screen(&sw, &sh);                              /* fewer rows on a short screen */
    if (sh >= 80 + 4 * BF_CH && bf_rows > (int)(sh - 80) / BF_CH)
        bf_rows = (sh - 80) / BF_CH;
    int fd = bwm_connect();
    if (fd < 0) {
        puts("bfiles: no window server");
        return 2;
    }
    int id = bwm_create(fd, BF_WIDTH, BF_HEIGHT, "Files");
    if (id < 0) {
        puts("bfiles: cannot open a window");
        return 1;
    }
    bf_load_dir();
    if (first[0])
        bf_load_file(first);
    bf_paint();
    if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
        return 1;
    printf("bfiles: window %d\n", id);
    fflush(stdout);
    static unsigned char buf[BWM_MAX_PAYLOAD];
    unsigned prev = 0;
    for (;;) {
        int t = bwm_event(fd, buf);
        if (t < 0 || t == BWM_CLOSE)
            break;
        if (t == BWM_KEY) {
            int c = ((struct bwm_key *)buf)->ch;
            if (bf_renaming) {
                bf_rename_key(c);
                bf_paint();
                if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
                    break;
                continue;
            }
            if (!bf_editing && !bf_viewing) {          /* arrow keys and Enter in the list */
                if (bf_esc == 0 && c == 27) {
                    bf_esc = 1;
                    continue;
                }
                if (bf_esc == 1) {
                    bf_esc = c == '[' ? 2 : 0;
                    continue;
                }
                if (bf_esc >= 2 && bf_esc <= 4) {
                    if (c == ';') {
                        if (bf_esc == 2) bf_escnum = 0;
                        bf_esc = 4;
                        continue;
                    }
                    if (c >= '0' && c <= '9') {
                        if (bf_esc != 4) {
                            bf_escnum = bf_esc == 2 ? c - '0' : bf_escnum * 10 + c - '0';
                            bf_esc = 3;
                        }
                        continue;
                    }
                    int num = bf_esc >= 3 ? bf_escnum : 0;
                    bf_esc = 0;
                    int page = BF_ROWS - 2;
                    if (c == 'A') bf_move_sel(-1);
                    else if (c == 'B') bf_move_sel(1);
                    else if (c == 'H' || (c == '~' && (num == 1 || num == 7))) bf_move_sel(-bf_nent);
                    else if (c == 'F' || (c == '~' && (num == 4 || num == 8))) bf_move_sel(bf_nent);
                    else if (c == '~' && num == 5) bf_move_sel(-page);
                    else if (c == '~' && num == 6) bf_move_sel(page);
                    else
                        continue;
                    bf_paint();
                    if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
                        break;
                    continue;
                }
                if (c == '\r' || c == '\n') {
                    bf_open(bf_sel);
                    bf_paint();
                    if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
                        break;
                    continue;
                }
            }
            int was_confirm = bf_confirm;
            bf_confirm = 0;
            char was_tpath[PATH_MAX];
            snprintf(was_tpath, sizeof was_tpath, "%s", bf_tpath);
            if (c == 'D' && !bf_editing && bf_target()) {
                if (was_confirm && strcmp(was_tpath, bf_tpath) == 0) {   /* the same entry as the first D */
                    struct stat st;
                    int gone = lstat(bf_tpath, &st) == 0 && S_ISDIR(st.st_mode) ? rmdir(bf_tpath) : unlink(bf_tpath);
                    if (gone == 0) {
                        printf("bfiles: deleted %s\n", bf_tpath);
                        bf_viewing = 0;
                        bf_load_dir();
                    } else {
                        printf("bfiles: cannot delete %s\n", bf_tpath);
                    }
                    fflush(stdout);
                } else {
                    bf_confirm = 1;
                }
            } else if (bf_editing)
                be_key(c);
            else if (c == 'r' && bf_target()) {
                const char *b = strrchr(bf_tpath, '/');
                snprintf(bf_rname, sizeof bf_rname, "%s", b ? b + 1 : bf_tpath);
                bf_rlen = strlen(bf_rname);
                bf_renaming = 1;
            } else if (c == 'c' && bf_viewing)
                bf_copy_file();
            else if (c == 'c' && !bf_viewing)
                bf_mark(0);
            else if (c == 'x' && !bf_viewing)
                bf_mark(1);
            else if (c == 'v' && !bf_viewing)
                bf_paste();
            else if (c == 'i' && bf_viewing)
                bf_spawn("bview", bf_vpath, "image");
            else if (c == 'e' && bf_viewing)
                be_start();
            else if (c == 'n' && !bf_viewing)
                bf_new_file();
            else if (c == 'R' && !bf_viewing) {
                bf_load_dir();
                printf("bfiles: reloaded %d entries\n", bf_nent);
                fflush(stdout);
            } else if (c == 's' && !bf_viewing) {
                bf_by_size = !bf_by_size;
                printf("bfiles: sorted by %s\n", bf_by_size ? "size" : "name");
                fflush(stdout);
                bf_load_dir();
            } else if (c == 'h' && !bf_viewing) {
                bf_hidden = !bf_hidden;
                printf("bfiles: dotfiles %s\n", bf_hidden ? "hidden" : "shown");
                fflush(stdout);
                bf_load_dir();
            } else if (c == 't' && !bf_viewing) {
                bf_spawn("bterm", NULL, "terminal in");
            } else if (c == 'N' && !bf_viewing)
                bf_new_dir();
            else if (c == 'g' || c == 'G') {
                bf_scroll(c == 'g' ? -1000000 : 1000000);
                printf("bfiles: top %d\n", bf_viewing ? bf_vtop : bf_top);
                fflush(stdout);
            } else if (c == 'j')
                bf_scroll(1);
            else if (c == 'k')
                bf_scroll(-1);
            else if (c == ' ')
                bf_scroll(BF_ROWS - 2);
            else if (c == 'b')
                bf_scroll(-(BF_ROWS - 2));
            else if (c == 127 || c == 8) {
                if (bf_viewing) {
                    bf_viewing = 0;
                    if (bf_stale) {
                        bf_stale = 0;
                        bf_load_dir();
                    }
                } else {
                    bf_go_up();
                    bf_load_dir();
                }
            } else
                continue;
        } else if (t == BWM_MOUSE) {
            struct bwm_mouse *m = (struct bwm_mouse *)buf;
            int wheel = (signed char)((m->buttons >> 8) & 0xFF);       /* negative is up */
            if (wheel) {
                if (bf_editing) {
                    be_move_rows(wheel * 3);
                } else {
                    bf_scroll(wheel * 3);
                    printf("bfiles: top %d\n", bf_viewing ? bf_vtop : bf_top);
                    fflush(stdout);
                }
                bf_paint();
                if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
                    break;
                continue;
            }
            int pressed = (m->buttons & 1) && !(prev & 1);
            prev = m->buttons & 0xFF;
            if (!pressed)
                continue;
            if (bf_editing)
                be_click(m->y / BF_CH, m->x);
            else
                bf_click(m->y / BF_CH);
        } else {
            continue;
        }
        bf_paint();
        if (bwm_draw(fd, id, bf_pix, BF_WIDTH, 0, 0, BF_WIDTH, BF_HEIGHT) != 0)
            break;
    }
    return 0;
}

#endif
