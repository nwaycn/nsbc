#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "realtime.h"
#include "nsbc_common.h"

#include <stdio.h>
#include <time.h>

uint64_t nsbc_wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

uint64_t nsbc_wall_ms(void) { return nsbc_wall_us() / 1000ULL; }

int nsbc_wall_iso8601(char *out, size_t outsz)
{
    struct timespec ts;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    long off = tmv.tm_gmtoff;
    char sign = off >= 0 ? '+' : '-';
    long aoff = off >= 0 ? off : -off;
    int n = snprintf(out, outsz,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03ld%c%02ld:%02ld",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                     ts.tv_nsec / 1000000L, sign, aoff / 3600,
                     (aoff % 3600) / 60);
    return (n > 0 && (size_t)n < outsz) ? NSBC_OK : NSBC_NOMEM;
}
