/* 拓扑隐藏——粗骨架（REQ-SEC-004）
 *
 * 现状：策略对象与判定为纯逻辑；消息面改写依赖 sip_build 序列化层
 * 的"删头/改 URI host"原语（尚未导出），故需要改写时返回 NOTSUP，
 * 由组合根在装配批次接通，届时去掉本文件 NOTSUP 分支。
 * 未配置任何改写时返回 OK（直通），保证默认路径可调用。
 */
#include "topo_hide.h"

#define TOPO_DEFAULT_UA "SIP/2.0 Edge"

void topo_policy_default(topo_pol_t *p)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));
    p->strip_via_branch = 1;
    p->strip_record_route = 1;
    p->rewrite_contact = 1;
    p->strip_server_ua = 1;
    p->strip_p_headers = 1;
    p->privacy_id = 0;              /* 按呼叫 Privacy 头决定，不强制 */
    snprintf(p->user_agent, sizeof(p->user_agent), "%s", TOPO_DEFAULT_UA);
}

static int any_mutation(const topo_pol_t *p)
{
    return p->strip_via_branch || p->strip_record_route ||
           p->rewrite_contact || p->strip_server_ua ||
           p->strip_p_headers || p->privacy_id ||
           p->edge_name[0] != '\0';
}

int topo_ingress(sip_msg_t *msg, const topo_pol_t *p, int trusted)
{
    (void)msg;
    if (p == NULL)
        return NSBC_INVAL;
    if (trusted) {
        /* 可信域间中继：保留 PAI/PPAI 与 RR（码号与 in-dialog 路由依赖），
         * 仍剥 Server/UA 指纹项。 */
        if (!p->strip_server_ua)
            return NSBC_OK;
        return NSBC_NOTSUP; /* TODO：仅删 Server/UA，sip_build 原语 */
    }
    if (!any_mutation(p))
        return NSBC_OK;
    /* TODO(接线批)：
     *  - 删非边缘 Via 之外的内部 Via 参数（received/rport 保留首跳）；
     *  - 删内部 Record-Route/Contact 私网地址（改边缘由 egress 统一）；
     *  - 删 Server/User-Agent/Organization 指纹；
     *  - strip_p_headers 删 PAI/PPAI/P-Early-Media（不可信来流）。 */
    return NSBC_NOTSUP;
}

int topo_egress(sip_msg_t *msg, const topo_pol_t *p, int trusted)
{
    if (p == NULL || msg == NULL)
        return NSBC_INVAL;
    if (trusted && !p->strip_server_ua)
        return NSBC_OK;
    if (!any_mutation(p))
        return NSBC_OK;
    /* TODO(接线批)：
     *  - rewrite_contact：Contact/Record-Route host 统一 edge_name，
     *    边缘按 Record-Route 维持 in-dialog 信令可达；
     *  - privacy_id：Privacy:id 呼叫剥 PAI 或换匿名 PAI；
     *  - 统一 User-Agent（消息构建侧 sip_build 直接写 topo_user_agent）。 */
    return NSBC_NOTSUP;
}

const char *topo_user_agent(const topo_pol_t *p)
{
    if (p != NULL && p->user_agent[0] != '\0')
        return p->user_agent;
    return TOPO_DEFAULT_UA;
}

int topo_strip_header(sip_msg_t *msg, sip_hdr_type_t ht)
{
    const sip_hdr_t *h;
    int n = 0;
    if (msg == NULL || ht <= SIP_H_UNKNOWN || ht >= SIP_H__COUNT)
        return NSBC_INVAL;
    for (h = sip_hdr_get(msg, ht); h != NULL; h = sip_hdr_next(msg, h))
        ++n;
    if (n == 0)
        return NSBC_NOENT;
    /* TODO(接线批)：sip_build 序列化删头/重建 raw 后改返回删除数。
     * 当前只完成存在性扫描，返回 NOTSUP 提示调用方未真正改写。 */
    return NSBC_NOTSUP;
}
