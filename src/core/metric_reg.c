/* 指标源注册表（REQ-OBS-001~003，P0）
 * A3：机制从 src/oam/obs_core.c 原样下沉为 core 框架设施（符号改名
 * obs_*→metric_*）；oam 只保留 exporter 与事件总线。注册表为纯静态
 * 表 + 互斥锁，无业务依赖、无需 init。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "metric_reg.h"
#include "nsbc_common.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static metric_source_t g_src[METRIC_SOURCES_MAX];
static pthread_mutex_t g_src_lock = PTHREAD_MUTEX_INITIALIZER;

int metric_source_register(const char *name, const char *help,
                           metric_render_fn render, void *ud)
{
    if (!name || !render)
        return NSBC_INVAL;
    pthread_mutex_lock(&g_src_lock);
    int slot = -1;
    for (int i = 0; i < METRIC_SOURCES_MAX; ++i) {
        if (g_src[i].used && strcmp(g_src[i].name, name) == 0) {
            slot = i; /* 同名覆盖（reload） */
            break;
        }
        if (!g_src[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_src_lock);
        return NSBC_NOSPC;
    }
    metric_source_t *s = &g_src[slot];
    snprintf(s->name, sizeof(s->name), "%s", name);
    if (help)
        snprintf(s->help, sizeof(s->help), "%s", help);
    else
        s->help[0] = '\0';
    s->render = render;
    s->ud = ud;
    s->used = 1;
    pthread_mutex_unlock(&g_src_lock);
    return NSBC_OK;
}

int metric_source_unregister(const char *name, metric_render_fn render,
                             void *ud)
{
    if (!name || !render)
        return NSBC_INVAL;
    pthread_mutex_lock(&g_src_lock);
    for (int i = 0; i < METRIC_SOURCES_MAX; ++i) {
        metric_source_t *s = &g_src[i];
        if (s->used && strcmp(s->name, name) == 0 &&
            s->render == render && s->ud == ud) {
            memset(s, 0, sizeof(*s));
            pthread_mutex_unlock(&g_src_lock);
            return NSBC_OK;
        }
    }
    pthread_mutex_unlock(&g_src_lock);
    return NSBC_NOENT;
}

int metric_render_all(char *buf, size_t cap)
{
    size_t used = 0;
    pthread_mutex_lock(&g_src_lock);
    for (int i = 0; i < METRIC_SOURCES_MAX; ++i) {
        metric_source_t *s = &g_src[i];
        if (!s->used)
            continue;
        int w = snprintf(buf + used, cap - used, "# HELP %s %s\n"
                                                "# TYPE %s gauge\n",
                         s->name,
                         s->help[0] ? s->help : s->name, s->name);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used;
        if (used >= cap)
            break;
        int n = s->render(buf + used, cap - used, s->ud);
        if (n > 0)
            used += (size_t)n < cap - used ? (size_t)n : cap - used;
        if (used >= cap)
            break;
    }
    pthread_mutex_unlock(&g_src_lock);
    if (used < cap)
        buf[used] = '\0';
    return (int)used;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
