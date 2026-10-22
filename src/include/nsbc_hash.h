#ifndef NSBC_HASH_H
#define NSBC_HASH_H

/* 哈希函数（base 层）：
 * - fnv1a：通用字符串/长度敏感哈希，事务/对话分片使用；
 * - 分片键拼接必须带长度（零拷贝令牌 {p,len}，禁止 strlen 猜测）；
 * - 进程启动生成随机种子（nsbc_hash_seed），防哈希碰撞攻击（外部不可预测）。
 */

#include <stdint.h>
#include <stddef.h>

uint64_t nsbc_hash_seed(void);

static inline uint64_t nsbc_hash_bytes(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ULL ^ nsbc_hash_seed();
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static inline uint64_t nsbc_hash_str(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL ^ nsbc_hash_seed();
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

/* 两令牌拼接哈希（如 Call-ID+from-tag），显式长度分隔防混淆 */
static inline uint64_t nsbc_hash2(const void *a, size_t alen, const void *b,
                                  size_t blen)
{
    uint64_t h = 0xcbf29ce484222325ULL ^ nsbc_hash_seed();
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < alen; ++i) { h ^= pa[i]; h *= 0x100000001b3ULL; }
    h ^= (uint8_t)alen;
    h *= 0x100000001b3ULL;
    for (size_t i = 0; i < blen; ++i) { h ^= pb[i]; h *= 0x100000001b3ULL; }
    h ^= (uint8_t)blen;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static inline uint32_t nsbc_hash_shard(uint64_t h, uint32_t shards)
{
    /* shards 必须为 2 的幂（建表保证）；用高 xor 低提高分布 */
    return (uint32_t)((h ^ (h >> 32)) & ((uint64_t)shards - 1));
}

#endif /* NSBC_HASH_H */
