/* 事件环形缓冲 + 订阅推送（REQ-OAM-004 / REQ-OAM-005 / REQ-OAM-006，P0）
 *
 * 两路径：
 *   A. 环形缓冲（obs_events_tail 拉取式）—— OBS_EVENT_RING=1024 条，
 *      mutex 保护，OBS_TOPIC_METRICS 由 metrics exporter 周期帧写入。
 *   B. 订阅推送（obs_sub 长连接）—— 每条事件同时 obs_sub_publish_events
 *      推给已订阅 OBS_TOPIC_EVENTS 的 UDS/TCP 客户端。订阅者每连接定长
 *      帧环 OBS_SUB_RING=16，满丢最旧帧并计 drop——绝不反压呼叫线程。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "obs.h"
#include "obs_sub.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define OBS_EVENT_RING 1024
#define OBS_EVENT_MAX 512

static struct {
    char line[OBS_EVENT_RING][OBS_EVENT_MAX];
    unsigned head; /* 下一条写入位 */
    unsigned total;
    pthread_mutex_t lock;
} g_ev;

static pthread_once_t once = PTHREAD_ONCE_INIT;

static void ev_lock_init(void)
{
    pthread_mutex_init(&g_ev.lock, NULL);
}

/* 单生产者：任何线程可直接调 obs_event（caller 语义）；
 * 内部 mutex 保护 + 写完立刻 try 订阅推送（obs_sub_publish_events
 * 内部自带锁，不会与本函数死锁——锁序固定 g_ev.lock → g_sub_lock）。 */
void obs_event(const char *domain_action, const char *fmt, ...)
{
    if (domain_action == NULL) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmv);
    int mspart = (int)(ts.tv_nsec / 1000000);

    char body[OBS_EVENT_MAX - 64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char full[OBS_EVENT_MAX];
    snprintf(full, sizeof(full), "%s.%03d %s %s", stamp, mspart,
             domain_action, body);

    pthread_once(&once, ev_lock_init);

    /* A. 写环形缓冲（拉取式） */
    pthread_mutex_lock(&g_ev.lock);
    snprintf(g_ev.line[g_ev.head], OBS_EVENT_MAX, "%s", full);
    g_ev.head = (g_ev.head + 1) % OBS_EVENT_RING;
    ++g_ev.total;
    pthread_mutex_unlock(&g_ev.lock);

    /* B. 订阅推送（推送式）——obs_sub_publish_events 内部带锁，
     * O(N) memcpy 遍历 + send，非阻塞，EAGAIN 则丢进订阅者帧环 */
    obs_sub_publish_events("%s", full);
}

int obs_events_tail(char *buf, size_t cap, int max)
{
    if (max <= 0 || cap == 0)
        return 0;
    pthread_once(&once, ev_lock_init);
    pthread_mutex_lock(&g_ev.lock);
    unsigned have = g_ev.total < OBS_EVENT_RING ? g_ev.total
                                                : OBS_EVENT_RING;
    if ((unsigned)max < have)
        have = (unsigned)max;
    size_t used = 0;
    unsigned start = (g_ev.head + OBS_EVENT_RING - have) % OBS_EVENT_RING;
    for (unsigned k = 0; k < have; ++k) {
        const char *line = g_ev.line[(start + k) % OBS_EVENT_RING];
        int n = snprintf(buf + used, cap - used, "%s\n", line);
        if (n <= 0 || (size_t)n >= cap - used)
            break;
        used += (size_t)n;
    }
    pthread_mutex_unlock(&g_ev.lock);
    return (int)used;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
