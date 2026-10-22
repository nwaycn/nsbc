/* event_bus.h — Phase 1 新建：Per-Core 无锁事件总线 + event_handler 订阅
 *
 * 白皮书 §3：高性能事件总线
 * §event_handler_register：同步订阅式事件处理（SESSION_CREATE/ANSWER/END）
 *
 * 分工：
 *   Registry Hook Chain（同步）— security 拦截 / registrar 处理 / SDP offer 改写
 *   Event Bus Ring（异步）— CDR emit / 监控观测 / 告警通知
 */
#ifndef NSBC_EVENT_BUS_H
#define NSBC_EVENT_BUS_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 事件名（event_handler 订阅用）===== */
#define EVT_SESSION_CREATE  "SESSION_CREATE"   /* 入站 INVITE 首次到达 b2bua */
#define EVT_SESSION_ANSWER  "SESSION_ANSWER"   /* 200 OK 应答（建立媒体） */
#define EVT_SESSION_END     "SESSION_END"      /* 呼叫终结（BYE/CANCEL/超时）*/
#define EVT_REGISTER_OK     "REGISTER_OK"      /* endpoint 注册成功 */
#define EVT_REGISTER_FAIL   "REGISTER_FAIL"    /* endpoint 注册失败 */
#define EVT_AUTH_FAIL       "AUTH_FAIL"        /* Digest 认证失败 */
#define EVT_MEDIA_BUG       "MEDIA_BUG"        /* media_bug per-call attach */

/* ===== 事件处理器回调（带卸载安全）===== */
/* ev_context 由内核构造，包含：event_name + payload_json + call_key + session_id */
typedef struct event_context {
    const char *event_name;
    const char *payload;       /* JSON 字符串（可能 NULL） */
    const void *call_key;      /* b2b_call_t* 不透明指针（可选，按事件类型）*/
    uint64_t    event_seq;     /* 全局单调递增序列号（Per-Core 合并后）*/
} event_context_t;

typedef int (*event_handler_fn)(const event_context_t *ev, void *ud);

/* ===== 同步订阅（event_handler_register）===== */
/*
 * register: 订阅某事件（返回 NSBC_OK 或 NSBC_EXISTS）
 *          支持通配 "*" 订阅所有事件
 * unregister: 标记 unloading + 自旋 wait refcnt→0 + 清空
 * trigger: 内核在 call_create / call_answer / call_end 时调
 *          返回值：NSBC_OK=全部 handler 执行成功；NSBC_PERM_DENIED=某 handler 拒绝（致命）
 *          致命拒绝可在 call_create 时阻止呼叫建立
 */
int  event_handler_register(const char *event_name, event_handler_fn fn, void *ud);
int  event_handler_unregister(const char *event_name, event_handler_fn fn, void *ud);
int  event_handler_trigger(const char *event_name, const char *payload,
                           const void *call_key, uint64_t *out_seq);

/* ===== 异步 Event Bus Ring（Per-Core 无锁）===== */
/*
 * nsbc_event_push: 零锁零 malloc 推送，满时 drop（生产绝对优先）
 * per_core_ring_snapshot: 读侧拿某个 core 的 ring 状态（只读，无修改）
 *
 * Per-Core Ring Buffer 生命周期：event_bus_init 启动时按在线 CPU 数建
 *                                 event_bus_fini 释放所有 ring
 */
int  event_bus_init(void);
void event_bus_fini(void);
int  nsbc_event_push(const char *event_name, const char *payload_json);

/* OAM show event_bus：每个 core ring 的 write_pos/read_pos/dropped_count */
typedef struct {
    int core_id;
    uint64_t write_pos;
    uint64_t read_pos;
    uint64_t dropped_count;
    uint64_t current_size;     /* write_pos - read_pos */
} per_core_ring_stat_t;

int  event_bus_core_count(void);
int  event_bus_get_stat(int core_id, per_core_ring_stat_t *out);
int  event_bus_collect_all_stats(per_core_ring_stat_t *out_array, int max);

/* 全局事件序列号（Per-Core 合并后单调递增，用于跨 Core 时序追溯）*/
uint64_t event_bus_next_seq(void);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_EVENT_BUS_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
