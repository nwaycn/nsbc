#ifndef NSBC_SIP_TXN_H
#define NSBC_SIP_TXN_H

/* RFC3261 第 17 章事务层（P0）
 *
 * 四类事务 + 全部标准定时器，全部挂 REQ-ARCH-007 分层时间轮，
 * 无 timerfd/无堆：
 *   ICT  客户端 INVITE：Timer A(重传,×2,≤T2) B(64×T1) D(≥32s UDP)
 *   NICT 客户端非 INVITE：Timer E(重传) F(64×T1) K(T4 UDP)
 *   IST  服务端 INVITE：Timer G(末响应重传) H(64×T1) I(T4)
 *   NIST 服务端非 INVITE：Timer J(64×T1 UDP)
 * 匹配键：top Via branch（z9hG4bK magic cookie）+ CSeq 方法 + 对端地址；
 * 无 cookie 的 RFC2543 报文走无状态旁路（txn 返回 TXN_STATELESS）。
 * 事务对象从 SIGNAL 域 slab 取放，重传报文从 PACKET 域 arena 复制，
 * 快路径零 malloc。双层哈希桶 + 分片锁 + 引用计数。
 */
#include "mem.h"
#include "net_sock.h"
#include "sip_msg.h"
#include "timer_wheel.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct txn_mgr txn_mgr_t;
typedef struct sip_txn sip_txn_t;

typedef enum {
    TXN_ICT = 0,
    TXN_NICT,
    TXN_IST,
    TXN_NIST
} txn_kind_t;

typedef enum {
    TXN_S_NONE = 0,
    /* ICT */
    TXN_ICT_CALLING,
    TXN_ICT_PROCEEDING,
    TXN_ICT_COMPLETED,
    TXN_ICT_TERMINATED,
    /* NICT */
    TXN_NICT_TRYING,
    TXN_NICT_PROCEEDING,
    TXN_NICT_COMPLETED,
    TXN_NICT_TERMINATED,
    /* IST */
    TXN_IST_PROCEEDING,
    TXN_IST_COMPLETED,
    TXN_IST_CONFIRMED,
    TXN_IST_TERMINATED,
    /* NIST */
    TXN_NIST_TRYING,
    TXN_NIST_PROCEEDING,
    TXN_NIST_COMPLETED,
    TXN_NIST_TERMINATED
} txn_state_t;

/* 上报 TU（B2BUA/registrar）的事件 */
typedef enum {
    TXN_EV_NEW_REQUEST = 1,   /* 服务端：新事务首请求 */
    TXN_EV_ACK,               /* IST：收到 ACK */
    TXN_EV_PROVISIONAL,       /* 客户端：1xx */
    TXN_EV_FINAL,             /* 客户端：2xx~699 */
    TXN_EV_TIMEOUT,           /* B/F/H 超时 */
    TXN_EV_TRANSPORT_ERR
} txn_event_t;

/* TU 回调在分片锁外调用；m 为触发消息（响应/ACK），可能为 NULL（超时） */
typedef void (*txn_tu_fn)(sip_txn_t *t, const sip_msg_t *m, txn_event_t ev,
                          void *ud);

/* 发送由传输层实现：reliable 决定重传是否启用 */
typedef int (*txn_send_fn)(void *ud, const char *buf, size_t len,
                           const nsbc_sa_t *dst, int reliable);

typedef struct {
    mem_arena_t *sig_arena;
    mem_arena_t *pkt_arena;
    mem_slab_t *slab;       /* 可空：NULL 时内部自建（容量 capacity） */
    int capacity;           /* slab 自建时的事务容量 */
    tw_t *tw;
    uint32_t t1_ms, t2_ms, t4_ms, timer_d_ms;
    txn_send_fn send;
    void *send_ud;
    txn_tu_fn server_tu;   /* 新服务端事务统一上报口 */
    void *server_ud;
} txn_mgr_cfg_t;

int txn_mgr_create(const txn_mgr_cfg_t *cfg, txn_mgr_t **out);
void txn_mgr_destroy(txn_mgr_t *m);
/* 装配期后置绑定服务端 TU（reactor 运行前调用） */
void txn_mgr_set_server_tu(txn_mgr_t *m, txn_tu_fn tu, void *ud);

/* 建客户端事务并首发请求（buf 为完整 SIP 报文；内部复制重传副本）。
 * kind 仅可为 TXN_ICT/TXN_NICT。返回事务指针（失败 NULL）。 */
sip_txn_t *txn_client_new(txn_mgr_t *m, txn_kind_t kind,
                          const char *branch, size_t blen,
                          sip_method_t method, const nsbc_sa_t *dst,
                          int reliable, const char *buf, size_t len,
                          txn_tu_fn tu, void *ud, uint64_t now_ms);

/* 入向请求分发（已解析）。返回：
 *   NSBC_OK              命中既有事务（重传/ACK，已自动处理）
 *   NSBC_AGAIN 旁路基值  —— 实际用 TXN_NEW / TXN_STATELESS 常量（见下）
 */
#define TXN_NEW 1       /* 新建了服务端事务（调用方继续 B2BUA 处理） */
#define TXN_STATELESS 2 /* 无 magic cookie：无状态处理 */
#define TXN_DROP 3      /* 已吸收/应丢弃 */
int txn_server_input(txn_mgr_t *m, sip_msg_t *msg, const nsbc_sa_t *src,
                     int reliable, uint64_t now_ms);

/* 入向响应分发；返回 TXN_DROP 表示无主响应丢弃 */
int txn_client_input(txn_mgr_t *m, sip_msg_t *msg, const nsbc_sa_t *src,
                     int reliable, uint64_t now_ms);

/* TU 经事务发响应：自动缓存并在请求重传时重发；
 * 2xx~699 进入 COMPLETED 定时器，1xx 留在 PROCEEDING。 */
int txn_send_response(sip_txn_t *t, const char *buf, size_t len,
                      uint64_t now_ms);

/* 2xx ACK 对话层吸收（RFC3261 §13.3.1.4）：2xx 的 ACK 用新 branch，
 * 按 branch 匹配不到 IST（txn_server_input 走 TXN_STATELESS）。
 * TU 对话层按 Call-ID 匹配到呼叫后调本接口停 Timer G/H 转 CONFIRMED。
 * 返回 1 成功吸收；0 事务不存在/状态不符（非 2xx 重传窗口）。 */
int txn_ist_absorb_ack(sip_txn_t *t, uint64_t now_ms);

/* 强制解除 IST 的 Timer H/G（竞态兜底：absorb_ack 失败后仍需解除 Timer H） */
void txn_ist_force_disarm_h(sip_txn_t *t);

/* 取事务首请求（NUL 终止的报文副本；*len 可空） */
const char *txn_first_request(const sip_txn_t *t, size_t *len);
txn_state_t txn_state(const sip_txn_t *t);
txn_kind_t txn_kind(const sip_txn_t *t);
const nsbc_sa_t *txn_peer(const sip_txn_t *t);
void *txn_ud(const sip_txn_t *t);
void txn_set_ud(sip_txn_t *t, void *ud);
void txn_set_tu(sip_txn_t *t, txn_tu_fn tu);
const char *txn_state_name(txn_state_t s);

/* 主动终止事务（不等 Timer H/B/F 超时）。用于 BYE→200 OK 后
 * call_destroy 立即释放 INVITE txn，避免 txn_active 堆积。
 * 幂等：已 dead 的 txn 直接返回。 */
void txn_force_terminate(sip_txn_t *t);

/* 统计（OBS） */
uint64_t txn_mgr_active(txn_mgr_t *m);
uint64_t txn_mgr_retrans_in(txn_mgr_t *m);
uint64_t txn_mgr_retrans_out(txn_mgr_t *m);
uint64_t txn_mgr_timeouts(txn_mgr_t *m);

/* 容量查询：slab 总容量（= core.max_calls 默认 200000） */
uint64_t txn_mgr_capacity(txn_mgr_t *m);
/* 使用率：0.0 ~ 1.0，O(1) 原子读 */
double   txn_mgr_usage(txn_mgr_t *m);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
