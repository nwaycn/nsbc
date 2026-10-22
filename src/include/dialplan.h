#ifndef NSBC_DIALPLAN_H
#define NSBC_DIALPLAN_H

#include "patmatch.h"

/* 对应 REQ-PBX-DP-001：分层 Dialplan + REQ-DP-APP-001：Application 链 */
typedef enum {
    DP_LAYER_EXT = 0, /* 分机内呼 */
    DP_LAYER_BIZ,     /* 企业业务号：IVR/会议/队列/语音信箱 */
    DP_LAYER_OUT,     /* 出局路由（中继组） */
    DP_LAYER_CNT
} dp_layer_t;

/* REQ-DP-SIP-001：拨号规则级 SIP 头改写容器。
 *
 * 所有 DP 命中的 SIP 头 override 都存在这里，管道末端（b2bua B-leg INVITE 构建）
 * 调 b2b_apply_hdr_rewrite() 统一合成最终头值。这样做的好处：
 *   1. 单结构体贯穿 dp_rule_t → dp_result_t → b2b_call_t，管道零散字段维护；
 *   2. UI action_json 直接存同名键，前端零特殊映射；
 *   3. b2b_apply_hdr_rewrite() 里有清晰优先级链（full_uri > component > domain > 透传）。
 *
 * —— 字段分组（对齐 FreeSWITCH sofia_glue.c 1173-1184） ——
 * From 组：   sip_invite_from (完整 From 值) > sip_invite_from_uri (纯 URI 覆盖)
 *             > sip_from_uri + sip_from_display (组件覆盖) > clip (legacy)
 *             最后追加 sip_invite_from_params
 * To 组：     sip_invite_to_uri (完整 To 覆盖) + sip_invite_to_params
 * R-URI 组：  sip_invite_req_uri (完整 R-URI) + sip_invite_tel_params (tel: 追加)
 * Contact 组：sip_invite_contact_params (追加到 Contact)
 * Domain 组： sip_invite_domain (强制覆盖所有 URI 的 host:port)
 * Extra 组：  sip_invite_params (已格式化额外头块，带 \r\n) +
 *             sip_invite_pid_params (P-Asserted-Identity 完整头行) */
typedef struct sip_hdr_rewrite {
    /* From 头 */
    char sip_invite_from[128];      /* 完整 From 值覆盖（含 display、URI、params） */
    char sip_invite_from_uri[128];  /* 纯 URI 覆盖（对应 FS sip_invite_from_uri） */
    char sip_from_uri[128];         /* 组件级：仅 URI 部分 */
    char sip_from_display[128];     /* 组件级：仅 display 部分 */
    char sip_invite_from_params[128]; /* 追加到 From 头的 params */
    /* To 头 */
    char sip_invite_to_uri[128];
    char sip_invite_to_params[128];
    /* Request-URI */
    char sip_invite_req_uri[128];
    char sip_invite_tel_params[128];
    /* Contact */
    char sip_invite_contact_params[128];
    /* Domain override */
    char sip_invite_domain[64];
    /* Extra headers */
    char sip_invite_params[512];  /* 3GPP IMS 场景需要装 PAI+PPID+PANI+PCPI 多行头块 */
    char sip_invite_pid_params[256];
} sip_hdr_rewrite_t;

/* REQ-DP-APP-001：Application 链条目定义在 app_registry.h 的
 * `struct dp_app_action`（本文件只做前向声明，避免循环包含） */
struct dp_app_action;

typedef struct dp_rule {
    uint32_t id;
    dp_layer_t layer;       /* ⚠️ 保留但废弃：reload JSON 兼容旧数据；dp_execute 里由 target 前缀自动推断 */
    pm_pattern_t match;
    int prio;              /* 同 profile 内小值优先 */
    uint8_t strip;         /* 去掉被叫前导位数 */
    char add_prefix[48];  /* 被叫追加前缀 */
    char target[64];      /* 目的地：extension:<num> / queue:<name> / ivr:<name> / gateway:<name> / route:<name> / hangup */
    char dst_override[64]; /* 被叫号码覆盖（空=用 strip/add_prefix 结果；可写 ${1}/${2} 或纯字面量）
                             * ★ 关键设计：与 target 解耦——target 只管路由，dst_override 只管被叫号码
                             * 这样 UI 不用自动生成组合选项（避免笛卡尔积爆炸） */
    char clip[48];        /* 强制主叫 CLIP，空串不改写（legacy，等价 sip_from_uri 仅填 user） */
    /* === FreeSWITCH 风格精细化主叫覆盖 ===
     * 比 clip 更灵活：clip 等价 sip_from_uri 填完整 "sip:xxx"，
     * 而 orig_caller_id_num 只填 URI user part（host 自动走 trunk.resolve_from_host 或 local_host），
     * orig_caller_id_name 单独指定 Display name。
     * 优先级：sip_invite_from/_uri/sip_from_uri > orig_caller_id_num > clip > 透传 */
    char orig_caller_id_num[48];  /* FreeSWITCH origination_caller_id_number — 纯 user part，host 自动 */
    char orig_caller_id_name[48]; /* FreeSWITCH origination_caller_id_name — Display name，空→同 num */
    uint8_t perm;         /* 所需最低出局权限等级 */
    uint8_t immediate;    /* DigitMap 收齐即匹配 */
    uint8_t enabled;
    /* 业务动作（REQ-REC-003）：0=none, 1=recording=start, 2=recording=stop */
    uint8_t rec_action;
    /* §18 可选录音路径模板，空串=系统默认 yyyymmdd/callid_mix_seq.wav
     * 变量替换：{caller} {callee} {callid} {date} {time} {mix} */
    char rec_path_template[128];
    /* REQ-DP-SIP-001：SIP 头重写（统一容器，从 action_json 解析） */
    sip_hdr_rewrite_t hdr_rw;
    /* REQ-DP-APP-001：Application 链（JSON 字段 "app_chain"）。
     * 先执行 action_json 号码变换（strip/add_prefix/clip/perm/immediate），
     * 后按 app_chain 顺序依次调用应用。
     * RCU 热加载：指针由 reload 分配 RCU 可释放内存，旧表释放时统一回收。 */
    struct dp_app_action *app_chain;
    size_t app_chain_n;

    /* ====== §15 新增（Profile 强制分区 + 规则级 IP 准入） ====== */
    char profile_name[32];   /* 归属的 SIP Profile；空串="" 异常值 */
    char src_ip_cidr[64];    /* 规则级 IP 准入 CIDR，默认 "0.0.0.0/0" */

    /* ====== §XX 媒体模式控制（REQ-MEDIA-DP-001） ======
     * 覆盖 media_bridge 自动决策。仅对非 WebRTC 有效（WebRTC 必须走 SRTP_RELAY，
     * 强制 bypass 会导致 DTLS 无法终结 → 通话无媒体）。
     * 取值：0=auto（默认，按现有逻辑自动决策）
     *       1=bypass（媒体绕过 SBC，双方直连 RTP）
     *       2=relay  （强制 SBC 转发媒体，即使是私网也不让 bypass） */
    uint8_t media_mode;
} dp_rule_t;

typedef struct dp_table {
    dp_rule_t *rules;
    size_t count;
    size_t cap;
} dp_table_t;

typedef struct dp_ctx {
    /* 热加载活动表；读写分离。真实实现以 liburcu 管理宽限期与回收 */
    dp_table_t *active;
} dp_ctx_t;

typedef struct {
    const char *called;
    const char *caller;
    uint8_t perm;

    /* ====== §15 新增 ====== */
    const char *profile_name;   /* 来源 SIP Profile（由 sip_profile_match_src 或 transport 入端口识别） */
    const char *src_ip;         /* INVITE 源 IP（CIDR 匹配用） */

    /* REQ-DP-APP-001：不透明 call_key → b2b_call_t*
     * 传递给 app_ctx_t.call_key，供 Application 执行时访问 call_session */
    void *call_key;
} dp_call_t;

typedef struct {
    dp_layer_t layer;        /* 由 dp_execute 根据 target 前缀自动推断（不再从 dp_rule_t 复制） */
    uint32_t rule_id;
    char dst_number[64]; /* 号码变换后的被叫 */
    char clip[48];       /* 生效主叫（legacy CLIP） */
    char orig_caller_id_num[48];  /* FreeSWITCH origination_caller_id_number — 纯 user part，host 自动 */
    char orig_caller_id_name[48]; /* FreeSWITCH origination_caller_id_name — Display name */
    char target[64];     /* 目的地：分机/业务号/中继组（变量替换后的最终值） */
    uint8_t rec_action;  /* 由命中规则透传：0=none, 1=start, 2=stop */
    char rec_path_template[128]; /* §18 路径模板透传，空=系统默认 */
    sip_hdr_rewrite_t hdr_rw; /* REQ-DP-SIP-001：SIP 头重写（命中规则全量透传） */

    /* Advisor 2026-10-12n: 正则捕获组（pm_match_groups 输出）
     * groups[0] = 完整匹配串 ($0), groups[1..] = 正则捕获组 ($1, $2...)
     * 用于 dp_rule.target / add_prefix / clip / hdr_rw 等字段里的 ${N} 变量替换 */
    char groups[PM_MAX_GROUPS][PM_GROUP_SZ];

    /* ====== §15 新增 ====== */
    char profile_name[32];   /* 命中的规则归属哪个 profile（日志/CDR 记录） */

    /* ====== §XX 媒体模式（REQ-MEDIA-DP-001） ======
     * 从命中的规则透传，media_bridge 在 SDP 决策时 override：
     *   0=auto  按现有逻辑（offer 有 WebRTC → SRTP_RELAY，私网 → BYPASS 等）
     *   1=bypass 强制媒体绕过 SBC，双方直连（仅非 WebRTC 有效）
     *   2=relay  强制 SBC 转发媒体，禁止直连 */
    uint8_t media_mode;
} dp_result_t;

int dp_ctx_init(dp_ctx_t *ctx);
void dp_ctx_fini(dp_ctx_t *ctx);

/* 配置构建期接口（管理面） */
int dp_table_init(dp_table_t *t, size_t cap);
int dp_rule_add(dp_table_t *t, dp_rule_t *rule);
int dp_table_sort(dp_table_t *t);
void dp_table_free(dp_table_t *t);

/* 热加载：原子发布新表，旧表通过 old 返回，由调用方在宽限期后释放 */
int dp_ctx_publish(dp_ctx_t *ctx, dp_table_t *new_table, dp_table_t **old);

/* 业务执行期接口（数据面，无锁读） */
int dp_execute(const dp_ctx_t *ctx, const dp_call_t *call, dp_result_t *res);

/* DigitMap 辅助：是否存在 immediate 的精确命中 */
const dp_rule_t *dp_find_immediate(const dp_ctx_t *ctx, const char *digits);

#endif /* NSBC_DIALPLAN_H */
