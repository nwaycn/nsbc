#ifndef NSBC_PORT_TABLE_H
#define NSBC_PORT_TABLE_H

/* RTP 端口表（REQ-MEDIA-FWD-001 / 7.6，P0）
 *
 * 每本地 IP 一张 65536 指针表：报文到达 O(1) 定位会话；变更（分配/释放）
 * 走分片互斥（P1 换 RCU 无锁读）。UDP 端口池按 (RTP, RTCP=RTP+1)
 * 偶数成对分配；bypass 呼叫不分配端口、表中无项（REQ-MEDIA-MODE）。
 *
 * 多本地 IP（IP_PKTINFO）各自独立表与端口池，IPv4/IPv6 通用。
 *
 * REQ-SIP-PROFILE-001：per-profile RTP 端口段已接入——media_bridge 经
 * mb_cfg_t.call_port_range 回调（组合根注册，按 b2b_call_t::prof 提供
 * rtp_port_start/end）调 ptable_alloc_pair_range 限定分配；段内耗尽
 * 回落表默认段（隔离失败不拆呼叫）。表 cfg.range_lo..hi 仍是端口宇宙。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ptable ptable_t;

typedef struct {
    uint16_t range_lo; /* 默认 20000 */
    uint16_t range_hi; /* 默认 60000，自动取偶 */
    int max_local_ips; /* 默认 64 */
} ptable_cfg_t;

int ptable_create(const ptable_cfg_t *cfg, ptable_t **out);
void ptable_destroy(ptable_t *t);

/* 分配连续 (rtp=偶数, rtcp=rtp+1)；owner 写入两个槽，返回 NSBC_NOSPC
 * 表示端口池耗尽（呼叫回退 bypass 或回 488/503 由上层决策）。 */
int ptable_alloc_pair(ptable_t *t, const char *local_ip, void *owner,
                      uint16_t *rtp_port, uint16_t *rtcp_port);

/* 范围限定分配（REQ-SIP-PROFILE-001 per-profile RTP 端口段）：
 * 仅在 [lo,hi]（内部取偶归一）内分配。lo==0/hi==0 或覆盖表全段时
 * 等价 ptable_alloc_pair（栈顶 O(1) 快路径）。段内耗尽返回 NSBC_NOSPC
 * ——上层决策回落默认段（传 0,0 重试）或回 488。
 * 实现注记：free_even 为无序栈，范围分配从栈顶向下找首个落段元素、
 * 与栈顶交换后弹出（摊销 O(1)，段内近满时退化为 O(n) 可接受——
 * 端口池耗尽本身就是故障态）。release 语义与全局池完全一致。
 * 段语义：表 cfg.range_lo..hi 是端口宇宙（ip_get 初始化的栈范围）；
 * 请求段超出表段的部分不存在于栈中——段与表段无交集即 NOSPC。 */
int ptable_alloc_pair_range(ptable_t *t, const char *local_ip, void *owner,
                            uint16_t lo, uint16_t hi,
                            uint16_t *rtp_port, uint16_t *rtcp_port);
void ptable_release_pair(ptable_t *t, const char *local_ip,
                         uint16_t rtp_port);

/* 收包快路径：O(1) 查会话 owner；未占用返回 NULL。 */
void *ptable_lookup(ptable_t *t, const char *local_ip, uint16_t port);

uint64_t ptable_in_use(ptable_t *t);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
