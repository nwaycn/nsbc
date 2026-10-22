#include "nsbc_common.h"

#include <time.h>

/* REQ-ARCH-004：CLOCK_MONOTONIC，不受 NTP 跳变影响 */
uint64_t nsbc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
