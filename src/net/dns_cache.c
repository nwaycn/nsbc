#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dns_cache.h"
#include "nsbc_atomic.h"
#include "nsbc_hash.h"

#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct dns_entry {
    struct dns_entry *next;
    char *host;
    nsbc_sa_t sa;
    uint64_t expire_ms;
} dns_entry_t;

struct dns_cache {
    dns_entry_t **buckets;
    uint32_t nbucket;
    uint64_t ttl_ms;
    pthread_mutex_t lock;
    uint64_t hits;
    uint64_t misses;
};

dns_cache_t *dns_cache_create(uint32_t buckets, uint64_t ttl_ms)
{
    if (buckets == 0)
        buckets = 64;
    dns_cache_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->buckets = calloc(buckets, sizeof(dns_entry_t *));
    if (!c->buckets) {
        free(c);
        return NULL;
    }
    c->nbucket = buckets;
    c->ttl_ms = ttl_ms ? ttl_ms : 60000;
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

void dns_cache_destroy(dns_cache_t *c)
{
    if (!c)
        return;
    for (uint32_t i = 0; i < c->nbucket; ++i) {
        dns_entry_t *e = c->buckets[i];
        while (e) {
            dns_entry_t *n = e->next;
            free(e->host);
            free(e);
            e = n;
        }
    }
    free(c->buckets);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

static uint64_t mono_ms(void)
{
    extern uint64_t nsbc_now_ms(void);
    return nsbc_now_ms();
}

int dns_cache_lookup(dns_cache_t *c, const char *host, uint16_t port,
                     nsbc_sa_t *out)
{
    uint32_t b = (uint32_t)(nsbc_hash_str(host) % c->nbucket);
    uint64_t now = mono_ms();
    pthread_mutex_lock(&c->lock);
    dns_entry_t *e = c->buckets[b];
    while (e) {
        if (strcmp(e->host, host) == 0) {
            if (e->expire_ms > now) {
                *out = e->sa;
                nsbc_sa_set_port(out, port);
                ++c->hits;
                pthread_mutex_unlock(&c->lock);
                return NSBC_OK;
            }
            break; /* 过期：下面解析后原地更新 */
        }
        e = e->next;
    }
    ++c->misses;
    pthread_mutex_unlock(&c->lock);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return NSBC_NOENT;
    nsbc_sa_t sa;
    nsbc_sa_clear(&sa);
    memcpy(&sa.ss, res->ai_addr, res->ai_addrlen);
    sa.len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    nsbc_sa_set_port(&sa, port);
    *out = sa;

    pthread_mutex_lock(&c->lock);
    e = c->buckets[b];
    while (e && strcmp(e->host, host) != 0)
        e = e->next;
    if (e) {
        e->sa = sa;
        e->expire_ms = now + c->ttl_ms;
    } else {
        e = calloc(1, sizeof(*e));
        if (e && (e->host = strdup(host)) != NULL) {
            e->sa = sa;
            e->expire_ms = now + c->ttl_ms;
            e->next = c->buckets[b];
            c->buckets[b] = e;
        } else {
            free(e);
        }
    }
    pthread_mutex_unlock(&c->lock);
    return NSBC_OK;
}

void dns_cache_stats(dns_cache_t *c, uint64_t *hits, uint64_t *misses,
                     uint64_t *entries)
{
    pthread_mutex_lock(&c->lock);
    if (hits) *hits = c->hits;
    if (misses) *misses = c->misses;
    if (entries) {
        uint64_t n = 0;
        for (uint32_t i = 0; i < c->nbucket; ++i)
            for (dns_entry_t *e = c->buckets[i]; e; e = e->next)
                ++n;
        *entries = n;
    }
    pthread_mutex_unlock(&c->lock);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
