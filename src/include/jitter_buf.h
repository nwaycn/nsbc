#ifndef NSBC_JITTER_BUF_H
#define NSBC_JITTER_BUF_H

/* 自适应抖动缓冲（REQ-TRANS-003，P1；转码/放音链路前置）
 *
 * 按 RTP seq 定序入队、按播放时钟出队；自适应目标时延根据到达间隔
 * 方差伸缩（E-Model 时延/丢包折衷）；bypass 模式不经本缓冲。
 * 内存走 mem 域固定槽（无堆活动），容量按 ptime×最大深度预分配。
 */
#include "nsbc_common.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JB_DEFAULT_DEPTH_MS  60
#define JB_MAX_DEPTH_MS      200
#define JB_SLOT_CAP          1600 /* 单载荷上限（Opus 12k 帧有界） */

typedef enum {
    JB_OK = 0,
   JB_DROP_OLD,    /* 迟到且已过播放点 */
    JB_DROP_DUP,
    JB_OVERFLOW,
    JB_NEED_MORE    /* 出队数据尚不足一个播放点 */
} jb_rc_t;

typedef struct {
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
    uint8_t  payload[JB_SLOT_CAP];
    size_t   len;
    uint64_t recv_ms; /* 单调时钟到达时间 */
} jb_pkt_t;

typedef struct jitter_buf jitter_buf_t;

typedef struct {
    int      clock_rate;
    int      ptime_ms;
    int      init_depth_ms;
    int      max_depth_ms;
    int      adaptive;       /* 1=自适应伸缩，0=固定深度 */
} jb_cfg_t;

int  jb_create(const jb_cfg_t *cfg, jitter_buf_t **out);
void jb_destroy(jitter_buf_t *jb);

void jb_reset(jitter_buf_t *jb);
jb_rc_t jb_push(jitter_buf_t *jb, const jb_pkt_t *pkt);

/* 按播放截止点 now_ms 出队下一个该播包；无包 NEED_MORE 并由调用方
 * 插 PLC（REQ-TRANS 丢包补偿走 media_dsp）。 */
jb_rc_t jb_pop(jitter_buf_t *jb, uint64_t now_ms, jb_pkt_t *out);

typedef struct {
    uint32_t pushed;
    uint32_t popped;
    uint32_t drops_late;
    uint32_t drops_dup;
    uint32_t plc_events;   /* NEED_MORE 次数 */
    int      cur_depth_ms; /* 自适应当前深度 */
} jb_stat_t;
void jb_stat(const jitter_buf_t *jb, jb_stat_t *out);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_JITTER_BUF_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
