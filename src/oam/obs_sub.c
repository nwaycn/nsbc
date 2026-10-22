/* OAM 订阅推送（REQ-OBS-005/006，P1；TCP 长连接）
 *
 * 线程模型：hub 级一把 pthread_mutex_t 保护 subs[] 所有访问。
 * 任何线程（呼叫/billing/oam control）都能调 obs_sub_publish_*；
 * publish 内部对订阅者做 memcpy 帧入环（锁内 < 1μs），再 try flush
 * 被命中的订阅者（非阻塞 send；EAGAIN/失效直接 break，绝不阻塞
 * 呼叫线程）。慢消费者按每订阅者定长环 OBS_SUB_RING=16 丢最旧帧并
 * 计 drop——订阅语义允许丢帧，绝不反压呼叫线程。
 *
 * 全局单例：obs_sub_global_init/destroy 由 obs_control（oam 模块）
 * 在模块 start 时创建（send = UDS fd 非阻塞 send），shutdown 时销毁。
 * 业务模块通过 obs_sub_publish_events/cdr/log 便捷函数无需感知 hub。
 *
 * 帧格式：以 \n 结尾的纯文本行，客户端按行 readline() 即可。
 *   events 帧: "YYYY-MM-DDTHH:MM:SS.mmm <DOMAIN_ACTION> <body>\n"
 *   cdr 帧:   "YYYY-MM-DDTHH:MM:SS.mmm CDR <k=v k=v ...>\n"
 *   log 帧:   （同 log 环格式，留给 log_ring_tail 的订阅推送用）
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "obs_sub.h"
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>


/* ----------------------------------------------------------------------
 * 订阅者数据结构
 * ---------------------------------------------------------------------- */

typedef struct {
    void *conn;           /* NULL=空槽 */
    unsigned topics;      /* 1<<obs_topic_t */
    char ring[OBS_SUB_RING][OBS_FRAME_MAX];
    int  len[OBS_SUB_RING];
    int  head;
    int  tail;
    uint64_t dropped;
} obs_sub_t;

struct obs_sub_hub {
    obs_send_fn send;
    obs_sub_t subs[OBS_SUBSCRIBER_MAX];
    uint64_t total_dropped;
    pthread_mutex_t lock;   /* 保护 subs[] 与 total_dropped */
};


/* ----------------------------------------------------------------------
 * 生命周期
 * ---------------------------------------------------------------------- */

obs_sub_hub_t *obs_sub_create(obs_send_fn send)
{
    if (send == NULL)
        return NULL;
    obs_sub_hub_t *h = (obs_sub_hub_t *)calloc(1, sizeof(*h));
    if (h == NULL)
        return NULL;
    h->send = send;
    pthread_mutex_init(&h->lock, NULL);
    return h;
}

void obs_sub_destroy(obs_sub_hub_t *h)
{
    if (h == NULL) return;
    pthread_mutex_destroy(&h->lock);
    free(h);
}


/* ----------------------------------------------------------------------
 * 订阅者查找 / attach / detach
 * ---------------------------------------------------------------------- */

static obs_sub_t *by_conn_locked(obs_sub_hub_t *h, const void *conn)
{
    int i;
    for (i = 0; i < OBS_SUBSCRIBER_MAX; ++i) {
        if (h->subs[i].conn == conn)
            return &h->subs[i];
    }
    return NULL;
}

int obs_sub_attach(obs_sub_hub_t *h, void *conn)
{
    int i;
    if (h == NULL || conn == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&h->lock);
    if (by_conn_locked(h, conn) != NULL) {
        pthread_mutex_unlock(&h->lock);
        return NSBC_EXISTS;
    }
    for (i = 0; i < OBS_SUBSCRIBER_MAX; ++i) {
        if (h->subs[i].conn == NULL) {
            memset(&h->subs[i], 0, sizeof(h->subs[i]));
            h->subs[i].conn = conn;
            pthread_mutex_unlock(&h->lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&h->lock);
    return NSBC_NOSPC;
}

int obs_sub_detach(obs_sub_hub_t *h, const void *conn)
{
    if (h == NULL || conn == NULL)
        return NSBC_INVAL;
    pthread_mutex_lock(&h->lock);
    obs_sub_t *s = by_conn_locked(h, conn);
    if (s == NULL) {
        pthread_mutex_unlock(&h->lock);
        return NSBC_NOENT;
    }
    memset(s, 0, sizeof(*s)); /* conn 置 NULL 即空槽 */
    pthread_mutex_unlock(&h->lock);
    return NSBC_OK;
}

int obs_sub_add(obs_sub_hub_t *h, const void *conn, obs_topic_t t)
{
    if (h == NULL || conn == NULL || t < 0 || t >= OBS_TOPIC_MAX)
        return NSBC_INVAL;
    pthread_mutex_lock(&h->lock);
    obs_sub_t *s = by_conn_locked(h, conn);
    if (s != NULL)
        s->topics |= 1u << (unsigned)t;
    pthread_mutex_unlock(&h->lock);
    return s != NULL ? NSBC_OK : NSBC_NOENT;
}

int obs_sub_del(obs_sub_hub_t *h, const void *conn, obs_topic_t t)
{
    if (h == NULL || conn == NULL || t < 0 || t >= OBS_TOPIC_MAX)
        return NSBC_INVAL;
    pthread_mutex_lock(&h->lock);
    obs_sub_t *s = by_conn_locked(h, conn);
    if (s != NULL)
        s->topics &= ~(1u << (unsigned)t);
    pthread_mutex_unlock(&h->lock);
    return s != NULL ? NSBC_OK : NSBC_NOENT;
}


/* ----------------------------------------------------------------------
 * 帧环操作 + flush（锁内完成，非阻塞 send）
 * ---------------------------------------------------------------------- */

static int ring_push_locked(obs_sub_t *s, const char *frame, int n)
{
    if (s->len[s->tail] != 0) {
        /* 尾槽未消费 = 环满，丢最旧 */
        s->len[s->head] = 0;
        s->head = (s->head + 1) % OBS_SUB_RING;
        ++s->dropped;
    }
    memcpy(s->ring[s->tail], frame, (size_t)n);
    s->ring[s->tail][n] = '\0';
    s->len[s->tail] = n + 1;
    s->tail = (s->tail + 1) % OBS_SUB_RING;
    return n + 1;
}

/* 在锁内 try flush 一个订阅者：非阻塞 send；EAGAIN/失效 break */
static size_t flush_one_locked(obs_sub_hub_t *h, obs_sub_t *s)
{
    size_t bytes = 0;
    while (s->len[s->head] != 0) {
        int n = s->len[s->head] - 1; /* 去掉 '\0' */
        if (h->send(s->conn, s->ring[s->head], (size_t)n) != 0)
            break; /* EAGAIN / 连接失效：break，等下次 publish 再试 */
        bytes += (size_t)n;
        s->len[s->head] = 0;
        s->head = (s->head + 1) % OBS_SUB_RING;
    }
    h->total_dropped += s->dropped;
    s->dropped = 0;
    return bytes;
}


/* ----------------------------------------------------------------------
 * obs_sub_publish —— 多线程安全（加锁）
 * ----------------------------------------------------------------------
 * 设计：锁内一次遍历，命中者帧入环 + 立刻 try flush。
 * 总耗时 ≈ O(N) memcpy + O(hits) send(非阻塞)。呼叫线程调可接受：
 * 256 订阅者 × 2KB 帧 ≈ 500KB memcpy ≈ 1~2μs。
 * 满订阅者 256 的 send 全部 EAGAIN ≈ 256 次系统调用 ≈ 可能上到 20μs。
 * 当前 CPS 规模下可以接受；未来 CPS 爆炸时分片自旋优化。
 * ---------------------------------------------------------------------- */

int obs_sub_publish(obs_sub_hub_t *h, obs_topic_t t, const char *fmt, ...)
{
    char frame[OBS_FRAME_MAX];
    va_list ap;
    int n, hits = 0;
    if (h == NULL || fmt == NULL || t < 0 || t >= OBS_TOPIC_MAX)
        return NSBC_INVAL;
    va_start(ap, fmt);
    n = vsnprintf(frame, sizeof(frame) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return NSBC_ERR;
    if (n >= OBS_FRAME_MAX - 1)
        n = OBS_FRAME_MAX - 2;
    frame[OBS_FRAME_MAX - 1] = '\0';

    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < OBS_SUBSCRIBER_MAX; ++i) {
        obs_sub_t *s = &h->subs[i];
        if (s->conn != NULL && (s->topics & (1u << (unsigned)t)) != 0) {
            ring_push_locked(s, frame, n);
            ++hits;
        }
    }
    /* 立刻 try flush 所有命中订阅者——避免新事件 publish 之间客户端
     * 长时间等不到已入环的帧。EAGAIN/失效 break，不阻塞。 */
    if (hits > 0) {
        for (int i = 0; i < OBS_SUBSCRIBER_MAX; ++i) {
            obs_sub_t *s = &h->subs[i];
            if (s->conn != NULL && s->len[s->head] != 0)
                flush_one_locked(h, s);
        }
    }
    pthread_mutex_unlock(&h->lock);
    return hits;
}

size_t obs_sub_flush(obs_sub_hub_t *h, const void *conn)
{
    size_t bytes = 0;
    if (h == NULL || conn == NULL)
        return 0;
    pthread_mutex_lock(&h->lock);
    obs_sub_t *s = by_conn_locked(h, conn);
    if (s != NULL && s->conn != NULL)
        bytes = flush_one_locked(h, s);
    pthread_mutex_unlock(&h->lock);
    return bytes;
}

uint64_t obs_sub_drop_count(obs_sub_hub_t *h)
{
    uint64_t n = 0;
    if (h == NULL) return 0;
    pthread_mutex_lock(&h->lock);
    n = h->total_dropped;
    for (int i = 0; i < OBS_SUBSCRIBER_MAX; ++i)
        n += h->subs[i].dropped;
    pthread_mutex_unlock(&h->lock);
    return n;
}


/* ----------------------------------------------------------------------
 * Topic 名解析
 * ---------------------------------------------------------------------- */

int obs_topic_parse(const char *name, obs_topic_t *out)
{
    if (name == NULL || out == NULL)
        return NSBC_INVAL;
    if (strcmp(name, "metrics") == 0)
        *out = OBS_TOPIC_METRICS;
    else if (strcmp(name, "events") == 0)
        *out = OBS_TOPIC_EVENTS;
    else if (strcmp(name, "cdr") == 0)
        *out = OBS_TOPIC_CDR;
    else if (strcmp(name, "log") == 0)
        *out = OBS_TOPIC_LOG;
    else
        return NSBC_NOENT;
    return NSBC_OK;
}

const char *obs_topic_name(obs_topic_t t)
{
    switch (t) {
    case OBS_TOPIC_METRICS: return "metrics";
    case OBS_TOPIC_EVENTS:  return "events";
    case OBS_TOPIC_CDR:     return "cdr";
    case OBS_TOPIC_LOG:     return "log";
    }
    return "unknown";
}


/* ======================================================================
 * 全局单例 + 便捷 publish 函数（业务模块无感知）
 * ======================================================================
 *
 * obs_sub_global_init 需在 obs_control_start 里传入自己的 send 适配
 * 函数（send 函数不定义在本文件，因为它要访问 obs_control 内部的
 * ctl_conn_t 结构体）。
 * ====================================================================== */

static obs_sub_hub_t *g_sub_hub = NULL;

int obs_sub_global_init(obs_send_fn send_fn)
{
    if (g_sub_hub != NULL)
        return NSBC_OK; /* 已经初始化过 */
    g_sub_hub = obs_sub_create(send_fn);
    if (g_sub_hub == NULL)
        return NSBC_NOMEM;
    return NSBC_OK;
}

void obs_sub_global_destroy(void)
{
    obs_sub_destroy(g_sub_hub);
    g_sub_hub = NULL;
}

obs_sub_hub_t *obs_sub_global(void)
{
    return g_sub_hub;
}

/* ---- 便捷函数：业务模块直接调，无需感知 hub ---- */

int obs_sub_publish_events(const char *fmt, ...)
{
    if (g_sub_hub == NULL || fmt == NULL) return 0;
    char frame[OBS_FRAME_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(frame, sizeof(frame) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if (n >= OBS_FRAME_MAX - 1) n = OBS_FRAME_MAX - 2;
    return obs_sub_publish(g_sub_hub, OBS_TOPIC_EVENTS, "%s\n", frame);
}

int obs_sub_publish_cdr(const char *fmt, ...)
{
    if (g_sub_hub == NULL || fmt == NULL) return 0;
    char frame[OBS_FRAME_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(frame, sizeof(frame) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if (n >= OBS_FRAME_MAX - 1) n = OBS_FRAME_MAX - 2;
    return obs_sub_publish(g_sub_hub, OBS_TOPIC_CDR, "%s\n", frame);
}

int obs_sub_publish_log(const char *fmt, ...)
{
    if (g_sub_hub == NULL || fmt == NULL) return 0;
    char frame[OBS_FRAME_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(frame, sizeof(frame) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if (n >= OBS_FRAME_MAX - 1) n = OBS_FRAME_MAX - 2;
    return obs_sub_publish(g_sub_hub, OBS_TOPIC_LOG, "%s\n", frame);
}

int obs_sub_publish_metrics(const char *fmt, ...)
{
    if (g_sub_hub == NULL || fmt == NULL) return 0;
    char frame[OBS_FRAME_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(frame, sizeof(frame) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if (n >= OBS_FRAME_MAX - 1) n = OBS_FRAME_MAX - 2;
    return obs_sub_publish(g_sub_hub, OBS_TOPIC_METRICS, "%s\n", frame);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
