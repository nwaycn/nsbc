#ifndef NSBC_SIP_EVENT_H
#define NSBC_SIP_EVENT_H

/* RFC 6665 事件通知框架（SUBSCRIBE/NOTIFY，替代 RFC 3265）
 * 对应 REQ-SIP-002（部分）：SUBSCRIBE/NOTIFY 方法族当前在 b2bua 裸回 501，
 * 本头定义事件包登记、订户状态机（pending/active/terminated）与超时清单；
 * P0 仅落粗骨架，P1 随 BLF（REQ-PBX-PRES-001）接线生产。
 *
 * 分层：sip 层；只依赖 parse（sip_msg）与 base，不引 mod/presence——
 * 具体事件包（dialog/message-summary/presence）经描述符在组合根登记。
 */
#include "nsbc_common.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIP_EV_NAME_MAX     32   /* Event 包头名长度，如 "presence" */
#define SIP_EV_CTYPE_MAX    64   /* NOTIFY Content-Type */
#define SIP_EV_MAX_SUBS     65536 /* 容量模型对齐 10k 并发通话 */

/* RFC 6665 §4.4 订阅状态机 */
typedef enum {
    SIP_SUB_PENDING = 0,   /* 已收 SUBSCRIBE，授权/资源未决 */
    SIP_SUB_ACTIVE,        /* 已发首个 NOTIFY 200 */
    SIP_SUB_TERMINATED     /* 到期/拒绝/订阅者退订 */
} sip_sub_state_t;

typedef enum {
    SIP_REF_NONE = 0,      /* 本端非 refresher */
    SIP_REF_UAC,           /* refresher=uac */
    SIP_REF_UAS            /* refresher=uas */
} sip_refresher_t;

/* 订阅稳定键：Call-ID + from-tag（RFC 6665 §4.2.1 同一 Call-ID/tag
 * 的 SUBSCRIBE 属于同一订阅；零拷贝令牌仅在解析报文存活期有效，
 * 入表时由实现层拷贝到 arena/slab）。 */
typedef struct {
    sip_tok_t callid;
    sip_tok_t from_tag;
    sip_tok_t event;       /* Event 头包名（不含参数） */
    sip_tok_t resource;    /* R-URI 标识的被订阅资源 */
} sip_sub_key_t;

typedef struct sip_subscription {
    sip_sub_state_t  state;
    uint64_t         expires_ms;   /* 单调时钟到期点；0=立即终止 */
    uint32_t         version;      /* NOTIFY 体版本（SN 递增） */
    sip_refresher_t  refresher;
    void            *pkg_ud;       /* 事件包私有数据 */
} sip_subscription_t;

/* 事件包描述符：presence/dialog/message-summary 等由上层登记。
 * on_subscribe：授权与接受决策（骨架返回 NOTSUP）。
 * on_notify_build：生成 NOTIFY 正文（骨架不生成）。 */
typedef struct {
    char     name[SIP_EV_NAME_MAX];
    char     content_type[SIP_EV_CTYPE_MAX];
    int    (*on_subscribe)(void *mgr_ud, const sip_msg_t *sub,
                           sip_subscription_t *s);
    int    (*on_notify_build)(void *mgr_ud, sip_subscription_t *s,
                              char *buf, size_t cap, size_t *out_len);
    void    *ud;
} sip_evpkg_t;

typedef struct sip_ev_mgr sip_ev_mgr_t;

typedef struct {
    int  shard_bits;          /* 分片哈希 2^n（目标 64 片） */
    int  default_expires_s;  /* Expires 缺省（RFC 建议 3600） */
    int  min_expires_s;      /* Min-Expires 下限 */
    int  max_expires_s;      /* Expires 上限 */
} sip_ev_cfg_t;

int  sip_ev_mgr_create(const sip_ev_cfg_t *cfg, sip_ev_mgr_t **out);
void sip_ev_mgr_destroy(sip_ev_mgr_t *m);

/* 事件包登记（启动期单线程；重名 EXISTS） */
int  sip_ev_pkg_register(sip_ev_mgr_t *m, const sip_evpkg_t *pkg);

/* 入向 SUBSCRIBE：建/刷新/终止订阅（expires=0 退订） */
int  sip_ev_on_subscribe(sip_ev_mgr_t *m, const sip_msg_t *msg);
/* 入向 NOTIFY（UAC 侧订阅者）：按 Event/Call-ID 定位订阅并消费 */
int  sip_ev_on_notify(sip_ev_mgr_t *m, const sip_msg_t *msg);

/* 周期扫描：终止到期订阅并触发 NOTIFY terminated（由时间轮驱动） */
int  sip_ev_tick(sip_ev_mgr_t *m, uint64_t now_ms, uint32_t *expired_n);

uint64_t sip_ev_active(sip_ev_mgr_t *m);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SIP_EVENT_H */
