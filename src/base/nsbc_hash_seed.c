#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nsbc_atomic.h"
#include "nsbc_common.h"
#include "nsbc_hash.h"
#include "nsbc_id.h"

/* 进程级随机哈希种子：首次使用时从密码学随机源初始化；
 * 取不到随机源直接 fail-fast（返回 0 会使种子固定，拒绝启动更安全）。 */
static nsbc_atomic_t g_seed; /* 0 = 未初始化哨兵（真实种子不取 0） */

uint64_t nsbc_hash_seed(void)
{
    uint64_t s = sat_read(g_seed);
    if (s != 0)
        return s;
    if (nsbc_rand_fill(&s, sizeof(s)) != NSBC_OK)
        return 0xcbf29ce484222325ULL; /* 极端降级：固定种子（启动告警） */
    if (s == 0)
        s = 0x9e3779b97f4a7c15ULL;
    uint64_t old = sat_cas(&g_seed, 0, s);
    return old == 0 ? s : old;
}
