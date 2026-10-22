#ifndef NSBC_LOCATION_DB_H
#define NSBC_LOCATION_DB_H

/* 注册位置库（REQ-REG，P0）
 *
 * AoR → 多 Contact 绑定。幂等：同 Call-ID 且 CSeq 不回退时刷新；
 * Contact: * 注销整个 AoR。64 分片互斥（P1 读侧换 RCU 无锁），
 * 绑定对象从 SIGNAL 域 slab 取放，绝对到期时间 CLOCK_MONOTONIC。
 *
 * P0 不含：GRUU（RFC 5627）、reg-event 同步、多实例持久化/XFR 同步。
 */
#include "mem.h"
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOC_AOR_MAX       128
#define LOC_CONTACT_MAX   256
#define LOC_CALLID_MAX    128
#define LOC_INSTANCE_MAX  128
#define LOC_SHARDS        64

typedef struct {
    char aor[LOC_AOR_MAX];
    char contact[LOC_CONTACT_MAX];   /* 原始 Contact（审计用，可能是私网地址） */
    char callid[LOC_CALLID_MAX];
    char instance[LOC_INSTANCE_MAX]; /* +sip.instance（去重键之一） */
    uint64_t expire_ms;   /* 绝对到期（CLOCK_MONOTONIC）；0=本次不续期标记 */
    uint32_t cseq;
    uint32_t reg_id;
    uint16_t q;           /* 0=未带 q；否则 0..1000 */
    uint16_t regs;
    /* RFC 7118：WS 注册的 Contact host 是 .invalid 假域名（浏览器随机生成），
     * 无法 DNS/sockaddr 路由 — 被叫 resolve 时直接用此 conn 走 WS 通道。
     * 生命周期：ws_listener conn 惰性回收（closed 标记不 free），
     * 发送前必须 ws_conn_alive() 校验（见 signaling.c resolve_cb）。 */
    void *ws_conn;

    /* === NAT 元数据（REQ-NAT-001）===
     * FreeSWITCH fs_nat=1 / OpenSIPS fix_nated_contact 等效。
     * registrar.c 在 REGISTER 处理时填充：NAT 检测算法 = Contact host RFC1918
     * 或 Via received 参数存在且与 Contact host 不同。 */
    char     received_ip[64];              /* Via received= 或 src IP（NAT 后真实到达） */
    uint16_t received_port;                /* Via rport= 或 src port */
    int      nat_detected;                 /* 1=检测到 NAT，0=公网直连 */
    char     public_contact[LOC_CONTACT_MAX]; /* NAT 改写后的可达 Contact（转发 INVITE 用这个） */
    int      transport_proto;              /* SIP_TR_UDP/TCP/TLS/WS/WSS */
} loc_binding_t;

typedef struct loc_db loc_db_t;

typedef struct {
    mem_arena_t *arena;
    int capacity;      /* 绑定总数上限，默认 100000 */
    int max_contacts;  /* 单 AoR Contact 上限，默认 10 */
} loc_db_cfg_t;

int loc_db_create(const loc_db_cfg_t *cfg, loc_db_t **out);
void loc_db_destroy(loc_db_t *db);

typedef enum {
    LOC_OK = 0,
    LOC_FULL,       /* 容量/单 AoR 上限 */
    LOC_STALE_CSEQ, /* Call-ID 相同但 CSeq 回退：400/401 口径由上层定 */
    LOC_NOENT
} loc_rc_t;

/* 增/改/刷新。expire_abs=0 表示立即过期（等价注销单 contact）。
 * 幂等键：contact 相同；或 instance 相同；或同 callid+cseq 刷新。 */
loc_rc_t loc_upsert(loc_db_t *db, const loc_binding_t *b, uint64_t now_ms);

/* 删单个 contact（contact=NULL 或 "*"：整 AoR 注销）。返回删除条数 */
int loc_remove(loc_db_t *db, const char *aor, const char *contact);

/* 查询有效绑定（按 q 降序）；返回复制条数 */
int loc_query(loc_db_t *db, const char *aor, loc_binding_t *out, int max,
              uint64_t now_ms);

/* 到期扫描（由 oam ticker 周期调用）；返回删除条数 */
int loc_sweep(loc_db_t *db, uint64_t now_ms);

uint64_t loc_db_count(loc_db_t *db);

/* 遍历所有有效（未过期）绑定；cb 返回非 0 截断（缓冲满场景）。
 * expired=0 条目跳过，回调与 loc_query 同口径。now_ms 取 nsbc_now_ms()。 */
typedef int (*loc_bind_iter_cb)(const loc_binding_t *b, void *ud);
int loc_db_foreach(loc_db_t *db, uint64_t now_ms,
                   loc_bind_iter_cb cb, void *ud);

#ifdef __cplusplus
}
#endif
#endif
