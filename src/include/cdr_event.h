#ifndef NSBC_CDR_EVENT_H
#define NSBC_CDR_EVENT_H

/* CDR 中立事件结构（REQ-PBX-CDR-001）
 *
 * 分层定位：框架白名单头（与 app_ctx 同族），sip/core 产生事件、
 * mod/billing 落库，双方只依赖本结构，sip 层不 include 任何 mod 头。
 * 时间戳口径：单调墙钟毫秒由 core 注入（nsbcd 主时钟，NTP 校时后
 * 统一）；billsec=应答→挂机秒，duration=起呼→挂机秒，未应答为 0。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDR_CALLID_MAX  40
#define CDR_NUM_MAX     48
#define CDR_TENANT_MAX  32
#define CDR_TRUNK_MAX   32
#define CDR_REC_MAX     160
#define CDR_CODEC_MAX   32   /* 编解码名（如 "PCMU" / "G.729" / "opus"） */
#define CDR_RTP_MAX     255  /* 上限钳位，防 bug 写超大值 */
#define CDR_IP_MAX      64   /* IP+port（IPv4=16 / IPv6=[::1]:12345≈64） */

typedef enum {
    CDR_DIR_INBOUND = 0,   /* 入中继 → 内部分机/业务 */
    CDR_DIR_OUTBOUND,      /* 内部分机 → 出中继 */
    CDR_DIR_INTERNAL,      /* 分机互拨（域内） */
    CDR_DIR_UNKNOWN
} cdr_dir_t;

typedef enum {
    CDR_PHASE_START = 0,   /* 收到/发出 INVITE */
    CDR_PHASE_ANSWER,      /* 200 OK ACK 完成 */
    CDR_PHASE_END          /* 呼叫释放（终态，写一行话单） */
} cdr_phase_t;

typedef enum {
    CDR_DISP_ANSWERED = 0,
    CDR_DISP_NOANSWER,
    CDR_DISP_BUSY,
    CDR_DISP_FAILED,
    CDR_DISP_ABANDONED     /* 主叫排队放弃 */
} cdr_disp_t;

typedef struct {
    char        call_id[CDR_CALLID_MAX];
    char        tenant[CDR_TENANT_MAX];
    char        from[CDR_NUM_MAX];
    char        to[CDR_NUM_MAX];
    char        trunk[CDR_TRUNK_MAX];  /* 出局/入局中继组名 */
    char        rec_path[CDR_REC_MAX]; /* 录音文件（无录音为空） */
    cdr_dir_t   dir;
    cdr_phase_t phase;
    cdr_disp_t  disposition;
    uint64_t    start_ms;
    uint64_t    answer_ms;   /* 0=未应答 */
    uint64_t    end_ms;
    uint32_t    billsec;
    uint32_t    duration_s;
    int         cause;       /* Q.850 释放原因 */
    int         emergency;   /* 紧急呼叫标记（对账留存，不随隐私策略删） */
    /* ---- 网络端点 ---- */
    char        from_ip[CDR_IP_MAX];   /* A 腿来源 IP（入向 INVITE 的 src） */
    char        a_peer_ip[CDR_IP_MAX]; /* A 腿 RTP peer（answer SDP c=IN IP4 ... m=... port） */
    char        b_peer_ip[CDR_IP_MAX]; /* B 腿 RTP peer（answer SDP c=IN IP4 ... m=... port） */
    /* REQ-CDR-001 媒体质量指标（零编译追加，calloc 自动归零→P0 未填时 0 合法） */
    char        codec_a[CDR_CODEC_MAX];  /* A 腿编解码（如 "PCMU"） */
    char        codec_b[CDR_CODEC_MAX];  /* B 腿编解码（bypass 时与 codec_a 同） */
    uint8_t     rtp_loss_pct;            /* 丢包率 %（0-100，CDR_RTP_MAX 钳位） */
    uint16_t    rtp_jitter_ms;           /* 平均抖动 ms */
    int32_t     mos_x1000;              /* MOS×1000（如 3.62→3620，-1=无效） */
    /* REQ-SIP-016 早期媒体追溯：early_bytes>0 表示存在早期媒体流量 */
    uint32_t    early_bytes;             /* 183/180 SDP 到 200 OK 之间累计字节 */
    uint32_t    early_duration_s;        /* 早期媒体时长秒（early_end_ms - 183_ms） */
} cdr_event_t;

/* cdr_event_t → JSON（malloc 字符串，调用方 free）。
 * cdr.c 内实现（cdr_event_to_json）；b2bua.c / signaling.c 的 UDS 推送点复用，
 * 避免另造一份残缺 snprintf 漏掉 codec / IP / 媒体指标等字段。
 * NULL 输入 → 返回 NULL。 */
char *cdr_event_to_json(const cdr_event_t *ev);

static inline const char *cdr_dir_name(cdr_dir_t d)
{
    switch (d) {
    case CDR_DIR_INBOUND:  return "inbound";
    case CDR_DIR_OUTBOUND: return "outbound";
    case CDR_DIR_INTERNAL: return "internal";
    case CDR_DIR_UNKNOWN:  break;
    }
    return "unknown";
}

static inline const char *cdr_phase_name(cdr_phase_t p)
{
    switch (p) {
    case CDR_PHASE_START:  return "start";
    case CDR_PHASE_ANSWER: return "answer";
    case CDR_PHASE_END:    return "end";
    }
    return "end";
}

static inline const char *cdr_disp_name(cdr_disp_t d)
{
    switch (d) {
    case CDR_DISP_ANSWERED:  return "answered";
    case CDR_DISP_NOANSWER:  return "noanswer";
    case CDR_DISP_BUSY:      return "busy";
    case CDR_DISP_FAILED:    return "failed";
    case CDR_DISP_ABANDONED: return "abandoned";
    }
    return "failed";
}

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CDR_EVENT_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
