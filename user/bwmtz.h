#ifndef BWMTZ_H
#define BWMTZ_H
/* The time zone of the desktop: bwm's clock and bsys read it the same way. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minutes east of UTC for the clock: the TZ variable, or the first line of /etc/TZ, in the POSIX form NAME[+-]H[:MM]
 * ("JST-9" is nine hours east; "EST5" five west). No zone database: a name or no variable means UTC. */
static int tz_minutes(void)
{
    char buf[64];
    const char *tz = getenv("TZ");
    if (!tz || !*tz) {
        FILE *f = fopen("/etc/TZ", "r");
        if (!f)
            return 0;
        char *got = fgets(buf, sizeof buf, f);
        fclose(f);
        if (!got)
            return 0;
        buf[strcspn(buf, "\r\n")] = 0;
        tz = buf;
    }
    if (*tz == '<') {
        while (*tz && *tz != '>')
            tz++;
        if (*tz)
            tz++;
    } else {
        while ((*tz >= 'A' && *tz <= 'Z') || (*tz >= 'a' && *tz <= 'z'))
            tz++;
    }
    int sign = 1;
    if (*tz == '-' || *tz == '+')
        sign = *tz++ == '-' ? -1 : 1;
    int h = 0, m = 0;
    while (*tz >= '0' && *tz <= '9' && h < 100)
        h = h * 10 + *tz++ - '0';
    if (*tz == ':') {
        tz++;
        while (*tz >= '0' && *tz <= '9' && m < 100)
            m = m * 10 + *tz++ - '0';
    }
    int east = -sign * (h * 60 + m);
    return east > 14 * 60 || east < -12 * 60 ? 0 : east;
}

#endif
