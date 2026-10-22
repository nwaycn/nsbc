#ifndef NSBC_REGISTRAR_H
#define NSBC_REGISTRAR_H

/* 注册处理核心（REQ-REG / 说明书 6.1.2，P0）
 *
 * - REGISTER 无 Authorization → 401 + RFC2617 Digest 质询（nonce 签名）；
 * - HA1 由 endpoints 凭证模块回调提供，明文密码不入本模块；
 * - 注册风暴防护：IP / AoR / 全局三级令牌桶，超限 503 + Retry-After；
 * - 绑定增删改/Contact:* 注销/幂等刷新，过期时间由位置库统一扫描；
 * - 423 Interval Too Brief：expires 小于 min_expires。
 *
 * 无状态应答（REGISTER 经 UDP，重传幂等：无凭证重质询、有凭证幂等刷新）。
 */
#include "location_db.h"
#include "sip_digest.h"
#include "sip_msg.h"
#include "sip_transport.h"
#include "sip_txn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct reg_core reg_core_t;

typedef struct {
    loc_db_t *db;
    sip_transport_t *tr;
    digest_ha1_fn ha1_cb;
    void *ha1_ud;
    const char *realm;
    const char *digest_secret;
    uint64_t nonce_ttl_ms;
    int min_expires;     /* 默认 60s */
    int max_expires;     /* 默认 3600s */
    int default_expires; /* 默认 3600s */
    int ip_cps;          /* 单 IP 每秒 REGISTER 上限，默认 50 */
    int aor_cps;         /* 单 AoR 每秒上限，默认 10 */
    int global_cps;      /* 全局每秒上限，默认 100000 */
    int retry_after_s;
} reg_cfg_t;

int reg_core_create(const reg_cfg_t *cfg, reg_core_t **out);
void reg_core_destroy(reg_core_t *r);

/* 直接作为 b2b_cfg_t.on_register 挂接（ud 必须为 reg_core_t*）
 * txn 是 server 侧事务指针，用于 txn_send_response 让 txn 自动吸收重传 */
int reg_on_register(sip_tr_proto_t proto, const nsbc_sa_t *src,
                    sip_msg_t *msg, const char *raw, size_t rawlen,
                    sip_txn_t *txn, void *ud);

/* OBS 计数 */
uint64_t reg_total(reg_core_t *r);
uint64_t reg_rejected(reg_core_t *r);
uint64_t reg_throttled(reg_core_t *r);

#ifdef __cplusplus
}
#endif
#endif
