#ifndef NSBC_ROUTE_TABLE_H
#define NSBC_ROUTE_TABLE_H

/* 中继与出局路由（REQ-PBX-ROUTE-002 中继对象 / 003 中继组选路）
 *
 * 现状：生产出局只在 core/signaling.c loc_query+字面直连，无中继
 * 概念，b2b_originate 硬编码 UDP。本头定义：
 *  - trunk：注册型（对端为注册服务器，本端发 REGISTER）/静态型
 *    （IP 白名单直送）/认证外呼（INVITE 带 Digest），传输 UDP/TCP/TLS；
 *  - trunk_group：成员按 priority+weight 排序，故障（503/超时/熔断）
 *    顺序切换，支持同优先级负载散列；
 *  - 与 dialplan 的衔接：dp_result_t.target=中继组名，由组合根查本表。
 * 分层：mod；只引 net 地址类型与 base。
 */
#include "nsbc_common.h"
#include "net_sock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_NAME_MAX     32
#define RT_HOST_MAX     128
#define RT_USER_MAX     64
#define RT_REALM_MAX    96
#define RT_GROUP_MAX    32
#define RT_CODECSET_MAX 32

typedef enum {
    TRUNK_STATIC_PEER = 0, /* 固定对端 IP，IP 白名单免鉴权 */
    TRUNK_REGISTER,        /* 本端作为 UAC 向对端注册 */
    TRUNK_AUTH_OUT         /* 发呼叫时被质询再鉴权（不预注册） */
} trunk_kind_t;

typedef struct {
    char          name[RT_NAME_MAX];
    trunk_kind_t  kind;
    int           proto;          /* sip_tr_proto_t：0 UDP/1 TCP/2 TLS */
    char          host[RT_HOST_MAX];
    uint16_t      port;
    char          auth_user[RT_USER_MAX];
    char          realm[RT_REALM_MAX];
    char          codec_set[RT_CODECSET_MAX]; /* 引用编解码策略名 */
    int           weight;         /* 同优先级权重 1..100 */
    int           priority;       /* 小值优先（主/备） */
    int           enabled;
    int           max_channels;   /* 并发上限（0=不限） */
    int           failover_code;  /* 触发切换的应答码掩码组（配置位） */
    /* Advisor 2026-10-01l：DNS 预解析缓存（rt_trunk_add 时填充，
     * resolve_cb OUT 路径直接 memcpy 用、跳过阻塞 getaddrinfo）。
     * len==0 表示未预解析（host 是动态 DNS → 留空 fallback 阻塞解析）。 */
    nsbc_sa_t     cached_sa;
    char          profile_name[RT_NAME_MAX]; /* 绑定的 SIP Profile（Web DB trunks.profile_name 外键） */

    /* === FreeSWITCH gateway 兼容字段（文档 §1.1） === */
    char          password[128];          /* 明文密码（Web 侧 reload 时传入） */
    char          from_user[RT_USER_MAX];  /* From 头用户名覆盖（空=auth_user） */
    char          from_domain[RT_HOST_MAX]; /* From 头域覆盖（空=走回退链） */
    char          proxy[RT_HOST_MAX];      /* 代理主机（空=同 host） */
    char          register_proxy[RT_HOST_MAX]; /* 注册专用代理（空=同 proxy） */
    uint32_t      expire_seconds;          /* 默认 3600 */
    uint32_t      retry_seconds;           /* 退避基数，默认 5s（§3.4） */
    uint8_t       caller_id_in_from;       /* 呼出 From user 部分用入站 callerid */
    char          contact_params[128];      /* Contact 额外参数 */
    uint8_t       extension_in_contact;    /* Contact 带 extension */
    uint32_t      ping_seconds;            /* OPTIONS 健康探测间隔，0=关闭 */
    char          inbound_extension[RT_USER_MAX]; /* 入站 DID 映射目标 */
    char          cid_type[32];             /* rpid 等预留 */
    uint8_t       rfc5626_enabled;          /* RFC 5626 Outbound */
    char          reg_id[32];               /* RFC 5626 reg-id */
    char          peer_ip[256];             /* 源 IP 白名单（CIDR 列表，专家 1.4） */
    /* === 用户新增 === */
    char          caller_id_pool[2048];     /* 逗号分隔号码池（如 "138xxxxxx,139xxxxxx"） */
    uint32_t      per_number_max_channels;  /* 单号码最大并发（0=不限） */
    /* 自定义 SIP 头（REQ-TRUNK-HDR-001）：出站 INVITE 时拼到 extra 头区。
     * 支持变量替换：${callid} ${caller} ${callee} ${from_user} ${to_user}
     * 示例："MyID=${callid}\r\nX-Biz-${callee}"
     * 每条头必须自带 \r\n 结尾；多头发在一个字符串里用 \r\n 分隔。 */
    char          custom_headers[1024];

    /* === 3GPP IMS 自动头注入（REQ-TRUNK-IMS-001） ===
     * ims_enabled=1 时，出站 INVITE 构建 extra 头块自动拼：
     *   P-Asserted-Identity: <sip:{auth_user}@{ims_domain}>   （强制带 PAI，运营商计费必需）
     *   P-Preferred-Identity: <sip:{auth_user}@{ims_domain}> （同 PAI，展示主叫）
     *   P-Called-Party-ID: <sip:{to_user}@{ims_domain}>       （被叫 E.164，IMS 必需）
     *   P-Access-Network-Info: E-UTRAN-TDD; ...               （自动或用户在 custom_headers 配）
     *   Privacy: id                                            （CLIR，可选）
     *
     * 拨号规则 action_json.sip_invite_* 会拼在 trunk 自动头**之后**——规则级配置优先。 */
    uint8_t       ims_enabled;
    char          ims_domain[128];       /* IMS 根域，如 "ims.mnc001.mcc460.3gppnetwork.org" */
    uint8_t       ims_auto_privacy;      /* 1=自动加 Privacy: id（CLIR，对端 From anonymize） */
    uint8_t       ims_auto_pani;         /* 1=自动加 P-Access-Network-Info（默认 "E-UTRAN-TDD"） */
    char          ims_pani_value[128];   /* 自定义 PANI 值，空=默认 "E-UTRAN-TDD" */
    /* === 3GPP IMS 计费头（RFC 7315） ===
     * 运营商核心网计费系统用这两个头关联在线计费会话。 */
    uint8_t       ims_auto_charging;                    /* 1=自动拼 P-Charging-Function-Addresses / Vector */
    char          ims_pcfa[128];                        /* P-Charging-Function-Addresses 值，如 "ccf=a;ecf=b" */
    char          ims_pcv[64];                          /* P-Charging-Vector 值，如 "icid-xxxx;orig-ioi=ims.mnc001.mcc460.3gppnetwork.org" */
} trunk_t;

typedef struct {
    char     name[RT_NAME_MAX];
    char     members[RT_GROUP_MAX][RT_NAME_MAX];
    int      nmembers;
    int      sticky_by_caller;   /* 主叫粘性（同主叫尽量同中继） */
    uint64_t in_use;             /* 当前占用通道（OAM/选路） */
} trunk_group_t;

typedef struct route_table route_table_t;

int  rt_create(route_table_t **out);
void rt_destroy(route_table_t *t);

int  rt_trunk_add(route_table_t *t, const trunk_t *tr);
int  rt_trunk_remove(route_table_t *t, const char *name);
const trunk_t *rt_trunk_get(route_table_t *t, const char *name);

/* ---------- 呼入 trunk 四层匹配（P2-14，文档 §4.2.1 层 2/3/4）----------
 *
 * 层 2：TRUNK_AUTH_OUT + src_ip ∈ peer_ip CIDR 列表 → 命中
 * 层 3：TRUNK_STATIC_PEER + src_ip == trunk.host（端口忽略） → 命中
 * 层 4：回退——任意 trunk host IP 匹配，多 trunk 命中取 priority 最小的
 *
 * 注意：层 1（register 反向关联 auth_user → To/PAI）在 outreg 侧，
 *   调用方（handle_new_invite）先调 outreg_find_by_auth_user，
 *   没命中再调此函数。
 *
 * 返回 NULL 表示全部未命中（走 DP → EXT 层正常分机路由）。
 * src_ip 为 NULL/空串时跳过层 2/3/4（无法按 IP 匹配）。 */
const trunk_t *rt_match_inbound_trunk(route_table_t *t, const char *src_ip);

int  rt_group_add(route_table_t *t, const trunk_group_t *g);
const trunk_group_t *rt_group_get(route_table_t *t, const char *name);

/* 选路：按组 priority/weight 选一个 enabled 且未熔断的中继；
 * fail_reason 为上次失败应答码（0 首次），用于跳过熔断成员；
 * 选中后调用方 rt_acquire/release 维护 in_use。 */
const trunk_t *rt_select(route_table_t *t, const char *group,
                         const char *caller, int fail_reason);
void rt_acquire(route_table_t *t, const char *name);
void rt_release(route_table_t *t, const char *name);

/* 熔断：连续失败计数达阈值标熔断（粗骨架仅状态，计时恢复 P1） */
void rt_mark_fail(route_table_t *t, const char *name);
void rt_mark_ok(route_table_t *t, const char *name);
int  rt_is_open(route_table_t *t, const char *name);

/* OAM 遍历接口（trunks.list 诊断命令用）：
 * rt_trunk_count 返回当前 route_table 中的 trunk 数量；
 * rt_trunk_at 按索引返回 trunk 指针（0..count-1），越界返回 NULL。
 * 注意：route_table 是 RCU 原子替换的——调用前应由调用方保证
 * ctx->rt 在遍历期间不被替换（或持有 snapshot）。 */
int  rt_trunk_count(const route_table_t *t);
const trunk_t *rt_trunk_at(const route_table_t *t, int idx);
int  rt_group_count(const route_table_t *t);
const trunk_group_t *rt_group_at(const route_table_t *t, int idx);
uint64_t rt_trunk_in_use(const route_table_t *t, int idx);
int  rt_trunk_is_open(const route_table_t *t, int idx);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_ROUTE_TABLE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
