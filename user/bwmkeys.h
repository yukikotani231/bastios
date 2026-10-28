/* The keyboard layouts bwm can type with, and the file that says which one is in use: the first line of
 * /etc/keymap, "us", "jp106" or "dvorak" (anything else is "us"). bwm reads it again every few seconds, and
 * the System window (bsys) writes it. The keys are evdev codes; only the characters differ between layouts. */
#ifndef BWMKEYS_H
#define BWMKEYS_H

#include <stdio.h>
#include <string.h>
#include <time.h>

#define KB_FILE "/etc/keymap"
#define KB_CODES 58                       /* the codes up to the space bar have a character */
#define KB_N 3
static const char *const kb_names[KB_N] = { "us", "jp106", "dvorak" };

static char kb_lo[KB_N][KB_CODES + 1], kb_hi[KB_N][KB_CODES + 1];
static int kb_ready, kb_cur;

static void kb_build(void)
{
    static const char us_lo[] = "\0\033" "1234567890-=\177\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char us_hi[] = "\0\033" "!@#$%^&*()_+\177\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    for (int i = 0; i < KB_N; i++) {
        memcpy(kb_lo[i], us_lo, KB_CODES);
        memcpy(kb_hi[i], us_hi, KB_CODES);
    }
    /* JIS (jp106): the symbols move; the Hankaku/Zenkaku key (41) makes nothing. */
    kb_hi[1][3] = '"'; kb_hi[1][7] = '&'; kb_hi[1][8] = '\''; kb_hi[1][9] = '('; kb_hi[1][10] = ')'; kb_hi[1][11] = 0;
    kb_hi[1][12] = '='; kb_lo[1][13] = '^'; kb_hi[1][13] = '~';
    kb_lo[1][26] = '@'; kb_hi[1][26] = '`'; kb_lo[1][27] = '['; kb_hi[1][27] = '{';
    kb_hi[1][39] = '+'; kb_lo[1][40] = ':'; kb_hi[1][40] = '*';
    kb_lo[1][41] = 0; kb_hi[1][41] = 0; kb_lo[1][43] = ']'; kb_hi[1][43] = '}';
    /* Dvorak: the letters and punctuation of the three letter rows. */
    memcpy(kb_lo[2] + 16, "',.pyfgcrl/=", 12);
    memcpy(kb_hi[2] + 16, "\"<>PYFGCRL?+", 12);
    memcpy(kb_lo[2] + 30, "aoeuidhtns-", 11);
    memcpy(kb_hi[2] + 30, "AOEUIDHTNS_", 11);
    memcpy(kb_lo[2] + 44, ";qjkxbmwvz", 10);
    memcpy(kb_hi[2] + 44, ":QJKXBMWVZ", 10);
    kb_lo[2][12] = '['; kb_hi[2][12] = '{'; kb_lo[2][13] = ']'; kb_hi[2][13] = '}';
    kb_ready = 1;
}

/* The layout the file names, by index in kb_names. */
static int kb_read(void)
{
    char line[32] = "";
    FILE *f = fopen(KB_FILE, "r");
    if (f) {
        if (fgets(line, sizeof line, f))
            line[strcspn(line, "\r\n")] = 0;
        fclose(f);
    }
    for (int i = 0; i < KB_N; i++)
        if (strcmp(line, kb_names[i]) == 0)
            return i;
    return 0;
}

/* Reads the file again at most every 3 s (bsys changes it). */
static void kb_poll(void)
{
    static time_t at;
    time_t now = time(NULL);
    if (!kb_ready)
        kb_build();
    if (!at || now - at >= 3 || now < at) {
        int c = kb_read();
        if (c != kb_cur)
            printf("bwm: keyboard %s\n", kb_names[c]);
        kb_cur = c;
        at = now;
        fflush(stdout);
    }
}

/* The character of evdev key CODE in the layout in use (0 for none). */
static char kb_char(unsigned code, int shift)
{
    if (!kb_ready)
        kb_build();
    if (kb_cur == 1 && (code == 89 || code == 124))           /* the Ro key (backslash, underscore) and the Yen key */
        return code == 89 ? (shift ? '_' : '\\') : (shift ? '|' : '\\');
    if (code >= KB_CODES)
        return 0;
    return shift ? kb_hi[kb_cur][code] : kb_lo[kb_cur][code];
}

#endif
