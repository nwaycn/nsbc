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
    clock_gettime(CLOCK_REALTIME, &ts);
    /* P3-17：localtime_r + 日期格式化的结果在同一秒内不变，按线程缓存，
     * 每次日志只重算毫秒与时区偏移，输出格式与精度完全不变。 */
    static _Thread_local time_t c_sec;
    static _Thread_local char   c_prefix[24];  /* "YYYY-MM-DDTHH:MM:SS" */
    static _Thread_local long   c_off;
    static _Thread_local char   c_sign;
    if (ts.tv_sec != c_sec) {
        struct tm tmv;
        localtime_r(&ts.tv_sec, &tmv);
        c_off = tmv.tm_gmtoff;
        c_sign = c_off >= 0 ? '+' : '-';
        snprintf(c_prefix, sizeof(c_prefix), "%04d-%02d-%02dT%02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        c_sec = ts.tv_sec;
    }
    long aoff = c_off >= 0 ? c_off : -c_off;
    int n = snprintf(out, outsz, "%s.%03ld%c%02ld:%02ld", c_prefix,
                     ts.tv_nsec / 1000000L, c_sign, aoff / 3600,
                     (aoff % 3600) / 60);
    return (n > 0 && (size_t)n < outsz) ? NSBC_OK : NSBC_NOMEM;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
