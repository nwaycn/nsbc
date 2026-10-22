#ifndef NSBC_MEDIA_VQ_H
#define NSBC_MEDIA_VQ_H

/* 语音质量采集（REQ-VQ-001，P1；OBS-010 uuid_dump vq 字段数据源）
 *
 * 输入：RTP 接收侧统计（丢包/抖动/时延）+ RTCP SR/RR（RFC 3550）与
 * RTCP-XR（RFC 3611：DLRR 单向时延、MOS-LR 等）；输出每 Leg 滚动
 * R 因子/MOS 估算与计数，供 uuid_dump、Prometheus、CDR 质量字段。
 * 现状：parse/rtp/rtcp.c 可解析但零消费（W4 uuid_dump 五值恒
 * UNAVAILABLE）；本模块为唯一汇总点，媒体线程喂、OAM 线程只读快照。
 */
#include "nsbc_common.h"
#include "rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vq_collector vq_collector_t;

typedef struct {
    int      clock_rate;
    int      window_ms;     /* 滚动窗（默认 5s） */
} vq_cfg_t;

/* 每窗/累计质量快照（粗骨架字段，对齐 RTCP-XR 常用块） */
typedef struct {
    uint32_t packets_expected;
    uint32_t packets_lost;
    int      loss_pct_milli;   /* 千分比损失率 */
    uint32_t jitter_ms;        /* 平滑抖动 */
    uint32_t rtt_ms;           /* LRR/DLSR 计算 */
    uint32_t oneway_ms;        /* DLRR 单向（需时钟对齐标记） */
    int      mos_e3;            /* MOS×1000（E-model 估算，0=无效） */
    int      r_factor_e2;       /* R×100 */
    uint32_t discards;         /* JB 过深/迟到丢弃 */
} vq_report_t;

int  vq_create(const vq_cfg_t *cfg, vq_collector_t **out);
void vq_destroy(vq_collector_t *v);

/* RTP 收包侧每包喂入（seq/ts 到达时间单调时钟） */
void vq_on_rtp(vq_collector_t *v, uint16_t seq, uint32_t ts,
               uint64_t recv_ms, int marker);
/* RTCP RR 报告块喂入（fraction/lost/jitter/LSR/DLSR） */
void vq_on_rr(vq_collector_t *v, const rtcp_rb_t *rb, uint64_t now_ms);
/* RTCP-XR 块喂入（DLRR/统计/语音质量块，按 bt 分发） */
void vq_on_xr(vq_collector_t *v, const rtcp_xr_block_t *b);
/* JB/DSP 侧补充：丢弃与 PLC 次数 */
void vq_on_discard(vq_collector_t *v, uint32_t n);
void vq_on_plc(vq_collector_t *v, uint32_t n);

/* 取当前快照（无锁读原子量聚合，粗骨架为锁内拷贝，接线改 __atomic） */
void vq_report(const vq_collector_t *v, vq_report_t *out);
void vq_reset(vq_collector_t *v);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_MEDIA_VQ_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
