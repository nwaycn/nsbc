#ifndef NSBC_HA_REPLICA_H
#define NSBC_HA_REPLICA_H

/* HA 状态复制（REQ-HA-002 状态同步 P1 / REQ-HA-004 热升级）
 *
 * 复制对象（粗粒度优先，避免每包复制）：
 *  - REPL_REG：注册绑定增删改（位置库行级）；
 *  - REPL_DIALOG：B2BUA 对话锚点（Call-ID/路由集/媒体锚，故障后能续接）；
 *  - REPL_CDR_FLUSH：备机待写话单水位（防主挂丢话单，P2）；
 *  - REPL_FULL：全量快照（首同步/缺口过大时整表推送）。
 * 模型：主→备单推，64 位单调 seq + 备端 ACK 水位；备端发现缺口
 * （乱序/丢条目）回调 on_gap 请主端重传或转全量；载荷由各业务模块
 * 自行序列化（长度前缀 + 版本号，禁止直接 memcpy 含指针对象）。
 * 分层：ha（3），传输由 core 装配（独立 TCP/复用 ha 心跳通道）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REPL_PAYLOAD_MAX  4096
#define REPL_RING_CAP     8192

typedef enum {
    REPL_REG = 0,
    REPL_DIALOG,
    REPL_CDR_FLUSH,
    REPL_FULL,
    REPL_TYPE_MAX
} repl_type_t;

typedef struct {
    uint64_t    seq;
    repl_type_t type;
    uint16_t    ver;          /* 载荷结构版本（热升级跨版本判定） */
    uint16_t    len;
    uint8_t     payload[REPL_PAYLOAD_MAX];
} repl_rec_t;

/* 传输/回调（core 装配）：
 *  send：主→备发送一条（失败由内部环保留待重试，不阻塞呼叫线程）；
 *  apply：备端按序应用（返回非 0 计入 apply_fail 指标并告警，不跳序）；
 *  on_gap：备端发现缺口（期望 seq 未到），请求重传/全量（可空）。 */
typedef struct {
    int  (*send)(void *ud, const repl_rec_t *r);
    int  (*apply)(void *ud, const repl_rec_t *r);
    int  (*on_gap)(void *ud, uint64_t expect_seq, uint64_t got_seq);
    void *ud;
} repl_ops_t;

typedef struct ha_repl ha_repl_t;

int  repl_create(const repl_ops_t *ops, int is_active, ha_repl_t **out);
void repl_destroy(ha_repl_t *r);

/* 主端：发布变更（填 seq/入环/回调 send）。返回发布 seq，<0 错误码。
 * 环满（备端长期 ACK 落后）返回 NOSPC——调用方转全量/告警，不阻塞。 */
int64_t repl_publish(ha_repl_t *r, repl_type_t type, uint16_t ver,
                     const void *payload, uint16_t len);

/* 备端：接收（网络层解码后）；乱序/缺口按 on_gap 处理，重复丢弃 */
int  repl_ingest(ha_repl_t *r, const repl_rec_t *rec);

/* 备端 ACK 水位推进（主端据此淘汰环；粗骨架水位计数 OAM 只读） */
void repl_ack(ha_repl_t *r, uint64_t ack_seq);
/* 主端全量推送完成后调（备端水位对齐，清零缺口计数） */
void repl_resync_done(ha_repl_t *r, uint64_t snapshot_seq);

/* OAM 只读水位 */
uint64_t repl_published_seq(ha_repl_t *r);
uint64_t repl_applied_seq(ha_repl_t *r);
uint64_t repl_gap_count(ha_repl_t *r);

const char *repl_type_name(repl_type_t t);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_HA_REPLICA_H */
