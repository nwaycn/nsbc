#ifndef NSBC_SIP_REFER_H
#define NSBC_SIP_REFER_H

/* REFER 信令原语（RFC 3515）+ Replaces（RFC 3891）+
 * Referred-By（RFC 3892）+ 显式无订阅（RFC 4488 norefersub）
 *
 * 对应：REQ-SIP-002（REFER 方法族，当前 b2bua 裸回 501）、
 * REQ-PBX-SS-001（盲转/咨询转的信令载体，业务策略在 mod/applications，
 * 本头只提供无业务语义的原语，sip 层不得 include mod）。
 *
 * REFER 隐含创建 event=refer 的订阅（RFC6665 框架），转接触发结果
 * 经 NOTIFY sipfrag（100 Trying/200 OK）回送；Replaces 头用于咨询转
 * 接续已存在对话（attended transfer）。
 */
#include "nsbc_common.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REFER_TARGET_MAX   192  /* 对齐 dlg DLG_URI_MAX，不引对话头 */
#define REFER_CID_MAX      128
#define REFER_TAG_MAX      64

typedef enum {
    REFER_ST_NONE = 0,
    REFER_ST_RCVD,       /* 收到 REFER，202 Accepted */
    REFER_ST_STARTED,    /* 已向目标发起 INVITE（100 sipfrag） */
    REFER_ST_ALERTING,   /* 180/183 sipfrag */
    REFER_ST_CONNECTED,  /* 200 sipfrag → 经 BYE 拆 REFER 订阅 */
    REFER_ST_FAILED      /* 4xx/5xx/6xx sipfrag */
} refer_state_t;

/* Replaces 头拆分结果（RFC 3891 §3）：call-id;to-tag=;from-tag= */
typedef struct {
    sip_tok_t callid;
    sip_tok_t to_tag;
    sip_tok_t from_tag;
    int       early_only; /* early-only 参数 */
} refer_replaces_t;

/* 一次 REFER 请求的解析视图（零拷贝，随报文生命周期） */
typedef struct {
    sip_tok_t        refer_to;    /* Refer-To 目标 URI（含 Require?） */
    sip_tok_t        referred_by; /* Referred-By（可空） */
    refer_replaces_t replaces;    /* Refer-To 参数 ?Replaces= 解出，可空 */
    int              has_replaces;
    int              no_refersub; /* Refer-To 参数 norefersub（RFC4488） */
} refer_req_t;

/* 从已解析消息抽 REFER 视图（失败 PROTO：缺 Refer-To） */
int  refer_parse(const sip_msg_t *msg, refer_req_t *out);

/* 从 Refer-To URI 参数或独立 Replaces 头解析三元组 */
int  refer_parse_replaces(const sip_tok_t *param, refer_replaces_t *out);

/* NOTIFY sipfrag 正文生成："SIP/2.0 <code> <reason>"（有界写入） */
int  refer_sipfrag_render(int status, const char *reason,
                          char *buf, size_t cap, size_t *out_len);

/* REFER 状态迁移合法性（非法迁移返回 INVAL） */
int  refer_state_next(refer_state_t cur, refer_state_t want);

/* REFER 资源是否匹配（Replaces 三元组与现存对话比对，零拷贝） */
int  refer_replaces_match(const refer_replaces_t *r,
                          const char *callid, size_t cid_len,
                          const char *to_tag, size_t tt_len,
                          const char *from_tag, size_t ft_len);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SIP_REFER_H */
