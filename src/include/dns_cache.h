#ifndef NSBC_DNS_CACHE_H
#define NSBC_DNS_CACHE_H

/* DNS 缓存（net 层）
 *
 * 只在配置/中继建连等低频路径解析主机名（getaddrinfo 同步），
 * 结果带 TTL 缓存；SIP/RTP 关键路径永远使用已解析的 nsbc_sa_t，
 * 严禁在转发路径做名字解析。
 */

#include <stdint.h>
#include "net_sock.h"

typedef struct dns_cache dns_cache_t;

dns_cache_t *dns_cache_create(uint32_t buckets, uint64_t ttl_ms);
void dns_cache_destroy(dns_cache_t *c);

/* 命中未过期：NSBC_OK；未命中：解析并回填。
 * 解析失败 NSBC_ERR/NSBC_NOENT（上游按中继不可用处理）。 */
int dns_cache_lookup(dns_cache_t *c, const char *host, uint16_t port,
                     nsbc_sa_t *out);

void dns_cache_stats(dns_cache_t *c, uint64_t *hits, uint64_t *misses,
                     uint64_t *entries);

#endif /* NSBC_DNS_CACHE_H */
