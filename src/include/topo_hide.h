#ifndef NSBC_TOPO_HIDE_H
#define NSBC_TOPO_HIDE_H

/* 拓扑隐藏（REQ-SEC-004，P1；UA 伪装配置 P0）
 *
 * 入向（ingress，来自不可信域）：剥离上游私网 Via 分支参数/Received、
 * Record-Route 中的内网地址、Server/User-Agent、私有 P-* 头；
 * 出向（egress）：Contact/Record-Route 统一改写为对外边缘名（PBX/
 * 分机用户数/主机名不外泄），按 Privacy:id 策略剥 PAI/PPAI。
 * 对可信中继（域间白名单）不剥 PAI（来号码需要），按 trunk 配置豁免。
 * 分层：sip（2）；纯消息改写，无网络/业务依赖。
 */
#include "nsbc_common.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOPO_NAME_MAX 96

typedef struct {
    int  strip_via_branch;       /* 剥内部 Via 分支拓扑参数 */
    int  strip_record_route;     /* 出向隐藏内部 RR，仅留边缘 */
    int  rewrite_contact;         /* Contact host→边缘对外名 */
    int  strip_server_ua;
    int  strip_p_headers;        /* P-Asserted/P-Preferred 等私有头 */
    int  privacy_id;              /* 强制 Privacy: id 语义 */
    char edge_name[TOPO_NAME_MAX]; /* 对外边缘标识（SIP URI host） */
    char user_agent[TOPO_NAME_MAX];/* 全局 UA 串（cfg sip.user_agent） */
} topo_pol_t;

/* 缺省策略（配置缺失时的安全默认：全隐藏 + 通用 UA 占位） */
void topo_policy_default(topo_pol_t *p);

/* 入向清洗：trusted=1 表示对端为可信域间中继（保留 PAI，不改 RR） */
int topo_ingress(sip_msg_t *msg, const topo_pol_t *p, int trusted);
/* 出向改写：在消息发送前统一出口形态 */
int topo_egress(sip_msg_t *msg, const topo_pol_t *p, int trusted);

/* UA 串选择：配置空→固定中性串（不暴露产品/版本） */
const char *topo_user_agent(const topo_pol_t *p);

/* 单头剥离辅助（按头类型，粗骨架：遍历删除标记，真正的 buffer
 * 重写由 sip_build 序列化层在接线批实现） */
int topo_strip_header(sip_msg_t *msg, sip_hdr_type_t ht);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_TOPO_HIDE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
