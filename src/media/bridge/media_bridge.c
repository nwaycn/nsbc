/*
 * NSBC media bridge — RTP relay, DTLS-SRTP, SDP rewrite, transcoding orchestration.
 * Copyright 2024-2026 李浩 (Li Hao), 上海宁卫信息技术有限公司
 * Contact: 1354608370@qq.com
 *
 * Licensed under the Apache License, Version 2.0. See LICENSE in project root.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "media_bridge.h"
#include "b2bua.h"        /* b2b_call_t.muted[] + .ivr_sess — uuid_mute + DTMF→IVR 入口 */
#include "sip_profile.h"  /* sip_profile_t.enable_precondition — RFC 3312 gate 守卫 */
#include "sdp.h"
#include "rtp.h"
#include "dtmf.h"         /* dtmf_rx_t — RFC4733 去重器，relay_recv_one 喂 */
#include "ivr.h"          /* ivr_on_dtmf — relay_recv_one 消费 DTMF */
#include "sip_common.h"   /* sip_ci_eq */
#include "media_bug.h"
#include "media_codec.h"   /* codec registry + transcode engine */
#ifdef NSBC_WITH_OPENSSL
#include "openssl_compat.h" /* NSBC_OPENSSL_HAS_DTLS_FEED 版本探测（先于 dtls_srtp.h） */
#endif
#include "dtls_srtp.h"
#include "net_sock.h"
#include "net_mmsg.h"
#include "nsbc_id.h"
#include "log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define BR_SHARDS 64
#define SSRC_MAP_N 16
#define RELAY_BUF 2048
#define MB_REC_HOOK_MAX 4

typedef struct cnode {
    struct cnode *next;
    void *call; /* 不透明呼叫键（b2b_call_t*），仅比较/哈希，禁止解引用 */
    rtp_relay_t *relay;
    mb_mode_t mode;
    char a_ip[64];
    uint16_t a_port; /* A 的 SDP 媒体地址（peer A） */
    /* ★ per-call NAT 公网 RTP IP（REQ-NAT-002）——offer_hook 里从 profile 回调查一次，
     * 缓存到这里，answer_hook 直接复用。空=关闭 NAT（用 local_ip 做 SDP rewrite fallback）。 */
    char external_rtp_ip[64];
    /* ★ 端点 WebRTC 标志——从原始 offer SDP 媒体层分析得出，
     * 独立于 dtls[]（dtls[] 在 Phase C SRTP_RELAY 模式下双侧都创建，
     * 不再能区分端点类型）。offer_hook 里设置，answer_hook 读取。 */
    uint8_t a_ws;  /* A 侧端点是否 WebRTC（有 DTLS-SRTP SDP） */
    uint8_t b_ws;  /* B 侧端点是否 WebRTC */
    /* ★ A 原始 offer 的 m= 段数 —— answer_hook 按 RFC 3264 把 answer
     * 截断到相同段数（offer_hook 为 WebRTC B 追加的 rejected video
     * 会被浏览器回显回来，多出的段不能发给只 offer 了 audio 的 A）。 */
    uint8_t a_mlcount;
    /* REQ-DTLS-001：DTLS-SRTP 终结器，按 relay 物理侧索引：
     * Phase C 后 SRTP_RELAY 模式双侧都创建（relay 做 DTLS demux），
     * 与端点类型无关。明文侧（BYPASS/双明文）恒 NULL。 */
    void *dtls[2];
    /* ★ G.711 转码会话（REQ-TRANS-004 P0）—— per-direction:
     * transcode[side] 处理 "从 side 来的 RTP → 转码后发往 other"
     * 例：side=0（A→B）transcode 把 A 腿发的 PCMU 转成 PCMA 给 B 腿。
     * MB_TRANSCODE 模式下由 bridge_confirm 创建，bridge_release_call 释放。 */
    transcode_sess_t *transcode[2];
    codec_id_t         src_codec[2]; /* 每侧实际发的 codec（运行时动态识别） */
    codec_id_t         dst_codec[2]; /* 对侧期望的 codec（从 answer SDP 提取） */
    uint8_t            pt_allowed[2][8]; /* 每侧允许通过的 PT（从 answer SDP fmt-list） */
    int                pt_allowed_count[2];
    /* ★ PT rewrite：同 codec 不同 PT（如 WebRTC G729 PT=96 → SIP G729 PT=18）
     * 只改 RTP header 的 PT 字节，不做 PCM 往返转码。比 transcode 快得多。 */
    struct { uint8_t src_pt; uint8_t dst_pt; } pt_rewrite[2];
    int pt_rewrite_active[2];
    /* ★ 2026-10-16 诊断：transcode_seen 从 static __thread 改 per-call，
     * 避免长寿命媒体线程跨呼叫累积导致短通话（~400 包）永远碰不到 %1000 阈值。 */
    int transcode_seen_diag[2];
    /* ★ RFC 3312 Precondition 前提条件门控——offer_hook 解析 offer SDP 置位
     * precond_active（1=Require precondition 或 a=des:qos mandatory），
     * answer_hook 解析 answer SDP 置位 precond_met（1=a=curr 覆盖所有 mandatory des）。
     * relay_recv_one 转发前检查：precond_active && !precond_met → 丢包。 */
    uint8_t precond_active;
    uint8_t precond_met;
    /* ★ DTMF RFC4733 — 每侧实际协商的 telephone-event 载荷 PT（0=未协商）。
     * bridge_confirm 从 answer SDP 的 rtpmap 里提取 enc="telephone-event" 时填入。
     * relay_recv_one 里 pkt.pt == dtmf_pt[side] 时走 DTMF 专用分支：
     *   rtp_parse_dtmf → dtmf_rx_on_rfc4733 → ivr_on_dtmf(call->ivr_sess, digit)
     * 转发 continue（避免对端也收到 RFC4733 包重复解析）。 */
    uint8_t   dtmf_pt[2];     /* 每侧 telephone-event PT；0=未协商 */
    void     *dtmf_rx[2];    /* dtmf_rx_t* 每侧去重器；0=未创建 */
} cnode_t;

typedef struct {
    pthread_mutex_t lock;
    cnode_t *head;
} br_shard_t;

/* REQ-AI-001：rec_feed_fn 动态 hook 槽（ai_dialog 等可选模块运行时注册，
 * 不碰 mb_cfg_t.rec_feed_fn 主槽（signaling 创建时设的 recording 回调保持不变）。
 * 媒体线程在调用 cfg.rec_feed_fn 之后，遍历 rec_hooks 链表逐个调用。
 * 链表挂在 bridge_mgr 上，pthread_mutex 保护增删；遍历前持 r->m->cfg.rec_feed_fn
 * 调用已经离开 bridge 内部状态锁——hook 链表遍历用自己的锁保护安全。 */
typedef struct mb_rec_hook {
    void (*fn)(void *ud, void *call_key, const void *payload, size_t len, int leg, int pt);
    void *ud;
    unsigned refs; /* protected by bridge_mgr.rec_hook_lock */
    struct mb_rec_hook *next;
} mb_rec_hook_t;

static __thread unsigned rec_hook_dispatch_depth;

struct bridge_mgr {
    mb_cfg_t cfg;
    br_shard_t sh[BR_SHARDS];
    pthread_mutex_t rec_hook_lock;  /* 保护下方 hook 链表增删 */
    pthread_cond_t rec_hook_cond;   /* 等待已摘除 hook 的在途回调结束 */
    mb_rec_hook_t *rec_hooks;       /* AI dialog / 未来 transcript 等可选订阅 */
};

/* ------------------------------------------------------------------ */
/* SSRC 出向重写映射                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t in;
    uint32_t out;
    int used;
} smap_entry_t;

typedef struct {
    smap_entry_t e[SSRC_MAP_N];
    uint8_t hint;   /* 上次命中槽 — 稳定音频流 O(16)→O(1) */
} smap_t;

/* ★ REQ-VQ-001：RTP seq 跟踪按 SSRC 分槽
 * 之前 d_last_seq[side] 单槽被 audio + rejected video 交替污染，
 * 6 秒内 fake lost=32974、exp=34002——完全不可用。
 * 改为每个 side 最多 SEQ_SLOTS 个 SSRC 独立跟踪，OAM 汇总时累加。 */
#define SEQ_SLOTS 8
typedef struct {
    uint32_t ssrc;
    uint16_t last_seq;
    uint32_t expected;  /* relay_cb 单线程写 __atomic_fetch_add；OAM __atomic_load_n 读 */
    uint32_t lost;      /* 同上 */
    int      primed;
} seq_track_t;

static uint32_t smap_lookup(smap_t *m, uint32_t in)
{
    /* 先试 hint 槽（稳定音频流 99% 命中 — O(1)） */
    uint8_t h = m->hint;
    if (h < SSRC_MAP_N && m->e[h].used && m->e[h].in == in)
        return m->e[h].out;
    /* 全表扫描找到命中槽，更新 hint */
    for (int i = 0; i < SSRC_MAP_N; ++i) {
        if (m->e[i].used && m->e[i].in == in) {
            m->hint = (uint8_t)i;
            return m->e[i].out;
        }
    }
    /* 未命中：找空槽分配新映射 */
    for (int i = 0; i < SSRC_MAP_N; ++i) {
        if (!m->e[i].used) {
            m->e[i].used = 1;
            m->e[i].in = in;
            do
                m->e[i].out = nsbc_ssrc_random();
            while (!m->e[i].out || m->e[i].out == in);
            m->hint = (uint8_t)i;
            return m->e[i].out;
        }
    }
    return in; /* 表满：原样（计数异常可观测） */
}

/* ------------------------------------------------------------------ */
/* relay 实例                                                           */
/* ------------------------------------------------------------------ */
struct rtp_relay {
    bridge_mgr_t *m;
    reactor_t *home_reactor;  /* P3: relay 归属的 reactor（arm 时路由选定，stop 时用） */
    cnode_t *node;
    int fd_rtp[2];
    int fd_rtcp[2];
    uint16_t rtp_port[2]; /* 端口表回收凭据（不依赖 socket 名） */
    nsbc_sa_t peer[2];  /* 配置期望对端（可空：纯 latching） */
    int have_peer[2];
    nsbc_sa_t latch[2];
    int latched[2];
    ssrc_policy_t pol[2];
    smap_t map[2];
    uint64_t pkts, drops;
    /* REQ-DTLS-001：per-side SRTP 会话（该侧 dtls_server ESTABLISHED 后
     * 经 dtls_server_take_srtp() 导出挂这里）。哪一侧是 WebRTC 由
     * cnode_t.dtls[side] 非空标识（A 或 B 均可能，双 WebRTC 时两侧都有）；
     * 无 dtls 的侧恒 NULL 明文。 */
    nsbc_srtp_sess_t *srtp[2];
    /* OAM 诊断槽（REQ-MEDIA-SEC-003⑤/REQ-OBS-010）：媒体线程写、
     * bridge_call_view 读，全部 __atomic；view 不持 relay 引用锁。 */
    int d_latched[2];
    uint32_t d_in_ssrc[2];   /* 入向学习主源（0=未学习） */
    uint32_t d_out_ssrc[2];  /* 重写后发往对侧的主源 SSRC */
    int d_have_rr[2];
    uint32_t d_rr_frac[2], d_rr_lost[2], d_rr_jitter[2];
    /* RTP seq-gap 本地丢包计数（按 SSRC 分槽，避免多 SSRC 交替污染） */
    seq_track_t seq[2][SEQ_SLOTS];
    uint32_t d_rtp_jitter_ticks[2]; /* RTP 时钟 tick 原 jitter（未换算 ms，仅 RTCP 旁证） */
    /* REQ-MEDIA-SEC-003⑤ P0：SSRC↔CNAME 绑定（SDES CNAME）；
     * 媒体线程写，relay 侧 [0]朝A/[1]朝B。 */
    char d_cname[2][64];
    int d_have_cname[2];
    /* ★ RACE-FIX: arm 标志。rtp_relay_start 只 open fd + alloc port，
     * 延迟到所有初始化完成后 rtp_relay_arm 才 reactor_add。
     * rtp_relay_stop 里 arm=0 时跳过 reactor_del（fd 还没注册）。 */
    int armed;
    /* ★ P0-2 RACE-FIX: rtp_relay_stop 置 dead=1，relay_recv_one 循环每次检查
     * 并立即退出。解决 "reactor_call del fd 返回后，已进入 relay_recv_one 的
     * for(;;) 循环还在内核队列剩余数据包 → bridge_release_call free(r) 时
     * relay_recv_one 还在访问 r→node → UAF"。ARMED 置 0 在 reactor_call 之前。 */
    _Atomic int dead;
    /* REQ-ORIGINATE-003 &echo 实现：RTP 环回 mode。
     * 置 1 后 relay_recv_one 把收到的包发回同一侧（side→side），
     * 而非正常的 side→other。两侧各自环回自己的 peer。 */
    int echo_mode;
};

static void relay_rtcp_observe(rtp_relay_t *r, int side, const uint8_t *buf,
                               size_t len);

/* REQ-MEDIA-SSRC-003②：从远端 SDP 提取所有 a=ssrc: / a=ssrc-group:FID，
 * 预登记进 ssrc_policy，避免 "先来先学" 策略让后到的视频 SSRC 被 inject_drops 丢弃。
 *
 * 位置：bridge_offer_hook（side=0，offer SDP）和 bridge_answer_hook（side=1，answer SDP），
 * 必须在 bridge_rewrite_sdp 之前（rewrite 会剥掉 a=ssrc: / a=ssrc-group: 行）。 */
static void admit_ssrc_from_sdp(rtp_relay_t *r, int side,
                                const char *sdp, size_t len)
{
    if (!r || !sdp || !len || side < 0 || side > 1)
        return;
    ssrc_policy_t *pol = &r->pol[side];
    int admitted = 0;

    /* 扫描所有 a=ssrc:<n> ... 行 */
    const char *p = sdp;
    const char *end = sdp + len;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        const char *line_end_nul = line_end ? line_end : end;
        size_t llen = (size_t)(line_end_nul - p);

        /* 去掉尾部 \r */
        while (llen > 0 && p[llen - 1] == '\r') --llen;

        if (llen >= 8 && strncmp(p, "a=ssrc:", 7) == 0) {
            uint32_t ssrc = 0;
            const char *num_start = p + 7;
            char *tail = NULL;
            unsigned long v = strtoul(num_start, &tail, 10);
            if (tail != num_start) {
                ssrc = (uint32_t)v;
                ssrc_policy_admit(pol, ssrc, 0);  /* 先按主源登记 */
                ++admitted;
            }
        } else if (llen >= 20 && strncmp(p, "a=ssrc-group:FID ", 18) == 0) {
            /* a=ssrc-group:FID <primary> <rtx> — primary 主源，secondary RTX 修复流 */
            uint32_t primary = 0, rtx = 0;
            const char *nums = p + 18;
            char *p2 = NULL, *r2 = NULL;
            unsigned long v1 = strtoul(nums, &p2, 10);
            if (p2 && *p2 == ' ') {
                unsigned long v2 = strtoul(p2 + 1, &r2, 10);
                if (r2 != p2 + 1) {
                    primary = (uint32_t)v1;
                    rtx = (uint32_t)v2;
                    ssrc_policy_admit(pol, primary, 0);  /* primary */
                    ssrc_policy_admit(pol, rtx, 1);      /* RTX aux   */
                    admitted += 2;
                }
            }
        }
        /* a=rtpmap:<pt> rtx/... — 登记 RTX payload type，让 is_aux_pt(pt) 命中 */
        /* 这条是额外保险：即使漏扫 FID group，rtpmap 也能让 RTX pt 被识别 */
        if (llen >= 16 && strncmp(p, "a=rtpmap:", 9) == 0) {
            /* a=rtpmap:103 rtx/90000 cname:... — rtx 在斜杠前的 token */
            const char *pt_end = memchr(p + 9, ' ', llen - 9);
            if (pt_end && pt_end > p + 9) {
                char pt_buf[16];
                size_t ptlen = (size_t)(pt_end - (p + 9));
                if (ptlen < sizeof(pt_buf)) {
                    memcpy(pt_buf, p + 9, ptlen);
                    pt_buf[ptlen] = '\0';
                    int pt = atoi(pt_buf);
                    if (pt > 0 && pt < 256) {
                        /* 查一下本行里有没有 "rtx/" */
                        const char *rtx_marker = memmem(p, llen, "rtx/", 4);
                        if (rtx_marker)
                            ssrc_policy_set_aux_pt(pol, (uint8_t)pt);
                    }
                }
            }
        }

        if (!line_end) break;
        p = line_end + 1;
    }
    if (admitted > 0) {
        llog(LOG_DEBUG, "media",
             "admit_ssrc_from_sdp side=%d admitted=%d max_sources=%d "
             "learned_primary=%d",
             side, admitted, pol->cfg.max_sources, pol->learned_primary);
    }
}

/* 首字节分型（与 dtls_srtp.c pkt_classify 同规则的本文件拷贝）：
 * 1=STUN 2=DTLS 3=RTP/SRTP -1=未知 */
static int rx_classify(const uint8_t *buf, size_t len)
{
    if (len < 2) return -1;
    uint16_t t = (uint16_t)((buf[0] << 8) | buf[1]);
    if ((t & 0xC000) == 0x0000 && len >= 20) {
        /* 用 memcpy 避免 ARM/SPARC 严格对齐平台 SIGBUS (CVE-style 防御) */
        uint32_t ck_raw;
        memcpy(&ck_raw, buf + 4, sizeof(ck_raw));
        uint32_t ck = ntohl(ck_raw);
        if (ck == 0x2112A442u) return 1;
    }
    if (buf[0] >= 20 && buf[0] <= 25) return 2;
    if ((buf[0] & 0xC0) == 0x80) return 3;
    return -1;
}

/* ★ RFC 5761 §4 rtcp-mux 解复用（纯 RTP/RTCP 包判别，不含 STUN/DTLS）：
 *
 * RTP：byte[1] = M(1) | PT(7)，PT∈0-127 → byte[1] 可以是 0-127（M=0）
 *       或 128-255（M=1, PT≥0）
 * RTCP：byte[1] = PT(8)，PT∈200-255 → byte[1] 固定在 200-255
 *
 * 冲突区：byte[1] ∈ 200-255 既可能是 RTP(M=1,PT≥72)
 *         也可能是 RTCP(PT≥200)
 *
 * 两级验证：
 *   L1. compound 精确匹配：所有 RTCP 子包 length 正好铺满 buffer → RTCP
 *   L2. 弱验证回退（L1 失败时）：第一个子包 PT ∈[200,255] 且 length 合理
 *       且子包不溢出 → 判 RTCP（Chrome 有时发非标准 padding 的 compound）
 *   RTP 的 seq 字段当 RTCP length 解析会巨大（>64KB）→ 溢出 → RTP */
static int is_rtcp_packet(const uint8_t *buf, size_t len)
{
    if (len < 4)
        return 0;
    /* RTP/RTCP Version 必须是 2（高两位 10） */
    if ((buf[0] & 0xC0) != 0x80)
        return 0;
    uint8_t pt_byte = buf[1];
    if (pt_byte < 200)
        return 0;   /* byte[1] < 200：明确是 RTP（M=0,PT<200） */

    /* ★ L1：精确 compound 匹配 */
    size_t offset = 0;
    int all_ok = 1;
    while (offset + 4 <= len) {
        uint16_t lw = (uint16_t)((buf[offset + 2] << 8) | buf[offset + 3]);
        size_t sub_len = ((size_t)lw + 1) * 4;
        if (sub_len < 4) {
            all_ok = 0; break;  /* 非法 length */
        }
        if (offset + sub_len > len) {
            all_ok = 0; break;  /* 子包超出 buffer */
        }
        offset += sub_len;
    }
    if (all_ok && offset == len)
        return 1;               /* L1 精确匹配成功 */

    /* ★ L2：弱验证回退。
     * 第一个子包必须 PT ∈[200,255]（已知 pt_byte >=200），
     * length 字段解析后子包不溢出 buffer → 判 RTCP。
     * 真 RTP 包的 bytes[2-3] 是 sequence number（通常 > 1000），
     * 解析成 sub_len 后会 > len → 溢出 → L2 也失败 → RTP。 */
    uint16_t lw1 = (uint16_t)((buf[2] << 8) | buf[3]);
    size_t sub_len1 = ((size_t)lw1 + 1) * 4;
    if (sub_len1 >= 4 && sub_len1 <= len) {
        /* 第一个子包合法且不溢出 → RTCP */
        return 1;
    }
    return 0;   /* 弱验证也失败 → RTP */
}

/* ---------- 运行时转码辅助函数（放在 relay_recv_one 之前避免隐式声明） ---------- */

/* 判断一个 PT 是否在允许列表里 */
static int pt_in_list(uint8_t pt, const uint8_t *list, int count)
{
    for (int i = 0; i < count; ++i)
        if (list[i] == pt) return 1;
    return 0;
}

/* PT → codec_id 映射（G.711 + G.729 + G.722 静态 PT 基线）
 * 静态 PT 未命中（WebRTC 动态 PT 96+）时返回 CODEC_NONE，
 * 由 sdp_extract_audio_codecs 用 rtpmap enc name 二次映射。 */
static codec_id_t pt_to_codec(int pt)
{
    switch (pt) {
    case 0:  return CODEC_PCMU;
    case 8:  return CODEC_PCMA;
    case 9:  return CODEC_G722;
    case 18: return CODEC_G729;
    default: return CODEC_NONE;
    }
}

/* ---------- relay_recv_one ---------- */

static void relay_recv_one(rtp_relay_t *r, int side, int is_rtcp,
                           int fd)
{
    uint8_t buf[RELAY_BUF];
    /* REQ-DTLS-001：本侧是否 DTLS lane（WebRTC 混流 fd：STUN/DTLS/SRTP）。
     * 方向无关：dtls[side] 非空即本侧是 WebRTC（A 或 B 侧均可能）。 */
    dtls_server_t *ds =
        (r->node && r->node->dtls[side])
            ? (dtls_server_t *)r->node->dtls[side] : NULL;
    const int srtp_lane = (ds != NULL);

    /* ★DIAG 2026-10-15：无差别方向计数——两侧各前 5 次 RECV 都打
     * SIP 侧（side=0）是明文 RTP srtp_lane=0，之前 DIAG ENTRY 只打 srtp_lane=1
     * 导致 SIP 侧零 RX 时整条 DIAG 静默，根本不知道是不是收不到包 */
    static int diag_any_seen[4] = {0}; /* 2 side × (rtp + rtcp) */
    int slot = side * 2 + is_rtcp;
    if (diag_any_seen[slot] < 5) {
        llog(LOG_NOTICE, "relay",
             "★RECV side=%d fd=%d is_rtcp=%d srtp_lane=%d rtp_port=%u",
             side, fd, is_rtcp, srtp_lane, (unsigned)r->rtp_port[side]);
        ++diag_any_seen[slot];
    }
    for (;;) {
        /* ★ P0-2 RACE-FIX: 每次循环开头检查 dead flag。rtp_relay_stop
         * 置 dead=1 后，即使 reactor_call 返回时本回调还在跑（内核队列
         * 有残留数据包），也会在这里立即退出，不会在 bridge_release_call
         * free(r/e) 后继续访问 r→node 等成员。 */
        if (__atomic_load_n(&r->dead, __ATOMIC_ACQUIRE))
            return;
        nsbc_sa_t src;
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        ssize_t n;

        if (srtp_lane) {
            /* 先 PEEK 分型再决定谁消费——DTLS record 在 OpenSSL<1.1.1 上
             * 必须留给 BIO_dgram 自己从 fd 读（无 BIO_CTRL_DGRAM_SET_DATA
             * 注入通道），抢先 recvfrom 会让 SSL_accept 永远拿不到 ClientHello；
             * STUN 则必须我们消费并立即应答（BIO 会把它当垃圾丢弃，
             * 浏览器 ICE check 无应答 = 480 根因）。 */
            ssize_t pn = recvfrom(fd, buf, sizeof(buf), MSG_PEEK,
                                  (struct sockaddr *)&ss, &sl);
            if (pn <= 0)
                return;
            int cls = rx_classify(buf, (size_t)pn);
            if (cls == 1) { /* STUN：消费 + Binding Response（ICE consent） */
                n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&ss, &sl);
                if (n <= 0)
                    return;
                src.ss = ss;
                src.len = sl;
                /* ★ REQ-ICE-LATCH-001：用 STUN 源地址建立 latch。
                 * WebRTC 复用同一 UDP socket 收发 STUN/DTLS/RTP
                 * （RFC 8445 §14.1 强制要求）—— 收到 STUN 的源地址就是
                 * 对端收 RTP 的地址。之前只在收 RTP 时 latch，导致
                 * "对端只发 STUN 不发 RTP（track 未启用等）" 时 latched 永为 0，
                 * 对侧 RTP 全被 "other latch not ready" 丢弃。
                 * ★ 同 RTP latch：清 have_peer 避免旧 SDP 地址导致 mismatch */
                if (!r->latched[side] && !r->have_peer[side]) {
                    r->latch[side] = src;
                    r->latched[side] = 1;
                    r->have_peer[side] = 0;
                    __atomic_store_n(&r->d_latched[side], 1, __ATOMIC_RELEASE);
                    llog(LOG_DEBUG, "relay", "LATCH side=%d by STUN "
                         "from_family=%d", side, ss.ss_family);
                }
                char ipa[INET6_ADDRSTRLEN] = {0};
                uint16_t port = 0;
                if (ss.ss_family == AF_INET) {
                    inet_ntop(AF_INET,
                              &((struct sockaddr_in *)&ss)->sin_addr,
                              ipa, sizeof(ipa));
                    port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
                } else if (ss.ss_family == AF_INET6) {
                    inet_ntop(AF_INET6,
                              &((struct sockaddr_in6 *)&ss)->sin6_addr,
                              ipa, sizeof(ipa));
                    port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
                }
                llog(LOG_DEBUG, "dtls", "STUN rx side=%d fd=%d from=%s:%u len=%zd",
                     side, fd, ipa, (unsigned)port, n);
                int rc = dtls_ice_handle_stun(ds, buf, (size_t)n, &src);
                llog(LOG_DEBUG, "dtls", "STUN Binding resp side=%d rc=%d", side, rc);
                continue;
            }
            if (cls == 2) { /* DTLS record：喂握手 */
                int st_before = dtls_server_state(ds);
#if NSBC_OPENSSL_HAS_DTLS_FEED
                n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&ss, &sl);
                if (n > 0)
                    dtls_server_feed(ds, buf, (size_t)n); /* 注入+握手+导出 */
#else
                if (dtls_server_state(ds) == DTLS_ST_ESTABLISHED) {
                    /* ESTABLISHED 后的残留 record（close_notify 等）：
                     * 消费丢弃——此路径 BIO_dgram 已不再读 fd，不消费
                     * 的话 peek 会反复命中同一包形成死循环 */
                    recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&ss, &sl);
                } else {
                    /* 不消费——BIO_dgram 从 fd 自读 + SSL_accept 推进 */
                    dtls_server_on_read(ds);
                }
#endif
                int st_after = dtls_server_state(ds);
                if (st_before != DTLS_ST_ESTABLISHED && st_after == DTLS_ST_ESTABLISHED) {
                    llog(LOG_INFO, "dtls", "DTLS ESTABLISHED side=%d fd=%d", side, fd);
                }
                if (dtls_server_state(ds) == DTLS_ST_ESTABLISHED &&
                    !r->srtp[side]) {
                    r->srtp[side] = dtls_server_take_srtp(ds);
                    llog(LOG_DEBUG, "dtls", "SRTP relay: session attached "
                         "side=%d", side);
                }
                continue;
            }
            /* cls==3（SRTP/RTP）或 -1：落入下方统一 recvfrom 消费 */
            /* ★DIAG 2026-10-13：DTLS ESTABLISHED 后 WebRTC fd 流量探测
             * 若 Chrome 不发 SRTP 回来（单通根因），此处 cls 永远不会到 3 */
            if (srtp_lane && ds &&
                dtls_server_state(ds) == DTLS_ST_ESTABLISHED) {
                static int diag_post_dtls_seen[2] = {0};
                if (diag_post_dtls_seen[side] < 5) {
                    llog(LOG_DEBUG, "relay",
                         "★DIAG POST-DTLS-RX side=%d fd=%d cls=%d pn=%zd "
                         "srtp_sess=%p",
                         side, fd, cls, pn, (void *)r->srtp[side]);
                    ++diag_post_dtls_seen[side];
                }
            }
        }

        n = recvfrom(fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&ss, &sl);
        if (n <= 0)
            return;
        src.ss = ss;
        src.len = sl;

        if (srtp_lane && r->srtp[side]) {
            /* SRTP → 解密后继续明文 relay 路径。
             * RTP/RTCP 判别见文件头 is_rtcp_packet() 注释（含 M-bit 冲突处理）。 */
            int pkt_rtcp = is_rtcp_packet(buf, (size_t)n);
            size_t slen = (size_t)n;
            int rc = pkt_rtcp
                ? nsbc_srtp_unprotect_rtcp(r->srtp[side], buf, &slen)
                : nsbc_srtp_unprotect_rtp(r->srtp[side], buf, &slen);
            if (rc != NSBC_OK) {
                /* ★ REQ-MEDIA-SEC-001：RTCP unprotect 失败静默 drop
                 *
                 * 根因：libsrtp2 的 ssrc_any_inbound 通配对 RTP OK，
                 * 但 RTCP 仍做 SSRC 硬校验 (rc=14 ssrc_mismatch)，
                 * 且 RTP/RTCP 共享 replay window 导致序号竞争 (rc=7).
                 * 这些都是 RTCP 统计包（Sender/Receiver Report），
                 * 丢失只影响 MOS/丢包率等诊断数据，不影响 RTP 媒体流。
                 * 声音正常说明 RTP 解密完全 OK，密钥方向正确。 */
                if (pkt_rtcp) {
                    if ((r->drops + r->pkts) % 100 == 0) {
                        llog(LOG_DEBUG, "relay", "SRTP RTCP unprotect "
                             "silent-drop side=%d rc=%d n=%zd (libsrtp2 "
                             "ssrc_any_inbound RTCP bug, non-fatal)",
                             side, rc, n);
                    }
                    continue;  /* 静默丢弃 RTCP 密文，不 WARN 不计 drops */
                }
                /* RTP 解密失败仍然严重（密钥错或包损坏）——保持 WARN + drop + 计数 */
                char hex[64] = {0};
                size_t hlen = (size_t)n < 16 ? (size_t)n : 16;
                for (size_t i = 0; i < hlen && i * 3 < sizeof(hex) - 1; ++i)
                    snprintf(hex + i * 3, sizeof(hex) - i * 3,
                             "%02x ", buf[i]);
                llog(LOG_WARN, "relay", "SRTP RTP unprotect FAIL side=%d rc=%d "
                     "n=%zd first16=[%s]",
                     side, rc, n, hex);
                ++r->drops;
                continue;
            }
            n = (ssize_t)slen;
            is_rtcp = pkt_rtcp;
            /* ★ 关键 INFO 日志：确认 unprotect 真的在跑。
             * 之前是 DEBUG 级别，混在海量 TC-GUARD 里看不到。 */
             
        }
        /* ★ REQ-MEDIA-SEC-005b：srtp_lane 但 srtp 会话还没建立 → 直接 drop。
         * 之前注释说"握手未完的早到媒体当明文处理"——但浏览器发的是加密 SRTP，
         * 不是明文 RTP！RTP/SRTP 头部结构相同（12B + PT/SSRC），rtp_parse() 会恰好
         * 把密文解析成"合法 RTP"，转发到对端 → 对端解不开 → 全丢。
         * DTLS record（cls=2）和 STUN（cls=1）已在上文 PEEK 分支被消费，
         * 能走到这里的 cls==3 只有 SRTP——必须等 DTLS 握手完成才能解密转发。
         *
         * ★ P0 关键诊断：DTLS ESTABLISHED 前 Chrome 会一直发 SRTP 过来，
         * 但 dtls_server 状态机卡住时所有这些包都被 drop 形成"单向不通"。
         * 这里前 10 次 drop 明确告诉用户 dtls_server 当前 state +
         * 等待时长，grep "SRTP-DROP-BEFORE-DTLS" 即可定位。 */
        if (srtp_lane && !r->srtp[side]) {
            static int diag_drop_before_dtls[2] = {0};
            static struct timespec diag_dtls_start[2] = {0};
            ++r->drops;
            if (diag_drop_before_dtls[side] < 10) {
                /* 首次记录起点 */
                if (diag_drop_before_dtls[side] == 0) {
                    clock_gettime(CLOCK_MONOTONIC, &diag_dtls_start[side]);
                }
                int dtls_st = ds ? (int)dtls_server_state(ds) : -1;
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double wait_ms = (now.tv_sec - diag_dtls_start[side].tv_sec) * 1000.0 +
                                 (now.tv_nsec - diag_dtls_start[side].tv_nsec) / 1e6;
                llog(LOG_WARN, "relay",
                     "★SRTP-DROP-BEFORE-DTLS side=%d drops=%d dtls_state=%d "
                     "srtp_sess=NULL wait=%.0fms (Chrome 以为 DTLS connected "
                     "但 SBC dtls_server 没 ESTABLISHED — 检查 DTLS ClientHello "
                     "是否到达 dtls fd=%d)",
                     side, diag_drop_before_dtls[side]++, dtls_st, wait_ms,
                     fd);
            } else if (diag_drop_before_dtls[side] == 10) {
                llog(LOG_ERR, "relay",
                     "★★★ FATAL side=%d srtp_lane=%d DTLS never ESTABLISHED "
                     "after %d drops — WebRTC↔SIP 全不通。grep 'DTLS' in logs",
                     side, srtp_lane, diag_drop_before_dtls[side]);
                ++diag_drop_before_dtls[side];  /* 停打 */
            }
            continue;
        }

        if (r->have_peer[side] && !nsbc_sa_eq_addr(&src, &r->peer[side])) {
            /* ★ P0 修复：第一次 peer mismatch drop 后立即清 have_peer。
             * 原代码：have_peer[side] 是 SDP 预设的旧地址，NAT 后分机真实源不同，
             * 每次收到都触发 mismatch drop，永远走不到下面的 comedia 学习。
             * 清 have_peer 后，后续包跳过 mismatch 检查直接进入 comedia 学习。 */
            static int diag_mismatch[4] = {0};  /* 2 side × (rtp+rtcp) */
            int slot = side * 2 + is_rtcp;
            if (diag_mismatch[slot] < 5) {
                char ipa[INET6_ADDRSTRLEN] = {0}, ppa[INET6_ADDRSTRLEN] = {0};
                uint16_t sp = 0, pp = 0;
                if (src.ss.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in *)&src)->sin_addr,
                              ipa, sizeof(ipa));
                    sp = ntohs(((struct sockaddr_in *)&src)->sin_port);
                }
                if (r->peer[side].ss.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in *)&r->peer[side])->sin_addr,
                              ppa, sizeof(ppa));
                    pp = ntohs(((struct sockaddr_in *)&r->peer[side])->sin_port);
                }
                llog(LOG_WARN, "relay",
                     "★PEER-MISMATCH side=%d drop#%d have_peer=1 "
                     "src=%s:%u peer(SDP)=%s:%u → CLEAR have_peer, "
                     "letting next pkt comedia-learn",
                     side, diag_mismatch[slot]++, ipa, sp, ppa, pp);
            }
            r->have_peer[side] = 0;  /* ★ 立即清！让下次包走 comedia */
            ++r->drops;
            continue;
        }
        /* === REQ-NAT-001 comedia 学习（FreeSWITCH comedia=on 等效）===
         * NAT 场景：SDP c= 是私网地址，但 recvfrom 拿到真实公网 src。
         * 旧逻辑：set_peer 预设 latch=SDP 地址后，latched=1 → 不再更新 → NAT 后分机 RTP 被判 mismatch DROP。
         * 新逻辑：每次收到 RTP 都学习最新 src 覆盖 latch，允许 NAT 穿透。
         *         Latched 首次设 1（OAM 视角 gate_open=1）；后续每次更新实际地址。
         *         对称 NAT 场景 src 每次都变（不可能命中同一端口）——这种场景本项目依赖 STUN/ICE
         *         （WebRTC 已覆盖），非 WebRTC 的对称 NAT 需用户侧 STUN 辅助。
         *
         * ★ P0 关键修复：comedia 学习到真实源地址后，必须清除 have_peer[side]！
         * 根因：answer_hook/offer_hook 会 set_peer_a/set_peer_b 把 have_peer[side]=1
         * 设为 SDP 里的 c=/m= 地址。但 NAT 后分机的真实 RTP 源完全不同。
         * 旧代码在 comedia 学习后只更新 latch[side]，但 have_peer[side] 保持为旧地址，
         * 导致 line 545 的 peer addr mismatch 检查：
         *   if (have_peer[side] && !sa_eq(src, peer[side])) → 每次收到 NAT 后的 RTP 都 drop！
         * 清除 have_peer 后，peer addr mismatch 检查恒为假，comedia 学到的真实
         * latch[side] 就成为唯一可信的源地址。 */
        if (!r->latched[side]) {
            /* 首次：锁定 latch，标志 relay "已就绪" */
            r->latch[side] = src;
            r->latched[side] = 1;
            r->have_peer[side] = 0;  /* comedia 覆盖 SDP 预设地址 */
            __atomic_store_n(&r->d_latched[side], 1, __ATOMIC_RELEASE);
            llog(LOG_DEBUG, "relay", "★ LATCH side=%d by RTP src_fam=%d latched[0]=%d latched[1]=%d",
                 side, src.ss.ss_family, r->latched[0], r->latched[1]);
        } else if (!nsbc_sa_eq(&src, &r->latch[side])) {
            /* comedia：真实 RTP 源变化（NAT 穿透）→ 覆盖 latch，不丢弃
             * 非对称 NAT：首次 RTP 即从 NAT 后真实地址到达 → 覆盖 SDP 预设私网地址
             * 对称 NAT：src 每次都变 — 此处会反复覆盖，发回最新 src（尽力而为）
             * ★ 关键：变化也清 have_peer，避免 stale peer 地址导致 mismatch drop */
            llog(LOG_DEBUG, "relay", "comedia learn side=%d latch updated", side);
            r->latch[side] = src;
            r->have_peer[side] = 0;  /* ★ 同样清除 */
        }

        int other = side ^ 1;
        /* REQ-ORIGINATE-003 &echo：环回模式把包发回同一侧（side→side）。
         * 其余 latch 检查、peer 选择、sendto fd 下标全部复用 other 变量
         * —— 不引入新变量、不分支，零额外热路径开销。 */
        if (r->echo_mode)
            other = side;
        /* REQ-NAT-001 comedia：对侧是否就绪看 latched OR have_peer
         * （have_peer 来自 SDP，可公网直达；latched 来自 comedia 学习） */
        if (!r->latched[other] && !r->have_peer[other]) {
            /* 对侧尚未就绪：丢弃早期突发 */
            if ((r->drops + r->pkts) % 100 == 0) {
                llog(LOG_DEBUG, "relay", "★ DROP other latch not ready "
                     "side=%d other=%d latched[0]=%d latched[1]=%d have_peer[0]=%d have_peer[1]=%d",
                     side, other,
                     r->latched[0], r->latched[1],
                     r->have_peer[0], r->have_peer[1]);
            }
            ++r->drops;
            continue;
        }

        /* ★ RFC 3312 Precondition 前提条件门控：
         * offer_hook 解析 offer SDP 置 precond_active=1，
         * answer_hook 解析 answer SDP 置 precond_met=1。
         * active 但未 met → 所有 RTP/RTCP 丢包（gate=LOCKED）。 */
        if (r->node && r->node->precond_active && !r->node->precond_met) {
            ++r->drops;
            continue;
        }

        /* REQ-MEDIA-BUG-001：BUG_RAW_RTP + BUG_RTCP_MIRROR 原始帧投递
         * 所有经 relay 的 RTP/RTCP 帧，在 SSRC 重写 + 转发 之前捕获。
         * src=latch[side]（真实源地址），dst=peer/latch[other]（对端地址）。
         * IPv4 仅（P0），IPv6 填 0；地址提取一次，RTP/RTCP 分支共用。 */
        /* === REQ-NAT-001 comedia：对侧目标地址选择 ===
         * 优先 latched[other]（comedia 学到的真实 RTP 源，NAT 后可达），
         * fallback 到 have_peer[other]（SDP 预设地址，公网直达场景）。 */
        const nsbc_sa_t *dst_hint = r->latched[other]
                                    ? &r->latch[other]
                                    : (r->have_peer[other]
                                       ? &r->peer[other]
                                       : &r->latch[other]);
        uint32_t s_ip = 0, d_ip = 0;
        uint16_t s_port = 0, d_port = 0;
        if (src.ss.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&src.ss;
            s_ip = sin->sin_addr.s_addr;
        }
        if (dst_hint->ss.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&dst_hint->ss;
            d_ip = sin->sin_addr.s_addr;
        }

        if (!is_rtcp && r->node && r->node->call) {
            media_bug_dispatch_rtp_raw(r->node->call, side,
                buf, (size_t)n, s_ip, s_port, d_ip, d_port);
        }

        if (!is_rtcp) {
            rtp_pkt_t pkt;
            if (rtp_parse(buf, (size_t)n, &pkt) != NSBC_OK ||
                pkt.version != 2) {
                ++r->drops;
                continue;
            }
            /* ★ REQ-VQ-001：RTP seq-gap 丢包计数——按 SSRC 分槽
             * 多 SSRC（audio + rejected video 交替）不再互相污染。
             * relay_cb 单线程写 primed/ssrc/last_seq（无原子），
             * expected/lost 用 __atomic_fetch_add（OAM view 无锁 __atomic_load_n 读）。 */
            {
                seq_track_t *st = NULL;
                for (int i = 0; i < SEQ_SLOTS; ++i) {
                    if (r->seq[side][i].primed &&
                        r->seq[side][i].ssrc == pkt.ssrc) {
                        st = &r->seq[side][i];
                        break;
                    }
                }
                if (!st) {
                    for (int i = 0; i < SEQ_SLOTS; ++i) {
                        if (!r->seq[side][i].primed) {
                            st = &r->seq[side][i];
                            st->primed    = 1;
                            st->ssrc      = pkt.ssrc;
                            st->last_seq  = pkt.seq;
                            __atomic_store_n(&st->expected, 1u, __ATOMIC_RELEASE);
                            __atomic_store_n(&st->lost,     0u, __ATOMIC_RELEASE);
                            break;
                        }
                    }
                }
                if (st) {
                    int16_t diff = (int16_t)(pkt.seq - st->last_seq);
                    if (diff > 0) {
                        __atomic_fetch_add(&st->expected, (uint32_t)diff,
                                           __ATOMIC_RELAXED);
                        if (diff > 1)
                            __atomic_fetch_add(&st->lost, (uint32_t)(diff - 1),
                                               __ATOMIC_RELAXED);
                        st->last_seq = pkt.seq;
                    }
                }
            }
            src_decision_t dc = ssrc_policy_on_rtp(
                &r->pol[side], pkt.ssrc, (uint8_t)pkt.pt, nsbc_now_ms());
            if (dc == SRC_DEC_DROP) {
                /* ★ 诊断日志：ssrc_policy 丢弃了这个 SSRC。
                 * 如果大量出现 pt=96-127（视频 PT 范围），说明 ssrc_policy 默认策略
                 * 只认第一个到达的 SSRC（音频），后到的视频 SSRC 全被 drop。 */
                static __thread int drop_log_burst = 0;
                if (drop_log_burst < 50 || ++drop_log_burst % 1000 == 0) {
                    llog(LOG_DEBUG, "relay",
                         "SSRC_DROP side=%d ssrc=%08x pt=%u total_drops=%u",
                         side, pkt.ssrc, (unsigned)pkt.pt, (unsigned)r->drops);
                }
                ++r->drops;
                continue;
            }

            /* ★ DTMF RFC4733 拦截：本 side 的 telephone-event PT → 消费不转发。
             * 位置：SSRC policy 之后（过了合法性校验），PT rewrite/transcode 之前（DTMF 不走 codec 链）。 */
            if (r->node && r->node->dtmf_pt[side] &&
                (uint8_t)pkt.pt == r->node->dtmf_pt[side] &&
                (int)pkt.payload_len >= 4) {  /* RFC4733 最少 4 字节 payload */
                rtp_dtmf_t d;
                if (rtp_parse_dtmf(pkt.payload, pkt.payload_len, &d) == NSBC_OK) {
                    dtmf_rx_t *dx = (dtmf_rx_t *)r->node->dtmf_rx[side];
                    dtmf_event_t ev;
                    if (dtmf_rx_on_rfc4733(dx, &d, &ev) == 1) {
                        /* dtmf_rx 去重通过（首个 end-of-event 包或状态转移）→ 喂 IVR */
                        if (r->node->call) {
                            b2b_call_t *call = (b2b_call_t *)r->node->call;
                            if (call->ivr_sess) {
                                ivr_action_t act;
                                int rc = ivr_on_dtmf(call->ivr_sess, ev.digit, &act);
                                static __thread int di_cnt = 0;
                                ++di_cnt;
                                if (di_cnt < 20 || di_cnt % 500 == 0)
                                    llog(LOG_INFO, "dtmf",
                                         "★DTMF rx side=%d digit='%c' act=%d ivr_rc=%d call=%p",
                                         side, ev.digit, (int)act.act, rc, call);
                            }
                        }
                    }
                }
                continue;  /* 丢弃 DTMF RTP 包 — 对端不需要收到 RFC4733 */
            }

            /* ★ PT rewrite fast-path：同 codec 不同 PT
             * 例：WebRTC G729 PT=96 → SIP G729 PT=18
             * 只改 RTP header PT 字节，payload 不动，零开销。 */
            if (r->node && r->node->pt_rewrite_active[side] &&
                (uint8_t)pkt.pt == r->node->pt_rewrite[side].src_pt) {
                uint8_t new_pt = r->node->pt_rewrite[side].dst_pt;
                buf[1] = (buf[1] & 0x80) | (new_pt & 0x7F);
                pkt.pt = new_pt;
            }
            /* ★ REQ-TRANS-004 P0：运行时 G.711 转码
             *
             * 触发条件：
             *   1) cnode.mode == MB_TRANSCODE 且 transcode[side] 存在
             *   2) 当前 RTP 的 PT 不在对侧允许的 fmt-list 里
             *
             * 执行：用 transcode_sess 把 payload 从 src_codec 解成 PCM
             *       再编成 dst_codec 的 payload，重写 RTP header 的 PT 字段。
             *
             * G.711 PCMA↔PCMU payload 大小不变（1 byte/sample），
             * 所以 RTP header 的 timestamp/seq/marker 全部保留原值即可。 */
            {
                /* ★ 2026-10-16 转码入口守卫：说明为何没进转码（静态在函数内，
                 * relay per-call 所以等价于 per-call，不会跨呼叫泄漏）
                 * ★ 2026-10-16 v2：去掉 transcode[side] 条件 — side=1(WebRTC)
                 * 即使没有 transcode 也要打，才能看到 Chrome 是否持续发 RTP。 */
                //static int guard_seen[2] = {0};
                if (r->node) {
                    // if (guard_seen[side] < 5 || ++guard_seen[side] % 100 == 0) {
                    //     int in_list = pt_in_list((uint8_t)pkt.pt,
                    //                               r->node->pt_allowed[other],
                    //                               r->node->pt_allowed_count[other]);
                    //     llog(LOG_INFO, "relay",
                    //          "★TC-GUARD side=%d in_pt=%u transcode=%p "
                    //          "pt_allowed[%d]=(n=%d) in_list=%d WILL_XCODE=%d",
                    //          side, (unsigned)pkt.pt,
                    //          (void *)r->node->transcode[side],
                    //          other, r->node->pt_allowed_count[other],
                    //          in_list, !in_list);
                    // }
                }
            }
            if (r->node && r->node->transcode[side] &&
                !pt_in_list((uint8_t)pkt.pt,
                            r->node->pt_allowed[other],
                            r->node->pt_allowed_count[other])) {
                transcode_sess_t *tc = r->node->transcode[side];
                size_t new_out_len = 0;
                uint8_t old_pt = (uint8_t)pkt.pt;
                /* cap = relay buffer 剩余空间：sizeof(buf) - header_ext_len
                 * 允许未来 G.729/Opus 等输出大于输入的 codec
                 * pkt.payload 是 rtp_pkt_t 视图（const），底层是可写的 buf[]，
                 * cast 掉 const 原地覆盖是安全的。 */
                size_t cap = sizeof(buf) - (size_t)(pkt.payload - buf);
                int rc = transcode_process(tc,
                                           pkt.payload, pkt.payload_len,
                                           (uint8_t *)pkt.payload, cap,
                                           &new_out_len);
                if (rc == NSBC_OK) {
                    /* 转码成功 — 更新 payload_len + 总包长 n
                     * G.711 8k→8k 场景 new_out_len == 原 payload_len */
                    pkt.payload_len = new_out_len;
                    n = (ssize_t)((pkt.payload - buf) + new_out_len);
                    /* RTP header byte 1：M(bit7) + PT(bit6-0)
                     * 保留 marker bit，更新 PT */
                    codec_id_t dst_id = transcode_dst_codec_id(tc);
                    /* ★ 用 registry 里的 default_pt — 任何 codec 都能正确映射
                     * （G.729 default_pt=18, Opus default_pt=111 等） */
                    uint8_t new_pt = 0;
                    const codec_ops_t *ops_dst = codec_registry_find(dst_id);
                    if (ops_dst && ops_dst->default_pt >= 0 && ops_dst->default_pt < 256)
                        new_pt = (uint8_t)ops_dst->default_pt;
                    else {
                        /* fallback：如果 codec ops 没注册 default_pt，
                         * 用 codec_id 本身（P0 过渡，后续 ops 注册完整后消失） */
                        new_pt = (uint8_t)(dst_id >= 0 ? dst_id : 0);
                    }
                    /* 保留 marker bit (bit7)，替换低 7 位为新 PT */
                    buf[1] = (buf[1] & 0x80) | (new_pt & 0x7F);
                    pkt.pt = new_pt;
                    /* ★ 诊断：用 cnode->transcode_seen_diag[side]（per-call），
                     * 从 static __thread 改过来——避免媒体线程跨呼叫累积
                     * 导致短通话（~400 包）碰不到 %1000 静默。周期改 %100 更易观察。 */
                    if (r->node) {
                        int *ts = &r->node->transcode_seen_diag[side];
                        ++(*ts);  /* ★ 无条件先递增，避免 || 短路导致计数器永远停在 0 */
                        if (*ts <= 10 || *ts % 100 == 0) {
                            llog(LOG_DEBUG, "relay",
                                 "TRANSCODE side=%d pt_in=%u→%u "
                                 "bytes=%zu call=%p",
                                 side, (unsigned)old_pt, (unsigned)new_pt,
                                 pkt.payload_len,
                                 (void *)(r->node ? r->node->call : NULL));
                        }
                    }
                } else if (rc == NSBC_NOTSUP) {
                    /* codec registry 未注册目标 codec — 记一次 WARN，
                     * 继续转发原始包（让上层决定是否丢弃） */
                    static __thread int notsup_seen = 0;
                    if (notsup_seen < 5 || ++notsup_seen % 1000 == 0) {
                        llog(LOG_WARN, "relay",
                             "transcode_process side=%d pt=%u rc=%d — "
                             "payload forwarded as-is (codec not registered)",
                             side, (unsigned)pkt.pt, rc);
                    }
                } else {
                    /* 其他错误（NOMEM / INVAL）— 记一次 WARN 后继续原样转发 */
                    llog(LOG_WARN, "relay",
                         "transcode_process FAIL side=%d pt=%u rc=%d",
                         side, (unsigned)pkt.pt, rc);
                }
            }
            /* REQ-REC-006：raw RTP payload 录制回调
             * 在 SSRC 重写之前喂（payload 与 SSRC 无关，保持原始）。
             * call_key=r->node->call（b2b_call_t*，供回调反查 rec_sess_t）。
             * cfg.rec_feed_fn 为 NULL 时跳过（未接线）。 */
            if (r->m->cfg.rec_feed_fn && pkt.payload && pkt.payload_len) {
                r->m->cfg.rec_feed_fn(r->m->cfg.rec_feed_ud,
                                      r->node ? r->node->call : NULL,
                                      pkt.payload, pkt.payload_len,
                                      side, (int)pkt.pt);
            }
            /* REQ-AI-001：动态 hook 链表（ai_dialog 等可选模块运行时注册）。
             * cfg.rec_feed_fn 是 signaling 创建时设的录音主槽；rec_hooks 是
             * 模块 dlopen 后追加的 hook。锁内拷贝 fn+ud 到栈、解锁后执行 ——
             * Phase 0: 锁内执行外部 hook 会嵌套 recording.c 的 sess.lock（之前 P0-2 锁内套锁）。*/
            if (pkt.payload && pkt.payload_len &&
                __atomic_load_n(&r->m->rec_hooks, __ATOMIC_ACQUIRE)) {
                mb_rec_hook_t *local[MB_REC_HOOK_MAX];
                int nh = 0;
                pthread_mutex_lock(&r->m->rec_hook_lock);
                for (mb_rec_hook_t *h =
                         __atomic_load_n(&r->m->rec_hooks, __ATOMIC_RELAXED);
                     h && nh < MB_REC_HOOK_MAX; h = h->next) {
                    ++h->refs;
                    local[nh++] = h;
                }
                pthread_mutex_unlock(&r->m->rec_hook_lock);
                /* 锁外遍历执行 hook —— 各自内部锁互不嵌套 */
                ++rec_hook_dispatch_depth;
                for (int hi = 0; hi < nh; ++hi)
                    local[hi]->fn(local[hi]->ud, r->node ? r->node->call : NULL,
                                  pkt.payload, pkt.payload_len, side, (int)pkt.pt);
                --rec_hook_dispatch_depth;
                if (nh > 0) {
                    pthread_mutex_lock(&r->m->rec_hook_lock);
                    for (int hi = 0; hi < nh; ++hi)
                        --local[hi]->refs;
                    pthread_cond_broadcast(&r->m->rec_hook_cond);
                    pthread_mutex_unlock(&r->m->rec_hook_lock);
                }
            }
            /* 出向 SSRC 重写（固定头偏移 8，4 字节网络序）
             * 之前 Bug：只改 p[3] 一个字节，导致 SSRC 变成 orig[0..2] | (new & 0xFF)。
             *
             * ★ REQ-MEDIA-SSRC-002 双 WebRTC 透传优化：
             *   两侧都是 WebRTC 时，两端各自生成天然不冲突的 SSRC，Chrome 走
             *   mid/msid 匹配 track，不依赖 SSRC 数值。此时做 SSRC 重写反而
             *   造成 RTP 与未重写的 RTCP SR sender SSRC / SDES SSRC 不一致
             *   → Chrome 丢弃收到的 RTP/RTCP。
             *   mixed-mode（一端明文 SIP）才需要重写（防两端 SSRC 碰撞）。 */
            uint32_t newssrc;
            if (r->echo_mode) {
                /* REQ-ORIGINATE-003 &echo 环回：透传原始 SSRC，
                 * 否则 UA 收到改写后的 SSRC 可能导致 jitter buffer reset 或丢弃 */
                newssrc = pkt.ssrc;
            } else if (r->node && r->node->dtls[side] && r->node->dtls[other]) {
                /* 双 WebRTC：透传原始 SSRC，不重写 */
                newssrc = pkt.ssrc;
            } else {
                newssrc = smap_lookup(&r->map[side], pkt.ssrc);
                uint32_t newssrc_net = htonl(newssrc);
                memcpy(buf + 8, &newssrc_net, 4);
            }
            /* 主源学习/切换：记录入向↔出向 SSRC 对（OAM 诊断） */
            if (dc == SRC_DEC_PRIMARY || dc == SRC_DEC_NEW_PRIMARY) {
                __atomic_store_n(&r->d_in_ssrc[side], pkt.ssrc,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&r->d_out_ssrc[side], newssrc,
                                 __ATOMIC_RELEASE);
            }
            /* === REQ-MEDIA-MUTE-001：mute 侧音频（uuid_mute 实现）===
             * call->muted[side]=1 → 把该侧收到的 RTP payload 清零。
             * 这里还在 RTP 分支内（pkt 在作用域）；SSRC 已重写、payload 还在 buf。
             * RTCP 不经过此处（走下面 else 分支），协议控制必须继续流动。 */
            if (pkt.payload && pkt.payload_len && r->node && r->node->call) {
                b2b_call_t *bc = (b2b_call_t *)r->node->call;
                if (bc->muted[side]) {
                    /* payload 在 buf 里紧跟 header 之后，偏移 = pkt.hdr_len */
                    if (pkt.hdr_len + pkt.payload_len <= sizeof(buf)) {
                        memset(buf + pkt.hdr_len, 0, pkt.payload_len);
                    }
                }
            }
        } else {
            /* RTCP：旁路只读聚合 VQ，不改变转发/丢弃决策
             * （SSRC 校验丢弃/出向重写属 REQ-MEDIA-SEC-003 转发面，P1 补） */
            relay_rtcp_observe(r, side, buf, (size_t)n);
            /* REQ-MEDIA-BUG-001：BUG_RTCP_MIRROR 投递（与 RTP 分支共用 src/dst） */
            if (r->node && r->node->call) {
                media_bug_dispatch_rtcp(r->node->call, side,
                    buf, (size_t)n, s_ip, s_port, d_ip, d_port);
            }
        }
        /* === REQ-NAT-001 comedia：sendto 目标地址 ===
         * 与上方 dst_hint 同逻辑：优先 comedia 学到的真实 latch。 */
        const nsbc_sa_t *dst = r->latched[other]
                               ? &r->latch[other]
                               : (r->have_peer[other]
                                  ? &r->peer[other]
                                  : &r->latch[other]);
        /* REQ-DTLS-001：明文 RTP → WebRTC 侧（对侧有 dtls 终结器）前加密。
         * RTP/RTCP 按 PT 字节判（与入向对称，rtcp-mux 兼容）。
         * RELAY_BUF=2048 对 1500B 以太网包 + AEAD tag(~30B) 余量充足。 */
        size_t send_len = (size_t)n;
        if (r->node && r->node->dtls[other] && r->srtp[other]) {
            int out_rtcp = is_rtcp_packet(buf, (size_t)n);
            size_t cap = sizeof(buf);
            int rc = out_rtcp
                ? nsbc_srtp_protect_rtcp(r->srtp[other], buf, &send_len, cap)
                : nsbc_srtp_protect_rtp(r->srtp[other], buf, &send_len, cap);
            if (rc != NSBC_OK) {
                llog(LOG_WARN, "relay", "SRTP protect FAIL side=%d→%d rc=%d "
                     "out_rtcp=%d send_len_before=%zu cap=%zu",
                     side, other, rc, out_rtcp, send_len, cap);
                ++r->drops;
                continue;
            }
        }
        /* ★ 2026-10-13 诊断：sendto 目标地址（前 5 包 INFO，后续每 100 包 DEBUG）
         * 确认 SBC 把 SRTP 发到 Chrome ICE 候选的真实地址
         * ★ 2026-10-16 v2：加 SSRC 字段，确认 mixed-mode 下重写后的值 */
        {
            static int diag_sendto_seen[2] = {0};
            char ipa[INET6_ADDRSTRLEN] = {0};
            uint16_t dport = 0;
            if (dst->ss.ss_family == AF_INET) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)&dst->ss)->sin_addr,
                          ipa, sizeof(ipa));
                dport = ntohs(((struct sockaddr_in *)&dst->ss)->sin_port);
            } else if (dst->ss.ss_family == AF_INET6) {
                inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&dst->ss)->sin6_addr,
                          ipa, sizeof(ipa));
                dport = ntohs(((struct sockaddr_in6 *)&dst->ss)->sin6_port);
            }
            /* 从 RTP header 取 SSRC（offset 8, 4 bytes）— mixed-mode 下是重写后的值 */
            uint32_t ssrc = 0;
            if (n >= 12) {
                memcpy(&ssrc, buf + 8, 4);
                ssrc = ntohl(ssrc);
            }
            if (diag_sendto_seen[side] < 5) {
                llog(LOG_NOTICE, "relay",
                     "★ SENDTO side=%d→%d pt=%u ssrc=0x%08x dst=%s:%u send_len=%zu "
                     "latched[other]=%d have_peer[other]=%d srtp_protect=%d",
                     side, other, (unsigned)(buf[1] & 0x7F), ssrc,
                     ipa, (unsigned)dport, send_len,
                     r->latched[other], r->have_peer[other],
                     !!(r->node && r->node->dtls[other] && r->srtp[other]));
                ++diag_sendto_seen[side];
            } else if (r->pkts % 100 == 0) {
                llog(LOG_DEBUG, "relay", "FWD side=%d→%d send_len=%zu "
                     "dst=%s:%u dst_fam=%d",
                     side, other, send_len, ipa, (unsigned)dport,
                     dst->ss.ss_family);
            }
        }
        ssize_t sret = sendto(r->fd_rtp[other], buf, send_len, MSG_NOSIGNAL,
                              (const struct sockaddr *)&dst->ss, dst->len);
        if (sret != (ssize_t)send_len) {
            llog(LOG_WARN, "relay", "sendto FAIL side=%d→%d ret=%zd errno=%d "
                 "expected=%zd dst=%s", side, other, sret, errno, send_len, "?");
        }
        ++r->pkts;
        /* ★ 诊断：每 100 包打一条 INFO，同时看两个方向转发量 */
        if (r->pkts % 100 == 0) {
            llog(LOG_DEBUG, "relay", "★ FWD side=%d→%d pkts=%llu drops=%u "
                 "latched[0]=%d latched[1]=%d srtp[0]=%p srtp[1]=%p",
                 side, other,
                 (unsigned long long)r->pkts, (unsigned)r->drops,
                 r->latched[0], r->latched[1],
                 (void *)r->srtp[0], (void *)r->srtp[1]);
        }
    }
}

/* RTCP 旁路 VQ 聚合 + SDES CNAME 绑定（REQ-MEDIA-SEC-003⑤ / REQ-OBS-010）：
 * 只解析不干预转发。
 * P0 单音频 m-line：取 compound 中首个带报告块的 SR(200)/RR(201) 的
 * 第一块（fraction lost/累计丢包/到达抖动，RFC 3550 §6.4.1），并扫
 * SDES(pt=202) 取 CNAME 与当前侧已学习主源 SSRC 匹配。
 * jitter 为媒体时钟 tick 原值（时钟率随 payload，未解析不做 ms 换算，
 * 严禁估算）；R-Factor/MOS 模型 P1。 */
static void relay_rtcp_observe(rtp_relay_t *r, int side, const uint8_t *buf,
                               size_t len)
{
    rtcp_iter_t it;
    rtcp_pkt_t pk;
    int rr_seen = 0;
    rtcp_iter_init(&it, buf, len);
    while (rtcp_next(&it, &pk)) {
        /* SDES(pt=202)：CNAME 与本侧已学习入向主源 SSRC 匹配则落槽。
         * 不匹配则跳过（可能是附加流或 BUNDLE 其他 m-line 的 SDES，
         * P0 单音频场景无冲突，多 m-line 取首段）。 */
        if (pk.pt == 202) {
            uint32_t ssrc;
            sip_tok_t cname;
            if (rtcp_sdes_cname(&pk, &ssrc, &cname) == NSBC_OK &&
                cname.len > 0) {
                uint32_t learned = __atomic_load_n(
                    &r->d_in_ssrc[side], __ATOMIC_ACQUIRE);
                if (learned && ssrc == learned) {
                    size_t n = cname.len < sizeof(r->d_cname[side]) - 1
                                   ? cname.len
                                   : sizeof(r->d_cname[side]) - 1;
                    /* 序列计数器守护：先清 0 让 OAM 读者跳过，
                     * memcpy 完成后置 len+1（≥1，REVISION）。
                     * 读者 double-check 防 torn read（char 数组非原子）。 */
                    __atomic_store_n(&r->d_have_cname[side], 0,
                                     __ATOMIC_RELEASE);
                    memcpy(r->d_cname[side], cname.p, n);
                    r->d_cname[side][n] = '\0';
                    __atomic_store_n(&r->d_have_cname[side], (int)(n + 1),
                                     __ATOMIC_RELEASE);
                }
            }
            continue;
        }
        if (rr_seen)
            continue;
        if ((pk.pt != 200 && pk.pt != 201) || pk.rc < 1)
            continue;
        rtcp_rb_t rb;
        if (rtcp_rb_get(&pk, 0, &rb) != NSBC_OK)
            continue;
        __atomic_store_n(&r->d_have_rr[side], 1, __ATOMIC_RELEASE);
        __atomic_store_n(&r->d_rr_frac[side], (uint32_t)rb.fraction,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&r->d_rr_lost[side], rb.lost, __ATOMIC_RELEASE);
        __atomic_store_n(&r->d_rr_jitter[side], rb.jitter,
                         __ATOMIC_RELEASE);
        rr_seen = 1;
    }
}

static void relay_cb(int fd, uint32_t ev, void *arg)
{
    (void)ev;
    rtp_relay_t *r = arg;
    /* REQ-DTLS-001：STUN/DTLS/SRTP demux 全在 relay_recv_one 内部
     * （peek 分型：STUN 应答、DTLS 喂握手、SRTP 解密后 relay）——
     * 此处不做状态分流，避免 BIO_dgram 与 relay 层抢读同 fd。 */
    for (int side = 0; side < 2; ++side) {
        if (fd == r->fd_rtp[side]) {
            relay_recv_one(r, side, 0, fd);
            return;
        }
        if (fd == r->fd_rtcp[side]) {
            relay_recv_one(r, side, 1, fd);
            return;
        }
    }
}

static uint32_t ptr_shard(const void *c)
{
    return ((uintptr_t)c >> 4) & (BR_SHARDS - 1);
}

static cnode_t *entry_find(bridge_mgr_t *m, const void *c)
{
    br_shard_t *s = &m->sh[ptr_shard(c)];
    for (cnode_t *p = s->head; p; p = p->next)
        if (p->call == c)
            return p;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* mgr                                                                  */
/* ------------------------------------------------------------------ */
int bridge_mgr_create(const mb_cfg_t *cfg, bridge_mgr_t **out)
{
    bridge_mgr_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NSBC_NOMEM;
    m->cfg = *cfg;
    if (!m->cfg.local_ip)
        m->cfg.local_ip = "0.0.0.0";
    for (int i = 0; i < BR_SHARDS; ++i)
        pthread_mutex_init(&m->sh[i].lock, NULL);
    pthread_mutex_init(&m->rec_hook_lock, NULL);  /* REQ-AI-001 */
    pthread_cond_init(&m->rec_hook_cond, NULL);

    /* codec 注册由 src/mod/codecs 模块 init 自注册到全局 codec_registry，
     * 本函数只负责 bridge 管理器创建，不硬编码任何 codec。
     * bridge_confirm 查 registry，没 codec 就跳过转码。 */

    *out = m;
    return NSBC_OK;
}

void bridge_mgr_destroy(bridge_mgr_t *m)
{
    if (!m)
        return;
    pthread_mutex_lock(&m->rec_hook_lock);
    mb_rec_hook_t *h = __atomic_load_n(&m->rec_hooks, __ATOMIC_RELAXED);
    __atomic_store_n(&m->rec_hooks, NULL, __ATOMIC_RELEASE);
    for (mb_rec_hook_t *it = h; it; it = it->next) {
        while (it->refs > 0)
            pthread_cond_wait(&m->rec_hook_cond, &m->rec_hook_lock);
    }
    pthread_mutex_unlock(&m->rec_hook_lock);
    while (h) { mb_rec_hook_t *nx = h->next; free(h); h = nx; }
    pthread_cond_destroy(&m->rec_hook_cond);
    pthread_mutex_destroy(&m->rec_hook_lock);
    free(m);
}

/* ------------------------------------------------------------------ */
/* 决策                                                                 */
/* ------------------------------------------------------------------ */
static int is_private_v4(const char *ip)
{
    unsigned a = 0, b = 0;
    if (sscanf(ip, "%u.%u", &a, &b) != 2)
        return 0;
    if (a == 10 || a == 127 || (a == 169 && b == 254))
        return 1;
    if (a == 172 && b >= 16 && b <= 31)
        return 1;
    if (a == 192 && b == 168)
        return 1;
    return 0;
}

/* 令牌转 NUL 串后判私网 */
static int is_private_tok(const char *ip, size_t len)
{
    char tmp[64];
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1;
    memcpy(tmp, ip, len);
    tmp[len] = '\0';
    return is_private_v4(tmp);
}

static int codec_intersect(const char *off, size_t ol, const char *ans,
                           size_t al)
{
    sdp_t so, sa;
    if (sdp_parse(&so, off, ol) != NSBC_OK ||
        sdp_parse(&sa, ans, al) != NSBC_OK)
        return 1; /* 解析保守不判 transcode */
    const sdp_media_t *mo = NULL, *ma = NULL;
    for (int i = 0; i < so.nmedia; ++i)
        if (so.media[i].type == SDP_M_AUDIO) { mo = &so.media[i]; break; }
    for (int i = 0; i < sa.nmedia; ++i)
        if (sa.media[i].type == SDP_M_AUDIO) { ma = &sa.media[i]; break; }
    if (!mo || !ma)
        return 1;
    sdp_media_build_codecs(&so, (sdp_media_t *)mo);
    sdp_media_build_codecs(&sa, (sdp_media_t *)ma);
    for (int i = 0; i < ma->ncodecs; ++i) {
        for (int j = 0; j < mo->ncodecs; ++j) {
            if (ma->codecs[i].enc.len == mo->codecs[j].enc.len &&
                !memcmp(ma->codecs[i].enc.p, mo->codecs[j].enc.p,
                        ma->codecs[i].enc.len))
                return 1;
        }
    }
    return 0;
}

/* WebRTC SDP 特征：DTLS-SRTP + ICE。
 * relay 层仅处理明文 RTP（RTP/AVP / RTP/SAVP 非加密），
 * WebRTC 的 UDP/TLS/RTP/SAVPF / a=fingerprint / a=ice-ufrag 必须 BYPASS，
 * 让浏览器走 P2P ICE/DTLS 直连。内网场景 NAT 穿透可用 host candidate。 */
static int is_webrtc_sdp(const char *sdp, size_t len)
{
    if (!sdp || !len) return 0;
    if (memmem(sdp, len, "UDP/TLS", 7)) return 1;
    if (memmem(sdp, len, "a=fingerprint:", 14)) return 1;
    if (memmem(sdp, len, "a=ice-ufrag:", 13)) return 1;
    return 0;
}

/* ★ REQ-MEDIA-SEC-004：从 SDP 里提取 a=fingerprint:sha-256
 * 之前用 sdp_parse → sdp_fingerprint_get，但 sdp_parse 对 CRLF 行尾
 * 或某些浏览器 SDP 的细节会返回 FAIL，导致 peer_fp 永远没注入 →
 * DTLS 握手走"宽松模式"（不校验证书指纹）。
 * 这里用 memmem 直接定位 "a=fingerprint:sha-256 " 前缀，
 * 完全绕过完整 SDP 解析器，顺手做 \r 清理 + 小写规范化。 */
static int extract_fp_simple(const char *sdp, size_t len,
                             char *out, size_t outsz)
{
    static const char pfx[] = "a=fingerprint:sha-256 ";
    const size_t plen = sizeof(pfx) - 1;
    const char *p = memmem(sdp, len, pfx, plen);
    if (!p) return -1;
    p += plen;
    const char *end = memchr(p, '\n', (size_t)(sdp + len - p));
    if (!end) end = sdp + len;
    /* 回剥尾部 \r \t 空格 */
    while (end > p && (end[-1] == '\r' || end[-1] == ' '
                       || end[-1] == '\t'))
        --end;
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= outsz) return -1;
    for (size_t i = 0; i < n; ++i)
        out[i] = (char)tolower((unsigned char)p[i]);
    out[n] = '\0';
    return 0;
}

static void inject_peer_fp(dtls_server_t *ds, const char *sdp, size_t len)
{
    if (!ds || !sdp || len == 0) return;
    char fp_str[NSBC_SSL_FP_SHA256_SIZE];
    if (extract_fp_simple(sdp, len, fp_str, sizeof(fp_str)) != 0) {
        llog(LOG_WARN, "dtls", "inject_peer_fp: fp extract FAIL fd=%d",
             dtls_server_fd(ds));
        return;
    }
    llog(LOG_INFO, "dtls", "inject_peer_fp: fd=%d fp=[%s]",
         dtls_server_fd(ds), fp_str);
    dtls_server_set_peer_fp(ds, fp_str);
}

int bridge_decide(bridge_mgr_t *m, const char *offer, size_t offer_len,
                  const char *answer, size_t answer_len, mb_mode_t *mode)
{
    /* DTLS-SRTP (WebRTC) 腿 → 模式决策：
     *   两侧都是 WebRTC (offer_ws && answer_ws) → 可以 BYPASS
     *     （两端浏览器用 ICE/DTLS 建 P2P 隧道，SBC 只转发 SIP 信令；
     *      L840-844 注释原本就写明 WebRTC "必须 BYPASS 让浏览器走 P2P
     *      ICE/DTLS 直连"，之前手滑写成 OR 错误强制 RELAY）；
     *   只有一侧 WebRTC (offer_ws XOR answer_ws) → 必须 SRTP_RELAY
     *     （另一侧 MicrosIP 等软电话不支持 DTLS-SRTP，SBC 终结 WebRTC
     *     侧 DTLS，明文侧走 RTP relay）。
     * 关键：offer 侧是 WebRTC 但 answer 还没到（answer=NULL，bridge_offer_hook
     * 阶段）时，此处不提前强制 SRTP_RELAY——hook 层有 b_ws 回调从 leg[1]
     * 反查 B 腿是否也是 WebRTC，权威信息更完整。 */
    const int offer_ws = is_webrtc_sdp(offer, offer_len);
    const int answer_ws = answer ? is_webrtc_sdp(answer, answer_len) : 0;
    const int answer_present = answer != NULL;
    const int mixed_ws = answer_present && (offer_ws != answer_ws);
    const int both_ws  = answer_present && offer_ws && answer_ws;

    /* 混合腿（恰好一侧 WebRTC 且 answer 已知）→ 必须 SRTP_RELAY */
    if (mixed_ws) {
        *mode = MB_SRTP_RELAY;
        return NSBC_OK;
    }
    /* 两侧都 WebRTC 且 answer 已知 → auto 默认 SRTP_RELAY（SBC 终结两侧 DTLS）
     *   BYPASS 让浏览器 P2P 必须由 rule=bypass 显式指定，auto 不盲目放
     *   （两侧浏览器同网段 NAT-free 才值得 P2P，公网场景 ICE 穿透经常失败
     *    反而丢媒体——auto 保守锚定；rule=bypass 作为优化路径）。*/
    if (both_ws) {
        *mode = MB_SRTP_RELAY;
        return NSBC_OK;
    }
    /* offer 是 WebRTC 但 answer 还没到 → 留给 hook 层完整决策（hook 有 b_ws 回调
     * 从 leg[1] 反查 B 腿类型，比 bridge_decide 信息更完整） */
    if (offer_ws && !answer_present) {
        /* 继续往下跑 NAT/allow_bypass 决策作为初始值，hook 层稍后会修正 */
    }

    if (answer) {
        if (!codec_intersect(offer, offer_len, answer, answer_len)) {
            /* codec 不相交 → 保守走 RELAY 而不是强制 TRANSCODE
             *
             * P0 保守策略：TRANSCODE 依赖 codec_registry（g711_register 必须成功），
             * 如果 registry 空或 transcode_open 失败，走 TRANSCODE 会 drop 媒体。
             * RELAY 模式下 codec 不同不会主动 drop——RTP payload 原样透传，
             * 两端各自按自己的 codec 解码（SBC 不做 transcoding，只做 transport）。
             * 这是参考版能正常转发的关键：纯 relay，不碰 codec。
             *
             * 真正需要 TRANSCODE 的场景（G711↔Opus 异构）P1 再上。 */
            llog(LOG_INFO, "media",
                 "bridge_decide: codec_intersect=0 → RELAY (codec mismatch, conservative)");
            *mode = m->cfg.allow_bypass ? MB_BYPASS : MB_RELAY;
            return NSBC_OK;
        }
        /* 双方地址（取首个音频 c=） */
        sdp_t sa;
        sip_tok_t ip = {NULL, 0};
        if (sdp_parse(&sa, answer, answer_len) == NSBC_OK) {
            for (int i = 0; i < sa.nmedia; ++i) {
                const sdp_conn_t *c =
                    sdp_media_effective_conn(&sa, &sa.media[i]);
                if (c && c->addr.p) { ip = c->addr; break; }
            }
        }
        if (m->cfg.trust_private && ip.p &&
            is_private_tok(ip.p, ip.len)) {
            *mode = MB_BYPASS;
            return NSBC_OK;
        }
    }
    *mode = m->cfg.allow_bypass ? MB_BYPASS : MB_RELAY;
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */
/* SDP 重写                                                             */
/* ------------------------------------------------------------------ */
/* 写一行到 out buffer。溢出返回 NSBC_NOSPC，而非静默丢弃——
 * 之前静默 return 导致 bridge_rewrite_sdp 在 buffer 紧张时输出残缺 SDP
 * （半截属性行 + 后续行错位），上层 memcpy 出来就是脏数据。 */
static int put_line(char *out, size_t cap, size_t *used, const char *p,
                    size_t n)
{
    if (*used + n > cap)
        return NSBC_NOSPC;
    memcpy(out + *used, p, n);
    *used += n;
    return NSBC_OK;
}

/* 需剥离的 ICE/DTLS 属性行前缀（PLAIN 消毒 / WS 换凭证 共用）：
 * PLAIN——WebRTC 特征行明文端不认识（MicroSIP 400 Bad SDP 根因）；
 * WS——剥对端旧凭证，调用方随后注入本侧 dtls_server 的新凭证。 */
static int attr_is_ice_dtls(const char *p, size_t llen)
{
    static const char *const pfx[] = {
        "a=ice-ufrag:", "a=ice-pwd:", "a=ice-options:",
        "a=fingerprint:", "a=setup:", "a=candidate:",
        "a=remote-candidates:", "a=identity:",
        /* REQ-MEDIA-SSRC-001：剥掉 SSRC 声明行
         * relay 会重写入向 RTP 的 SSRC —— Chrome 严格按 a=ssrc: 匹配，
         * 剥掉后走 mid/msid 匹配。 */
        "a=ssrc:", "a=ssrc-group:", "a=ssrc-semantics:",
        /* ★ WebRTC answer 清理：剥掉 B 侧（普通 SIP）私货属性，
         * 避免泄漏到 A 侧（WebRTC）引发歧义或与 rtcp-mux 冲突。 */
        "a=X-nat:", "a=rtcp:",
        NULL,
    };
    for (int i = 0; pfx[i]; ++i) {
        size_t n = strlen(pfx[i]);
        if (llen >= n && memcmp(p, pfx[i], n) == 0)
            return 1;
    }
    return 0;
}

/* 剥掉 b=AS: / b=TIAS: 带通行（session 级 + media 级都可能出现）。
 * WebRTC answer 不需要这些传统 SIP 带通声明。 */
static int line_is_bandwidth(const char *p, size_t llen)
{
    static const char *const pfx[] = { "b=AS:", "b=TIAS:", NULL };
    for (int i = 0; pfx[i]; ++i) {
        size_t n = strlen(pfx[i]);
        if (llen >= n && memcmp(p, pfx[i], n) == 0)
            return 1;
    }
    return 0;
}

/* PLEN 方向（A=WebRTC → B=UDP 明文侧）额外剥离属性：
 * 这些是 WebRTC 浏览器私货，UDP 呼叫中心/IVR/FreeSWITCH 不认识，
 * 而且让 SDP 体积膨胀 2~3KB（每种 H264 profile + RTX + rtcp-fb 各占几行）。
 *
 * 目标：把 WebRTC offer 4KB+ 精简到 <1400B，避开 UDP SIP MTU 分片风险。 */
static int attr_is_webrtc_only(const char *p, size_t llen)
{
    static const char *const pfx[] = {
        /* 分组/协商 */
        "a=group:", "a=msid-semantic:",
        /* RTP header extensions（浏览器专用） */
        "a=extmap:",
        /* RTCP 扩展 */
        "a=rtcp-mux", "a=rtcp-rsize",
        /* MSID（浏览器唯一标识） */
        "a=msid:",
        /* RTCP feedback（全部剥）+ fmtp 只留 H264 PT=102 profile 42e01f 一个 */
        "a=rtcp-fb:",
        "a=fmtp:103", "a=fmtp:104", "a=fmtp:108", "a=fmtp:114",
        "a=fmtp:116", "a=fmtp:39",  "a=fmtp:118",
        "a=fmtp:105", "a=fmtp:119", "a=fmtp:107", "a=fmtp:109",
        "a=fmtp:115", "a=fmtp:117", "a=fmtp:40",  "a=fmtp:99",
        "a=fmtp:101", "a=fmtp:98",  "a=fmtp:100",
        /* RTP retransmission（RTX 冗余 PT）。
         * ★ 注意 101 必须带 " rtx" 后缀：裸 "a=rtpmap:101" 会误伤音频段的
         * "a=rtpmap:101 telephone-event/8000"（浏览器 answer 回显 A offer 的
         * DTMF PT）→ rtpmap 被剥 → RFC2833 无法协商。视频 RTX 行是
         * "a=rtpmap:101 rtx/90000"，带 rtx 前缀照样命中。 */
        "a=rtpmap:103", "a=rtpmap:107", "a=rtpmap:109", "a=rtpmap:115",
        "a=rtpmap:117", "a=rtpmap:40",  "a=rtpmap:99",  "a=rtpmap:101 rtx",
        /* VP9（只留 H264 profile 42e01f baseline 一个即可） */
        "a=rtpmap:98 VP9", "a=rtpmap:100 VP9",
        /* H264 高级 profile（packetization-mode=1/0 都留一个就够） */
        "a=rtpmap:104 H264", "a=rtpmap:108 H264",
        "a=rtpmap:114 H264", "a=rtpmap:116 H264",
        "a=rtpmap:39 H264",  "a=rtpmap:118 H264",
        /* 上面这些 H264 的 RTX 冗余 PT */
        "a=rtpmap:105", "a=rtpmap:119",
        /* 48k telephone-event — PLEN audio 只留 8k（126），48k（110）剥 */
        "a=rtpmap:110 telephone-event",
        /* mid 标识 — WebRTC BUNDLE 用的，PLEN 不需要 */
        "a=mid:",
        NULL,
    };
    for (int i = 0; pfx[i]; ++i) {
        size_t n = strlen(pfx[i]);
        if (llen >= n && memcmp(p, pfx[i], n) == 0)
            return 1;
    }
    return 0;
}

/* 精简 m= 行的 fmt-list —— PLEN 模式只保留原始 offer 里已有的子集：
 *   audio 段：PCMA(8) / PCMU(0) + telephone-event 8k(126)
 *   video 段：只保留一个 H264 baseline PT(102)
 * 关键：从原始 fmt-list 挑子集，**不塞新 PT**（否则后面没 rtpmap 会让 SIP 端困惑）。
 * 原始 m=audio "8 110 126" → 精简成 "8 126"
 * 原始 m=video "102 103 104 107 108 ...(18 个)" → 精简成 "102" */
static int plen_slim_fmt_list(char *mline, size_t mline_cap)
{
    /* m=<type> <port> <proto> <pt1> <pt2> ... → 只留前三个 token + 精简后的 PT 子集 */
    const char *sp1 = strchr(mline, ' ');
    if (!sp1) return 0;
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return 0;
    const char *sp3 = strchr(sp2 + 1, ' ');
    if (!sp3) return 0;  /* 已经没 fmt-list，不用精 */
    /* ★ media_type 要跳过 "m=" 前缀！mline="m=audio 59996..." → media_type="audio" */
    const char *media_type = mline + 2;  /* skip "m=" */
    size_t type_len = (size_t)(sp1 - media_type);  /* "audio"=5, "video"=5 */
    const char *port_str = sp1 + 1;
    size_t port_len = (size_t)(sp2 - sp1 - 1);
    const char *proto_str = sp2 + 1;
    size_t proto_len = (size_t)(sp3 - sp2 - 1);

    const char *pt_tail = sp3 + 1;  /* 原始 fmt-list 开始 */
    while (*pt_tail == ' ') ++pt_tail;  /* 跳过 proto 和 fmt-list 之间的空格 */
    char buf[128];
    size_t tlen = strlen(pt_tail);
    if (tlen >= sizeof(buf)) tlen = sizeof(buf) - 1;
    memcpy(buf, pt_tail, tlen); buf[tlen] = '\0';

    char keep[128]; keep[0] = '\0';
    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &saveptr); tok;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        int keep_it = 0;
        if (type_len == 5 && memcmp(media_type, "audio", 5) == 0) {
            keep_it = (strcmp(tok, "8") == 0       /* PCMA */
                    || strcmp(tok, "0") == 0       /* PCMU */
                    || strcmp(tok, "126") == 0);   /* telephone-event 8k */
        } else if (type_len == 5 && memcmp(media_type, "video", 5) == 0) {
            keep_it = (strcmp(tok, "102") == 0);   /* 唯一保留的 H264 */
        }
        if (keep_it) {
            size_t cur = strlen(keep);
            if (cur + strlen(tok) + 2 < sizeof(keep)) {
                if (cur > 0) { keep[cur] = ' '; cur++; }
                strncpy(keep + cur, tok, sizeof(keep) - cur - 1);
                keep[sizeof(keep) - 1] = '\0';
            }
        }
    }
    if (!keep[0]) {
        llog(LOG_WARN, "media", "plen_slim: KEEP_EMPTY type=%.*s orig='%s'",
             (int)type_len, media_type, mline);
        return 0;
    }

    char tmp[128];
    int n = snprintf(tmp, sizeof(tmp),
                     "m=%.*s %.*s %.*s %s",
                     (int)type_len, media_type,
                     (int)port_len, port_str,
                     (int)proto_len, proto_str,
                     keep);
    if (n > 0 && (size_t)n < mline_cap) {
        memcpy(mline, tmp, (size_t)n);
        mline[n] = '\0';
        return n;
    }
    return 0;
}

/* ---- per-section ICE/DTLS 注入 ----
 *
 * 原始 offer/answer 里的 a=ice-* / a=fingerprint / a=setup / a=candidate
 * 行会被 bridge_rewrite_sdp(dir=WS) 全部剥掉。调用方（offer_hook/answer_hook）
 * 需要把 dtls_server 自己生成的凭证注入回去。
 *
 * 关键约束：浏览器对 BUNDLE 模式要求**每个 m= 段**（audio + video ...）
 * 都必须有 a=fingerprint + a=setup + a=candidate。如果只在末尾追加一份，
 * 按 RFC 4566 媒体级属性属于"紧接前面的 m= 段"——导致只有最后一个媒体段
 * 有凭证、前面的段全空。Chrome 对 BUNDLE base（通常是 audio/mid:0）做检查时
 * 发现没 fingerprint → setRemoteDescription 直接失败 → 480。
 *
 * 所以这里实现：扫描 SDP，在每个 m= 段末尾（下一个 m= 行之前或 SDP 结尾）
 * 插入注入块，每段一份相同的。 */
static int sdp_inject_per_section(const char *in, size_t inlen,
                                  char *out, size_t outsz,
                                  const char *inject, size_t inject_len,
                                  int skip_rejected)
{
    if (!in || !out || !inject || inlen == 0 || inject_len == 0)
        return NSBC_INVAL;
    if (inlen >= outsz)
        return NSBC_NOSPC;
    memset(out, 0, outsz);  /* 防御：out 不能带脏数据 */
    size_t used = 0;
    const char *p = in;
    const char *end = in + inlen;
    int has_section = 0;
    int section_rejected = 0;  /* 当前 m= 段是否被拒（port=0） */
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        size_t llen = (size_t)(line_end - p);

        if (llen >= 2 && p[0] == 'm' && p[1] == '=') {
            /* 新媒体段开始——先把上一段的注入块写出去（如果有且未拒绝） */
            if (has_section && !section_rejected) {
                if (used + inject_len >= outsz) return NSBC_NOSPC;
                memcpy(out + used, inject, inject_len);
                used += inject_len;
            }
            has_section = 1;
            /* 检测本段是否被拒：m=<media> 0 <proto> ... */
            section_rejected = 0;
            if (skip_rejected) {
                const char *mt_end = p + 2;
                while (mt_end < line_end && *mt_end != ' ') ++mt_end;
                const char *ps = mt_end;
                while (ps < line_end && *ps == ' ') ++ps;
                if (ps < line_end && *ps == '0' &&
                    (ps[1] == ' ' || ps[1] == '\r'))
                    section_rejected = 1;
            }
        }
        /* 原样复制当前行 + 行尾 \n */
        if (used + llen + 1 >= outsz) return NSBC_NOSPC;
        memcpy(out + used, p, llen);
        used += llen;
        if (eol) { out[used] = '\n'; ++used; }

        p = eol ? eol + 1 : end;
    }
    /* 最后一段也要注入（如果未拒绝） */
    if (has_section && !section_rejected) {
        if (used + inject_len >= outsz) return NSBC_NOSPC;
        memcpy(out + used, inject, inject_len);
        used += inject_len;
    }
    if (used >= outsz) return NSBC_NOSPC;
    out[used] = '\0';
    return (int)used;
}

/* ---- 缺失 video m-line 补全 ----
 *
 * 纯 SIP 终端（MicroSIP）的 offer/answer 可能只有 audio m-line（没有 m=video）。
 * 但 WebRTC 侧（Chrome）的 RTCPeerConnection 默认创建 audio + video 两个
 * transceivers —— setRemoteDescription 收到只有 1 段 m= 的 offer 时，Chrome
 * 无法匹配本地 video transceiver → 报 WebRTC Error。
 *
 * 修复：向 WebRTC 侧（dtls[0] 或 dtls[1] 非空）发送的 SDP 里，如果没有 video
 * m-line，补一个 rejected video m-line（port=0 + 空 PT 列表 + a=inactive）。
 *
 * 这个函数在 bridge_rewrite_sdp 之后调用，只做"有没有 m=video"检测 + 追加
 * rejected video 段。追加后后续的 sdp_inject_per_section / sdp_build_webrtc_answer
 * 会正常识别并处理 skip_rejected（不注入 ICE，只补 mid/inactive）。 */
static int sdp_ensure_video_mline(char *buf, size_t cap, int *len,
                                  const char *proto_line)
{
    const char *needle = "m=video ";
    if (strstr(buf, needle)) {
        llog(LOG_INFO, "sdp",
             "★ENSURE_VIDEO: m=video already present (skip append)");
        return NSBC_OK;   /* 已经有 video m-line，什么都不做 */
    }

    llog(LOG_WARN, "sdp",
         "★ENSURE_VIDEO: NO m=video found! Appending port=0 placeholder...");

    /* 追加 rejected video 段——RFC 4566 要求 m= 行必须有 fmt-list，
     * rejected 段至少放一个 PT（96 是动态 payload type），且必须带
     * 对应的 a=rtpmap 声明（JsSIP 严格校验 fmt-list 里的每个 PT
     * 都要有 rtpmap，即使 port=0 拒绝段也不例外）。
     *
     * 后续 sdp_build_webrtc_answer 会在 c= 行后补 a=mid:N + a=inactive
     * + a=rtcp-mux（skip_rejected 路径，不注入 ICE），产出完整的拒绝段。 */
    char extra[256];
    int ne = snprintf(extra, sizeof(extra),
        "m=video 0 %s 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:96 H264/90000\r\n",
        proto_line ? proto_line : "UDP/TLS/RTP/SAVPF");
    if (ne <= 0) return NSBC_NOSPC;

    if ((size_t)*len + (size_t)ne >= cap) {
        llog(LOG_WARN, "sdp", "ensure_video_mline overflow len=%d ne=%d cap=%zu",
             *len, ne, cap);
        return NSBC_NOSPC;
    }
    memcpy(buf + *len, extra, (size_t)ne);
    *len += ne;
    buf[*len] = '\0';
    llog(LOG_DEBUG, "sdp", "ensure_video_mline: appending rejected m=video 0");
    return NSBC_OK;
}

/* ---- WebRTC answer 骨架补全 ----
 *
 * bridge_rewrite_sdp(dir=WS) 只做了 proto/port/c= 的替换 + 剥掉 ICE/DTLS 旧属性。
 * 但 answer 还需要 WebRTC 特有的骨架才能被 Chrome/JsSIP 接受：
 *   a=group:BUNDLE 0 1 2 ...             ← session 级（第一 m= 之前）
 *   每段：a=mid:<idx> + a=rtcp-mux        ← BUNDLE 强制要求
 *   非拒绝段：追加 ice_extra（ICE/DTLS 凭证）
 *   拒绝段（port=0）：追加 rejected_extra（a=inactive），不加 ICE
 * 这个函数替代 answer_hook 里对 sdp_inject_per_section 的调用。 */
static int sdp_build_webrtc_answer(
    const char *in, size_t inlen,
    char *out, size_t outsz,
    const char *ice_extra, size_t ice_extra_len,
    const char *rejected_extra, size_t rejected_extra_len,
    int start_mid)
{
    if (!in || !out || inlen == 0 || inlen >= outsz) return NSBC_INVAL;
    memset(out, 0, outsz);
    size_t used = 0;
    const char *p = in;
    const char *end = in + inlen;
    int media_idx = 0;
    int first_mline = 1;   /* 还没遇到第一个 m= 行 */
    int section_rejected = 0;
    /* 先扫一遍数 m= 段数量，构建 BUNDLE 字符串——我们需要知道后面有几个才好拼 */
    int n_mline = 0;
    {
        const char *sp = in;
        while (sp < end) {
            const char *se = memchr(sp, '\n', (size_t)(end - sp));
            const char *se_nul = se ? se : end;
            size_t slen = (size_t)(se_nul - sp);
            if (slen >= 2 && sp[0] == 'm' && sp[1] == '=') ++n_mline;
            if (!se) break;
            sp = se + 1;
        }
    }
    /* 构建 session 级 a=group:BUNDLE 字符串："a=group:BUNDLE 0 1 ... \r\n" */
    char bundle_line[128];
    int bpos = snprintf(bundle_line, sizeof(bundle_line), "a=group:BUNDLE");
    for (int i = 0; i < n_mline && bpos > 0 && (size_t)bpos < sizeof(bundle_line) - 16; ++i) {
        bpos += snprintf(bundle_line + bpos, sizeof(bundle_line) - (size_t)bpos,
                         " %d", start_mid + i);
    }
    if (bpos > 0 && (size_t)bpos < sizeof(bundle_line) - 4)
        bpos += snprintf(bundle_line + bpos, sizeof(bundle_line) - (size_t)bpos, "\r\n");

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        size_t llen = (size_t)(line_end - p);
        int fix_rejected_video_pt = 0;  /* ★ 提到外层作用域——在 m= 块内设置，块外复制时使用 */

        /* ★ 在 session 级（还没遇到任何 m= 行）跳过 a=group: 和 a=sendrecv
         * sdp_build_webrtc_answer 会在第一个 m= 行前（media_idx==0）自己重建这俩行。
         * bridge_rewrite_sdp(dir=WS) 不剥 a=group:（attr_is_webrtc_only 只在 dir=PLAIN 时生效），
         * 如果这里不手动跳过就会导致输出 SDP 里 a=group:BUNDLE × 2 → 浏览器
         * setRemoteDescription 报 "duplicate group" → reject → 488 Not Acceptable Here。 */
        if (first_mline && llen >= 2 && p[0] == 'a' && p[1] == '=') {
            if ((llen >= 14 && memcmp(p, "a=group:BUNDLE", 14) == 0) ||
                (llen >= 13 && memcmp(p, "a=sendrecv\r\n", 12) == 0) ||
                (llen >= 11 && memcmp(p, "a=sendrecv", 10) == 0)) {
                p = eol ? eol + 1 : end;
                continue;
            }
        }

        if (llen >= 2 && p[0] == 'm' && p[1] == '=') {
            /* 新媒体段 —— 先处理上一段的尾巴 */
            if (!first_mline) {
                /* 上一段（非拒绝）：补 mid/rtcp-mux + ICE */
                if (!section_rejected) {
                    char mid_line[64], mux_line[32];
                    int ml = snprintf(mid_line, sizeof(mid_line),
                                      "a=mid:%d\r\n", start_mid + media_idx - 1);
                    int rl = snprintf(mux_line, sizeof(mux_line), "a=rtcp-mux\r\n");
                    if (ml > 0 && used + (size_t)ml < outsz) {
                        memcpy(out + used, mid_line, (size_t)ml); used += (size_t)ml;
                    }
                    if (rl > 0 && used + (size_t)rl < outsz) {
                        memcpy(out + used, mux_line, (size_t)rl); used += (size_t)rl;
                    }
                    /* ICE/DTLS 凭证（如果有） */
                    if (ice_extra && ice_extra_len > 0 &&
                        used + ice_extra_len < outsz) {
                        memcpy(out + used, ice_extra, ice_extra_len);
                        used += ice_extra_len;
                    }
                } else {
                    /* 上一段被拒：补 a=mid + rejected_extra（通常是 a=inactive）。
                     * 不补 ICE（rejected 段不参与 DTLS）、不补 rtcp-mux
                     * （没有实际 RTP 流，rtcp-mux 无意义）。
                     * 关键：a=mid 必须有，BUNDLE 组里列了这个 mid，
                     * Chrome 需要 a=mid 来匹配 transceiver → direction=inactive。 */
                    char mid_line[64];
                    int ml = snprintf(mid_line, sizeof(mid_line),
                                      "a=mid:%d\r\n", start_mid + media_idx - 1);
                    if (ml > 0 && used + (size_t)ml < outsz) {
                        memcpy(out + used, mid_line, (size_t)ml); used += (size_t)ml;
                    }
                    if (rejected_extra && rejected_extra_len > 0 &&
                        used + rejected_extra_len < outsz) {
                        memcpy(out + used, rejected_extra, rejected_extra_len);
                        used += rejected_extra_len;
                    }
                }
            }
            first_mline = 0;
            /* ★ 检测本段是否被拒：m=<media> 0 <proto> ...
             *
             * 只对**明确声明的 rejected 段**生效：port=0 且确实想拒绝。
             * 关键：**video 段即使 port=0 也不判 rejected！** 因为：
             *   a) auto_offer 给 video 分配了 placeholder port（1 或 0），
             *      想表示"有 video codec 但还没分配 relay"，不是想 reject；
             *   b) bridge_rewrite_sdp 可能没处理到 video 段（PLAIN 模式只改 audio）；
             *   c) 浏览器看到 m=video 0 + a=inactive 会**直接丢弃 video 段**，
             *      answer 里不回 m=video 段 → 视频协商彻底失败。
             *
             * 所以 video 段保留原样（port 哪怕 0），让浏览器自己决定要不要协商。
             * 如果 rewrite 成功改了 video port → 正常 ICE/DTLS 注入 → 双向 video；
             * 如果 rewrite 没改 → port=0 但 fmt-list 正确 → 浏览器可能仍协商成功。 */
            section_rejected = 0;
            const char *mt_end = p + 2;
            while (mt_end < line_end && *mt_end != ' ') ++mt_end;
            const char *mt_name = p + 2;
            size_t mt_name_len = (size_t)(mt_end - mt_name);
            const char *ps = mt_end;
            while (ps < line_end && *ps == ' ') ++ps;

            /* 只有 audio 段的 port=0 才判 rejected。
             * video 段保留原样——哪怕 port=0 也让浏览器决定。 */
            if (mt_name_len == 5 && memcmp(mt_name, "audio", 5) == 0 &&
                ps < line_end && *ps == '0' &&
                (ps[1] == ' ' || ps[1] == '\r' || ps[1] == '\n')) {
                section_rejected = 1;
                llog(LOG_INFO, "dtls",
                     "★ANSWER_MLINE_REJECTED: audio port=0 at_idx=%d", media_idx);
            }

            /* ★ REQ-MEDIA-VIDEO-002：rejected video m-line 必须带合法 PT 列表。
             * bridge_rewrite_sdp 对 port=0 清空了 tail_len（避免 PCMU PT=0），
             * 结果变成 m=video 0 UDP/TLS/RTP/SAVPF（无 PT）→ RFC 4566 非法。
             * 而且就算保留了 B 的 PT=0（PCMU 音频），video 段也不能用音频 PT。
             * 所以在复制 m= 行之前，对 rejected video 段整行替换为合法视频 PT 列表。
             * port=0 + a=inactive 已经表达拒绝语义，Chrome 不会真的解码。 */
            if (section_rejected && memcmp(p, "m=video ", 8) == 0)
                fix_rejected_video_pt = 1;

            /* 注入 session 级 BUNDLE（在第一个 m= 行之前） + a=sendrecv 强制双向
             * REQ-MEDIA-DIR-001：SIP PBX 的原始 offer 方向可能是 recvonly（"我只收"），
             * Chrome 作为 answerer 会被迫选 sendonly（只发不收）→ SBC 转过去的 SRTP
             * Chrome 不播放，形成单通。这里强制 a=sendrecv，B2BUA 必须双向音频。 */
            if (media_idx == 0) {
                char dir_line[32];
                int dl = snprintf(dir_line, sizeof(dir_line), "a=sendrecv\r\n");
                if (dl > 0 && used + (size_t)dl < outsz) {
                    memcpy(out + used, dir_line, (size_t)dl); used += (size_t)dl;
                }
                if (bpos > 0 && (size_t)bpos < outsz - used) {
                    memcpy(out + used, bundle_line, (size_t)bpos);
                    used += (size_t)bpos;
                }
            }
            ++media_idx;
        }
        /* ★ 复制当前行——如果是 rejected video m-line 且 PT 需要修复，
         * 输出带合法 PT 列表的新行；否则原样复制。 */
        {
            static const char *VIDEO_REJECT_MLINE =
                "m=video 0 UDP/TLS/RTP/SAVPF "
                "102 103 104 107 108 109 114 115 116 117 39 40 98 99 100 101 118 119";
            const char *to_copy;
            size_t to_copy_len;
            if (fix_rejected_video_pt) {
                /* 新行必须包含行尾 \r\n 或 \n——但我们要先把原行的 \r\n 跳过去，
                 * 因为下面 memcpy 会覆盖。实际上这里更简单：输出新的完整 m= 行
                 * 带 \r\n，不复制原行。 */
                to_copy = VIDEO_REJECT_MLINE;
                to_copy_len = strlen(VIDEO_REJECT_MLINE);
            } else {
                to_copy = p;
                to_copy_len = llen;
            }
            if (used + to_copy_len + 1 >= outsz) return NSBC_NOSPC;
            memcpy(out + used, to_copy, to_copy_len);
            used += to_copy_len;
            if (fix_rejected_video_pt) {
                /* 手动加 \r\n（原行尾不管，后面 memmove 跳过它） */
                if (used + 2 >= outsz) return NSBC_NOSPC;
                out[used++] = '\r';
                out[used++] = '\n';
            } else {
                if (eol) { out[used] = '\n'; ++used; }
            }
        }

        p = eol ? eol + 1 : end;
    }
    /* 最后一段尾巴 */
    if (!first_mline) {
        if (!section_rejected) {
            char mid_line[64], mux_line[32];
            int ml = snprintf(mid_line, sizeof(mid_line),
                              "a=mid:%d\r\n", start_mid + media_idx - 1);
            int rl = snprintf(mux_line, sizeof(mux_line), "a=rtcp-mux\r\n");
            if (ml > 0 && used + (size_t)ml < outsz) {
                memcpy(out + used, mid_line, (size_t)ml); used += (size_t)ml;
            }
            if (rl > 0 && used + (size_t)rl < outsz) {
                memcpy(out + used, mux_line, (size_t)rl); used += (size_t)rl;
            }
            if (ice_extra && ice_extra_len > 0 && used + ice_extra_len < outsz) {
                memcpy(out + used, ice_extra, ice_extra_len);
                used += ice_extra_len;
            }
        } else {
            /* 最后一段被拒：同样补 a=mid + rejected_extra */
            char mid_line[64];
            int ml = snprintf(mid_line, sizeof(mid_line),
                              "a=mid:%d\r\n", start_mid + media_idx - 1);
            if (ml > 0 && used + (size_t)ml < outsz) {
                memcpy(out + used, mid_line, (size_t)ml); used += (size_t)ml;
            }
            if (rejected_extra && rejected_extra_len > 0 &&
                used + rejected_extra_len < outsz) {
                memcpy(out + used, rejected_extra, rejected_extra_len);
                used += rejected_extra_len;
            }
        }
    }
    if (used >= outsz) return NSBC_NOSPC;
    out[used] = '\0';
    return (int)used;
}

/* ★ RFC 3264 段数对齐：answer 的 m= 段数必须等于 A 原始 offer 的段数。
 * offer_hook 会为 WebRTC B 追加 rejected video m-line（sdp_ensure_video_mline），
 * 浏览器 answer 把它原样回显 → answer 比 A 的 offer 多一段。
 * PJSIP pjmedia_sdp_neg 校验 offer/answer 段数不符 → EINVANSW
 * → MicroSIP ACK 200 OK 后 0ms 发 BYE。
 * 从第 (max_m+1) 个 m= 行行首截断：SBC 追加的段总在尾部，
 * 其后的属性行都属于该段，直接砍到缓冲区尾即可。 */
static int sdp_truncate_mlines(char *sdp, size_t len, int max_m)
{
    if (!sdp || max_m <= 0) return (int)len;
    const char *p = sdp;
    const char *end = sdp + len;
    int cnt = 0;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t ll = (size_t)(le - p);
        if (ll >= 2 && p[0] == 'm' && p[1] == '=') {
            ++cnt;
            if (cnt > max_m) {
                size_t keep = (size_t)(p - sdp);
                sdp[keep] = '\0';
                return (int)keep;
            }
        }
        p = eol ? eol + 1 : end;
    }
    return (int)len;
}

int bridge_rewrite_sdp(const char *in, size_t in_len, char *out,
                       size_t outsz, const char *ip, uint16_t rtp_port,
                       mb_sdp_dir_t dir)
{
    size_t used = 0;
    const char *p = in;
    const char *end = in + in_len;
    uint16_t media_port = rtp_port;
    int media_idx = 0;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        size_t llen = (size_t)(line_end - p);

        /* ★ FIX: 统一回剥行尾 \r。
         * 所有行（原样复制 / 改写）都不再自带 \r，
         * 由各自的 put_line("\r\n") 统一补回。
         * 消除混合行尾 (\n vs \r\n) 和双 \r 问题。 */
        while (llen > 0 && p[llen - 1] == '\r')
            --llen;

        const char *outp = p;
        size_t outlen = llen;
        char tmp[256];

        /* ---- 剥离行（ICE/DTLS/WebRTC-only/bandwidth）---- */
        if (dir != MB_SDP_ASIS && llen >= 2 && p[0] == 'a' && p[1] == '=' &&
            attr_is_ice_dtls(p, llen)) {
            p = eol ? eol + 1 : end;
            continue;
        }
        if (dir == MB_SDP_PLAIN && llen >= 2 && p[0] == 'a' && p[1] == '=' &&
            attr_is_webrtc_only(p, llen)) {
            p = eol ? eol + 1 : end;
            continue;
        }
        if (dir != MB_SDP_ASIS && llen >= 2 && p[0] == 'b' && p[1] == '=' &&
            line_is_bandwidth(p, llen)) {
            p = eol ? eol + 1 : end;
            continue;
        }

        /* ---- o= 行改写 ---- */
        if (dir != MB_SDP_ASIS && llen >= 2 && p[0] == 'o' && p[1] == '=') {
            const char *in_ip4 = memmem(p, llen, "IN IP4", 6);
            if (in_ip4) {
                const char *after_in_ip4 = in_ip4 + 6;
                const char *addr_ip = after_in_ip4;
                while (addr_ip < p + llen && *addr_ip == ' ')
                    ++addr_ip;
                const char *addr_end = addr_ip;
                while (addr_end < p + llen && *addr_end != ' ' && *addr_end != '\r')
                    ++addr_end;
                /* tail 只保留 addr 之后的非空白字符（通常为空） */
                size_t tail_len = 0;
                size_t head_len = (size_t)(after_in_ip4 - p);
                if (head_len + 1 + strlen(ip) + tail_len + 1 < sizeof(tmp)) {
                    char *tp = tmp;
                    memcpy(tp, p, head_len); tp += head_len;
                    *tp++ = ' ';
                    memcpy(tp, ip, strlen(ip)); tp += strlen(ip);
                    /* tail_len == 0 时不拷贝 */
                    *tp = '\0';
                    outp = tmp;
                    outlen = (size_t)(tp - tmp);
                }
            }
            /* ★ o= 行改写后 fall-through 到通用写入，
             * 通用路径现在统一补 \r\n，不再丢 \r */
        }

        /* ---- c= 行改写 ---- */
        if (media_port != 0 && llen >= 2 && p[0] == 'c' && p[1] == '=') {
            int n = snprintf(tmp, sizeof(tmp), "c=IN IP4 %s", ip);
            if (n > 0) {
                outp = tmp;
                outlen = (size_t)n;
            }
            /* ★ c= 行独立写入 + \r\n，不 fall-through */
            if (put_line(out, outsz, &used, outp, outlen) != NSBC_OK) {
                out[0] = '\0';
                return NSBC_NOSPC;
            }
            if (put_line(out, outsz, &used, "\r\n", 2) != NSBC_OK) {
                out[0] = '\0';
                return NSBC_NOSPC;
            }
            p = eol ? eol + 1 : end;
            continue;
        }

        /* ---- m= 行改写 ---- */
        if (llen >= 2 && p[0] == 'm' && p[1] == '=') {
            const char *mt = p + 2;
            const char *mt_end = mt;
            while (mt_end < p + llen && *mt_end != ' ') ++mt_end;

            /* DEBUG: 每个 m= 段都打一条 */
            llog(LOG_INFO, "media",
                 "★REWRITE_MLINE: type=%.*s media_idx=%d media_port=%u dir=%d",
                 (int)(mt_end - mt), mt, media_idx,
                 (unsigned)media_port, dir);

            /* ★ PLAIN 方向（发给硬 SIP 端）：
             *
             * 原始逻辑：跳过非 audio m-line + 后续属性行。
             * 原始意图：硬话机（CiscoSPA/PJMEDIA）不认识 m=video 段 → 400 Bad SDP。
             *
             * 但这个逻辑有致命 BUG：WebRTC originate 场景下，
             * a_ws=0（auto_offer 是 RTP/AVP 无 WebRTC 特征），
             * b_ws 还没判定（call_b_is_ws 可能返回 0），
             * dir 先被算成 PLAIN → video 段被删 → sdp_ensure_video_mline
             * 追加 port=0 占位 → 浏览器 answer 丢 video 段 → 协商彻底失败！
             *
             * 修复：**永远不跳过任何 m= 段！**
             *   - 硬话机遇到 m=video 段：大多数会忽略（RFC 3264 说 UA 可忽略未知媒体类型）
             *   - 真有硬话机 reject video 段：那也是边缘情况，比 WebRTC video 全灭优先级低
             *   - 所有 dir 都走统一 rewrite 流程 → video 段 port/proto/c= 都被正确改写 */
            if (dir == MB_SDP_PLAIN &&
                !(mt_end - mt == 5 && memcmp(mt, "audio", 5) == 0)) {
                /* 不再 skip！fall through 到下面的正常 m= 段改写逻辑！ */
            }
            const char *sp1 = mt_end;
            while (sp1 < p + llen && *sp1 == ' ') ++sp1;
            const char *port_start = sp1;
            const char *port_end = port_start;
            while (port_end < p + llen && *port_end >= '0' && *port_end <= '9')
                ++port_end;

            if (port_end > port_start) {
                uint32_t orig_port = 0;
                for (const char *q = port_start; q < port_end; ++q)
                    orig_port = orig_port * 10u + (uint32_t)(*q - '0');

                uint16_t this_port;
                if (!media_port) {
                    /* 没有 relay 要转发（BYPASS）→ 原样保留 orig_port */
                    this_port = (uint16_t)orig_port;
                    llog(LOG_INFO, "media",
                         "★REWRITE_BRANCH[%d]: orig_port=%u media_port=0 → BYPASS keep port=%u",
                         media_idx, orig_port, this_port);
                } else if (orig_port == 0) {
                    /* ★ REQ-ORIGINATE-ECHO-002：orig_port=0 通常是 auto_offer
                     * （originate 构造的最小 SDP）或被上一节点标记为 rejected。
                     * 但如果我们有 relay（media_port!=0），说明 offer_hook
                     * 已决定做 SRTP_RELAY 转发 → 必须用 relay 的真实端口，
                     * 不能保留 0 否则对端收到 m=audio 0 + a=inactive →
                     * 不回 200 OK 或回 200 但不发音频。
                     *
                     * ★ BUNDLE 规则（RFC 8843）：
                     *   dir == WS（WebRTC/BUNDLE）→ 所有 m= 段共用同一 transport，
                     *     端口统一 = media_port，不加 idx 偏移。
                     *   dir == PLAIN/ASIS → 硬 SIP 端无 BUNDLE，audio/video 独立 relay，
                     *     用 media_port + media_idx*2 分端口。 */
                    if (dir == MB_SDP_WS)
                        this_port = (uint16_t)media_port;
                    else
                        this_port = (uint16_t)(media_port + (uint16_t)(media_idx * 2));
                    llog(LOG_INFO, "media",
                         "★REWRITE_BRANCH[%d]: orig_port=0 relay active → %s port=%u media_port=%u",
                         media_idx,
                         dir == MB_SDP_WS ? "BUNDLE-unified" : "idx-offset",
                         this_port, (unsigned)media_port);
                } else {
                    /* ★ BUNDLE 规则（RFC 8843）：
                     *   WS 方向（WebRTC answer）→ audio/video 共用同一 transport，
                     *     所有 m= 段统一用 media_port，不加 idx 偏移。
                     *     Browser offer 有 a=group:BUNDLE → 我们的 answer 也必须
                     *     audio/video 同端口，否则 ICE connectivity check 只成功
                     *     audio 那个端口，video 端口没 ICE → DTLS 不通 → 全丢。
                     *   PLAIN/ASIS → 硬 SIP 端无 BUNDLE，audio/video 分端口独立 relay。 */
                    if (dir == MB_SDP_WS)
                        this_port = (uint16_t)media_port;
                    else
                        this_port = (uint16_t)(media_port + (uint16_t)(media_idx * 2));
                }

                const char *ps = port_end;
                while (ps < p + llen && *ps == ' ') ++ps;
                const char *pe = ps;
                while (pe < p + llen && *pe != ' ') ++pe;

                const char *proto_token;
                if (dir != MB_SDP_ASIS && pe > ps) {
                    proto_token = (dir == MB_SDP_WS)
                                     ? "UDP/TLS/RTP/SAVPF" : "RTP/AVP";
                } else if (pe > ps) {
                    proto_token = ps;
                } else {
                    proto_token = "RTP/AVP";
                }
                size_t proto_len = (proto_token == ps)
                                       ? (size_t)(pe - ps)
                                       : strlen(proto_token);

                size_t tail_len;
                const char *tail_src = pe;
                if (this_port == 0) {
                    /* rejected 段：保留第一个 fmt token */
                    const char *ts = pe;
                    while (ts < p + llen && *ts == ' ') ++ts;
                    const char *te = ts;
                    while (te < p + llen && *te != ' ' && *te != '\r') ++te;
                    if (te > ts) {
                        tail_len = (size_t)(te - pe);
                    } else {
                        tail_src = " 0";
                        tail_len = 2;
                    }
                } else {
                    /* ★ FIX: tail 不回剥 \r（已在循环开头统一回剥），
                     * 直接用 llen 边界计算 */
                    tail_len = (size_t)((p + llen) - pe);
                }

                int nh = snprintf(tmp, sizeof(tmp),
                                  "m=%.*s %u %.*s",
                                  (int)(mt_end - mt), mt,
                                  (unsigned)this_port,
                                  (int)proto_len, proto_token);
                if (nh > 0 && (size_t)nh + tail_len + 1 < sizeof(tmp)) {
                    memcpy(tmp + nh, tail_src, tail_len);
                    tmp[nh + tail_len] = '\0';

                    if (dir == MB_SDP_PLAIN && this_port != 0) {
                        llog(LOG_INFO, "media", "rewrite: PLEN slim_fmt IN: %s", tmp);
                        int slim = plen_slim_fmt_list(tmp, sizeof(tmp));
                        if (slim > 0) {
                            outlen = (size_t)slim;
                            llog(LOG_INFO, "media", "rewrite: PLEN slim_fmt OK out=%s", tmp);
                        } else {
                            llog(LOG_WARN, "media", "rewrite: PLEN slim_fmt FAILED (rc=0), keeping original tail");
                            outlen = (size_t)nh + tail_len;
                        }
                    } else {
                        outlen = (size_t)nh + tail_len;
                    }
                    outp = tmp;

                    /* 防御性截断：防止 NUL 泄漏到输出 */
                    size_t actual_len = strlen(outp);
                    if (outlen > actual_len)
                        outlen = actual_len;

                    /* DEBUG: 每个 m= 段改写后的结果 */
                    llog(LOG_INFO, "media",
                         "★REWRITE_MLINE_OUT: %.*s",
                         (int)outlen, outp);

                    if (used + outlen > outsz) break;
                    if (put_line(out, outsz, &used, outp, outlen) != NSBC_OK) {
                        out[0] = '\0';
                        return NSBC_NOSPC;
                    }
                    if (put_line(out, outsz, &used, "\r\n", 2) != NSBC_OK) {
                        out[0] = '\0';
                        return NSBC_NOSPC;
                    }
                    p = eol ? eol + 1 : end;
                    ++media_idx;
                    continue;
                }
            }
        }

        /* ---- 通用行写入（原样复制 / o= 改写 fall-through）---- */
        if (put_line(out, outsz, &used, outp, outlen) != NSBC_OK) {
            out[0] = '\0';
            return NSBC_NOSPC;
        }
        /* ★ FIX: 统一输出 \r\n。
         * 原代码 put_line(eol, 1) 只写 \n，导致改写行丢 \r。
         * 配合循环开头的 llen 回剥，原样复制行也不会双 \r。 */
        if (eol) {
            if (put_line(out, outsz, &used, "\r\n", 2) != NSBC_OK) {
                out[0] = '\0';
                return NSBC_NOSPC;
            }
        }
        p = eol ? eol + 1 : end;
    }

    if (used >= outsz) {
        out[0] = '\0';
        return NSBC_NOSPC;
    }
    out[used] = '\0';
    return (int)used;
}
/* ------------------------------------------------------------------ */
/* relay 生命周期                                                       */
/* ------------------------------------------------------------------ */
int rtp_relay_start(bridge_mgr_t *m, rtp_relay_t **out,
                    relay_ports_t *ports,
                    uint16_t range_lo, uint16_t range_hi)
{
    llog(LOG_INFO, "media", "relay_start: ENTER local_ip=%s range=%u-%u ptable=%p",
         m->cfg.local_ip ? m->cfg.local_ip : "(null)",
         (unsigned)range_lo, (unsigned)range_hi, (void *)m->cfg.ptable);
    rtp_relay_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NSBC_NOMEM;
    /* calloc 已把 dead/armed 等全部清零 → dead=0, armed=0 天然正确 */
    r->m = m;
    for (int i = 0; i < 2; ++i) {
        r->fd_rtp[i] = r->fd_rtcp[i] = -1;
        ssrc_policy_cfg_t pc = {0};
        ssrc_policy_init(&r->pol[i], &pc);
    }
    uint16_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    llog(LOG_INFO, "media", "relay_start: before alloc pair #0");
    if (ptable_alloc_pair_range(m->cfg.ptable, m->cfg.local_ip, r,
                                range_lo, range_hi, &r0, &c0) != NSBC_OK) {
        llog(LOG_ERR, "media", "relay_start: alloc #0 FAILED");
        free(r);
        return NSBC_NOSPC;
    }
    llog(LOG_INFO, "media", "relay_start: pair #0 r0=%u c0=%u", (unsigned)r0, (unsigned)c0);
    llog(LOG_INFO, "media", "relay_start: before alloc pair #1");
    if (ptable_alloc_pair_range(m->cfg.ptable, m->cfg.local_ip, r,
                                range_lo, range_hi, &r1, &c1) != NSBC_OK) {
        llog(LOG_ERR, "media", "relay_start: alloc #1 FAILED, releasing #0");
        ptable_release_pair(m->cfg.ptable, m->cfg.local_ip, r0);
        free(r);
        return NSBC_NOSPC;
    }
    llog(LOG_INFO, "media", "relay_start: pair #1 r1=%u c1=%u", (unsigned)r1, (unsigned)c1);
    r->rtp_port[0] = r0;
    r->rtp_port[1] = r1;
    /* P0 relay 纯 IPv4：dualstack=0 避免 v4-mapped family 不匹配
     * （recvfrom 返回 sockaddr_in6 但 peer 为 sockaddr_in → nsbc_sa_eq_addr
     * family 比较直接失败，所有 RTP 被 drop）。 */
    net_udp_cfg_t uc = {0, 0, 0, 0, 0};
    llog(LOG_INFO, "media", "relay_start: before udp_open rtp0");
    r->fd_rtp[0] = net_udp_open(m->cfg.local_ip, r0, &uc);
    llog(LOG_INFO, "media", "relay_start: rtp0 fd=%d", r->fd_rtp[0]);
    r->fd_rtcp[0] = net_udp_open(m->cfg.local_ip, c0, &uc);
    llog(LOG_INFO, "media", "relay_start: rtcp0 fd=%d", r->fd_rtcp[0]);
    r->fd_rtp[1] = net_udp_open(m->cfg.local_ip, r1, &uc);
    llog(LOG_INFO, "media", "relay_start: rtp1 fd=%d", r->fd_rtp[1]);
    r->fd_rtcp[1] = net_udp_open(m->cfg.local_ip, c1, &uc);
    llog(LOG_INFO, "media", "relay_start: rtcp1 fd=%d", r->fd_rtcp[1]);
    if (r->fd_rtp[0] < 0 || r->fd_rtp[1] < 0) {
        llog(LOG_WARN, "media",
             "relay_start: UDP open FAIL rtp0=%d rtp1=%d",
             r->fd_rtp[0], r->fd_rtp[1]);
        rtp_relay_stop(r);
        return NSBC_ERR;
    }
    llog(LOG_DEBUG, "media",
         "relay_start: OK r0=%d r1=%d fd0=%d fd1=%d local_ip=%s (defer arm)",
         r0, r1, r->fd_rtp[0], r->fd_rtp[1],
         m->cfg.local_ip ? m->cfg.local_ip : "(null)");
    /* ★ RACE-FIX: 延迟 reactor_add — 先构造 relay（open fd + alloc port），
     * 但不注册 reactor。offer_hook/answer_hook 在 dtls_server_create + admit_ssrc
     * 等所有初始化完成后再调 rtp_relay_arm 一次性注册。
     * 之前 relay_start 里先 reactor_add，reactor 线程立刻能收 STUN record
     * 触发 relay_cb，但此时 e->dtls[] / r->pol[] 还没初始化好 → data race
     * → SRTP 密文被当明文转发/SSRC 预登记半写 → 概率性单向视频。 */
    r->armed = 0;
    ports->a_rtp = r0; /* 对 A 广告（answer_hook 改写 answer 时用） */
    ports->a_rtcp = c0;
    ports->b_rtp = r1; /* 对 B 广告（offer_hook 改写 offer 时用） */
    ports->b_rtcp = c1;
    *out = r;
    return NSBC_OK;
}

/* P3: reactor_call 批量 arm/stop 目标函数。
 *
 * reactor_add/del 必须在 reactor owner 线程执行（P1 编译开关已防护）。
 * 批量提交代替 4 次跨线程调用，降低延迟。
 * 更重要的是：del + 后续 callback/fd close 在同一 owner 线程串行化——
 * 修复了之前 stop() 从业务线程 free relay 而 relay_cb 还在跑的 race。 */
/* F-11 修复：检查每次 reactor_add 返回值；部分失败时回滚已成功的 add。
 * 之前无条件 armed=1 导致部分 fd 注册失败时调用方仍认为 relay 完整 armed。 */
static int relay_batch_arm_fn(void *arg)
{
    rtp_relay_t *r = (rtp_relay_t *)arg;
    if (!r) return NSBC_INVAL;
    reactor_t *target = r->home_reactor;
    if (!target) return NSBC_INVAL;

    int rc;
    int ok_count = 0;
    int ok_fds[4] = { -1, -1, -1, -1 };  /* 记录哪些 fd 注册成功用于回滚 */

    rc = reactor_add(target, r->fd_rtp[0], EPOLLIN, relay_cb, r);
    if (rc != NSBC_OK) goto rollback;
    ok_fds[ok_count++] = r->fd_rtp[0];

    rc = reactor_add(target, r->fd_rtp[1], EPOLLIN, relay_cb, r);
    if (rc != NSBC_OK) goto rollback;
    ok_fds[ok_count++] = r->fd_rtp[1];

    if (r->fd_rtcp[0] >= 0) {
        rc = reactor_add(target, r->fd_rtcp[0], EPOLLIN, relay_cb, r);
        if (rc != NSBC_OK) goto rollback;
        ok_fds[ok_count++] = r->fd_rtcp[0];
    }
    if (r->fd_rtcp[1] >= 0) {
        rc = reactor_add(target, r->fd_rtcp[1], EPOLLIN, relay_cb, r);
        if (rc != NSBC_OK) goto rollback;
        ok_fds[ok_count++] = r->fd_rtcp[1];
    }

    r->armed = 1;
    return NSBC_OK;

rollback:
    llog(LOG_WARN, "media",
         "relay_batch_arm: reactor_add failed rc=%d fd=%d — rolling back %d succeeded",
         rc, ok_fds[ok_count], ok_count);
    for (int i = ok_count - 1; i >= 0; --i)
        reactor_del(target, ok_fds[i]);
    return rc;
}

static int relay_batch_stop_fn(void *arg)
{
    rtp_relay_t *r = (rtp_relay_t *)arg;
    if (!r) return NSBC_INVAL;
    reactor_t *target = r->home_reactor;
    if (!target) return NSBC_INVAL;

    /* 在 reactor owner 线程里一次性删除所有 fd。
     * 删除后 epoll 不再触发这些 fd 的事件，已在本次 epoll_wait 里的事件会继续处理
     * 但 reactor_del 会把 slot 里的回调清除 → 安全。 */
    reactor_del(target, r->fd_rtp[0]);
    reactor_del(target, r->fd_rtp[1]);
    if (r->fd_rtcp[0] >= 0)
        reactor_del(target, r->fd_rtcp[0]);
    if (r->fd_rtcp[1] >= 0)
        reactor_del(target, r->fd_rtcp[1]);
    return NSBC_OK;
}

/* ★ RACE-FIX: 延迟 arm — 所有 dtls 创建 + admit_ssrc 完成后才注册 reactor fd。
 *
 * P3 多 reactor：按 relay 起始端口 hash 路由到 reactor owner，
 * 通过 reactor_call 一次性批量注册（比 4 次跨线程调用延迟低）。
 * reactor_call 同步等待 owner 完成后才返回——arm 完成即表示 fd 已注册完毕。 */
static void rtp_relay_arm(bridge_mgr_t *m, rtp_relay_t *r)
{
    if (!m || !r || r->armed) return;

    reactor_t *target = NULL;
    if (m->cfg.mgr) {
        /* P3: 多 reactor 模式——按 rtp_port[0] hash 路由 */
        uint64_t hk = r->rtp_port[0] + (uint64_t)r->rtp_port[1] * 31;
        unsigned rid = reactor_mgr_route(m->cfg.mgr, hk);
        target = reactor_mgr_get(m->cfg.mgr, rid);
        llog(LOG_DEBUG, "media", "relay_arm: hash_key=%llu → reactor[%u]",
             (unsigned long long)hk, rid);
    } else {
        /* 旧路径：单 reactor */
        target = m->cfg.re;
    }
    if (!target) return;

    r->home_reactor = target;
    int rc = reactor_call(target, relay_batch_arm_fn, r);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "media", "relay_arm: reactor_call rc=%d", rc);
        r->home_reactor = NULL;
    } else {
        llog(LOG_DEBUG, "media", "relay_arm: fd0=%d fd1=%d reactor_armed",
             r->fd_rtp[0], r->fd_rtp[1]);
    }
}

void rtp_relay_stop(rtp_relay_t *r)
{
    if (!r)
        return;
    bridge_mgr_t *m = r->m;

    /* ★ P0-2 RACE-FIX: 先置 dead=1 再 reactor_call。
     * relay_recv_one for(;;) 循环每次迭代检查 dead，检测到立即 return。
     * 这样 reactor_call 返回时即使已进入的 relay_recv_one 还在内核队列里有
     * 残留数据包，也会在下一次迭代迅速退出，不会在 bridge_release_call free(r/e)
     * 后继续访问 r->node 等成员。 */
    __atomic_store_n(&r->dead, 1, __ATOMIC_RELEASE);

    /* P3: 通过 reactor_call 在 reactor owner 线程里批量删除所有 fd。
     * reactor_call 同步等待——owner 执行完 reactor_del 才返回，
     * 之后 close fd / free relay 安全（reactor 不会再触发 relay_cb）。 */
    if (r->home_reactor && r->armed) {
        int rc = reactor_call(r->home_reactor, relay_batch_stop_fn, r);
        if (rc != NSBC_OK)
            llog(LOG_WARN, "media", "relay_stop: reactor_call rc=%d", rc);
    } else if (m && m->cfg.re && r->armed) {
        /* 旧路径或 reactor_call 失败降级 */
        for (int i = 0; i < 2; ++i) {
            if (r->fd_rtp[i] >= 0)
                reactor_del(m->cfg.re, r->fd_rtp[i]);
            if (r->fd_rtcp[i] >= 0)
                reactor_del(m->cfg.re, r->fd_rtcp[i]);
        }
    }
    r->armed = 0;

    /* REQ-DTLS-001：释放 SRTP 会话 */
    for (int i = 0; i < 2; ++i) {
        if (r->srtp[i]) {
            nsbc_srtp_session_destroy(r->srtp[i]);
            r->srtp[i] = NULL;
        }
    }
    /* cnode_t 持有的 dtls_server（按侧）——relay_stop 释放前先拆关联；
     * cnode 本身由 bridge_release_call 释放（dtls_server_destroy 在这里） */
    if (r->node) {
        for (int i = 0; i < 2; ++i) {
            if (r->node->dtls[i]) {
                dtls_server_destroy((dtls_server_t *)r->node->dtls[i]);
                r->node->dtls[i] = NULL;
            }
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (r->fd_rtp[i] >= 0) {
            close(r->fd_rtp[i]);
            r->fd_rtp[i] = -1;
        }
        if (r->fd_rtcp[i] >= 0) {
            close(r->fd_rtcp[i]);
            r->fd_rtcp[i] = -1;
        }
        if (r->rtp_port[i])
            ptable_release_pair(m->cfg.ptable, m->cfg.local_ip,
                                r->rtp_port[i]);
    }
    free(r);
}

void rtp_relay_set_peer_a(rtp_relay_t *r, const nsbc_sa_t *sa)
{
    /* A 的报文收在 fd_rtp[0]：answer 对 A 广告 a_rtp(r0)，
     * A 按 SDP 发往 r0 → relay 侧 0 收到（上轮端口方向修正后 peer
     * 设置必须同步，否则两侧包全被 peer 检查丢弃）。 */
    r->peer[0] = *sa;
    r->latch[0] = *sa;
    r->have_peer[0] = 1;
    __atomic_store_n(&r->d_latched[0], 1, __ATOMIC_RELEASE);
}
void rtp_relay_set_peer_b(rtp_relay_t *r, const nsbc_sa_t *sa)
{
    /* B 的报文收在 fd_rtp[1]：offer 对 B 广告 b_rtp(r1)。 */
    r->peer[1] = *sa;
    r->latch[1] = *sa;
    r->have_peer[1] = 1;
    __atomic_store_n(&r->d_latched[1], 1, __ATOMIC_RELEASE);
}

uint64_t rtp_relay_pkts(const rtp_relay_t *r)
{
    return r ? r->pkts : 0;
}
uint64_t rtp_relay_drops(const rtp_relay_t *r)
{
    return r ? r->drops : 0;
}

/* REQ-ORIGINATE-003 &echo：开启/关闭 RTP 环回。
 * on=1：relay_recv_one 把 side 收到的 RTP 发回 side 自己（side→side）。
 *      两侧各自环回自己的 peer，实现"通话方听到自己回声"。
 * on=0：恢复正常 side→other 转发。
 * 调用时机：b2b_originate B 腿 LEG_CONFIRMED 之后，OAM 线程调。
 * 线程安全：echo_mode 是普通 int，仅在外部模块设置一次（B 腿 confirm 后）
 * + 媒体线程在 relay_recv_one 里读 —— RCU-like 单次写入，原子性不要求，
 * 允许一两包在"切换瞬间"走错方向（下一个 recv 循环即生效）。 */
void rtp_relay_set_echo(rtp_relay_t *r, int on)
{
    if (!r) return;
    int want = on ? 1 : 0;
    if (r->echo_mode == want) return;  /* 幂等 */
    r->echo_mode = want;
    llog(LOG_INFO, "relay", "echo_mode %d (call bridge)", want);
}

/* ------------------------------------------------------------------ */
/* b2bua 挂接                                                           */
/* ------------------------------------------------------------------ */
static void parse_media_addr(const char *sdp, size_t len, char *ip,
                             size_t ipsz, uint16_t *port)
{
    sdp_t s;
    memset(ip, 0, ipsz);
    *port = 0;
    int rc = sdp_parse(&s, sdp, len);
    if (rc != NSBC_OK)
        return;
    for (int i = 0; i < s.nmedia; ++i) {
        const sdp_conn_t *c = sdp_media_effective_conn(&s, &s.media[i]);
        if (c && c->addr.p) {
            size_t n = c->addr.len < ipsz - 1 ? c->addr.len : ipsz - 1;
            memcpy(ip, c->addr.p, n);
            ip[n] = '\0';
            *port = (uint16_t)s.media[i].port;
            return;
        }
    }
}

static cnode_t *entry_get(bridge_mgr_t *m, void *call)
{
    br_shard_t *s = &m->sh[ptr_shard(call)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call);
    if (e) {
        pthread_mutex_unlock(&s->lock);
        return e;
    }
    e = calloc(1, sizeof(*e));
    if (!e) {
        pthread_mutex_unlock(&s->lock);
        return NULL;
    }
    e->call = call;
    e->mode = MB_RELAY;
    e->next = s->head;
    s->head = e;
    pthread_mutex_unlock(&s->lock);
    return e;
}

int bridge_offer_hook(void *call, const char *in, size_t inlen,
                      char *out, size_t outsz, void *ud)
{
    bridge_mgr_t *m = ud;
    mb_mode_t mode;
    bridge_decide(m, in, inlen, NULL, 0, &mode);
    cnode_t *e = entry_get(m, call);
    if (!e) {
        llog(LOG_WARN, "media", "offer_hook: entry_get NOMEM call=%p", call);
        return NSBC_NOMEM;
    }
    /* ★ RFC 3312 Precondition 前提条件门控：
     * profile->enable_precondition=0（企业 PBX 等场景关闭）→ 永远置 0，gate 常开；
     * profile=NULL（无归属 profile）→ 默认关闭（普通 PSTN/SIP 中继场景）。
     * 只有显式 enable_precondition=1 的 profile 才解析 a=des/a=curr。 */
    e->precond_active = 0;
    e->precond_met = 0;
    {
        /* ★ 三层守卫：cfg（无）→ force_precondition (dialplan 三态) → profile.enable_precondition
         * REQ-ARCH-011 注：bridge_confirm 已在本文件 L959 强转过 b2b_call_t，
         * 同一模式下 call_key 在此处安全强转（signaling 层创建，media 层消费）。 */
        b2b_call_t *bc = (b2b_call_t *)call;
        sip_profile_t *pr = bc ? bc->prof : NULL;
        int prof_en = pr && pr->enable_precondition;  /* profile NULL 默认关 */
        int force_f = bc ? bc->force_precondition : 0;
        int enabled = (force_f == 1) ? 1 : (force_f == 2 ? 0 : prof_en);
        if (enabled && inlen > 0) {
            sdp_t s; memset(&s, 0, sizeof(s));
            if (sdp_parse(&s, in, inlen) == NSBC_OK && s.nmedia > 0) {
                e->precond_active = (uint8_t)sdp_media_preconditions_active(&s.media[0]);
                if (e->precond_active)
                    llog(LOG_INFO, "media", "offer_hook: precondition ACTIVE (wait answer met)");
            }
        }
    }
    /* ★ REQ-NAT-002：per-call NAT 公网 RTP IP —— offer 阶段查一次缓存到 cnode，
     * answer_hook 直接复用。profile 没配 → external_rtp_ip 空字符串 → fallback local_ip */
    e->external_rtp_ip[0] = '\0';
    if (m->cfg.call_external_rtp_ip) {
        const char *nip = m->cfg.call_external_rtp_ip(m->cfg.call_external_rtp_ip_ud, call);
        if (nip && nip[0]) {
            snprintf(e->external_rtp_ip, sizeof(e->external_rtp_ip), "%s", nip);
            llog(LOG_INFO, "media", "offer_hook: NAT ext_rtp_ip=[%s]", nip);
        } else {
            llog(LOG_INFO, "media", "offer_hook: NAT ext_rtp_ip EMPTY (profile not set)");
        }
    }

    /* ★ REQ-DTLS-001 方向感知 + REQ-MEDIA-DP-001 rule override
     * a_ws —— offer SDP 带 WebRTC 特征 → A 腿是浏览器；
     * b_ws —— call_b_is_ws 回调从 leg[1] 反查 B 腿注册类型。
     * 三层决策，依次应用：
     *   ① 腿组合硬约束（webRTC 腿不能走明文 RELAY）：
     *      mixed_ws → 强制 SRTP_RELAY，rule=bypass 无效（打 WARN）
     *      both_ws + mode=RELAY（明文 relay） → 强制 SRTP_RELAY
     *        （明文 relay 无法终结 DTLS，浏览器 SDP 端口/媒体类型对不上会 488）
     *      both_ws + mode=BYPASS + rule=auto → 强制 SRTP_RELAY
     *        （auto 保守锚定，公网 ICE 穿透经常失败丢媒体；
     *         rule=bypass 时才放开让浏览器 P2P）
     *   ② rule override（rule=auto(0) 时不动）：
     *      rule=bypass → 强制 BYPASS（both_ws 时让浏览器 P2P，明文腿时直连）
     *      rule=relay → 强制 SRTP_RELAY（both_ws）或 RELAY（明文腿）
     */
    const int a_ws = is_webrtc_sdp(in, inlen);
    int b_ws = 0;
    if (m->cfg.call_b_is_ws)
        b_ws = m->cfg.call_b_is_ws(m->cfg.call_b_is_ws_ud, call);
    const int mixed_ws = (a_ws != b_ws);   /* 恰好一侧 WebRTC */
    const int both_ws  = (a_ws && b_ws);   /* 两侧都是 WebRTC */

    /* ★ 存端点 WebRTC 标志到 cnode——answer_hook 需要读取，
     * 独立于 dtls[]（Phase C SRTP_RELAY 下双侧都创建 DTLS server，
     * 不再能从 dtls[] 反推端点类型）。 */
    e->a_ws = (uint8_t)a_ws;
    e->b_ws = (uint8_t)b_ws;

    /* ★ 数 A 原始 offer 的 m= 段数（RFC 3264 answer 段数对齐用）。
     * offer_hook 为 WebRTC B 追加 rejected video m-line 后段数 +1，
     * 浏览器 answer 会原样回显 → answer_hook 需要按 a_mlcount 截断。 */
    {
        int mc = 0;
        const char *mp = in;
        const char *mend = in + inlen;
        while (mp < mend) {
            const char *me = memchr(mp, '\n', (size_t)(mend - mp));
            const char *mle = me ? me : mend;
            size_t mll = (size_t)(mle - mp);
            if (mll >= 2 && mp[0] == 'm' && mp[1] == '=') ++mc;
            mp = me ? me + 1 : mend;
        }
        e->a_mlcount = (uint8_t)mc;
    }

    uint8_t rule_mm = 0;  /* 默认 auto */
    if (m->cfg.call_media_mode)
        rule_mm = m->cfg.call_media_mode(m->cfg.call_media_mode_ud, call);

    /* ① 硬约束 */
    if (mixed_ws) {
        if (rule_mm == 1)
            llog(LOG_WARN, "media", "offer_hook: rule=bypass but mixed-leg WebRTC (ignored, forcing SRTP_RELAY)");
        if (mode != MB_SRTP_RELAY) {
            mode = MB_SRTP_RELAY;
            llog(LOG_INFO, "media", "offer_hook: mixed WebRTC leg → force SRTP_RELAY");
        }
    }
    if (both_ws && rule_mm == 0) {
        /* auto 模式下 both_ws 强制 SRTP_RELAY——保守锚定，让 SBC 终结两侧 DTLS；
         * 只有 rule=bypass 才放开 P2P（rule=relay 也走 SRTP_RELAY，下方 override 处理） */
        if (mode != MB_SRTP_RELAY) {
            llog(LOG_INFO, "media", "offer_hook: both WebRTC auto → SRTP_RELAY (was %d)", mode);
            mode = MB_SRTP_RELAY;
        }
    }

    /* ② rule override（rule=auto(0) 时跳过） */
    if (rule_mm == 1) {  /* rule=bypass */
        if (mixed_ws) {
            /* 混合腿 rule=bypass 已在上面打 WARN，此处不动 mode（已被强制 SRTP_RELAY） */
        } else {
            llog(LOG_INFO, "media", "offer_hook: rule→force BYPASS%s", both_ws ? " (WebRTC↔WebRTC P2P)" : "");
            mode = MB_BYPASS;
        }
    } else if (rule_mm == 2) {  /* rule=relay */
        llog(LOG_INFO, "media", "offer_hook: rule→force RELAY%s", both_ws ? " (WebRTC↔WebRTC DTLS termination)" : "");
        mode = both_ws ? MB_SRTP_RELAY : MB_RELAY;
    }

    e->mode = mode;
    llog(LOG_INFO, "media", "offer_hook: call=%p mode=%d a_ws=%d b_ws=%d "
         "local_ip=%s", call, mode, a_ws, b_ws,
         m->cfg.local_ip ? m->cfg.local_ip : "(null)");

    /* A 的媒体地址（relay 对端）；A=WebRTC 时见下方 set_peer 注释 */
    parse_media_addr(in, inlen, e->a_ip, sizeof(e->a_ip), &e->a_port);
    llog(LOG_INFO, "media", "offer_hook: parse_media_addr done, a_ip=%s a_port=%u",
         e->a_ip[0] ? e->a_ip : "(null)", (unsigned)e->a_port);

    if (mode == MB_BYPASS) {
        if (inlen >= outsz)
            return NSBC_NOSPC;
        memcpy(out, in, inlen);
        out[inlen] = '\0';
        return (int)inlen;
    }
    /* REQ-SIP-PROFILE-001：per-call RTP 端口段——组合根按 call_key 归属
     * profile 提供（回调内解引用，本层仅取值）；未注册/无归属 → (0,0)
     * 表默认段。段内耗尽回落表默认段（隔离失败不拆呼叫，仅失去隔离）。 */
    uint16_t rlo = 0, rhi = 0;
    if (m->cfg.call_port_range)
        m->cfg.call_port_range(m->cfg.call_port_range_ud, call, &rlo, &rhi);
    llog(LOG_INFO, "media", "offer_hook: port_range=%u-%u call_port_range=%p",
         (unsigned)rlo, (unsigned)rhi, (void *)m->cfg.call_port_range);

    relay_ports_t ports;
    llog(LOG_INFO, "media", "offer_hook: about to rtp_relay_start");
    if (rtp_relay_start(m, &e->relay, &ports, rlo, rhi) != NSBC_OK &&
        (rlo != 0 || rhi != 0)) {
        llog(LOG_WARN, "media",
             "offer_hook: profile port range %u-%u exhausted, "
             "fallback to default range", rlo, rhi);
        if (rtp_relay_start(m, &e->relay, &ports, 0, 0) != NSBC_OK) {
            llog(LOG_WARN, "media", "offer_hook: relay_start FAIL");
            return NSBC_NOSPC;
        }
    }
    if (!e->relay) {
        llog(LOG_WARN, "media", "offer_hook: relay_start FAIL");
        return NSBC_NOSPC;
    }
    e->relay->node = e;
    llog(LOG_INFO, "media", "offer_hook: relay_start OK rtp0=%d rtp1=%d",
         ports.a_rtp, ports.b_rtp);
    /* ★ REQ-MEDIA-SSRC-003③：offer SDP 的 SSRC 预登记到 side=0（offer 来自 A 侧）。
     * 必须在 bridge_rewrite_sdp 之前（rewrite 剥 a=ssrc: 行）。 */
    llog(LOG_INFO, "media", "offer_hook: before admit_ssrc_from_sdp");
    admit_ssrc_from_sdp(e->relay, 0, in, inlen);
    llog(LOG_INFO, "media", "offer_hook: admit_ssrc done");
    if (e->a_ip[0] && !a_ws) {
        /* A=WebRTC 时不设固定 peer：ICE 实际源地址/端口与 SDP c=/m=
         * 可能不同（浏览器任选候选），该侧纯 latch 首包学习。 */
        nsbc_sa_t sa;
        if (nsbc_sa_from(&sa, e->a_ip, e->a_port, AF_UNSPEC) == NSBC_OK)
            rtp_relay_set_peer_a(e->relay, &sa);
    }
    /* B 侧 INVITE 重写：offer SDP 告诉 B 把 RTP 发到 SBC 的 B 侧 relay 端口
     * （b_rtp=fd_rtp[1]）。方向决定改写形态——最终根据 mode+dtls 结果选：
     *   MB_BYPASS  → ASIS：透传原始 SDP（不改地址/proto）
     *   B=WebRTC   → WS：proto→SAVPF，剥 A 旧凭证后下方注入本侧新凭证；
     *   A=WebRTC   → PLAIN：消毒（proto→RTP/AVP + 剥 ICE/DTLS）给明文端；
     *   双明文     → ASIS：仅改地址端口。 */

    /* REQ-DTLS-001：SRTP_RELAY 模式 → 只给**端点是 WebRTC** 的那一侧建 DTLS server。
     *
     * ★ 2026-10-16 关键修复：之前双侧无条件建 DTLS，导致 mixed_ws 下明文 SIP 侧
     * （side=1 朝 SIP，b_ws=0）也创建了 dtls_server。relay_recv_one 检测到
     * dtls[1] 非空 → srtp_lane=1，但 SIP 端发的是明文 RTP、永远不会发 DTLS
     * ClientHello → dtls_state 卡死 NOT_ESTABLISHED → srtp[1]=NULL → 所有 SIP
     * 发来的 RTP 被 drop 在 "SRTP-DROP-BEFORE-DTLS side=1"，媒体桥接彻底不通。
     *
     * 按端点类型条件创建：
     *   side=0 ↔ A 腿 ↔ a_ws 为真时才建 DTLS
     *   side=1 ↔ B 腿 ↔ b_ws 为真时才建 DTLS
     *   明文侧不建 → relay_recv_one 里 srtp_lane=0 → 明文字段直接转发。
     *
     * 边界情况：如果 b_ws=0 但 B 腿实际发了带 a=fingerprint 的 DTLS-SRTP answer
     * （某些 UDP SIP 端点也支持），answer_hook 里 is_webrtc_sdp(answer) 检测到后
     * 会补建 side=1 的 DTLS server —— 见 answer_hook 修复。 */
    if (mode == MB_SRTP_RELAY && m->cfg.re) {
        dtls_cfg_t dc = {
            .srtp_profile_id = 0x0007, /* SRTP_AEAD_AES_128_GCM */
        };
        int any_dtls_ok = 0;
        /* side ↔ 端点绑定：side=0↔A(a_ws), side=1↔B(b_ws) */
        const int need_dtls_on_side[2] = { a_ws, b_ws };
        for (int side = 0; side < 2; ++side) {
            if (!need_dtls_on_side[side])
                continue; /* 明文侧跳过，不建 DTLS server */
            if (e->dtls[side]) continue;
            dtls_server_t *ds = NULL;
            if (dtls_server_create(e->relay->fd_rtp[side], DTLS_ROLE_SERVER,
                                   &dc, &ds) == NSBC_OK) {
                e->dtls[side] = ds;
                any_dtls_ok = 1;
                llog(LOG_INFO, "dtls", "SRTP_RELAY: dtls[%d] on fd=%d "
                     "(side=%d, ws=%d)", side, e->relay->fd_rtp[side],
                     side, need_dtls_on_side[side]);
                /* side=0 的 peer fingerprint 在 offer SDP 里，立即注入 */
                if (side == 0)
                    inject_peer_fp(ds, in, inlen);
            } else {
                llog(LOG_WARN, "dtls", "dtls_server_create FAIL on fd=%d "
                     "(side=%d) — DTLS-SRTP 不可用",
                     e->relay->fd_rtp[side], side);
            }
        }
        /* ★ 降级条件更严格：只有双侧都不需要 DTLS（both_plain）或**该建的那侧**
         * 全失败才降级。mixed_ws 下一侧 DTLS 成功就够了。 */
        if (!any_dtls_ok) {
            llog(LOG_WARN, "media",
                 "DTLS-SRTP 无可用终结器 — 降级 BYPASS 让两端直连 "
                 "(need_dtls=[%d,%d])",
                 need_dtls_on_side[0], need_dtls_on_side[1]);
            if (e->relay) {
                rtp_relay_stop(e->relay);
                e->relay = NULL;
            }
            e->mode = MB_BYPASS;
        }
    }
    /* ★ RACE-FIX: dtls 创建 + node 赋值 + admit_ssrc 全部完成后，
     * 最后一次性注册 reactor fd。之前 relay_start 里先 reactor_add
     * 再逐步设 dtls[] → reactor 线程收到 STUN record 时 dtls 还没好
     * → srtp_lane=0 → SRTP 密文被当明文转发 → 概率性单向视频。 */
    if (e->relay)
        rtp_relay_arm(m, e->relay);

    /* ---- SDP 改写 ----
     * dir 选择——BYPASS 和 SRTP_RELAY 用同一套逻辑（都要按接收端类型
     * 改 proto + 消毒/注入 ICE/DTLS 属性），区别只在端口：
     *   BYPASS     → 端口不改（两端 LAN 直连，用 SDP 原始 c=/m=）
     *   SRTP_RELAY → 端口改 relay 端口（SBC 媒体中继）
     *   双明文     → ASIS（不改 proto，只改端口） */
    mb_sdp_dir_t dir;
    uint16_t rewrite_port = ports.b_rtp;
    const int is_bypass = (mode == MB_BYPASS || e->mode == MB_BYPASS);
    if (is_bypass) {
        /* BYPASS = 两端 LAN 直连 = SDP 必须完整透传！
         * 不能改 proto（保留 SAVPF）、不能剥 ICE/DTLS、不能改端口。
         * 否则浏览器收到残缺 SDP → createAnswer 报 "transport is missing"。 */
        dir = MB_SDP_ASIS;
        rewrite_port = 0;
    } else {
        /* SRTP_RELAY：按接收端类型选 dir（DTLS-SRTP 终结器决定了消毒/注入策略） */
        dir = b_ws ? MB_SDP_WS : (a_ws ? MB_SDP_PLAIN : MB_SDP_ASIS);
    }
    int n = bridge_rewrite_sdp(in, inlen, out, outsz,
                               (e->external_rtp_ip[0]) ? e->external_rtp_ip : m->cfg.local_ip,
                               rewrite_port, dir);
    llog(LOG_INFO, "media",
         "★REWRITE_CALL: rewrite_port=%u dir=%d mode=%d is_bypass=%d a_ws=%d b_ws=%d n=%d",
         (unsigned)rewrite_port, dir, mode, is_bypass, a_ws, b_ws, n);
    if (n < 0) return NSBC_NOSPC;

    /* ★★★ 暴力 dump：rewrite 输出逐行打，确认 video m= 行到底是什么！ */
    {
        const char *dp = out; const char *dend = out + n;
        for (int di = 0; dp && dp < dend; ++di) {
            const char *de = memchr(dp, '\n', (size_t)(dend - dp));
            size_t dl = de ? (size_t)(de - dp) : strlen(dp);
            if (dl >= 2 && ((dp[0] == 'm' && dp[1] == '=') ||
                dp[0] == 'c')) {
                llog(LOG_INFO, "media", "★REWRITE_OUT[%d]: %.*s", di, (int)dl, dp);
            }
            dp = de ? de + 1 : NULL;
        }
    }

    /* ★ 向 WebRTC 侧发送的 SDP，如果源 offer 只有 audio 一段没有 video，
     * Chrome 收到后 setRemoteDescription 无法匹配本地 video transceiver →
     * 报 WebRTC Error。补一个 rejected video m-line（port=0）。
     * 用 b_ws（端点标志）而非 e->dtls[1]——Phase C 后 SRTP_RELAY 双侧都建 DTLS server。 */
    if (b_ws) {
        sdp_ensure_video_mline(out, outsz, &n, "UDP/TLS/RTP/SAVPF");
    }

    /* ICE/DTLS 凭证注入（仅 SRTP_RELAY + B=WebRTC 时执行）。
     * 关键：必须 per-section 注入——每个 m= 段都要有一套相同的凭证。
     * 用 b_ws 而非 e->dtls[1]（Phase C 后 dtls[] 与端点类型解耦）。 */
    if (b_ws) {
            const dtls_cfg_t *dcp =
                dtls_server_cfg((const dtls_server_t *)e->dtls[1]);
            char ice_extra[352];
            int ne = snprintf(ice_extra, sizeof(ice_extra),
                     "a=ice-ufrag:%s\r\n"
                     "a=ice-pwd:%s\r\n"
                     "a=fingerprint:sha-256 %s\r\n"
                     "a=setup:actpass\r\n"
                     "a=candidate:1 1 udp 2113937151 %s %u typ host\r\n",
                     dcp->ice_ufrag, dcp->ice_pwd, dcp->fp_sha256,
                     e->external_rtp_ip[0] ? e->external_rtp_ip
                                           : (m->cfg.local_ip ? m->cfg.local_ip : "0.0.0.0"),
                     (unsigned)ports.b_rtp);
            if (ne <= 0 || (size_t)ne >= sizeof(ice_extra)) {
                llog(LOG_WARN, "dtls", "offer ice_extra truncated ne=%d", ne);
            } else {
                char tmp[8192];
                /* ★ 和 answer_hook 对称的 guard：
                 * A 侧端点是普通 SIP（!a_ws）→ SBC 构造 offer 发给 B（WebRTC），
                 *   原始 offer 没有 BUNDLE/mid/rtcp-mux，必须用 sdp_build_webrtc_answer
                 *   补骨架 + 注入 ICE。
                 * A 侧也是 WebRTC（a_ws）→ 原始 offer 已有完整骨架，
                 *   只注入新的 relay ICE 凭证即可。
                 * 注意：不能用 dtls[0] 判断——Phase C 后 SRTP_RELAY 双侧都建 DTLS server，
                 * dtls[] 只表示 relay 加密能力，与端点类型无关。 */
                int n2;
                if (!a_ws) {
                    const char *rejected_fmt = "a=inactive\r\n";
                    n2 = sdp_build_webrtc_answer(
                        out, (size_t)n, tmp, sizeof(tmp),
                        ice_extra, (size_t)ne,
                        rejected_fmt, strlen(rejected_fmt),
                        0 /* start_mid */);
                } else {
                    n2 = sdp_inject_per_section(out, (size_t)n, tmp, sizeof(tmp),
                                                ice_extra, (size_t)ne, 1);
                }
                if (n2 > 0) {
                    if ((size_t)n2 >= outsz) {
                        llog(LOG_WARN, "dtls", "offer inject outsz overflow n2=%d outsz=%zu", n2, outsz);
                        return NSBC_NOSPC;
                    }
                    memcpy(out, tmp, (size_t)n2);
                    n = n2;
                    out[n] = '\0';
                }
            }
            llog(LOG_DEBUG, "dtls", "SRTP_RELAY: offer inject B-side ICE credentials "
                 "fd=%d ice_ufrag=%s fp=%s",
                 e->relay->fd_rtp[1], dcp->ice_ufrag, dcp->fp_sha256);
    }
    /* ★ 诊断：原始 A 的 offer + 最终 offer（改写后） */
    {
        const char *lp = in;
        for (int li = 0; lp && lp < in + inlen; ++li) {
            const char *le = memchr(lp, '\n', (size_t)(in + inlen - lp));
            size_t ll = le ? (size_t)(le - lp) : strlen(lp);
            llog(LOG_DEBUG, "dtls", "★A_OFFER_RAW[%d]: %.*s", li, (int)ll, lp);
            lp = le ? le + 1 : NULL;
        }
    }
    {
        FILE *f = fopen("/tmp/nsbc_offer.sdp", "wb");
        if (f) { fwrite(out, 1, (size_t)n, f); fclose(f); }
        /* ★★ 日志也打印完整 SDP（每一行单独 INFO，grep "★OFFER" 即可） */
        {
            const char *lp = out;
            for (int li = 0; lp && lp < out + n; ++li) {
                const char *le = memchr(lp, '\n', (size_t)(out + n - lp));
                size_t ll = le ? (size_t)(le - lp) : strlen(lp);
                llog(LOG_DEBUG, "dtls", "★OFFER_OUT[%d]: %.*s", li, (int)ll, lp);
                lp = le ? le + 1 : NULL;
            }
        }
        /* 专项诊断：video m-line 检查 */
        {
            const char *vid = memmem(out, (size_t)n, "m=video", 7);
            if (vid) {
                const char *eol = memchr(vid, '\n', (size_t)(out + n - vid));
                size_t vlen = eol ? (size_t)(eol - vid) : 80;
                llog(LOG_DEBUG, "dtls", "★ OFFER VIDEO LINE: %.*s", (int)vlen, vid);
            } else {
                llog(LOG_WARN, "dtls", "★ OFFER MISSING m=video!");
            }
        }
    }
    return n;
}

int bridge_answer_hook(void *call, const char *in, size_t inlen,
                       char *out, size_t outsz, void *ud)
{
    bridge_mgr_t *m = ud;
    cnode_t *e = NULL;
    {
        br_shard_t *s = &m->sh[ptr_shard(call)];
        pthread_mutex_lock(&s->lock);
        e = entry_find(m, call);
        pthread_mutex_unlock(&s->lock);
    }
    if (!e || !e->relay) {
        if (inlen >= outsz)
            return NSBC_NOSPC;
        memcpy(out, in, inlen);
        out[inlen] = '\0';
        return (int)inlen;
    }
    mb_mode_t mode;
    if (bridge_decide(m, NULL, 0, in, inlen, &mode) == NSBC_OK &&
        mode == MB_TRANSCODE) {
        llog(LOG_WARN, "media", "transcode required but P0 unsupported");
        memcpy(out, in, inlen < outsz ? inlen : outsz - 1);
        out[inlen < outsz ? inlen : outsz - 1] = '\0';
        return NSBC_NOTSUP;
    }
    /* ★ RFC 3312 Precondition met 检测：
     * 三层守卫同 offer_hook：force_precondition (dialplan) → profile.enable_precondition */
    {
        b2b_call_t *bc = (b2b_call_t *)call;
        sip_profile_t *pr = bc ? bc->prof : NULL;
        int prof_en = pr && pr->enable_precondition;
        int force_f = bc ? bc->force_precondition : 0;
        int enabled = (force_f == 1) ? 1 : (force_f == 2 ? 0 : prof_en);
        if (enabled && inlen > 0) {
            sdp_t s; memset(&s, 0, sizeof(s));
            if (sdp_parse(&s, in, inlen) == NSBC_OK && s.nmedia > 0) {
                if (sdp_media_preconditions_active(&s.media[0]))
                    e->precond_active = 1;
                int new_met = sdp_media_preconditions_met(&s.media[0]);
                if (new_met && !e->precond_met)
                    llog(LOG_INFO, "media", "answer_hook: precondition MET (gate OPEN)");
                e->precond_met = (uint8_t)new_met;
            }
        }
    }
    relay_ports_t ports;
    ports.a_rtp = ports.b_rtp = 0; /* 已分配：取 fd 实际端口 */
    /* B 对端地址 */
    char ip[64];
    uint16_t port = 0;
    parse_media_addr(in, inlen, ip, sizeof(ip), &port);
    if (ip[0] && !e->dtls[1]) {
        /* B=WebRTC（dtls[1] 非空）时不设固定 peer：ICE 实际源地址/端口
         * 与 SDP c=/m= 可能不同（浏览器任选候选），该侧纯 latch 首包
         * 学习——与 offer_hook 的 set_peer_a(!a_ws) 同口径。 */
        nsbc_sa_t sa;
        int rc = nsbc_sa_from(&sa, ip, port, AF_UNSPEC);
        if (rc == NSBC_OK)
            rtp_relay_set_peer_b(e->relay, &sa);
    }
    /* 对 A 广告 a_rtp（从 relay A 侧 fd 取——answer SDP 告诉 A 把 RTP 发到
     * relay 的 A 侧端口；原代码错取 fd_rtp[1]=B 侧，导致 A 发错端口收不到） */
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(e->relay->fd_rtp[0], (struct sockaddr *)&ss, &sl) == 0) {
        nsbc_sa_t a;
        a.ss = ss;
        a.len = sl;
        ports.a_rtp = nsbc_sa_port(&a);
    }
    /* ★ REQ-MEDIA-SSRC-003③：answer SDP 的 SSRC 预登记到 side=1（answer 来自 B 侧）。
     * 必须在 bridge_rewrite_sdp 之前（rewrite 剥 a=ssrc: 行）。 */
    admit_ssrc_from_sdp(e->relay, 1, in, inlen);
    /* ★ 2026-10-16 P0 修复补漏：offer_hook 按 b_ws 条件创建 side=1 DTLS，但 b_ws 是
     * 从 B 腿注册类型（回调）反查的——注册类型可能是 SIP 但 B 腿实际发了 DTLS-SRTP
     * answer（某些 UDP SIP 端点也支持）。如果 answer 里有 a=fingerprint 且
     * e->dtls[1] 还空着，现在补建 side=1 的 DTLS server。 */
    if (!e->dtls[1] && m->cfg.re && e->mode == MB_SRTP_RELAY) {
        if (is_webrtc_sdp(in, inlen) && memmem(in, inlen, "a=fingerprint:", 14)) {
            dtls_cfg_t dc = { .srtp_profile_id = 0x0007 };
            dtls_server_t *ds = NULL;
            if (dtls_server_create(e->relay->fd_rtp[1], DTLS_ROLE_SERVER,
                                   &dc, &ds) == NSBC_OK) {
                e->dtls[1] = ds;
                e->b_ws = 1;
                llog(LOG_INFO, "dtls",
                     "answer_hook: 补建 side=1 dtls_server on fd=%d "
                     "(answer has fingerprint, b_ws upgraded 0→1)",
                     e->relay->fd_rtp[1]);
                inject_peer_fp(ds, in, inlen);
            } else {
                llog(LOG_WARN, "dtls",
                     "answer_hook: dtls_server_create FAIL on fd=%d "
                     "(side=1, answer has fingerprint)",
                     e->relay->fd_rtp[1]);
            }
        }
    }
    /* answer 改写方向（与 offer 对称、反着来）：
     *   BYPASS     → ASIS：完整透传（两端 LAN 直连）；
     *   A=WebRTC（dtls[0] 非空）→ WS：proto→SAVPF + 下方注入 A 侧新凭证；
     *   B=WebRTC（dtls[1] 非空）→ PLAIN：消毒（proto→RTP/AVP + 剥 ICE/DTLS）；
     *   双明文 → ASIS：仅改地址端口（a_rtp=fd_rtp[0] 侧）。 */
    mb_sdp_dir_t adir;
    uint16_t a_rewrite_port = ports.a_rtp;
    {
        mb_mode_t fmode;
        bridge_decide(m, NULL, 0, in, inlen, &fmode);
        /* ★ 关键 guard：只要 offer_hook 已经决定了 SRTP_RELAY（dtls[0] 或 dtls[1]
         * 已创建），answer_hook 就绝对不能降级 BYPASS。
         * 根因：MicroSIP 内网普通 SIP answer → bridge_decide(answer-only) 会因
         * trust_private + 内网 IP 返回 MB_BYPASS，导致 adir=ASIS + port=0 透传
         * B 的原始 RTP/AVP SDP，然后叠加 SBC 的 ICE/DTLS → 四不像被 JsSIP 拒绝。
         * offer_hook 已有对称 guard（line 1325-1329），answer_hook 之前缺。 */
        const int web_rtc_any = (e->dtls[0] || e->dtls[1]);
        const int a_bypass = !web_rtc_any &&
            (fmode == MB_BYPASS || e->mode == MB_BYPASS
             || (!e->dtls[0] && !e->dtls[1]
                 && (bridge_decide(m, NULL, 0, NULL, 0, &fmode),
                     fmode == MB_BYPASS)));
        if (a_bypass) {
            adir = MB_SDP_ASIS;
            a_rewrite_port = 0;
        } else {
            /* ★ 用端点标志而非 dtls[]——Phase C 后 SRTP_RELAY 双侧都建 DTLS server，
             * dtls[] 不再能区分端点类型。A 侧端点是 WebRTC → WS 模式保留 WebRTC
             * SDP 属性；否则 PLAIN 模式消毒传统 SIP answer。 */
            adir = e->a_ws ? MB_SDP_WS : MB_SDP_PLAIN;
        }
    }
    int n = bridge_rewrite_sdp(in, inlen, out, outsz,
                               (e->external_rtp_ip[0]) ? e->external_rtp_ip : m->cfg.local_ip,
                               a_rewrite_port, adir);
    if (n < 0) return NSBC_NOSPC;

    /* ★ RFC 3264 段数对齐：把 answer 截断到 A 原始 offer 的 m= 段数。
     * 多出来的 rejected video 段是 offer_hook 为 WebRTC B 追加、浏览器
     * 回显回来的，不能发给只 offer 了 audio 的 A（MicroSIP/PJSIP）
     * —— 段数不符 EINVANSW → ACK 后 0ms BYE。
     * 放在 sdp_ensure_video_mline（a_ws 路径往回补段）之前。 */
    {
        int tn = sdp_truncate_mlines(out, (size_t)n, (int)e->a_mlcount);
        if (tn >= 0 && (size_t)tn < (size_t)n) {
            llog(LOG_INFO, "dtls", "answer_hook: truncate m-lines %d→%d "
                 "(a_mlcount=%d)", n, tn, (int)e->a_mlcount);
            n = tn;
        }
    }

    /* ★ answer 方向同理：A=WebRTC（a_ws=1）时，B 侧普通 SIP answer
     * 可能只有 audio，补 rejected video m-line 让 Chrome 能正常协商。
     * 用 e->a_ws（端点标志）而非 e->dtls[0]——Phase C 后 SRTP_RELAY 双侧都建 DTLS server。 */
    if (e->a_ws) {
        sdp_ensure_video_mline(out, outsz, &n, "UDP/TLS/RTP/SAVPF");
    }

    /* A=WebRTC：answer 注入 A 侧（fd_rtp[0]）ICE 凭证 + host candidate。
     * setup:passive——浏览器是 Offerer（其 offer 带 setup:actpass），
     * answer 回 passive 则浏览器转 DTLS Client 发起握手，NSBC 恒 Server。
     * 凭证取自 dtls_server 实例（create 内部生成，调用方栈上的 dc 是空壳，
     * 与 offer_hook 注释同理）。
     * 同样必须 per-section 注入——每个 m= 段都要有一套相同的凭证。
     * 用 e->a_ws 而非 e->dtls[0]（Phase C 后 dtls[] 与端点类型解耦）。 */
    if (e->a_ws) {
        const dtls_cfg_t *dcp =
            dtls_server_cfg((const dtls_server_t *)e->dtls[0]);
        char ice_extra[352];
        int ne = snprintf(ice_extra, sizeof(ice_extra),
                 "a=ice-ufrag:%s\r\n"
                 "a=ice-pwd:%s\r\n"
                 "a=fingerprint:sha-256 %s\r\n"
                 "a=setup:passive\r\n"
                 "a=candidate:1 1 udp 2113937151 %s %u typ host\r\n",
                 dcp->ice_ufrag, dcp->ice_pwd, dcp->fp_sha256,
                 e->external_rtp_ip[0] ? e->external_rtp_ip
                                       : (m->cfg.local_ip ? m->cfg.local_ip : "0.0.0.0"),
                 (unsigned)ports.a_rtp);
        if (ne <= 0 || (size_t)ne >= sizeof(ice_extra)) {
            llog(LOG_WARN, "dtls", "answer ice_extra truncated ne=%d", ne);
        } else {
            char tmp[8192];
            /* ★ 关键 guard：只在 B 侧是普通 SIP（!b_ws）时补全 answer 骨架。
             * B 侧是 WebRTC（b_ws）时 answer 已有 BUNDLE/mid/rtcp-mux，
             * 用 sdp_inject_per_section 只注入 ICE 凭证即可，重复注入会被拒。
             * 注意：不能用 dtls[1] 判断——Phase C 后 SRTP_RELAY 双侧都建 DTLS server，
             * dtls[] 只表示 relay 加密能力，与端点类型无关。 */
            int n2;
            if (!e->b_ws) {
                /* B 侧是普通 SIP，answer 是 B 生成的传统 SIP SDP，
                 * 需要补全 WebRTC 骨架才能被 Chrome/JsSIP 接受 */
                const char *rejected_fmt = "a=inactive\r\n";
                n2 = sdp_build_webrtc_answer(
                    out, (size_t)n, tmp, sizeof(tmp),
                    ice_extra, (size_t)ne,
                    rejected_fmt, strlen(rejected_fmt),
                    0 /* start_mid */);
            } else {
                /* 双 WebRTC：B 的 answer 已有 BUNDLE/mid/rtcp-mux，只注入 ICE */
                n2 = sdp_inject_per_section(out, (size_t)n, tmp, sizeof(tmp),
                                            ice_extra, (size_t)ne, 1);
            }
            if (n2 > 0) {
                if ((size_t)n2 >= outsz) {
                    llog(LOG_WARN, "dtls", "answer inject outsz overflow n2=%d outsz=%zu", n2, outsz);
                    return NSBC_NOSPC;
                }
                memcpy(out, tmp, (size_t)n2);
                n = n2;
                out[n] = '\0';
            }
        }
        llog(LOG_DEBUG, "dtls", "SRTP_RELAY: answer inject A-side ICE credentials "
             "fd=%d ice_ufrag=%s fp=%s",
             e->relay->fd_rtp[0], dcp->ice_ufrag, dcp->fp_sha256);
    }
    /* B=WebRTC：answer SDP 来自 B（此时 answer_hook 的 in 就是 B 的 answer），
     * 提取 B 的 fingerprint 注入 dtls[1]，完成后握手自动校验。
     * 注意：dtls[1] 早在 offer_hook 就已创建，当时 B 的 answer SDP 还没来，
     * 所以 B 的 fingerprint 必须推迟到这里才注入——这就是 dtls_server_set_peer_fp
     * 存在的唯一理由。 */
    if (e->dtls[1]) {
        inject_peer_fp(e->dtls[1], in, inlen);
    }
    /* ★ 诊断：①原始 B 的 answer（改写前）②最终 answer（改写后） */
    {
        const char *lp = in;
        for (int li = 0; lp && lp < in + inlen; ++li) {
            const char *le = memchr(lp, '\n', (size_t)(in + inlen - lp));
            size_t ll = le ? (size_t)(le - lp) : strlen(lp);
            llog(LOG_INFO, "dtls", "★B_ANSWER_RAW[%d]: %.*s", li, (int)ll, lp);
            lp = le ? le + 1 : NULL;
        }
    }
    {
        const char *lp = out;
        for (int li = 0; lp && lp < out + n; ++li) {
            const char *le = memchr(lp, '\n', (size_t)(out + n - lp));
            size_t ll = le ? (size_t)(le - lp) : strlen(lp);
            llog(LOG_INFO, "dtls", "★ANSWER_OUT[%d]: %.*s", li, (int)ll, lp);
            lp = le ? le + 1 : NULL;
        }
    }
    return n;
}

void bridge_release_call(bridge_mgr_t *m, void *call)
{
    br_shard_t *s = &m->sh[ptr_shard(call)];
    pthread_mutex_lock(&s->lock);
    cnode_t **pp = &s->head;
    while (*pp) {
        if ((*pp)->call == call) {
            cnode_t *e = *pp;
            *pp = e->next;
            pthread_mutex_unlock(&s->lock);
            llog(LOG_INFO, "media", "bridge_release_call: call=%p relay=%p mode=%d",
                 call, (void *)e->relay, e->mode);
            if (e->relay)
                rtp_relay_stop(e->relay);
            /* ★ 转码会话清理（REQ-TRANS-004） */
            if (e->transcode[0]) { transcode_close(e->transcode[0]); e->transcode[0] = NULL; }
            if (e->transcode[1]) { transcode_close(e->transcode[1]); e->transcode[1] = NULL; }
            /* ★ DTMF RFC4733 去重器清理 */
            for (int si = 0; si < 2; ++si) {
                if (e->dtmf_rx[si]) {
                    dtmf_rx_destroy((dtmf_rx_t *)e->dtmf_rx[si]);
                    e->dtmf_rx[si] = NULL;
                }
                e->dtmf_pt[si] = 0;
            }
            free(e);
            return;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s->lock);
    llog(LOG_INFO, "media", "bridge_release_call: call=%p NOT FOUND in shard", call);
}

/* ======================================================================
 * REQ-AI-001：RTP payload 注入（给 ai_dialog / hold / 未来 broadcast 用）
 * 桥接层只做：找 relay → 选 dst（latch 优先 peer fallback）→ 构造 RTP 头 → sendto
 * 不做 codec 转换、不做 SSRC 白名单校验（外部注入走内部 SSRC 是已知可信场景）。
 * 每次调用自动推进 *seq / *ts（供下一次用）。
 * ====================================================================== */
int bridge_send_raw(bridge_mgr_t *m, void *call_key, int leg,
                    const void *payload, size_t len, uint8_t pt,
                    uint32_t ssrc, uint16_t *seq, uint32_t *ts, uint32_t ts_inc)
{
    if (!m || !call_key || !payload || len == 0 || leg < 0 || leg > 1) return -EINVAL;

    rtp_relay_t *r = NULL;
    nsbc_sa_t peer[2];
    if (bridge_lookup_relay(m, call_key, &r, peer) != NSBC_OK || !r)
        return -ENODEV; /* bypass 或 relay 还没建 */

    int dst_side = 1 - leg;  /* 向 leg 对侧发 */
    const nsbc_sa_t *dst = r->latched[dst_side] ? &r->latch[dst_side]
                          : (r->have_peer[dst_side] ? &r->peer[dst_side]
                          : NULL);
    if (!dst) return -ENODEV;

    if (len + 12 > 2048) return -EMSGSIZE; /* 大帧安全兜底 */

    /* 构造 RTP 头 + payload —— 栈上 2KB，不分配 */
    unsigned char frame[2048];
    frame[0] = 0x80;          /* V=2, P=0, X=0, CC=0 */
    frame[1] = pt & 0x7F;     /* M=0, PT（bit 7 是 marker）*/
    uint16_t seq_cur = *seq;
    uint32_t ts_cur  = *ts;
    frame[2] = (seq_cur >> 8) & 0xFF; frame[3] = seq_cur & 0xFF;
    frame[4] = (ts_cur >> 24) & 0xFF; frame[5] = (ts_cur >> 16) & 0xFF;
    frame[6] = (ts_cur >> 8) & 0xFF;  frame[7] = ts_cur & 0xFF;
    frame[8] = (ssrc >> 24) & 0xFF;   frame[9] = (ssrc >> 16) & 0xFF;
    frame[10] = (ssrc >> 8) & 0xFF;   frame[11] = ssrc & 0xFF;
    memcpy(frame + 12, payload, len);

    ssize_t ret = sendto(r->fd_rtp[dst_side], frame, 12 + len, MSG_NOSIGNAL,
                         (const struct sockaddr *)&dst->ss, dst->len);
    if (ret > 0) {
        /* 成功才推进计数器（让调用方知道下次用什么值） */
        *seq = (uint16_t)(seq_cur + 1);
        *ts  = ts_cur + ts_inc;
    }
    return ret;
}

/* REQ-AI-001：rec_feed_fn 动态 hook 注册。
 * add 返回 hook 节点指针（调用方保存，remove 时传同指针）。
 * 线程安全：持 rec_hook_lock 增删；移除会等待已开始的媒体回调结束。
 * hook 节点 malloc 在 mgr 上，bridge_mgr_destroy 时统一 free。 */
void *bridge_add_rec_feed_hook(bridge_mgr_t *m, mb_rec_hook_fn fn, void *ud)
{
    if (!m || !fn) return NULL;
    mb_rec_hook_t *h = (mb_rec_hook_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->fn = fn;
    h->ud = ud;
    pthread_mutex_lock(&m->rec_hook_lock);
    unsigned count = 0;
    for (mb_rec_hook_t *it =
             __atomic_load_n(&m->rec_hooks, __ATOMIC_RELAXED);
         it; it = it->next)
        ++count;
    if (count >= MB_REC_HOOK_MAX) {
        pthread_mutex_unlock(&m->rec_hook_lock);
        free(h);
        llog(LOG_WARN, "media", "recording hook limit reached (%u)",
             MB_REC_HOOK_MAX);
        return NULL;
    }
    h->next = __atomic_load_n(&m->rec_hooks, __ATOMIC_RELAXED);
    __atomic_store_n(&m->rec_hooks, h, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&m->rec_hook_lock);
    return h;
}

int bridge_remove_rec_feed_hook(bridge_mgr_t *m, void *hook_ptr)
{
    if (!m || !hook_ptr) return -EINVAL;
    if (rec_hook_dispatch_depth > 0) return -EDEADLK;
    mb_rec_hook_t *target = (mb_rec_hook_t *)hook_ptr;
    pthread_mutex_lock(&m->rec_hook_lock);
    mb_rec_hook_t *head =
        __atomic_load_n(&m->rec_hooks, __ATOMIC_RELAXED);
    mb_rec_hook_t *prev = NULL;
    for (mb_rec_hook_t *h = head; h; h = h->next) {
        if (h == target) {
            if (prev)
                prev->next = h->next;
            else
                __atomic_store_n(&m->rec_hooks, h->next, __ATOMIC_RELEASE);
            while (target->refs > 0)
                pthread_cond_wait(&m->rec_hook_cond, &m->rec_hook_lock);
            pthread_mutex_unlock(&m->rec_hook_lock);
            free(target);
            return 0;
        }
        prev = h;
    }
    pthread_mutex_unlock(&m->rec_hook_lock);
    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* 最终模式确认 + OAM 只读视图（REQ-MEDIA-SEC-003⑤/REQ-OBS-010）         */
/* ------------------------------------------------------------------ */
/* 同时提取 PT 和 codec_id 映射。优先静态 PT → pt_to_codec()，
 * 未命中则用 codec name（sdp_codec_t.enc）查 registry——覆盖 WebRTC 动态 PT 场景。 */
static int sdp_extract_audio_codecs(const char *sdp, size_t len,
                                    uint8_t *out_pts, codec_id_t *out_cids,
                                    int out_cap)
{
    sdp_t s;
    if (sdp_parse(&s, sdp, len) != NSBC_OK) {
        llog(LOG_WARN, "media", "sdp_extract_audio_codecs: sdp_parse FAIL len=%zu "
             "(bump SDP_MAX_ATTRS? trying fallback raw m= scan)", len);
        /* ★ 防御性 fallback：sdp_parse 因 attr 超限/未知 type 行等失败时，
         * 手动扫 m=audio 行的 fmt 字段。最糟情况返回 0 跟原来一样，
         * 好情况能救回 PT 列表让 bridge 正常跑。
         * —— 之前 Chrome WebRTC offer attr 数 > 48 直接导致整 parse 挂，
         * codec 协商全废，RTP 因 PT 不被识别被转码分支 drop */
        const char *p = sdp;
        const char *end = sdp + len;
        while (p < end) {
            if ((size_t)(end - p) < 7) break;
            if (p[0] == 'm' && p[1] == '=' &&
                strncmp(p + 2, "audio", 5) == 0 &&
                (p[7] == ' ' || p[7] == '\t')) {
                /* 找到了 m=audio 行 — 后面是 "port proto fmt1 fmt2 ..." */
                const char *q = p + 8;  /* 跳过 "m=audio " */
                /* 跳过 port 和 proto 两个字段 */
                int field = 0;
                while (q < end && field < 2) {
                    while (q < end && *q != ' ' && *q != '\r' && *q != '\n') ++q;
                    ++field;
                    if (q < end && *q == ' ') ++q;
                }
                /* 现在 q 指向 fmt 列表 */
                int n = 0;
                while (q < end && *q != '\r' && *q != '\n' && n < out_cap) {
                    while (q < end && (*q == ' ' || *q == '\t')) ++q;
                    if (q >= end || *q == '\r' || *q == '\n') break;
                    const char *fs = q;
                    while (q < end && *q != ' ' && *q != '\r' && *q != '\n') ++q;
                    char buf[16];
                    size_t fl = (size_t)(q - fs);
                    if (fl >= sizeof(buf)) fl = sizeof(buf) - 1;
                    memcpy(buf, fs, fl); buf[fl] = '\0';
                    char *ep = NULL;
                    long pt = strtol(buf, &ep, 10);
                    if (ep != buf && pt >= 0 && pt <= 255) {
                        out_pts[n] = (uint8_t)pt;
                        out_cids[n] = pt_to_codec((int)pt);
                        ++n;
                    }
                }
                if (n > 0) {
                    llog(LOG_WARN, "media",
                         "sdp_extract_audio_codecs: FALLBACK raw scan got n=%d PTs "
                         "(sdp_parse FAIL but bridge should still work)", n);
                }
                return n;
            }
            /* 找下一行 */
            while (p < end && *p != '\n') ++p;
            if (p < end && *p == '\n') ++p;
        }
        llog(LOG_WARN, "media", "sdp_extract_audio_codecs: fallback also no audio m-line");
        return 0;
    }
    for (int i = 0; i < s.nmedia; ++i) {
        sdp_media_t *m = &s.media[i];
        llog(LOG_INFO, "media",
             "sdp_extract_audio_codecs: media[%d] type=%d nfmt=%d ncodecs=%d built=%d",
             i, m->type, m->nfmt, m->ncodecs, m->codecs_built);
        if (m->type != SDP_M_AUDIO) continue;
        /* ★ 关键：sdp_parse 只填 fmt[]，codecs[] 是延迟构建的
         * 不调 build_codecs → ncodecs=0 → pt_allowed=(n=0) → 所有 RTP 包被静默丢弃 */
        if (!m->codecs_built)
            sdp_media_build_codecs(&s, m);
        llog(LOG_INFO, "media",
             "sdp_extract_audio_codecs: after build_codecs ncodecs=%d", m->ncodecs);
        int n = 0;
        for (int j = 0; j < m->ncodecs && n < out_cap; ++j) {
            const sdp_codec_t *c = &m->codecs[j];
            out_pts[n] = (uint8_t)c->pt;
            codec_id_t cid = pt_to_codec(c->pt);
            llog(LOG_INFO, "media",
                 "sdp_extract_audio_codecs: codec[%d] pt=%u enc.len=%zu enc=[%.*s] cid=%d",
                 j, (unsigned)c->pt, c->enc.len,
                 (int)c->enc.len, c->enc.p ? c->enc.p : "(null)",
                 (int)cid);
            /* 静态 PT 未命中 → 用 SDP rtpmap 里的 encoder name 查 registry
             * 这覆盖 WebRTC 动态 PT 场景（如 G.729 用 PT=96 但 enc="G729"） */
            if (cid == CODEC_NONE && c->enc.len > 0 && c->enc.p) {
                const codec_ops_t *ops = codec_registry_find_name(c->enc.p, c->enc.len);
                if (ops) cid = ops->id;
            }
            out_cids[n] = cid;
            ++n;
        }
        llog(LOG_INFO, "media", "sdp_extract_audio_codecs: audio media return n=%d", n);
        return n;
    }
    llog(LOG_WARN, "media", "sdp_extract_audio_codecs: no audio m-line, nmedia=%d", s.nmedia);
    return 0;
}

/* 在候选 codec 列表里找一个目标：与 src 不同、在 registry 里已注册、
 * 优先选另一个列表里也有的（offer 和 answer 都支持的 codec 作为"中转点"）。
 * P0 每次返回第一个命中的候选即可（单方向只建一个转码会话）。 */
static codec_id_t find_transcode_dst(codec_id_t src,
                                      const codec_id_t *candidates, int cand_n,
                                      const codec_id_t *other, int other_n)
{
    /* 第一优先级：candidates 和 other 的交集（offer 和 answer 都支持的 codec） */
    for (int i = 0; i < cand_n; ++i) {
        if (candidates[i] == CODEC_NONE || candidates[i] == src) continue;
        if (!codec_registry_find(candidates[i])) continue;
        for (int j = 0; j < other_n; ++j) {
            if (other[j] == candidates[i])
                return candidates[i];
        }
    }
    /* 第二优先级：candidates 里第一个已注册且不等于 src 的 codec */
    for (int i = 0; i < cand_n; ++i) {
        if (candidates[i] == CODEC_NONE || candidates[i] == src) continue;
        if (codec_registry_find(candidates[i]))
            return candidates[i];
    }
    return CODEC_NONE;
}

/* ★ 顶层 static 辅助：扫描 SDP 找 "a=rtpmap:<PT> telephone-event/..." 行
 * 返回 telephone-event 的 PT（0=未协商）。bridge_confirm 里调用。 */
static uint8_t find_dtmf_pt(const char *sdp, size_t len) {
    const char *p = sdp;
    const char *end = sdp + len;
    while ((p = memmem(p, (size_t)(end - p), "a=rtpmap:", 9)) != NULL) {
        p += 9;
        if (p >= end) break;
        const char *pt_start = p;
        while (pt_start < end && *pt_start >= '0' && *pt_start <= '9') pt_start++;
        if (pt_start == p) { p = pt_start; continue; }
        const char *q = pt_start;
        while (q < end && *q == ' ') q++;
        if (q + 14 <= end && memcmp(q, "telephone-event", 14) == 0) {
            unsigned pt = (unsigned)strtoul(p, NULL, 10);
            if (pt < 256) return (uint8_t)pt;
        }
        p = pt_start;
    }
    return 0;
}

int bridge_confirm(bridge_mgr_t *m, void *call, const char *offer,
                   size_t offer_len, const char *answer, size_t answer_len)
{
    if (!m || !call || !offer || !offer_len || !answer || !answer_len)
        return NSBC_INVAL;

    /* ★ 完整 SDP dump：offer=A 腿原始 INVITE SDP，answer=B 腿原始 200 OK SDP
     * 分段打（每 256B 一行），避免单行过长被日志截断 */
    llog(LOG_INFO, "media",
         "★BRIDGE_CONFIRM call=%p offer_len=%zu answer_len=%zu",
         call, offer_len, answer_len);
    llog(LOG_INFO, "media", "★OFFER_DUMP_BEGIN total=%zu", offer_len);
    for (size_t off = 0; off < offer_len; off += 256) {
        size_t chunk = offer_len - off;
        if (chunk > 256) chunk = 256;
        llog(LOG_INFO, "media", "★OFFER[%zu..%zu]: %.*s",
             off, off + chunk, (int)chunk, offer + off);
    }
    llog(LOG_INFO, "media", "★OFFER_DUMP_END");
    llog(LOG_INFO, "media", "★ANSWER_DUMP_BEGIN total=%zu", answer_len);
    for (size_t off = 0; off < answer_len; off += 256) {
        size_t chunk = answer_len - off;
        if (chunk > 256) chunk = 256;
        llog(LOG_INFO, "media", "★ANSWER[%zu..%zu]: %.*s",
             off, off + chunk, (int)chunk, answer + off);
    }
    llog(LOG_INFO, "media", "★ANSWER_DUMP_END");

    /* bridge_decide 全路径返回 NSBC_OK（与 offer/answer hook 同口径，
     * 不检查返回值）；初值兜底，决策结果必被其写入。 */
    mb_mode_t final_mode = MB_BYPASS;
    bridge_decide(m, offer, offer_len, answer, answer_len, &final_mode);

    /* ★ 通用 codec 列表提取（PT + codec_id）
     * 无论 bridge_decide 返回什么，都解析拿到两侧 audio codec 列表，
     * 为运行时通用转码做准备（G.711 互转 + G.729 互转 + 跨 codec）。 */
    uint8_t offer_pts[8], answer_pts[8];
    codec_id_t offer_cids[8], answer_cids[8];
    int offer_n = sdp_extract_audio_codecs(offer, offer_len, offer_pts, offer_cids, 8);
    int answer_n = sdp_extract_audio_codecs(answer, answer_len, answer_pts, answer_cids, 8);

    br_shard_t *s = &m->sh[ptr_shard(call)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call);
    if (!e) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_NOENT;
    }
    if (e->relay) {
        /* 媒体面已锚定：不降级 bypass
         *
         * ★ pt_allowed[side] 语义：转发到 side 这一侧时，对端接受的 PT 列表
         *   （relay_recv_one 检查 pt_allowed[other] —— 收到的包要发给 other，
         *    必须符合 other 这一侧的对端接受的 PT）：
         *     pt_allowed[0] = offer_pts   (A 腿=side 0，accepts offer's audio PTs)
         *     pt_allowed[1] = answer_pts  (B 腿=side 1，accepts answer's audio PTs)
         *   当 side=0 (A→NSBC) 收到包要转发到 other=1 (B)：检查 pt_allowed[1]=answer_pts
         *   当 side=1 (B→NSBC) 收到包要转发到 other=0 (A)：检查 pt_allowed[0]=offer_pts
         */
        e->pt_allowed_count[0] = offer_n;
        memcpy(e->pt_allowed[0], offer_pts, (size_t)offer_n);
        e->pt_allowed_count[1] = answer_n;
        memcpy(e->pt_allowed[1], answer_pts, (size_t)answer_n);

        /* ★ DTMF RFC4733：扫描 offer/answer SDP 提取 telephone-event PT
         * 顶层 static find_dtmf_pt（函数外定义）负责实际解析 */
        e->dtmf_pt[0] = find_dtmf_pt(offer, offer_len);
        e->dtmf_pt[1] = find_dtmf_pt(answer, answer_len);
        if (e->dtmf_pt[0] || e->dtmf_pt[1]) {
            llog(LOG_INFO, "media",
                 "★DTMF call=%p dtmf_pt[0]=%u dtmf_pt[1]=%u",
                 call, (unsigned)e->dtmf_pt[0], (unsigned)e->dtmf_pt[1]);
            /* 创建每侧 DTMF 去重器（只创建有 PT 的侧，PT 缺省 0 跳过） */
            if (e->dtmf_pt[0]) {
                dtmf_rx_t *rx = NULL;
                if (dtmf_rx_create((int)e->dtmf_pt[0], &rx) == NSBC_OK)
                    e->dtmf_rx[0] = rx;
            }
            if (e->dtmf_pt[1]) {
                dtmf_rx_t *rx = NULL;
                if (dtmf_rx_create((int)e->dtmf_pt[1], &rx) == NSBC_OK)
                    e->dtmf_rx[1] = rx;
            }
        }

        if (final_mode == MB_TRANSCODE && e->mode != MB_TRANSCODE) {
            e->mode = MB_TRANSCODE;
            llog(LOG_INFO, "media",
                 "call %p codec mismatch SDP-level, entering TRANSCODE mode",
                 call);
        }

        /* ★ 通用 codec 交集检查（REQ-TRANS-004 P1 泛化）
         * 对 offer 里每个 PT，检查 answer 有没有接受；没接受 → 用 find_transcode_dst
         * 找一个已注册的目标 codec → transcode_open(src→dst)。
         *
         * 覆盖场景：
         *   G.711 互转（offer=0+8, answer=8 only → 0→8）
         *   G.729 互转（offer=G729 PT=96, answer=G729 PT=18 → 96→18）
         *   跨 codec （offer=0, answer=18 → 0→18 走 G711 PCMA→PCM→G729） */

        /* --- side 0 (A→B)：offer codec 发往 answer 侧 --- */
        for (int i = 0; i < offer_n && !e->transcode[0] && !e->pt_rewrite_active[0]; ++i) {
            codec_id_t src = offer_cids[i];
            if (src == CODEC_NONE) continue;     /* 未注册 codec（telephone-event 等），跳过 */
            if (pt_in_list(offer_pts[i], answer_pts, answer_n)) continue; /* answer 已接受，不需处理 */

            /* ★ PT rewrite pre-check：同 codec_id 在 answer 里有没有（哪怕 PT 不同）
             * 例如 WebRTC 侧 G729 PT=96 vs SIP 侧 G729 PT=18 —— 只需改 PT 字节，
             * 无需 PCM 往返转码（快 ~500x，零延迟增加） */
            int found_same_codec = 0;
            uint8_t answer_pt = 0;
            for (int j = 0; j < answer_n; ++j) {
                if (answer_cids[j] == src && src != CODEC_NONE) {
                    found_same_codec = 1;
                    answer_pt = answer_pts[j];
                    break;
                }
            }
            if (found_same_codec) {
                e->pt_rewrite[0].src_pt = offer_pts[i];
                e->pt_rewrite[0].dst_pt = answer_pt;
                e->pt_rewrite_active[0] = 1;
                const codec_ops_t *ops = codec_registry_find(src);
                llog(LOG_INFO, "media",
                     "call %p PT rewrite[0] %s(pt=%u)→pt=%u (same codec, no transcode)",
                     call, ops ? ops->name : "?",
                     (unsigned)offer_pts[i], (unsigned)answer_pt);
                continue;
            }

            /* 真·codec 不同：走 transcode */
            codec_id_t dst = find_transcode_dst(src, offer_cids, offer_n,
                                                  answer_cids, answer_n);
            if (dst == CODEC_NONE) continue;
            transcode_sess_t *tc = NULL;
            int rc = transcode_open(src, dst, &tc);
            if (rc == NSBC_OK) {
                e->transcode[0] = tc;
                e->mode = MB_TRANSCODE;
                const codec_ops_t *ops_src = codec_registry_find(src);
                const codec_ops_t *ops_dst = codec_registry_find(dst);
                llog(LOG_INFO, "media",
                     "call %p transcode[0]=%s(pt=%u)→%s(pt=%u) enabled",
                     call,
                     ops_src ? ops_src->name : "?", (unsigned)offer_pts[i],
                     ops_dst ? ops_dst->name : "?",
                     ops_dst ? (unsigned)ops_dst->default_pt : (unsigned)answer_pts[0]);
            } else {
                const codec_ops_t *ops_src = codec_registry_find(src);
                const codec_ops_t *ops_dst = codec_registry_find(dst);
                llog(LOG_WARN, "media",
                     "call %p transcode_open(%s→%s) rc=%d — payload may be dropped",
                     call,
                     ops_src ? ops_src->name : "?",
                     ops_dst ? ops_dst->name : "?", rc);
            }
        }

        /* --- side 1 (B→A)：answer codec 发往 offer 侧 --- */
        for (int i = 0; i < answer_n && !e->transcode[1] && !e->pt_rewrite_active[1]; ++i) {
            codec_id_t src = answer_cids[i];
            if (src == CODEC_NONE) continue;
            if (pt_in_list(answer_pts[i], offer_pts, offer_n)) continue;

            /* PT rewrite pre-check */
            int found_same_codec = 0;
            uint8_t offer_pt = 0;
            for (int j = 0; j < offer_n; ++j) {
                if (offer_cids[j] == src && src != CODEC_NONE) {
                    found_same_codec = 1;
                    offer_pt = offer_pts[j];
                    break;
                }
            }
            if (found_same_codec) {
                e->pt_rewrite[1].src_pt = answer_pts[i];
                e->pt_rewrite[1].dst_pt = offer_pt;
                e->pt_rewrite_active[1] = 1;
                const codec_ops_t *ops = codec_registry_find(src);
                llog(LOG_INFO, "media",
                     "call %p PT rewrite[1] %s(pt=%u)→pt=%u (same codec, no transcode)",
                     call, ops ? ops->name : "?",
                     (unsigned)answer_pts[i], (unsigned)offer_pt);
                continue;
            }

            codec_id_t dst = find_transcode_dst(src, answer_cids, answer_n,
                                                  offer_cids, offer_n);
            if (dst == CODEC_NONE) continue;

            transcode_sess_t *tc = NULL;
            int rc = transcode_open(src, dst, &tc);
            if (rc == NSBC_OK) {
                e->transcode[1] = tc;
                e->mode = MB_TRANSCODE;
                const codec_ops_t *ops_src = codec_registry_find(src);
                const codec_ops_t *ops_dst = codec_registry_find(dst);
                llog(LOG_INFO, "media",
                     "call %p transcode[1]=%s(pt=%u)→%s(pt=%u) enabled",
                     call,
                     ops_src ? ops_src->name : "?", (unsigned)answer_pts[i],
                     ops_dst ? ops_dst->name : "?",
                     ops_dst ? (unsigned)ops_dst->default_pt : (unsigned)offer_pts[0]);
            }
        }

        if (e->mode == MB_TRANSCODE && !e->transcode[0] && !e->transcode[1]) {
            /* 确实需要转码但 codec registry 没有可用 codec → 记 WARN
             * relay_recv_one 会走原始透传路径，保持静默不丢包 */
            llog(LOG_WARN, "media",
                 "call %p codec mismatch but no transcode session created "
                 "(registry missing target codecs) — running plain relay",
                 call);
        }
    } else {
        e->mode = MB_BYPASS; /* 无 relay 实例：实际路径即透传 */
    }

    /* ★ bridge_confirm 最终状态快照 — 单通/静音调试时一目了然 */
    if (e->relay) {
        llog(LOG_INFO, "media",
             "bridge_confirm FINAL call=%p mode=%d transcode[0]=%p transcode[1]=%p "
             "rewrite[0]=%d(%u→%u) rewrite[1]=%d(%u→%u) "
             "pt_allowed[0]=(n=%d)[",
             call, e->mode,
             e->transcode[0], e->transcode[1],
             e->pt_rewrite_active[0], e->pt_rewrite[0].src_pt, e->pt_rewrite[0].dst_pt,
             e->pt_rewrite_active[1], e->pt_rewrite[1].src_pt, e->pt_rewrite[1].dst_pt,
             e->pt_allowed_count[0]);
        for (int i = 0; i < e->pt_allowed_count[0]; ++i)
            llog(LOG_INFO, "media", "  pt_allowed[0][%d]=%u", i, e->pt_allowed[0][i]);
        llog(LOG_INFO, "media", "  pt_allowed[1]=(n=%d)[", e->pt_allowed_count[1]);
        for (int i = 0; i < e->pt_allowed_count[1]; ++i)
            llog(LOG_INFO, "media", "  pt_allowed[1][%d]=%u", i, e->pt_allowed[1][i]);
    }
    pthread_mutex_unlock(&s->lock);
    return NSBC_OK;
}

int bridge_call_view(bridge_mgr_t *m, void *call, mb_view_t *v)
{
    if (!m || !call || !v)
        return NSBC_INVAL;
    memset(v, 0, sizeof(*v));
    br_shard_t *s = &m->sh[ptr_shard(call)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call);
    if (!e) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_NOENT;
    }
    v->valid = 1;
    v->mode = e->mode;
    rtp_relay_t *r = e->relay;
    if (!r) {
        /* bypass：设备不过媒体，gate 恒 OPEN，SSRC/VQ 无值（禁估算） */
        v->gate_open[0] = v->gate_open[1] = 1;
        pthread_mutex_unlock(&s->lock);
        return NSBC_OK;
    }
    /* 持 shard 锁读：与 release 的 free(relay) 互斥；原子槽由媒体线程
     * 并发写，OAM 低频读不阻塞转发路径（relay_cb 不持本锁）。 */
    for (int i = 0; i < 2; ++i) {
        v->gate_open[i] =
            __atomic_load_n(&r->d_latched[i], __ATOMIC_ACQUIRE);
        uint32_t sin =
            __atomic_load_n(&r->d_in_ssrc[i], __ATOMIC_ACQUIRE);
        if (sin) {
            v->ssrc_in[i] = sin;
            v->have_ssrc_in[i] = 1;
        }
        uint32_t sout =
            __atomic_load_n(&r->d_out_ssrc[i], __ATOMIC_ACQUIRE);
        if (sout) {
            v->ssrc_out[i] = sout;
            v->have_ssrc_out[i] = 1;
        }
        if (__atomic_load_n(&r->d_have_rr[i], __ATOMIC_ACQUIRE)) {
            v->have_rr[i] = 1;
            v->rr_fraction[i] = (uint8_t)__atomic_load_n(
                &r->d_rr_frac[i], __ATOMIC_ACQUIRE);
            v->rr_lost[i] =
                __atomic_load_n(&r->d_rr_lost[i], __ATOMIC_ACQUIRE);
            v->rr_jitter[i] =
                __atomic_load_n(&r->d_rr_jitter[i], __ATOMIC_ACQUIRE);
        }
        /* ★ REQ-VQ-001：RTP seq-gap 本地丢包推断——按 SSRC 分槽累加
         * 之前单槽 d_rtp_expected/d_rtp_lost 被 audio+video 交替污染成假数据。 */
        {
            uint32_t te = 0, tl = 0;
            for (int s = 0; s < SEQ_SLOTS; ++s) {
                if (r->seq[i][s].primed) {
                    te += __atomic_load_n(&r->seq[i][s].expected, __ATOMIC_ACQUIRE);
                    tl += __atomic_load_n(&r->seq[i][s].lost,     __ATOMIC_ACQUIRE);
                }
            }
            v->rtp_expected[i] = te;
            v->rtp_lost[i]     = tl;
        }
        /* REQ-MEDIA-SEC-003⑤ P0：CNAME 绑定 + 安全计数。
         * d_have_cname 序列计数器：0=未写，≥1=写完且值为 len+1。
         * char 数组非原子，OAM 低频读 double-check 防 torn read（媒体线程
         * 写：先清 0→memcpy→store len+1；读者：load→memcpy→reload 若一致则
         * 安全，否则最多重试 4 次放弃）。 */
        for (int retry = 0; retry < 4; ++retry) {
            int seq = __atomic_load_n(&r->d_have_cname[i],
                                      __ATOMIC_ACQUIRE);
            if (seq <= 0)
                break;
            size_t len = (size_t)(seq - 1);
            if (len >= sizeof(v->cname[i]))
                len = sizeof(v->cname[i]) - 1;
            memcpy(v->cname[i], r->d_cname[i], len);
            v->cname[i][len] = '\0';
            if (__atomic_load_n(&r->d_have_cname[i],
                                __ATOMIC_ACQUIRE) == seq) {
                v->have_cname[i] = 1;
                break;
            }
        }
        v->ssrc_inject_drops[i] = ssrc_policy_inject_drops(&r->pol[i]);
        v->ssrc_collision_drops[i] =
            ssrc_policy_collision_drops(&r->pol[i]);
    }
    pthread_mutex_unlock(&s->lock);
    return NSBC_OK;
}

/* ---- OAM 活跃会话遍历 ---- */

int bridge_foreach_call(bridge_mgr_t *m, bridge_iter_cb cb, void *ud)
{
    int si, rc = NSBC_OK;
    if (!m || !cb) return NSBC_INVAL;
    for (si = 0; si < BR_SHARDS && rc == NSBC_OK; ++si) {
        pthread_mutex_lock(&m->sh[si].lock);
        cnode_t *e = m->sh[si].head;
        while (e && rc == NSBC_OK) {
            rtp_relay_t *r = e->relay;
            uint16_t a_rtp = 0, b_rtp = 0;
            uint64_t pkts = 0, drops = 0;
            if (r) {
                /* relay->rtp_port[0]=朝A(fd_rtp[0])、[1]=朝B(fd_rtp[1])——
                 * 与 relay_start 的 ports->a_rtp=r0/b_rtp=r1 同口径 */
                a_rtp = r->rtp_port[0];
                b_rtp = r->rtp_port[1];
                pkts  = r->pkts;
                drops = r->drops;
            }
            rc = cb(e->call, e->mode, a_rtp, b_rtp, pkts, drops, ud);
            e = e->next;
        }
        pthread_mutex_unlock(&m->sh[si].lock);
    }
    return rc;
}

int bridge_active_count(bridge_mgr_t *m)
{
    int si, n = 0;
    if (!m) return 0;
    for (si = 0; si < BR_SHARDS; ++si) {
        pthread_mutex_lock(&m->sh[si].lock);
        cnode_t *e = m->sh[si].head;
        while (e) { ++n; e = e->next; }
        pthread_mutex_unlock(&m->sh[si].lock);
    }
    return n;
}

/* ---------- Phase 3 Playback Engine 桥接 ---------- */

int bridge_lookup_relay(bridge_mgr_t *m, void *call_key,
                        rtp_relay_t **out_relay, nsbc_sa_t out_peer[2])
{
    if (!m || !call_key || !out_relay || !out_peer)
        return NSBC_INVAL;
    *out_relay = NULL;
    memset(out_peer, 0, 2 * sizeof(nsbc_sa_t));

    br_shard_t *s = &m->sh[ptr_shard(call_key)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = s->head;
    while (e && e->call != call_key)
        e = e->next;
    if (!e || !e->relay) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_NOENT;
    }
    *out_relay = e->relay;
    /* 输出 peer：优先已 latch 的真实对端地址（入向学习到的），
     * 未 latch 则用 SDP offer 时记录的静态 peer。 */
    out_peer[0] = e->relay->latched[0] ? e->relay->latch[0] : e->relay->peer[0];
    out_peer[1] = e->relay->latched[1] ? e->relay->latch[1] : e->relay->peer[1];
    pthread_mutex_unlock(&s->lock);
    return NSBC_OK;
}

int rtp_relay_fd(const rtp_relay_t *r, int side)
{
    if (!r || side < 0 || side > 1) return -1;
    return r->fd_rtp[side];
}

int rtp_relay_latched(const rtp_relay_t *r, int side)
{
    if (!r || side < 0 || side > 1) return 0;
    return r->latched[side];
}

nsbc_sa_t rtp_relay_latch_addr(const rtp_relay_t *r, int side)
{
    nsbc_sa_t empty; memset(&empty, 0, sizeof(empty));
    if (!r || side < 0 || side > 1 || !r->latched[side]) return empty;
    return r->latch[side];
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
