#ifndef NSBC_METRIC_REG_H
#define NSBC_METRIC_REG_H

/* 指标源注册表（core 框架设施，REQ-OBS-001/002/003，P0）
 *
 * 中立机制，零业务依赖：任意层模块在自身 create 时登记 render 回调
 * （"代码内埋点"，指标归属主模块）；oam exporter（src/oam/obs_metrics.c）
 * 仅在 /metrics 被拉取时经 metric_render_all 统一渲染 text exposition，
 * 采集开销只发生在拉取时。关键路径不得直接调用 render。
 *
 * 分层口径：本头是 core 框架白名单头（白名单枚举见
 * scripts/check_include_order.sh 规则 2），各层可直接 include；
 * oam 不再借注册表横向读取兄弟模块（REQ-ARCH-010/011，A3）。
 * 事件总线（obs_event / obs_events_tail）不属本机制，仍在 oam 的 obs.h。
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METRIC_NAME_MAX 64
#define METRIC_HELP_MAX 128
#define METRIC_SOURCES_MAX 64

/* 单次渲染把多行 "name{...} value\n" 写入 buf；返回应写长度（>=cap 截断） */
typedef int (*metric_render_fn)(char *buf, size_t cap, void *ud);

typedef struct {
    char name[METRIC_NAME_MAX];
    char help[METRIC_HELP_MAX];
    metric_render_fn render;
    void *ud;
    int used;
} metric_source_t;

int metric_source_register(const char *name, const char *help,
                           metric_render_fn render, void *ud);

/* 输出完整 text exposition 到 buf；返回写出长度 */
int metric_render_all(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
