#ifndef NSBC_CALL_SESSION_H
#define NSBC_CALL_SESSION_H

/* 呼叫会话管理（应用层）：把 SIP/SDP/RTP 解析与内存模块结合——
 *
 * - 收包缓冲 pktbuf 来自 MEM_DOMAIN_PACKET 的类型化 slab，引用计数；
 * - sip_msg_t / sdp_t 从信令域 slab 经 per-thread magazine 取放，
 *   逐包处理路径零 glibc malloc；
 *   RTP 解析结果视图留在调用方栈上（解析器自身零分配）；
 * - call_session 用 Call-ID 作稳定关联键，分片哈希桶 + per-session
 *   原子状态迁移；终止时先摘链、经 RCU 宽限期后归还 slab。
 *
 * 对应 NSBC详细说明书.md 7.5（对象生命周期）、7.1.5/7.1.2。
 */

#include "mem.h"
#include "rtp.h"
#include "sdp.h"
#include "sip_msg.h"

typedef struct call_mgr call_mgr_t;
typedef struct call_worker call_worker_t;
typedef struct call_session call_session_t;

/* ---- 呼叫状态机 ---- */
typedef enum {
    CALL_STATE_INIT = 0,        /* 初始 INVITE 已建会话，未收 1xx */
    CALL_STATE_TRYING,          /* 100 Trying */
    CALL_STATE_EARLY,           /* 180/183，对话已建（To-tag），可早期媒体 */
    CALL_STATE_CONNECTED,        /* 200 INVITE，正常通话 */
    CALL_STATE_HELD,            /* re-INVITE/UPDATE inactive/hold */
    CALL_STATE_TERMINATED        /* 已终止，等待 RCU 回收 */
} call_state_t;

/* ---- 协商媒体信息（字段全部拷贝，不悬挂指向收包缓冲的令牌） ---- */
typedef struct {
    int remote_port;            /* m= 端口，0=媒体被拒 */
    int rtcp_mux;               /* a=rtcp-mux */
    int secure;                 /* SRTP：a=fingerprint / a=crypto */
    int t38;                    /* T.38 */
    sdp_dir_t dir;              /* 最终方向 */
    char codec[4][24];          /* 协商编解码名（立即拷贝） */
    int ncodec;
} call_media_t;

/* ---- 管理器配置 ---- */
typedef struct {
    size_t signal_bytes;        /* 信令域 Arena，0 → 64MiB */
    size_t packet_bytes;        /* 报文域 Arena，0 → 128MiB */
    size_t media_bytes;         /* 媒体域 Arena，0 → 64MiB */
    int max_calls;              /* call_session 容量，0 → 4096 */
    int pktbuf_count;           /* 收包缓冲数量，0 → 2*max_calls */
} call_mgr_cfg_t;

call_mgr_t *call_mgr_create(const call_mgr_cfg_t *cfg);
void call_mgr_destroy(call_mgr_t *m);

/* ---- 工作线程：每线程一个（magazine + RCU 读者） ---- */
call_worker_t *call_worker_create(call_mgr_t *m);
void call_worker_destroy(call_worker_t *w);

/* 应用读侧临界区（查找/遍历会话期间持有，配置热更新场景） */
void call_worker_read_lock(call_worker_t *w);
void call_worker_read_unlock(call_worker_t *w);

/* ---- 报文分发：一条 UDP 报文进入系统 ---- */
typedef enum {
    CALL_EV_NONE = 0,       /* 无关报文/重传，状态不变 */
    CALL_EV_CREATED,        /* 初始 INVITE，会话建立 */
    CALL_EV_PROGRESS,       /* 100 Trying */
    CALL_EV_EARLY,          /* 180/183/PRACK，早期对话 */
    CALL_EV_CONNECTED,      /* 200 INVITE/ACK，通话接通 */
    CALL_EV_UPDATED,        /* re-INVITE/UPDATE 已应用 */
    CALL_EV_HELD,           /* 进入 hold */
    CALL_EV_RESUMED,        /* 退出 hold */
    CALL_EV_ENDED,          /* BYE/CANCEL 正常结束 */
    CALL_EV_FAILED,         /* 错误响应导致呼叫失败 */
    CALL_EV_BUSY,           /* 会话池耗尽，拒绝新呼叫 */
    CALL_EV_GLARE,          /* 与未决会话更新冲突（应回 491） */
    CALL_EV_BADMSG          /* 报文无法解析 */
} call_event_t;

typedef struct {
    call_event_t ev;
    call_state_t state;
    call_session_t *session;
} call_msg_result_t;

int call_worker_dispatch(call_worker_t *w, const void *wire, size_t len,
                         call_msg_result_t *out);

/* ---- 媒体处理：一个媒体包到达会话绑定端口 ---- */
typedef enum {
    MEDIA_DROP = 0,     /* 闸门未开/SSRC 伪造，丢弃 */
    MEDIA_EARLY,        /* 早期媒体，已转发 */
    MEDIA_FORWARD,      /* 正常媒体，已转发 */
    MEDIA_RTCP,         /* RTCP，交控制面 */
    MEDIA_OTHER         /* STUN/TURN/DTLS 等，交对应处理 */
} call_media_act_t;

call_media_act_t call_session_media(call_session_t *cs, const uint8_t *buf,
                                    size_t len);

/* ---- 查找与访问 ---- */
call_session_t *call_mgr_find(call_mgr_t *m, const char *callid, size_t len);
call_state_t call_session_state(const call_session_t *cs);
const char *call_session_callid(const call_session_t *cs);
const call_media_t *call_session_media_info(const call_session_t *cs);
uint64_t call_session_started_ms(const call_session_t *cs);

/* ---- 统计 ---- */
typedef struct {
    uint64_t active;    /* 在表未终止 */
    uint64_t peak;      /* active 峰值 */
    uint64_t created;
    uint64_t ended;
    uint64_t recycled;  /* 已度过宽限期归还 slab */
    uint64_t failed;
} call_mgr_stat_t;

void call_mgr_stat(const call_mgr_t *m, call_mgr_stat_t *out);

/* 显式驱动一次宽限期推进（RCU），返回 NSBC_OK/NSBC_ERR */
int call_mgr_advance(call_mgr_t *m);

#endif /* NSBC_CALL_SESSION_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
