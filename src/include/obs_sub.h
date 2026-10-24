#ifndef NSBC_OBS_SUB_H
#define NSBC_OBS_SUB_H

/* OAM 订阅推送（REQ-OBS-005/006，P1；TCP 长连接）
 *
 * 控制通道上的主题订阅：客户端 "sub <topic>" 后，事件经长连接服务端
 * 推送（metrics 周期帧 / events 实时事件 / cdr 实时话单 / log 日志流）。
 * 与拉取式 show 互补：订阅不占请求往返，慢消费者按每订阅者定长环
 * 丢弃旧帧并计 drop（绝不反压呼叫线程：满环只丢不阻塞）。
 * 连接对象不透明（obs_control 的 TCP 会话），发送经 send_cb。
 * 分层：oam（3）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBS_SUB_TOPIC_MAX  24
#define OBS_TOPIC_MAX      16
#define OBS_SUBSCRIBER_MAX 256
/* ★ 256 槽而不是 16：CALL_INCOMING 被 INVITE 路径上瞬时爆发的 LOG 帧
 * 挤掉（SDP 解析 + codec 匹配 + resolve + 100 Trying 轻松 >16 帧），
 * 所有 topic 共享同一个 per-subscriber ring——LOG 洪水直接 EVICT head 处
 * 的 CALL_INCOMING。提到 256 可容纳任何正常呼叫的瞬时帧爆发。 */
#define OBS_SUB_RING       256
#define OBS_FRAME_MAX      2048

typedef enum {
    OBS_TOPIC_METRICS = 0,
    OBS_TOPIC_EVENTS,
    OBS_TOPIC_CDR,
    OBS_TOPIC_LOG,
    OBS_TOPIC_TRACE      /* SIP 原始包 trace（sofia siptrace）— 与 log 独立 */
} obs_topic_t;

typedef struct obs_sub_hub obs_sub_hub_t;

/* 发送回调（obs_control 装配：非阻塞写；EAGAIN 由订阅者环缓存）。
 * 返回非 0 视为连接失效，hub 自动摘除订阅者。 */
typedef int (*obs_send_fn)(void *conn, const void *data, size_t len);

obs_sub_hub_t *obs_sub_create(obs_send_fn send);
void           obs_sub_destroy(obs_sub_hub_t *h);

/* 连接建立/关闭（conn 为不透明会话键） */
int  obs_sub_attach(obs_sub_hub_t *h, void *conn);
int  obs_sub_detach(obs_sub_hub_t *h, const void *conn);

int  obs_sub_add(obs_sub_hub_t *h, const void *conn, obs_topic_t t);
int  obs_sub_del(obs_sub_hub_t *h, const void *conn, obs_topic_t t);

/* 发布：帧拷入订阅者环（满环丢旧，drop 计数）；单帧截断到
 * OBS_FRAME_MAX-1。呼叫线程可直接调（非阻塞、无锁需求见 .c 注释）。 */
int  obs_sub_publish(obs_sub_hub_t *h, obs_topic_t t,
                     const char *fmt, ...);

/* 连接可写时驱动输出（ reactor 回调）；返回 flush 的字节数 */
size_t obs_sub_flush(obs_sub_hub_t *h, const void *conn);

/* 订阅者的帧环是否为空？用于 EPOLLOUT 动态加减：EAGAIN 后加 EPOLLOUT，
 * flush 到环空后减 EPOLLIN only，避免 UDS 永久可写导致 reactor 空转。 */
int obs_sub_ring_empty(obs_sub_hub_t *h, const void *conn);

/* OAM 只读：丢弃帧计数（慢消费者告警） */
uint64_t obs_sub_drop_count(obs_sub_hub_t *h);
int obs_topic_parse(const char *name, obs_topic_t *out);
const char *obs_topic_name(obs_topic_t t);

/* ---- 全局单例（obs_control 在 oam 模块 start 时 init/shutdown）---- */
int            obs_sub_global_init(obs_send_fn send_fn);
void           obs_sub_global_destroy(void);
obs_sub_hub_t *obs_sub_global(void);

/* ---- 便捷 publish：业务模块无需感知 hub，内部带锁 ---- */
int obs_sub_publish_events(const char *fmt, ...);
int obs_sub_publish_cdr(const char *fmt, ...);
int obs_sub_publish_log(const char *fmt, ...);
int obs_sub_publish_metrics(const char *fmt, ...);
int obs_sub_publish_trace(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_OBS_SUB_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
