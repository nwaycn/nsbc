/* media_bug.h — 通用媒体拦截框架（REQ-MEDIA-BUG-001）
 *
 * 对标 FreeSWITCH uuid_media_bug：relay_recv_one 热路径 RCU bug 链。
 *
 * 设计约束（强制，违反即编译不过/运行崩溃）：
 * 1. 回调在 RTP relay 热路径（每包调用），**必须无锁**：
 *    call_session 持有 bug 列表指针经 __atomic_store_n 发布，
 *    relay 线程侧 __atomic_load_n 读指针无原子；attach/detach 在
 *    控制线程（OAM/Web）改另一个 buffer，双 buffer + active_idx 切换。
 * 2. 回调**禁止阻塞**：consumer 自己 MPSC/SPSC ring buffer 解耦，
 *    回调只做 ring_push（或 memcpy + 投递），主路径立即返回。
 * 3. 回调**禁止磁盘 I/O/系统调用**。
 * 4. frame 缓冲从媒体 Arena/对象池取，**严禁热路径 malloc**。
 */
#ifndef NSBC_MEDIA_BUG_H
#define NSBC_MEDIA_BUG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bug 类型枚举 */
typedef enum {
    BUG_RAW_RTP        = 0x01, /* 完整 RTP 帧（12B头+payload+padding）——rtp_parse 之前 hook */
    BUG_RTP_HEADER_ONLY = 0x02, /* 仅 RTP 固定头+扩展头（不含 payload）——rtp_parse 之后 hook */
    BUG_RTCP_MIRROR    = 0x04, /* RTCP 报文镜像 */
    BUG_SIPCAPTURE_TAP = 0x08, /* SIP 消息旁路（txn 状态机挂接，不进 relay） */
} media_bug_type_t;

/* 帧描述：回调输入参数（consumer 只读） */
typedef struct {
    void *call_key;           /* b2b_call_t*（不透明，仅做表键比较） */
    int   leg;                /* 0=A腿 1=B腿 */
    media_bug_type_t type;
    uint8_t  *data;           /* 帧数据（完整 RTP / RTCP / SIP） */
    size_t    data_len;       /* data 字节数 */
    /* RTP header fields（BUG_RAW_RTP/BUG_RTP_HEADER_ONLY 时有效） */
    uint32_t  ssrc;
    uint16_t  seq;
    uint32_t  ts;
    uint8_t   pt;
    /* 源/目的地址（网络字节序） */
    uint32_t  src_ip;
    uint32_t  dst_ip;
    uint16_t  src_port;
    uint16_t  dst_port;
    /* 精确纳秒时间戳（CLOCK_REALTIME，跨系统对齐用） */
    uint64_t  timestamp_ns;
} media_bug_frame_t;

/* 回调函数签名 */
typedef void (*media_bug_cb_t)(media_bug_frame_t *frame, void *ud);

/* 挂接 bug：返回 bug_id（全局唯一，call_destroy 自动 detach）。
 * leg_mask：bit0=A腿(0) bit1=B腿(1) → 3=双腿 */
uint64_t media_bug_attach(void *call_key, int leg_mask,
                          media_bug_type_t type,
                          media_bug_cb_t cb, void *ud);

/* 移除 bug */
void media_bug_detach(uint64_t bug_id);

/* 全局一键清除（紧急排障）——全程 log_audit（OAM 层负责） */
void media_bug_purge_all(void);

/* call_destroy 自动清理：不做 RCU 切换，直接从全局表移除 */
void media_bug_detach_all_for_call(void *call_key);

/* ===== 热路径 dispatch（relay_recv_one 内部调用）===== */

/* RTP 原始帧（rtp_parse 之前，BUG_RAW_RTP） */
void media_bug_dispatch_rtp_raw(void *call_key, int side,
                                const uint8_t *buf, size_t n,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port);

/* RTCP 镜像（BUG_RTCP_MIRROR） */
void media_bug_dispatch_rtcp(void *call_key, int side,
                             const uint8_t *buf, size_t n,
                             uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port);

/* SIP 旁路（txn 状态机调用，BUG_SIPCAPTURE_TAP） */
void media_bug_dispatch_sip(void *call_key, const uint8_t *sip_msg, size_t n,
                            uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port);

/* ===== 全局消费者注册（hep_sink / 质检 / 录音旁路等用）===== */

/* 全局消费者：不依赖手动 attach uuid_media_bug，
 * 所有经 dispatch 的帧自动投递（consumer 内部按 call_key/profile 过滤）。
 * name 用于 show media_bugs 等运维命令标识消费者来源。 */
#define MEDIA_BUG_NAME_MAX 32
typedef struct {
    char name[MEDIA_BUG_NAME_MAX];  /* "hep_sink" / "qcall" / ... */
    int type_mask;             /* BUG_RAW_RTP | BUG_RTCP_MIRROR | ... */
    media_bug_cb_t cb;
    void *ud;
} media_bug_consumer_t;

int media_bug_register_global_consumer(const media_bug_consumer_t *c);
void media_bug_unregister_global_consumer(const media_bug_consumer_t *c);

/* OAM 状态输出（供 uuid_media_bug list / hep status 内部复用） */
int media_bug_status(char *out, size_t outsz);

/* ===== Phase 2: media_bug_ops_t 多 ops 注册表 =====
 *
 * 白皮书 §2.1：独立于 per-call bug + global_consumer 的第二套注册体系。
 * 每个 ops 带 on_answer（呼叫建立时自动 attach）/ on_destroy（清理）/
 * frame_cb（热路径每帧回调），配合安全卸载模板。
 *
 * 老 API（media_bug_attach/register_global_consumer）保持兼容不删。 */
#include "b2bua.h"     /* b2b_call_t */

typedef struct media_bug_ops {
    const char *name;                                  /* "record", "hep", "ai_stream" */
    int  (*on_answer)(b2b_call_t *c, void *ud);        /* 自动 attach（可选 NULL）*/
    void (*on_destroy)(b2b_call_t *c, void *ud);       /* 呼叫销毁清理（可选 NULL）*/
    void (*frame_cb)(media_bug_frame_t *frame,         /* 热路径帧回调（可选 NULL）*/
                     void *ud);
    void *ud;                                          /* 注册时传入的私有句柄 */
} media_bug_ops_t;

int  media_bug_registry_register(const media_bug_ops_t *ops);
int  media_bug_registry_unregister(const char *name);

/* 查找 + INC/DEC（卸载安全）——调用方用完必须 dec */
const media_bug_ops_t *media_bug_registry_find_inc(const char *name, int *out_slot);
void                   media_bug_registry_dec(int slot);

/* 遍历 + 计数（OAM show media_bug_ops 用）*/
int media_bug_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_MEDIA_BUG_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
