/* RFC 6665 事件通知框架——粗骨架（REQ-SIP-002 部分）
 *
 * P0 阶段：仅类型/API 轮廓与模块边界，全部返回 NOTSUP，不接 b2bua。
 * P1 接线：on_subscribe 在 b2bua 方法分发替换 501（b2bua.c SUBSCRIBE 分支），
 * 订阅表走 arena+slab+分片锁（同 location_db 模式），到期由时间轮扫描；
 * BLF 事件包（REQ-PBX-PRES-001）在 mod/presence 经描述符登记，sip 层
 * 不反向依赖 mod（REQ-7.8 依赖方向）。 */
#include "sip_event.h"

struct sip_ev_mgr {
    sip_ev_cfg_t cfg;
    unsigned     inited;
    uint64_t     active;
};

int sip_ev_mgr_create(const sip_ev_cfg_t *cfg, sip_ev_mgr_t **out)
{
    sip_ev_mgr_t *m;
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    m = (sip_ev_mgr_t *)calloc(1, sizeof(*m));
    if (m == NULL)
        return NSBC_NOMEM;
    m->cfg = *cfg;
    m->inited = 1u;
    *out = m;
    return NSBC_OK;
}

void sip_ev_mgr_destroy(sip_ev_mgr_t *m)
{
    free(m);
}

int sip_ev_pkg_register(sip_ev_mgr_t *m, const sip_evpkg_t *pkg)
{
    /* TODO(P1, REQ-SIP-002)：包名哈希表登记，重名 NSBC_EXISTS */
    if (m == NULL || pkg == NULL)
        return NSBC_INVAL;
    (void)m;
    (void)pkg;
    return NSBC_NOTSUP;
}

int sip_ev_on_subscribe(sip_ev_mgr_t *m, const sip_msg_t *msg)
{
    /* TODO(P1, RFC 6665)：Event 头必填（489 Bad Event）、Expires 裁剪、
     * 200/202 + NOTIFY 首报、鉴权 401/403、订阅刷新与退订。 */
    if (m == NULL || msg == NULL)
        return NSBC_INVAL;
    return NSBC_NOTSUP;
}

int sip_ev_on_notify(sip_ev_mgr_t *m, const sip_msg_t *msg)
{
    /* TODO(P1, RFC 6665)：Subscription-State 解析、版本单调检查、
     * 481（无此订阅）/200、terminated 清理。 */
    if (m == NULL || msg == NULL)
        return NSBC_INVAL;
    return NSBC_NOTSUP;
}

int sip_ev_tick(sip_ev_mgr_t *m, uint64_t now_ms, uint32_t *expired_n)
{
    if (m == NULL)
        return NSBC_INVAL;
    (void)now_ms;
    if (expired_n != NULL)
        *expired_n = 0u;
    return NSBC_OK;
}

uint64_t sip_ev_active(sip_ev_mgr_t *m)
{
    return (m != NULL) ? m->active : 0u;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
