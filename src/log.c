#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void log_write(const char *level, const char *fmt, ...)
{
    int saved_errno = errno;  /* callers may still need errno afterwards */

    char ts[32] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm) != NULL)
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "%s [%s] ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    errno = saved_errno;
}
