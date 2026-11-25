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
#include "srtp_session.h" /*  HA P3-3: nsbc_srtp_session_get_material */

/*  media_bridge.h 里 MB_SRTP_MATERIAL_MAX 和 srtp_session.h 里 SRTP_KEY_MAX
 * 必须一致 — 前者是 bridge_call_view 里的本地常量（避免头文件污染），
 * 后者是 nsbc_srtp_material_t.key[] 数组大小。这里做编译期断言防漂移。 */
_Static_assert(MB_SRTP_MATERIAL_MAX == SRTP_KEY_MAX,
               "MB_SRTP_MATERIAL_MAX must equal SRTP_KEY_MAX");
#include "media_codec.h"   /* codec registry + transcode engine */
#include "video_xcode.h"   /* 视频转码会话（VP9↔H264） */
#include "rtp_reasm.h"     /* M1: H.264 RTP 重组器（视频录制前置） */
#include "rtp_video.h"     /* §3.4 snapshot：AU 关键帧判定 */
#include "media_video.h"   /* P0: video_build_pli — RTCP PLI 关键帧请求 */
#include "recorder.h"      /* M1: rec_feed_video —— 视频帧喂录音 sess */
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

/* M1 视频帧 hook 的回调上下文 —— vid_reasm / rec_xcode / tap 回调用 */
typedef struct vid_hook_ud {
    struct cnode *node;
    int           side;
    codec_id_t    cid;    /* tap 实例：源编码（写 BUG_VIDEO_AU 用） */
} vid_hook_ud_t;

typedef struct cnode {
    struct cnode *next;
    void *call; /* 不透明呼叫键（b2b_call_t*），仅比较/哈希，禁止解引用 */
    rtp_relay_t *relay;
    mb_mode_t mode;
    char a_ip[64];
    uint16_t a_port; /* A 的 SDP 媒体地址（peer A） */
    /*  per-call NAT 公网 RTP IP（REQ-NAT-002）——offer_hook 里从 profile 回调查一次，
     * 缓存到这里，answer_hook 直接复用。空=关闭 NAT（用 local_ip 做 SDP rewrite fallback）。 */
    char external_rtp_ip[64];
    /*  端点 WebRTC 标志——从原始 offer SDP 媒体层分析得出，
     * 独立于 dtls[]（dtls[] 在 Phase C SRTP_RELAY 模式下双侧都创建，
     * 不再能区分端点类型）。offer_hook 里设置，answer_hook 读取。 */
    uint8_t a_ws;  /* A 侧端点是否 WebRTC（有 DTLS-SRTP SDP） */
    uint8_t b_ws;  /* B 侧端点是否 WebRTC */
    /*  A 原始 offer 的 m= 段数 —— answer_hook 按 RFC 3264 把 answer
     * 截断到相同段数（offer_hook 为 WebRTC B 追加的 rejected video
     * 会被浏览器回显回来，多出的段不能发给只 offer 了 audio 的 A）。 */
    uint8_t a_mlcount;
    /*  每腿独立 SDP 终结（第 1 项）：
     *  a_offer_sdp 保存 A 的原始 offer，answer_hook 据此重建"回给 A 的 answer"，
     *  只宣告 SBC 与 A 真正协商的音频 codec；neg_* 是定型后的双向音频对，
     *  bridge_confirm 直接采用，避免再按启发式重新推导导致两腿不一致。
     *  neg_* 在 offer/answer hook 中更新，媒体线程只读。 */
    char              *a_offer_sdp;
    size_t             a_offer_len;
    /*  B 发起 re-INVITE 的对称终结：b_offer_sdp 保存 B 的原始 offer，
     *  answer_rev_hook 据此重建"回给 B 的 answer"（只宣告 B 侧最终 codec），
     *  neg_* 语义不变（side0=A / side1=B），bridge_confirm 直接采用。 */
    char              *b_offer_sdp;
    size_t             b_offer_len;
    /*  B 的原始 answer（SBC 发出 offer、B 应答）。leg=1 的 fmtp/解码参数
     *  （如 H.264 sprop-parameter-sets）从这里取；bridge_answer_hook 每次
     *  收到原始 200 OK 覆盖并置 b_answer_fresh，bridge_confirm 消费一次后
     *  清 0，release 释放。 */
    char              *b_answer_sdp;
    size_t             b_answer_len;
    uint8_t            b_answer_fresh; /* 1=b_answer_sdp 是本轮 answer hook 的原始 SDP */
    uint8_t            rev_pending;  /* 1=已收 B 的 offer，等待 A 的 answer 回 B */
    uint8_t            neg_valid;   /* 1=音频 codec 已终结定型 */
    uint8_t            neg_a_pt;    /* 朝 A（side 0）的音频 PT */
    uint8_t            neg_b_pt;    /* 朝 B（side 1）的音频 PT */
    codec_id_t         neg_a_cid;
    codec_id_t         neg_b_cid;
    uint8_t            neg_a_te;    /* A 侧 telephone-event PT（0=未协商） */
    uint8_t            neg_b_te;    /* B 侧 telephone-event PT（0=未协商） */
    /* REQ-DTLS-001：DTLS-SRTP 终结器，按 relay 物理侧索引：
     * Phase C 后 SRTP_RELAY 模式双侧都创建（relay 做 DTLS demux），
     * 与端点类型无关。明文侧（BYPASS/双明文）恒 NULL。 */
    void *dtls[2];
    /*  G.711 转码会话（REQ-TRANS-004 P0）—— per-direction:
     * transcode[side] 处理 "从 side 来的 RTP → 转码后发往 other"
     * 例：side=0（A→B）transcode 把 A 腿发的 PCMU 转成 PCMA 给 B 腿。
     * MB_TRANSCODE 模式下由 bridge_confirm 创建，bridge_release_call 释放。 */
    transcode_sess_t *transcode[2];
    codec_id_t         src_codec[2]; /* 每侧实际发的 codec（运行时动态识别） */
    codec_id_t         dst_codec[2]; /* 对侧期望的 codec（从 answer SDP 提取） */
    uint8_t            pt_allowed[2][8]; /* 每侧允许通过的 PT（从 answer SDP fmt-list） */
    int                pt_allowed_count[2];

    /* 逐腿音频 codec 快照：side 0=offer(A)/1=answer(B)。
     * 运行时用 audio_pt[side] 校验入包 PT 是否属于该腿音频 codec，
     * 保证视频 PT / 未知 PT 永不进入音频转码器。 */
    uint8_t            audio_pt[2][8];
    codec_id_t         audio_cid[2][8];
    int                audio_n[2];
    /* 每腿 payload map（§2.2）：名字做主键、PT 做索引，含旁路载荷
     * （telephone-event/CN/red/rtx）。bridge_confirm 构建后媒体线程只读；
     * 用于零转码 relay 判定（含 fmtp 兼容）与旁路载荷 PT 重写。 */
    leg_payload_t      pmap[2][8];
    int                pmap_n[2];
    /* 音频转码计划：入包 PT（源腿）/ 出包 PT（目标腿协商值）。
     * xcode_dst_pt 取代 default_pt 猜测，保证多 PT 协商时输出正确。 */
    uint8_t            xcode_src_pt[2];
    uint8_t            xcode_dst_pt[2];
    int                xcode_sig[2];   /* 计划签名，re-INVITE 变更检测 */
    /* 被 re-INVITE 替换的旧会话：媒体线程可能仍在读，延迟到 release 释放 */
    transcode_sess_t  *xcode_retired[8];
    int                n_xcode_retired;

    uint8_t            video_pt_allowed[2][8];
    int                video_pt_allowed_count[2];
    /*  抓图/录制入包白名单：保留该腿 m-line 的**完整**视频 PT 集合，不随逐腿
     *  终结收窄。浏览器常按自己的 PT 发视频（与 SBC 选定的 PT 不同），若这里
     *  也用收窄后的单 PT 白名单，该腿抓图/录制会永远收不到帧 → 只有单边图。 */
    uint8_t            vid_feed_pt[2][8];
    int                vid_feed_pt_n[2];
    /*  视频逐腿终结（H264/H265/VP8/VP9 互转，REQ-TRANS-VIDEO-002）：
     *  neg_v_* 由 answer hook 写入，bridge_confirm 据此创建 video_xcode。
     *  两条腿 codec 不同 → 转码；相同但 PT 不同 → 仅改 RTP 头 PT。 */
    uint8_t            neg_v_valid;
    uint8_t            neg_v_a_pt;
    uint8_t            neg_v_b_pt;
    codec_id_t         neg_v_a_cid;
    codec_id_t         neg_v_b_cid;
    video_xcode_t     *vxcode[2];       /* side 发来的视频 → 转码后发对侧 */
    int                vxcode_sig[2];
    uint8_t            vxcode_src_pt[2];
    uint8_t            vxcode_dst_pt[2];
    video_xcode_t     *vxcode_retired[8];
    int                n_vxcode_retired;
    uint8_t            video_rewrite_active[2];
    uint8_t            video_rewrite_src_pt[2];
    uint8_t            video_rewrite_dst_pt[2];
    /* M1 视频录制：H.264 RTP 重组器（bridge_confirm 检测到 H.264 时创建，
     * bridge_release_call 销毁）。side=0 = A 腿方向（录制 A→B 视频）。 */
    vid_reasm_t       *vid_reasm[2];
    vid_hook_ud_t      vid_ud[2];
    /*  M2 视频录制：VP8/VP9/H.265 腿的“录制定向转码”（src=该腿 codec → H.264）。
     *  重编码 AU 经 au sink 直接喂 rec_feed_video，输出仍是 .h264，
     *  由 stop 时的 ffmpeg 后处理合成 mp4。与 vid_reasm 按腿二选一。 */
    video_xcode_t     *rec_xcode[2];
    int                rec_xcode_sig[2];
    uint8_t            rec_xcode_pt[2];
    vid_hook_ud_t      rec_ud[2];
    /*  观察实例（tap）：非 H.264 且没有通话转码时也建立（同编码可直通），
     *  只解包不改写媒体流，源编码 AU 直接投递给 media_bug 供抓图使用。 */
    video_xcode_t     *tap_xcode[2];
    int                tap_xcode_sig[2];
    uint8_t            tap_xcode_pt[2];
    vid_hook_ud_t      tap_ud[2];
    /*  PT rewrite：同 codec 不同 PT（如 WebRTC G729 PT=96 → SIP G729 PT=18）
     * 只改 RTP header 的 PT 字节，不做 PCM 往返转码。比 transcode 快得多。 */
    struct { uint8_t src_pt; uint8_t dst_pt; } pt_rewrite[2];
    int pt_rewrite_active[2];
    /*  单腿本地媒体（AI 内循环）：无 B 腿，bridge_send_raw 一律发回 A 侧 */
    uint8_t local_media;
    /*  2026-10-16 诊断：transcode_seen 从 static __thread 改 per-call，
     * 避免长寿命媒体线程跨呼叫累积导致短通话（~400 包）永远碰不到 %1000 阈值。 */
    int transcode_seen_diag[2];
    /*  诊断：入包 audio PT 不在该腿协商集合（信令与媒体 PT 不一致）。
     *  典型：信令协商 PCMA(8)，浏览器实际发 PCMU(0)。只告警不丢包。 */
    int audio_badpt_diag[2];
    /*  RFC 3312 Precondition 前提条件门控——offer_hook 解析 offer SDP 置位
     * precond_active（1=Require precondition 或 a=des:qos mandatory），
     * answer_hook 解析 answer SDP 置位 precond_met（1=a=curr 覆盖所有 mandatory des）。
     * relay_recv_one 转发前检查：precond_active && !precond_met → 丢包。 */
    uint8_t precond_active;
    uint8_t precond_met;
    /*  DTMF RFC4733 — 每侧实际协商的 telephone-event 载荷 PT（0=未协商）。
     * bridge_confirm 从 answer SDP 的 rtpmap 里提取 enc="telephone-event" 时填入。
     * relay_recv_one 里 pkt.pt == dtmf_pt[side] 时走 DTMF 专用分支：
     *   rtp_parse_dtmf → dtmf_rx_on_rfc4733 → ivr_on_dtmf(call->ivr_sess, digit)
     * 转发 continue（避免对端也收到 RFC4733 包重复解析）。 */
    uint8_t   dtmf_pt[2];     /* 每侧 telephone-event PT；0=未协商 */
    void     *dtmf_rx[2];    /* dtmf_rx_t* 每侧去重器；0=未创建 */
    /*  re-INVITE media_update 方向门控 — bridge_confirm 每次被调用时
     * 从新 SDP 解析填入，relay_recv_one 转发前读取：
     *   dir[X]=SENDONLY(3)/SENDRECV(2) → X 能发 RTP → 允许
     *   dir[X]=RECVONLY(4)/INACTIVE(5) → X 不能发/hold → 丢包
     * legacy hold (c=0.0.0.0) 在 bridge_confirm 里强制 INACTIVE */
    sdp_dir_t dir[2];
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
    unsigned refs; /* P1-2: 原子引用计数（__atomic_* 访问），不再依赖每包加锁 */
    struct mb_rec_hook *next;
} mb_rec_hook_t;

static __thread unsigned rec_hook_dispatch_depth;

struct bridge_mgr {
    mb_cfg_t cfg;
    br_shard_t sh[BR_SHARDS];
    pthread_mutex_t rec_hook_lock;  /* 保护下方 hook 链表增删 */
    pthread_cond_t rec_hook_cond;   /* 等待已摘除 hook 的在途回调结束 */
    mb_rec_hook_t *rec_hooks;       /* AI dialog / 未来 transcript 等可选订阅 */
    /* P1-2：正在等待注销的线程数。为 0 时热路径完全不碰 cond/lock，
     * 仅在确有等待者且引用计数归零时才加锁 broadcast。 */
    int rec_hook_waiters;
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

/*  REQ-VQ-001：RTP seq 跟踪按 SSRC 分槽
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
    /*  明文腿的独立视频 RTP/RTCP 通道（每 m-line 一个端口）。
     *  WebRTC 腿一条 BUNDLE 传输共用 fd_rtp/fd_rtcp，这里恒 -1。
     *  原因：audio/video 两个 m-line 若同端口，对端会自行把 video 挪到
     *  port+2，正好落到对侧 socket 上被当成 SRTP 解密而丢弃。 */
    int fd_vrtp[2];
    int fd_vrtcp[2];
    uint16_t vrtp_port[2];
    nsbc_sa_t peer[2];  /* 配置期望对端（可空：纯 latching） */
    int have_peer[2];
    nsbc_sa_t latch[2];
    int latched[2];
    /*  明文腿视频流的独立 peer/latch。audio/video 是两个独立 m-line 端口，
     *  共用一个 latch 会让视频源端口覆盖音频目标端口（反之亦然）——
     *  表现为"音频发到视频端口/视频发到音频端口" → 单向无声、黑屏。 */
    nsbc_sa_t vlatch[2];
    int vlatched[2];
    /*  视频 RTCP 源（明文腿）：NAT 后 RTCP 端口与 SDP 的 RTP+1 无关，
     *  只能从实际到达 fd_vrtcp 的 RTCP/STUN 源学习（实测 Linphone
     *  SDP 52633 被 NAT 映射成 38160）。RTP 目标单独存 vlatched，
     *  两者互不污染：把 RTP 发到 RTCP 端口同样会整段丢包。 */
    nsbc_sa_t vrlatch[2];
    int vrlatched[2];
    nsbc_sa_t vpeer[2];       /* SDP video m-line 的地址（NAT 前） */
    int vhave_peer[2];
    /*  视频 NAT 回退（明文腿）：SDP 视频地址是私网（服务器不可达）时，
     *  用音频 comedia latch 的公网 IP + SDP 视频端口作为发送目标。
     *  媒体线程单线程写，无需加锁。 */
    nsbc_sa_t vnat_fb[2];
    int vnat_fb_logged[2];
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
    /*  REQ-MEDIA-SSRC-004：mixed-mode（一侧 WebRTC、一侧明文）时，发往
     *  WebRTC 腿的音频 SSRC 在 relay_start 预分配并固定，answer SDP 里
     *  用 a=ssrc 宣告同一个值。此前 SSRC 由 smap 在收到首包时才随机分配，
     *  answer 阶段无从得知 → 无法宣告 a=ssrc（JSEP/SKILL §4.2）。[source side] */
    uint32_t plan_out_ssrc[2];
    int d_have_rr[2];
    uint32_t d_rr_frac[2], d_rr_lost[2], d_rr_jitter[2];
    /* RTP seq-gap 本地丢包计数（按 SSRC 分槽，避免多 SSRC 交替污染） */
    seq_track_t seq[2][SEQ_SLOTS];
    uint32_t d_rtp_jitter_ticks[2]; /* RTP 时钟 tick 原 jitter（未换算 ms，仅 RTCP 旁证） */
    /* REQ-MEDIA-SEC-003⑤ P0：SSRC↔CNAME 绑定（SDES CNAME）；
     * 媒体线程写，relay 侧 [0]朝A/[1]朝B。 */
    char d_cname[2][64];
    int d_have_cname[2];
    /*  RACE-FIX: arm 标志。rtp_relay_start 只 open fd + alloc port，
     * 延迟到所有初始化完成后 rtp_relay_arm 才 reactor_add。
     * rtp_relay_stop 里 arm=0 时跳过 reactor_del（fd 还没注册）。 */
    int armed;
    /*  P0-2 RACE-FIX: rtp_relay_stop 置 dead=1，relay_recv_one 循环每次检查
     * 并立即退出。解决 "reactor_call del fd 返回后，已进入 relay_recv_one 的
     * for(;;) 循环还在内核队列剩余数据包 → bridge_release_call free(r) 时
     * relay_recv_one 还在访问 r→node → UAF"。ARMED 置 0 在 reactor_call 之前。 */
    _Atomic int dead;
    /* REQ-ORIGINATE-003 &echo 实现：RTP 环回 mode。
     * 置 1 后 relay_recv_one 把收到的包发回同一侧（side→side），
     * 而非正常的 side→other。两侧各自环回自己的 peer。 */
    int echo_mode;
    /* P2-18：原函数级 static 的诊断计数 / PLI 限频，改为呼叫私有。
     * relay 在 relay_recv_one 内单线程访问，无需额外锁。 */
    int diag_any_seen[4];      /* 2 side × (rtp + rtcp) */
    uint64_t last_pli_ms[2];
    /*  本侧入向视频流的原始 SSRC（重写前的值）。
     *  用途：对端发来的 PLI 里的 media SSRC 是 SBC 出向后重写的值，
     *  原样转发给源端会被忽略（源端不认识该 SSRC）→ 收不到关键帧。
     *  这里记录原始 SSRC，收到 PLI 时用它重新生成一条发给源端。 */
    uint32_t vid_ssrc[2];
    /* §3.4 snapshot：控制线程置位，媒体线程在下一包视频时发 PLI。
     * 避免控制线程直接调用 SRTP/socket（与媒体线程共享 session 非线程安全）。 */
    uint8_t  pli_req[2];
    /*  2026-11-22：SENDTO / SRTP 失败诊断改 per-call 计数（原来用
     *  _Thread_local 前 5 条，长寿命媒体线程第二通电话就永远打不出日志，
     *  导致无法判断"音频到底转发到哪"。 */
    uint32_t diag_sendto[2];   /* 每侧已打印的 SENDTO 条数 */
    uint32_t diag_srtp_fail[2];/* 每侧 SRTP unprotect 失败计数 */
    uint32_t diag_vrx[2];      /* 每侧已打印的 VIDEO_RX 条数 */
    uint32_t diag_vrx_nox[2];  /* 视频 PT 到了但没有 vxcode 的计数 */
    uint32_t diag_vsniff[2];   /* 视频载荷指纹采样次数（每侧前 3 包） */
    uint32_t diag_vkf[2];      /* 视频流起始主动请求关键帧的次数 */
    /*  视频 seq 缺口 → 代接收端向源腿补发 PLI（见 relay_recv_one 的
     *  VIDEO_GAP 分支）。根因：Linphone 这类 SIP 终端基本不发 PLI/FIR
     *  （现场抓包 A 侧视频 RTCP 只有 SR，RTCP_FB=0），WebRTC 源端又只在
     *  收到 PLI 时才重出 IDR。中继链路一旦丢包，接收端参考帧链永久断裂，
     *  画面就"前段正常、之后定格"。每侧独立跟踪，SSRC 变化即重新同步，
     *  补发限频 2 秒（PLI 会让源端整帧重编，过密会拉高码率）。 */
    uint32_t vgap_ssrc[2];
    uint16_t vgap_last_seq[2];
    uint8_t  vgap_primed[2];
    uint64_t vgap_pli_ms[2];
    uint32_t diag_vgap[2];     /* 缺口触发 PLI 的次数（诊断限频） */
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

        /*  REQ-MEDIA-VIDEO-003 / REQ-MEDIA-AUDIO-003：m=audio / m=video
         * <port> <proto> <pt1> <pt2> ... 提取所有 payload type 进对应白名单。
         * 让后续到达的全新 SSRC（传统 SIP 不带 a=ssrc:）也能通过 policy。
         * 音频必须有这张表：G.729 带 DTX 时音频首包可能晚于视频首包，
         * 只有 video 白名单会导致"视频先占住第一个源 → 音频 SSRC 被永久
         * 丢弃 → 对端听不到本端语音"。 */
        if (llen >= 8 &&
            (strncmp(p, "m=video ", 8) == 0 ||
             strncmp(p, "m=audio ", 8) == 0)) {
            const int is_vid_line = (p[2] == 'v');
            const char *tok = p + 8;  /* 跳过 "m=video " / "m=audio " */
            const char *line_end_ptr = p + llen;
            /* skip port token */
            while (tok < line_end_ptr && *tok != ' ') ++tok;
            while (tok < line_end_ptr && *tok == ' ') ++tok;
            /* skip proto token */
            while (tok < line_end_ptr && *tok != ' ') ++tok;
            while (tok < line_end_ptr && *tok == ' ') ++tok;
            /* fmt-list：逐个提取 PT 数字 */
            char *tail = NULL;
            while (tok < line_end_ptr) {
                unsigned long v = strtoul(tok, &tail, 10);
                if (tail == tok) break;  /* 不再是数字，fmt-list 结束 */
                if (v < 256) {
                    if (is_vid_line)
                        ssrc_policy_set_video_pt(pol, (uint8_t)v);
                    else
                        ssrc_policy_set_audio_pt(pol, (uint8_t)v);
                }
                tok = tail;
                while (tok < line_end_ptr && *tok == ' ') ++tok;
            }
        }

        if (!line_end) break;
        p = line_end + 1;
    }
    /* 每次 SDP 一条 INFO，便于现场确认 audio/video 白名单是否解析到位：
     * Linphone 明文腿 admitted=0（无 a=ssrc:），全靠 PT 表放行音频/视频源。 */
    llog(LOG_INFO, "media",
         "admit_ssrc_from_sdp side=%d admitted=%d audio_n=%d video_n=%d "
         "aux_n=%d max_sources=%d",
         side, admitted, pol->cfg.n_audio_pt, pol->cfg.n_video_pt,
         pol->cfg.n_aux_pt, pol->cfg.max_sources);
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

/*  RFC 5761 §4 rtcp-mux 解复用（纯 RTP/RTCP 包判别，不含 STUN/DTLS）：
 *
 * RTP：byte[1] = M(1) | PT(7)，PT∈0-127 → byte[1] 可以是 0-127（M=0）
 *       或 128-255（M=1, PT≥0）
 * RTCP：byte[1] = PT(8)，PT∈192-223（RFC 5760）
 *
 * 冲突区：RTP(M=1) 且 PT∈64-95 时 byte[1] 会落进 192-223，与 RTCP 撞车
 *         （本项目 SDP 不会协商 64-95 的 PT，实际不会发生）
 *
 * 两级验证：
 *   L1. compound 精确匹配：所有 RTCP 子包 length 正好铺满 buffer → RTCP
 *   L2. 弱验证回退（L1 失败时）：第一个子包 PT ∈[192,223] 且 length 合理
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
    /*  RTCP PT 合法区间是 192..223（RFC 5760）。
     * 不能再用 "byte[1] >= 200 就当 RTCP"：RTP 的 byte[1] = (Marker<<7)|PT，
     * 视频 PT 96 的每帧末包是 0xE0(224)、动态音频 PT 101 的末包是 0xE5(229)，
     * 旧判定会把它们当 RTCP → 走 SRTCP 加/解密（只保留前 8 字节、SSRC 被加密
     * 成随机值）→ 对端 RTP 头损坏、整帧缺末包无法解码。
     * 实测：Linphone→WebRTC 方向每帧末包被误判，Chrome 一直黑屏刷 PLI。 */
    if (pt_byte < 192 || pt_byte > 223)
        return 0;   /* 224..255 = RTP Marker+PT≥96，明确不是 RTCP */

    /*  L1：精确 compound 匹配 */
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

    /*  L2：弱验证回退。
     * 第一个子包必须 PT ∈[192,223]（上面已保证），
     * length 字段解析后子包不溢出 buffer → 判 RTCP。
     * 真 RTP 包的 bytes[2-3] 是 sequence number（通常 > 1000），
     * 解析成 sub_len 后会 > len → 溢出 → L2 也失败 → RTP。
     * 注意：视频流首包的 seq 也可能很小（实测从 0 开始），
     * 所以入口的 224-255 排除（M=1,PT≥96）才是硬门禁，L2 只做兜底。 */
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

/* M1+ 视频帧回调 —— vid_reasm 重组出完整 H.264 access unit 后调用。
 * side=0 → call->rec[0]（A 腿音频录音 sess）
 * side=1 → call->rec[1]（B 腿音频录音 sess）
 * 两条腿都挂 video=1（recording.c start 时设置）；无 rec_sess（纯视频通话无录音）则跳过。 */
static void bridge_video_frame_cb(void *ud, const uint8_t *frame,
                                  size_t len, uint32_t ts)
{
    vid_hook_ud_t *u = (vid_hook_ud_t *)ud;
    if (u == NULL || u->node == NULL || !b2b_call_is_alive(u->node->call))
        return;
    b2b_call_t *call = (b2b_call_t *)u->node->call;
    int leg = u->side;  /* side=0 → leg A, side=1 → leg B */
    if (leg < 0 || leg > 1) return;
    rec_sess_t *sess = (rec_sess_t *)call->rec[leg];
    if (sess != NULL)
        rec_feed_video(sess, frame, len, ts);

    /* §3.4 snapshot：把完整 AU 投递给 BUG_VIDEO_AU 消费者。
     * 仅在有消费者时才解析关键帧标志，避免无谓开销。 */
    if (media_bug_video_active()) {
        int key = rtp_video_is_keyframe(RTP_PAYLOAD_H264, frame, (size_t)len);
        uint32_t vssrc = __atomic_load_n(&u->node->relay->vid_ssrc[leg],
                                         __ATOMIC_RELAXED);
        media_bug_dispatch_video_au(call, leg, frame, len, CODEC_H264, key, ts,
                                    vssrc);
    }
}

/* M2 视频录制 —— VP8/VP9/H.265 腿经 rec_xcode 转成 H.264 后的 AU 回调。
 * 与 bridge_video_frame_cb 等价：按 side 取对应腿的录音 sess 喂帧。 */
static void bridge_rec_au_cb(void *ud, const uint8_t *au, size_t len,
                             uint32_t ts, int key)
{
    (void)key;   /* rec_feed_video 不需要关键帧标志；首帧已由 xcode 强制 IDR */
    vid_hook_ud_t *u = (vid_hook_ud_t *)ud;
    if (u == NULL || u->node == NULL || !b2b_call_is_alive(u->node->call))
        return;
    b2b_call_t *call = (b2b_call_t *)u->node->call;
    int leg = u->side;
    if (leg < 0 || leg > 1) return;
    /* rec stop 用 __atomic_exchange(NULL) 摘 sess —— 这里必须原子读取 */
    rec_sess_t *sess = (rec_sess_t *)__atomic_load_n(&call->rec[leg],
                                                     __ATOMIC_ACQUIRE);
    if (sess != NULL)
        rec_feed_video(sess, au, len, ts);
}

/* 观察实例（tap）的 AU 回调：交付源编码完整帧（H.264/H.265 为 Annex-B，
 * VP8/VP9 为裸帧），供 BUG_VIDEO_AU 消费者（抓图）使用。
 * 录像需要 H.264 落盘，仍由 vid_reasm / rec_xcode 负责。 */
static void bridge_tap_au_cb(void *ud, const uint8_t *au, size_t len,
                             uint32_t ts, int key)
{
    vid_hook_ud_t *u = (vid_hook_ud_t *)ud;
    if (u == NULL || u->node == NULL || !b2b_call_is_alive(u->node->call))
        return;
    if (!media_bug_video_active()) return;   /* 无抓图消费者：零开销 */
    int leg = u->side;
    if (leg < 0 || leg > 1 || au == NULL || len == 0) return;
    uint32_t vssrc = 0;
    if (u->node->relay != NULL)
        vssrc = __atomic_load_n(&u->node->relay->vid_ssrc[leg],
                                __ATOMIC_RELAXED);
    media_bug_dispatch_video_au(u->node->call, leg, au, len, u->cid, key, ts,
                                vssrc);
}

/* 从原始 SDP 提取指定 PT 的 a=fmtp 值（去掉前缀 "a=fmtp:<pt> "）。
 * 找不到返回 0；buf 始终 '\0' 结尾。 */
static int sdp_fmtp_for_pt(const char *sdp, size_t len, int pt,
                           char *buf, size_t cap)
{
    size_t i;
    if (buf == NULL || cap == 0) return 0;
    buf[0] = '\0';
    if (sdp == NULL) return 0;
    for (i = 0; i + 7 < len; ++i) {
        const char *p;
        int v = 0, digits = 0;
        size_t n = 0;
        if (memcmp(sdp + i, "a=fmtp:", 7) != 0) continue;
        p = sdp + i + 7;
        while (p < sdp + len && *p >= '0' && *p <= '9' && digits < 3) {
            v = v * 10 + (*p - '0');
            ++p;
            ++digits;
        }
        if (digits == 0 || v != pt || p >= sdp + len || *p != ' ') continue;
        ++p;
        while (p < sdp + len && *p != '\r' && *p != '\n' && n + 1 < cap)
            buf[n++] = *p++;
        buf[n] = '\0';
        return 1;
    }
    return 0;
}

/* PT → codec_id：注册表优先（含动态 PT 的名字映射结果），
 * 静态 PT 仅作为数据表兜底，不再写死在协商逻辑里。 */
static const struct {
    int        pt;
    codec_id_t cid;
} k_static_audio_pt[] = {
    { 0,  CODEC_PCMU },
    { 8,  CODEC_PCMA },
    { 9,  CODEC_G722 },
    { 13, CODEC_CN   },
    { 18, CODEC_G729 },
};

static codec_id_t pt_to_codec(int pt)
{
    const codec_ops_t *ops = codec_find_by_pt_kind(pt, CODEC_KIND_AUDIO);
    if (ops) return ops->id;
    for (size_t i = 0; i < NSBC_ARRAY_SIZE(k_static_audio_pt); ++i) {
        if (k_static_audio_pt[i].pt == pt)
            return k_static_audio_pt[i].cid;
    }
    return CODEC_NONE;
}

/* 入包 PT → 该腿音频 codec_id（仅 audio_pt/audio_cid 快照命中才返回非 NONE）*/
static codec_id_t leg_audio_pt_to_cid(const cnode_t *e, int side, int pt)
{
    int i;
    if (e == NULL || side < 0 || side > 1) return CODEC_NONE;
    for (i = 0; i < e->audio_n[side]; ++i) {
        if (e->audio_pt[side][i] == (uint8_t)pt)
            return e->audio_cid[side][i];
    }
    return CODEC_NONE;
}

/* 对端是否接受该 codec（按 codec 身份，而非 PT 数字——
 * 两腿 PT 空间独立，动态 PT 相同不代表同一 codec）。 */
static int peer_accepts_audio_cid(const cnode_t *e, int side, int pt)
{
    codec_id_t cid;
    int other = 1 - side, i;
    if (e == NULL) return 0;
    cid = leg_audio_pt_to_cid(e, side, pt);
    if (cid == CODEC_NONE) return 0;
    for (i = 0; i < e->audio_n[other]; ++i) {
        if (e->audio_cid[other][i] == cid) return 1;
    }
    return 0;
}

/* 入包 PT 在本腿 payload map 中的条目（NULL=map 未建立或该 PT 未协商）*/
static const leg_payload_t *leg_map_entry(const cnode_t *e, int side, int pt)
{
    if (e == NULL || side < 0 || side > 1 || e->pmap_n[side] <= 0)
        return NULL;
    return codec_payload_map_find(e->pmap[side], e->pmap_n[side], pt);
}

/* 对端腿能否按原 payload 零转码接收该入包 PT。
 * 有 payload map 时按「同 cid + fmtp 兼容」判断，否则退回 cid 列表。 */
static int peer_accepts_payload(const cnode_t *e, int side, int pt)
{
    const leg_payload_t *a;
    int other = side ^ 1, i;
    if (e == NULL) return 0;
    a = leg_map_entry(e, side, pt);
    if (a == NULL) return peer_accepts_audio_cid(e, side, pt);
    if (a->is_event) {
        for (i = 0; i < e->pmap_n[other]; ++i) {
            if (e->pmap[other][i].is_event &&
                e->pmap[other][i].cid == a->cid)
                return 1;
        }
        return 0;
    }
    for (i = 0; i < e->pmap_n[other]; ++i) {
        if (codec_payload_map_can_relay(a, &e->pmap[other][i]))
            return 1;
    }
    return 0;
}

/* 旁路载荷（CN/red/rtx）在目标腿的 PT；0xFF=对端未协商同 cid 载荷 */
static uint8_t peer_event_pt(const cnode_t *e, int side, int pt)
{
    const leg_payload_t *a;
    int other = side ^ 1, i;
    if (e == NULL) return 0xFF;
    a = leg_map_entry(e, side, pt);
    if (a == NULL || !a->is_event) return 0xFF;
    for (i = 0; i < e->pmap_n[other]; ++i) {
        if (e->pmap[other][i].is_event && e->pmap[other][i].cid == a->cid)
            return e->pmap[other][i].pt;
    }
    return 0xFF;
}

/* ---------- relay_recv_one ---------- */

static int mb_ipv4_is_private(const char *ip);  /* 定义见文件后部 */

/*  明文腿视频 RTP 的发送目标选择（含 NAT 回退）。
 *  优先级：comedia 视频 latch（真实源）> SDP 视频 peer > 音频 latch。
 *  NAT 回退：SDP 视频地址是私网（服务器不可达）时，把 IP 换成音频
 *  comedia latch 学到的公网 IP，端口仍用 SDP 视频端口——不能直接把视频
 *  发到音频 latch 的端口，否则对端会把视频 RTP 当成音频流丢弃。
 *  注意：对端若从未从视频 socket 发过包（Linphone a=recvonly / 未开视频），
 *  NAT 上不存在该端口的映射，任何地址都无法命中，必须由对端开启视频。 */
static const nsbc_sa_t *relay_video_dst_sel(rtp_relay_t *r, int side)
{
    if (r == NULL || side < 0 || side > 1) return NULL;
    if (r->fd_vrtp[side] < 0) {
        /* WebRTC 腿：音视频共用 BUNDLE 地址，走 audio latch/peer */
        return r->latched[side] ? &r->latch[side]
                                : (r->have_peer[side] ? &r->peer[side]
                                                      : &r->latch[side]);
    }
    if (r->vlatched[side]) return &r->vlatch[side];
    if (r->vhave_peer[side]) {
        if (r->latched[side] &&
            r->vpeer[side].ss.ss_family == AF_INET &&
            r->latch[side].ss.ss_family == AF_INET) {
            char ipa[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET,
                      &((const struct sockaddr_in *)&r->vpeer[side].ss)->sin_addr,
                      ipa, sizeof(ipa));
            if (mb_ipv4_is_private(ipa)) {
                r->vnat_fb[side] = r->latch[side];
                ((struct sockaddr_in *)&r->vnat_fb[side].ss)->sin_port =
                    ((const struct sockaddr_in *)&r->vpeer[side].ss)->sin_port;
                if (!r->vnat_fb_logged[side]) {
                    r->vnat_fb_logged[side] = 1;
                    llog(LOG_WARN, "relay",
                         "VIDEO_NAT_FALLBACK side=%d SDP 视频地址 %s 为私网 → "
                         "改用音频 latch 公网 IP + 视频端口 %u",
                         side, ipa,
                         (unsigned)ntohs(((struct sockaddr_in *)
                                          &r->vnat_fb[side].ss)->sin_port));
                }
                return &r->vnat_fb[side];
            }
        }
        return &r->vpeer[side];
    }
    return &r->latch[side];
}

/* 视频转码输出回调上下文：把 video_xcode 产出的 RTP 包发往对侧 */
typedef struct {
    rtp_relay_t *r;
    int          side;
    int          other;
} relay_video_ctx_t;

static void relay_video_emit(void *ud, const uint8_t *pkt, size_t len)
{
    relay_video_ctx_t *c = (relay_video_ctx_t *)ud;
    if (!c || !c->r || !pkt || len == 0) return;
    rtp_relay_t *r = c->r;

    /*  目标腿若拥有独立视频 socket，必须使用视频 latch/peer + 视频 fd；
     *  WebRTC 腿视频与音频共用地址，回退到 audio latch/fd。
     *  NAT 回退（SDP 视频私网 → 音频 latch 公网 IP）在 relay_video_dst_sel 内。 */

    const nsbc_sa_t *dst = relay_video_dst_sel(r, c->other);
    if (dst == NULL || dst->ss.ss_family == AF_UNSPEC || dst->len == 0) return;

    uint8_t sbuf[RELAY_BUF];
    if (len + 128 > sizeof(sbuf)) return;
    memcpy(sbuf, pkt, len);
    size_t slen = len;
    if (r->srtp[c->other] && r->node && r->node->dtls[c->other]) {
        if (nsbc_srtp_protect_rtp(r->srtp[c->other], sbuf, &slen,
                                  sizeof(sbuf)) != NSBC_OK)
            return;
    }
    /*  明文腿有独立视频 socket 时从该端口发出（源端口与对端发送目标一致，
     *  避免严格 symmetric-RTP 的 UA 丢弃源端口不匹配的视频）。 */
    int ofd = (r->fd_vrtp[c->other] >= 0) ? r->fd_vrtp[c->other]
                                          : r->fd_rtp[c->other];
    /*  视频转码输出（transcode），不是透传中继。复用 diag_vrx 计数限频：
     *  前 5 帧 + 每 100 帧一条 NOTICE，避免刷屏又能在短通话里看到。 */
    if (r->diag_vrx[c->side] <= 5 || r->diag_vrx[c->side] % 100 == 0) {
        llog(LOG_NOTICE, "relay",
             " SENDTO side=%d→%d pt=%u vid=1 path=transcode len=%zu（视频转码后转发）",
             c->side, c->other, (unsigned)(sbuf[1] & 0x7F), slen);
    }
    (void)sendto(ofd, sbuf, slen, MSG_NOSIGNAL,
                 (const struct sockaddr *)&dst->ss, dst->len);
}

/*  视频 RTCP（PLI/FIR/SR/RR）的发送目标与 fd。
 *  - 明文腿（有独立视频 socket）：优先 comedia 从 fd_vrtcp 学到的真实
 *    RTCP 源——NAT 后 RTCP 端口与 SDP 的 RTP+1 完全无关（实测 Linphone
 *    SDP 52633 被映射成 38160）；未学到时按 RFC 3550 用 SDP 视频地址 +1，
 *    并从 fd_vrtcp 发出（源端口必须是对端看到的 RTCP 端口）。
 *  - WebRTC BUNDLE 腿：RTCP 与 RTP 共用 socket，走 audio latch + fd_rtp。
 *  返回 0 成功；-1 无可用目标。 */
static int relay_vrtcp_dst(rtp_relay_t *r, int side, nsbc_sa_t *out,
                           int *out_fd)
{
    if (!r || !out || !out_fd || side < 0 || side > 1)
        return -1;
    if (r->fd_vrtp[side] >= 0) {
        if (r->vrlatched[side]) {
            *out = r->vrlatch[side];
        } else {
            *out = r->vlatched[side]
                       ? r->vlatch[side]
                       : (r->vhave_peer[side] ? r->vpeer[side] : r->latch[side]);
            if (out->ss.ss_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&out->ss;
                sin->sin_port = htons((uint16_t)(ntohs(sin->sin_port) + 1));
            } else if (out->ss.ss_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&out->ss;
                sin6->sin6_port = htons((uint16_t)(ntohs(sin6->sin6_port) + 1));
            }
        }
        *out_fd = r->fd_vrtcp[side];
    } else {
        *out = r->latched[side]
                   ? r->latch[side]
                   : (r->have_peer[side] ? r->peer[side] : r->latch[side]);
        *out_fd = r->fd_rtp[side];
    }
    if (out->ss.ss_family == AF_UNSPEC || out->len == 0)
        return -1;
    return 0;
}

/* 视频转码缺少关键帧时，向源侧请求 PLI（500ms 限频，与录制路径共享计时） */
static void relay_video_request_pli(rtp_relay_t *r, int side, uint32_t media_ssrc)
{
    uint64_t now = nsbc_now_ms();
    if (now - r->last_pli_ms[side] < 500) return;
    r->last_pli_ms[side] = now;

    uint8_t pli_buf[64];
    size_t pli_len = 0;
    const uint32_t pli_src_ssrc = 0x4E534243;  /* "NSBC" */
    if (video_build_pli(media_ssrc, pli_src_ssrc, pli_buf, sizeof(pli_buf),
                        &pli_len) != NSBC_OK)
        return;

    uint8_t pli_send[64];
    size_t pli_send_len = pli_len;
    memcpy(pli_send, pli_buf, pli_len);
    if (r->srtp[side])
        nsbc_srtp_protect_rtcp(r->srtp[side], pli_send, &pli_send_len,
                               sizeof(pli_send));

    nsbc_sa_t dst;
    int ofd = -1;
    if (relay_vrtcp_dst(r, side, &dst, &ofd) != 0) return;
    (void)sendto(ofd, pli_send, pli_send_len, MSG_NOSIGNAL,
                 (const struct sockaddr *)&dst.ss, dst.len);
}

/*  REQ-MEDIA-SSRC-004：mixed-mode（明文腿 → WebRTC 腿）转发 RTCP 时，
 *  把**发送方 SSRC**改写成与该方向 RTP 头一致的出向 SSRC。
 *  否则 RTP 用 plan_out_ssrc[side]，而 SR/SDES 仍带明文腿原始 SSRC，
 *  Chrome 按 SR 建立/维护接收流时对不上 SSRC → inbound-rtp 恒为 0
 *  （RFC 3550 §6.4：SR 用于接收端关联发送者；JSEP §5.9）。
 *
 *  必须在 SRTCP 加密之前调用：认证标签覆盖改写后的整包。
 *  报告块内的 source SSRC 与 FCI 一律不动（那是远端源语义）。
 *  from=入向主源 SSRC，vid=本腿视频 SSRC（视频通道另走 map，不套用固定值）。 */
static void relay_rtcp_rewrite_ssrc(uint8_t *buf, size_t len, uint32_t to,
                                    uint32_t from, uint32_t vid)
{
    if (!buf || from == 0 || to == 0 || from == to || from == vid || len < 8)
        return;
    size_t pos = 0;
    while (pos + 4 <= len) {
        uint8_t pt = buf[pos + 1];
        size_t sub_len = ((size_t)((buf[pos + 2] << 8) | buf[pos + 3]) + 1) * 4;
        if (pos + sub_len > len)
            break;  /* 结构非法（padding 等）：停止，不做猜测性改写 */
        switch (pt) {
        case 200: /* SR */
        case 201: /* RR */
        case 205: /* RTPFB（NACK/PLI 的发送方 SSRC） */
        case 206: /* PSFB */
            {
                uint32_t s = 0;
                memcpy(&s, buf + pos + 4, 4);
                if (ntohl(s) == from) {
                    uint32_t v = htonl(to);
                    memcpy(buf + pos + 4, &v, 4);
                }
            }
            break;
        case 202: /* SDES：只改第一个 chunk 的 SSRC（每腿单一媒体源） */
            {
                uint32_t s = 0;
                memcpy(&s, buf + pos + 4, 4);
                if (ntohl(s) == from) {
                    uint32_t v = htonl(to);
                    memcpy(buf + pos + 4, &v, 4);
                }
            }
            break;
        case 203: /* BYE：SSRC 列表 */
            {
                int cnt = buf[pos] & 0x1F;
                if (cnt > 31) cnt = 31;
                for (int i = 0; i < cnt; ++i) {
                    size_t off = pos + 4 + (size_t)i * 4;
                    if (off + 4 > pos + sub_len) break;
                    uint32_t s = 0;
                    memcpy(&s, buf + off, 4);
                    if (ntohl(s) == from) {
                        uint32_t v = htonl(to);
                        memcpy(buf + off, &v, 4);
                    }
                }
            }
            break;
        default:
            break;
        }
        pos += sub_len;
    }
}

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

    /* DIAG 2026-10-15：无差别方向计数——两侧各前 5 次 RECV 都打
     * SIP 侧（side=0）是明文 RTP srtp_lane=0，之前 DIAG ENTRY 只打 srtp_lane=1
     * 导致 SIP 侧零 RX 时整条 DIAG 静默，根本不知道是不是收不到包 */
    int slot = side * 2 + is_rtcp;
    /* P2-18：per-relay 计数（原函数级 static 全呼叫共享） */
    if (r->diag_any_seen[slot] < 5) {
        llog(LOG_DEBUG, "relay",
             "RECV side=%d fd=%d is_rtcp=%d srtp_lane=%d rtp_port=%u",
             side, fd, is_rtcp, srtp_lane, (unsigned)r->rtp_port[side]);
        ++r->diag_any_seen[slot];
    }
    int drain_budget = 64;   /* P2-8：单次回调最多处理 64 个报文，避免水平触发下饿死其它 fd */
    for (;;) {
        /*  P0-2 RACE-FIX: 每次循环开头检查 dead flag。rtp_relay_stop
         * 置 dead=1 后，即使 reactor_call 返回时本回调还在跑（内核队列
         * 有残留数据包），也会在这里立即退出，不会在 bridge_release_call
         * free(r/e) 后继续访问 r→node 等成员。 */
        if (__atomic_load_n(&r->dead, __ATOMIC_ACQUIRE))
            return;
        /*  P2-8：用尽预算即返回；未读尽的数据仍在 socket 队列，epoll 会再次触发本回调。 */
        if (--drain_budget < 0)
            return;
        nsbc_sa_t src;
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        ssize_t n;

        if (srtp_lane) {
            /*  P1-4：DTLS 握手完成后 BIO 不再从 fd 读取，所有报文都可由本函数
             * 消费并分派，无需 MSG_PEEK + 二次 recvfrom（每包系统调用减半）。
             * 握手未完成时仍需 PEEK：OpenSSL<1.1.1 无 BIO 注入通道，DTLS record
             * 必须留在内核队列给 BIO_dgram 自读；STUN 则必须我们消费并立即应答
             * （BIO 会把它当垃圾丢弃，浏览器 ICE check 无应答 = 480 根因）。 */
            int established = ds && dtls_server_state(ds) == DTLS_ST_ESTABLISHED;
            ssize_t pn;
            if (established) {
                pn = recvfrom(fd, buf, sizeof(buf), 0,
                              (struct sockaddr *)&ss, &sl);
                if (pn <= 0)
                    return;
                n = pn;
                src.ss = ss;
                src.len = sl;
            } else {
                pn = recvfrom(fd, buf, sizeof(buf), MSG_PEEK,
                              (struct sockaddr *)&ss, &sl);
                if (pn <= 0)
                    return;
            }
            int cls = rx_classify(buf, (size_t)pn);
            if (cls == 1) { /* STUN：消费 + Binding Response（ICE consent） */
                if (!established) {
                    n = recvfrom(fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&ss, &sl);
                    if (n <= 0)
                        return;
                    src.ss = ss;
                    src.len = sl;
                }
                /*  REQ-ICE-LATCH-001：用 STUN 源地址建立 latch。
                 * WebRTC 复用同一 UDP socket 收发 STUN/DTLS/RTP
                 * （RFC 8445 §14.1 强制要求）—— 收到 STUN 的源地址就是
                 * 对端收 RTP 的地址。之前只在收 RTP 时 latch，导致
                 * "对端只发 STUN 不发 RTP（track 未启用等）" 时 latched 永为 0，
                 * 对侧 RTP 全被 "other latch not ready" 丢弃。
                 *  同 RTP latch：清 have_peer 避免旧 SDP 地址导致 mismatch */
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
                if (established) {
                    /* P1-4：已建立后的残留 record 已随单次 recvfrom 消费，直接丢弃 */
                    continue;
                }
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
            /* DIAG 2026-10-13：DTLS ESTABLISHED 后 WebRTC fd 流量探测
             * 若 Chrome 不发 SRTP 回来（单通根因），此处 cls 永远不会到 3 */
            if (srtp_lane && ds &&
                dtls_server_state(ds) == DTLS_ST_ESTABLISHED) {
                /* P2-1：诊断计数改为线程局部，避免多 reactor 无锁竞争 */
                static _Thread_local int diag_post_dtls_seen[2] = {0};
                if (diag_post_dtls_seen[side] < 5) {
                    llog(LOG_DEBUG, "relay",
                         "DIAG POST-DTLS-RX side=%d fd=%d cls=%d pn=%zd "
                         "srtp_sess=%p",
                         side, fd, cls, pn, (void *)r->srtp[side]);
                    ++diag_post_dtls_seen[side];
                }
            }
            if (established) {
                /* P1-4：报文已在上面单次 recvfrom 收好，直接进入 RTP 处理 */
                goto packet_ready;
            }
        }

        n = recvfrom(fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&ss, &sl);
        if (n <= 0)
            return;
        src.ss = ss;
        src.len = sl;
packet_ready:

        /*  明文腿收到 STUN：必须在 comedia latch / media_bug / rtp_parse
         * 之前消费掉。现场（2026-11-24）：Linphone 尽管 SDP 里没有任何
         * ICE 属性，仍每 500ms 往 RTP/RTCP 端口各发一个 Binding Request。
         * 旧路径把它当明文 RTP 处理：
         *   1) 先进入 latch 学习 —— 若 STUN 源端口与真实 RTP 源端口不同，
         *      出向媒体会被改发到该端口（对端整段收不到）；
         *   2) 被 media_bug_dispatch_rtp_raw 当作 RTP 投递；
         *   3) rtp_parse 版本校验失败后计入 drops，污染丢包统计。
         * 本腿没有协商 ICE，SBC 也不参与该端点的连通性检查，静默丢弃即可
         * （回 Binding Error 反而会让侧端 ICE 状态机误判连通性）。 */
        if (!srtp_lane && rx_classify(buf, (size_t)n) == 1) {
            static _Thread_local int diag_plain_stun[2] = {0};
            if (diag_plain_stun[side] < 3 || diag_plain_stun[side] % 200 == 0) {
                llog(LOG_DEBUG, "relay",
                     "STUN rx on plaintext leg side=%d fd=%d is_rtcp=%d n=%zd "
                     "→ ignored (no ICE on this leg)",
                     side, fd, is_rtcp, n);
            }
            ++diag_plain_stun[side];
            continue;
        }

        if (srtp_lane && r->srtp[side]) {
            /* SRTP → 解密后继续明文 relay 路径。
             * RTP/RTCP 判别见文件头 is_rtcp_packet() 注释（含 M-bit 冲突处理）。 */
            int pkt_rtcp = is_rtcp_packet(buf, (size_t)n);
            size_t slen = (size_t)n;
            int rc = pkt_rtcp
                ? nsbc_srtp_unprotect_rtcp(r->srtp[side], buf, &slen)
                : nsbc_srtp_unprotect_rtp(r->srtp[side], buf, &slen);
            if (rc != NSBC_OK) {
                /*  REQ-MEDIA-SEC-001：RTCP unprotect 失败静默 drop
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
                /*  2026-11-22：加上来源地址/PT/seq/ssrc，并限流。
                 *  之前一条 FAIL 只有 side，无法判断失败包到底来自哪一端
                 *  （A 的明文 RTP 打到 B 的 SRTP socket 时，日志与"B 发坏包"
                 *   完全无法区分）；1286 条同样的 WARN 也把现场淹掉了。 */
                {
                    char fipa[INET6_ADDRSTRLEN] = {0};
                    uint16_t fport = 0;
                    if (src.ss.ss_family == AF_INET) {
                        inet_ntop(AF_INET, &((struct sockaddr_in *)&src)->sin_addr,
                                  fipa, sizeof(fipa));
                        fport = ntohs(((struct sockaddr_in *)&src)->sin_port);
                    } else if (src.ss.ss_family == AF_INET6) {
                        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&src)->sin6_addr,
                                  fipa, sizeof(fipa));
                        fport = ntohs(((struct sockaddr_in6 *)&src)->sin6_port);
                    }
                    uint16_t fseq = 0;
                    uint32_t fssrc = 0;
                    if (n >= 12) {
                        memcpy(&fseq, buf + 2, 2);
                        fseq = ntohs(fseq);
                        memcpy(&fssrc, buf + 8, 4);
                        fssrc = ntohl(fssrc);
                    }
                    if (r->diag_srtp_fail[side] < 5 ||
                        r->diag_srtp_fail[side] % 200 == 0) {
                        llog(LOG_WARN, "relay", "SRTP RTP unprotect FAIL side=%d rc=%d "
                             "n=%zd pt=%u seq=%u ssrc=0x%08x from=%s:%u first16=[%s]",
                             side, rc, n, (unsigned)(n >= 2 ? (buf[1] & 0x7F) : 0),
                             (unsigned)fseq, fssrc, fipa, (unsigned)fport, hex);
                    }
                    ++r->diag_srtp_fail[side];
                }
                ++r->drops;
                continue;
            }
            n = (ssize_t)slen;
            is_rtcp = pkt_rtcp;
            /*  关键 INFO 日志：确认 unprotect 真的在跑。
             * 之前是 DEBUG 级别，混在海量 TC-GUARD 里看不到。 */

        }

        /*  M1 视频录制 hook —— SRTP unprotect 之后、latch/转发门控之前。
         * 此处 buf 已是明文 RTP：先取 PT 字节做零开销预筛（非 video PT 直接过），
         * 命中该腿完整视频 PT 集合（vid_feed_pt）才 rtp_parse + 喂 H.264 重组器。
         * latch 未就绪 / precondition / direction gate 均不影响录制路径。 */
        if (!is_rtcp && n >= 2 && r->node != NULL &&
            (r->node->vid_reasm[side] != NULL ||
             r->node->rec_xcode[side] != NULL ||
             r->node->tap_xcode[side] != NULL)) {
            uint8_t vpt = (uint8_t)(buf[1] & 0x7F);
            /*  用完整视频 PT 集合（vid_feed_pt）而非转发面收窄后的白名单：
             *  浏览器/第三方终端常按自己的 PT 发视频，收窄白名单会把该腿视频
             *  全部挡在抓图/录制之外，表现为“只有单边图”。 */
            if (pt_in_list(vpt, r->node->vid_feed_pt[side],
                           r->node->vid_feed_pt_n[side])) {
                rtp_pkt_t pkt;
                if (rtp_parse(buf, (size_t)n, &pkt) == NSBC_OK &&
                    pkt.version == 2) {
                    /* H.264 腿：vid_reasm 重组 Annex-B → rec_feed_video */
                    if (r->node->vid_reasm[side] != NULL) {
                        vid_reasm_feed(r->node->vid_reasm[side],
                                       pkt.payload, pkt.payload_len,
                                       pkt.marker, pkt.seq, pkt.ts,
                                       nsbc_now_ms());

                        /*  P0：vid_reasm 检测到 seq gap / frame_drop → 请求 RTCP PLI
                         *   Chrome SVC WebRTC 只发初始 IDR，丢包后参考链永久断裂 → 花屏。
                         *   发 PLI 请求新关键帧，Chrome 收到后立即发新 IDR → 参考链重置。
                         *   限频：同 side 间隔 ≥ 2 秒才发（避免淹没 RTCP 通道）。 */
                        if (vid_reasm_consume_pli_request(r->node->vid_reasm[side])) {
                            /* P2-18：per-relay 限频（原函数级 static 使所有呼叫共用计时） */
                            uint64_t now = nsbc_now_ms();
                            if (now - r->last_pli_ms[side] >= 2000) {
                                r->last_pli_ms[side] = now;
                                uint8_t pli_buf[64];
                                size_t pli_len = 0;
                                /*  Sender SSRC 不能填 0 —— Chrome 会忽略 Sender SSRC=0 的 PLI。
                                 * 用固定魔法值 0x4E534243 ("NSBC") 作为 nsbcd 作为 SFU 的 RTCP SSRC。 */
                                const uint32_t pli_src_ssrc = 0x4E534243;
                                if (video_build_pli(pkt.ssrc, pli_src_ssrc, pli_buf, sizeof(pli_buf), &pli_len) == NSBC_OK) {
                                    uint8_t pli_send[64];
                                    size_t pli_send_len = pli_len;
                                    memcpy(pli_send, pli_buf, pli_len);
                                    if (r->srtp[side]) {
                                        nsbc_srtp_protect_rtcp(r->srtp[side], pli_send, &pli_send_len, sizeof(pli_send));
                                    }
                                    /* 关键帧请求同样必须走视频 RTCP 端口（见 relay_vrtcp_dst） */
                                    nsbc_sa_t kf_dst;
                                    int kf_fd = -1;
                                    if (relay_vrtcp_dst(r, side, &kf_dst, &kf_fd) == 0) {
                                        ssize_t pret = sendto(kf_fd, pli_send, pli_send_len, MSG_NOSIGNAL,
                                                              (const struct sockaddr *)&kf_dst.ss, kf_dst.len);
                                        llog(LOG_INFO, "relay",
                                             " PLI_SENT side=%d src_ssrc=0x%08x media_ssrc=0x%08x send_len=%zu ret=%zd",
                                             side, pli_src_ssrc, pkt.ssrc, pli_send_len, pret);
                                    }
                                }
                            }
                        }
                    }

                    /*  VP8/VP9/H.265 腿：rec_xcode 定向转码到 H.264；
                     *  重编码 AU 经 au sink（bridge_rec_au_cb）直接落录音，
                     *  因此这里 emit 传 NULL —— 不产出 RTP 包，不影响转发面。 */
                    video_xcode_t *rx = __atomic_load_n(
                        &r->node->rec_xcode[side], __ATOMIC_ACQUIRE);
                    if (rx != NULL) {
                        (void)video_xcode_input(rx, pkt.payload, pkt.payload_len,
                                                pkt.marker, pkt.ts, NULL, NULL);
                        if (video_xcode_take_pli(rx))
                            relay_video_request_pli(r, side, pkt.ssrc);
                    }

                    /*  观察实例：源编码 AU → media_bug（抓图）。只解包，
                     *  不改写转发面；与 rec_xcode 并存时各自独立解包。 */
                    video_xcode_t *tx = __atomic_load_n(
                        &r->node->tap_xcode[side], __ATOMIC_ACQUIRE);
                    if (tx != NULL) {
                        (void)video_xcode_input(tx, pkt.payload, pkt.payload_len,
                                                pkt.marker, pkt.ts, NULL, NULL);
                        if (video_xcode_take_pli(tx)) {
                            /* 与 vid_reasm 共用 2s 限频，避免关键帧等待期刷 PLI */
                            uint64_t tnow = nsbc_now_ms();
                            if (tnow - r->last_pli_ms[side] >= 2000) {
                                r->last_pli_ms[side] = tnow;
                                relay_video_request_pli(r, side, pkt.ssrc);
                            }
                        }
                    }
                }
            }
        }
        /*  REQ-MEDIA-SEC-005b：srtp_lane 但 srtp 会话还没建立 → 直接 drop。
         * 之前注释说"握手未完的早到媒体当明文处理"——但浏览器发的是加密 SRTP，
         * 不是明文 RTP！RTP/SRTP 头部结构相同（12B + PT/SSRC），rtp_parse() 会恰好
         * 把密文解析成"合法 RTP"，转发到对端 → 对端解不开 → 全丢。
         * DTLS record（cls=2）和 STUN（cls=1）已在上文 PEEK 分支被消费，
         * 能走到这里的 cls==3 只有 SRTP——必须等 DTLS 握手完成才能解密转发。
         *
         *  P0 关键诊断：DTLS ESTABLISHED 前 Chrome 会一直发 SRTP 过来，
         * 但 dtls_server 状态机卡住时所有这些包都被 drop 形成"单向不通"。
         * 这里前 10 次 drop 明确告诉用户 dtls_server 当前 state +
         * 等待时长，grep "SRTP-DROP-BEFORE-DTLS" 即可定位。 */
        if (srtp_lane && !r->srtp[side]) {
            /* P2-1：诊断计数/起点改为线程局部 */
            static _Thread_local int diag_drop_before_dtls[2] = {0};
            static _Thread_local struct timespec diag_dtls_start[2] = {0};
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
                     "SRTP-DROP-BEFORE-DTLS side=%d drops=%d dtls_state=%d "
                     "srtp_sess=NULL wait=%.0fms (Chrome 以为 DTLS connected "
                     "但 SBC dtls_server 没 ESTABLISHED — 检查 DTLS ClientHello "
                     "是否到达 dtls fd=%d)",
                     side, diag_drop_before_dtls[side]++, dtls_st, wait_ms,
                     fd);
            } else if (diag_drop_before_dtls[side] == 10) {
                llog(LOG_ERR, "relay",
                     " FATAL side=%d srtp_lane=%d DTLS never ESTABLISHED "
                     "after %d drops — WebRTC↔SIP 全不通。grep 'DTLS' in logs",
                     side, srtp_lane, diag_drop_before_dtls[side]);
                ++diag_drop_before_dtls[side];  /* 停打 */
            }
            continue;
        }

        /*  本包是否来自视频专用 socket（RTP 或 RTCP）。
         *  必须把 fd_vrtcp 也算进来：否则视频 RTCP 的源端口会被当作
         *  audio comedia 学习，把 audio 的发送目标改到 A 的视频端口。 */
        const int vid_fd = (fd == r->fd_vrtp[side] || fd == r->fd_vrtcp[side]);
        /*  视频包校验视频 peer，音频包校验音频 peer —— 两套地址互不干扰。 */
        const nsbc_sa_t *cur_peer = vid_fd ? &r->vpeer[side] : &r->peer[side];
        int *cur_have = vid_fd ? &r->vhave_peer[side] : &r->have_peer[side];
        if (*cur_have && !nsbc_sa_eq_addr(&src, cur_peer)) {
            /*  P0 修复：第一次 peer mismatch drop 后立即清 have_peer。
             * 原代码：have_peer[side] 是 SDP 预设的旧地址，NAT 后分机真实源不同，
             * 每次收到都触发 mismatch drop，永远走不到下面的 comedia 学习。
             * 清 have_peer 后，后续包跳过 mismatch 检查直接进入 comedia 学习。 */
            /* P2-1：诊断计数改为线程局部 */
            static _Thread_local int diag_mismatch[4] = {0};  /* 2 side × (rtp+rtcp) */
            int slot = side * 2 + is_rtcp;
            if (diag_mismatch[slot] < 5) {
                char ipa[INET6_ADDRSTRLEN] = {0}, ppa[INET6_ADDRSTRLEN] = {0};
                uint16_t sp = 0, pp = 0;
                if (src.ss.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in *)&src)->sin_addr,
                              ipa, sizeof(ipa));
                    sp = ntohs(((struct sockaddr_in *)&src)->sin_port);
                }
                if (cur_peer->ss.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in *)cur_peer)->sin_addr,
                              ppa, sizeof(ppa));
                    pp = ntohs(((struct sockaddr_in *)cur_peer)->sin_port);
                }
                llog(LOG_WARN, "relay",
                     "PEER-MISMATCH side=%d vid=%d drop#%d have_peer=1 "
                     "src=%s:%u peer(SDP)=%s:%u → CLEAR have_peer, "
                     "letting next pkt comedia-learn",
                     side, vid_fd, diag_mismatch[slot]++, ipa, sp, ppa, pp);
            }
            *cur_have = 0;  /*  立即清！让下次包走 comedia */
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
         *  P0 关键修复：comedia 学习到真实源地址后，必须清除 have_peer[side]！
         * 根因：answer_hook/offer_hook 会 set_peer_a/set_peer_b 把 have_peer[side]=1
         * 设为 SDP 里的 c=/m= 地址。但 NAT 后分机的真实 RTP 源完全不同。
         * 旧代码在 comedia 学习后只更新 latch[side]，但 have_peer[side] 保持为旧地址，
         * 导致 line 545 的 peer addr mismatch 检查：
         *   if (have_peer[side] && !sa_eq(src, peer[side])) → 每次收到 NAT 后的 RTP 都 drop！
         * 清除 have_peer 后，peer addr mismatch 检查恒为假，comedia 学到的真实
         * latch[side] 就成为唯一可信的源地址。 */
        /*  明文腿视频走独立 socket：迟到/首包学习必须与 audio 分开，
         *  否则视频源端口会覆盖 audio 的目标端口。
         *  且只从视频 RTP 学习：视频 RTCP 的源端口是 video+1，
         *  用它当发送目标会把 RTP 发错端口。
         *  ★ 音频同理（2026-11-22 现场日志）：对端 audio RTCP 源端口是
         *  RTP+1（实测出向语音被打到 139.226.66.211:13193，而 RTP 端口是
         *  13192）。一旦用 RTCP 源更新 audio latch，接下来所有出向语音都会
         *  发到对端的 RTCP 端口被整段丢弃——表现为"能听到但对端声音断续/
         *  偏小"。音频 latch 只允许 RTP 学习。 */
        if (vid_fd) {
            /*  视频 RTCP 源单独学习：PLI/SR/RR 要回送明文腿的 RTCP 端口，
             *  不能复用 RTP 的 vlatch（那是 RTP 目标），也不能反过来用
             *  RTCP 源覆盖 vlatch。 */
            if (is_rtcp && fd == r->fd_vrtcp[side] &&
                (!r->vrlatched[side] ||
                 !nsbc_sa_eq(&src, &r->vrlatch[side]))) {
                r->vrlatch[side] = src;
                r->vrlatched[side] = 1;
            }
            if (!is_rtcp &&
                (!r->vlatched[side] || !nsbc_sa_eq(&src, &r->vlatch[side]))) {
                r->vlatch[side] = src;
                r->vlatched[side] = 1;
                /*  视频目标地址变化（首次 comedia 学习，或端口漂移）：
                 *  之前发到旧地址（SDP peer 或旧端口）的视频包——尤其是
                 *  关键帧——很可能已丢失。linphone 又从不发 PLI/FIR
                 *  （现场日志 RTCP_FB=0），只能干等 Chrome 周期关键帧，
                 *  实测要 50s 才出图。这里主动向视频源腿请求关键帧，
                 *  把出图时间压到 1~2s。
                 *  vid_ssrc[other] 可能尚未记录（源腿还没发过视频），
                 *  此时 vssrc=0 直接跳过，安全。 */
                uint32_t vssrc =
                    __atomic_load_n(&r->vid_ssrc[side ^ 1], __ATOMIC_RELAXED);
                if (vssrc) {
                    llog(LOG_NOTICE, "relay",
                         " VIDEO_LATCH_CHANGE side=%d → 请求源腿关键帧 ssrc=0x%08x",
                         side, vssrc);
                    relay_video_request_pli(r, side ^ 1, vssrc);
                }
                llog(LOG_DEBUG, "relay",
                     " LATCH video side=%d latched[0]=%d latched[1]=%d",
                     side, r->vlatched[0], r->vlatched[1]);
            }
        } else if (!is_rtcp && !r->latched[side]) {
            /* 首次：锁定 latch，标志 relay "已就绪" */
            r->latch[side] = src;
            r->latched[side] = 1;
            r->have_peer[side] = 0;  /* comedia 覆盖 SDP 预设地址 */
            __atomic_store_n(&r->d_latched[side], 1, __ATOMIC_RELEASE);
            llog(LOG_DEBUG, "relay", " LATCH side=%d by RTP src_fam=%d latched[0]=%d latched[1]=%d",
                 side, src.ss.ss_family, r->latched[0], r->latched[1]);
        } else if (!is_rtcp && !nsbc_sa_eq(&src, &r->latch[side])) {
            /* comedia：真实 RTP 源变化（NAT 穿透）→ 覆盖 latch，不丢弃
             * 非对称 NAT：首次 RTP 即从 NAT 后真实地址到达 → 覆盖 SDP 预设私网地址
             * 对称 NAT：src 每次都变 — 此处会反复覆盖，发回最新 src（尽力而为）
             *  关键：变化也清 have_peer，避免 stale peer 地址导致 mismatch drop */
            llog(LOG_DEBUG, "relay", "comedia learn side=%d latch updated", side);
            r->latch[side] = src;
            r->have_peer[side] = 0;  /*  同样清除 */
        }

        int other = side ^ 1;
        /*  转发路径标记（诊断用）：
         *    transcode  = 做了编解码转换（音频 G.729↔PCMA、视频 VP8↔H264）
         *    rtp-relay  = 纯 RTP 中继/透传（含 PT rewrite——只改 RTP 头
         *                 PT 字节，不做 PCM/像素往返，仍算中继）。 */
        const char *path = "rtp-relay";
        /*  RTCP PLI/FIR 翻译：对端 PLI/FIR 里的 media SSRC 是 SBC 出向后
         *  重写过的值，视频源端不认识 → 原样转发拿不到关键帧，
         *  解码端只能等周期关键帧（表现为"视频第一帧很慢/久不出图"）。
         *  这里用本侧记录的真实源 SSRC 重新生成一条 PLI 发给源腿；
         *  原始 RTCP 仍照常转发，不改变既有行为。
         *  FMT=1 是 PLI、FMT=4 是 FIR（linphone 出图前常发 FIR），
         *  两者都统一翻译成 PLI——源端（Chrome/linphone）都认 PLI。 */
        if (is_rtcp && n >= 8) {
            size_t roff = 0;
            while (roff + 8 <= (size_t)n) {
                uint8_t rpt = buf[roff + 1];
                uint8_t rfmt = (uint8_t)(buf[roff] & 0x1Fu);
                size_t rlen = (((size_t)buf[roff + 2] << 8) |
                               (size_t)buf[roff + 3]) + 1;
                rlen *= 4;
                if (rpt == 206 && (rfmt == 1 || rfmt == 4)) { /* PSFB PLI/FIR */
                    /*  两侧 relay 回调可能在不同 reactor 线程，vid_ssrc
                     *  用原子读避免跨线程读到撕裂值。 */
                    uint32_t vssrc =
                        __atomic_load_n(&r->vid_ssrc[other], __ATOMIC_RELAXED);
                    llog(LOG_NOTICE, "relay",
                         " RTCP_FB side=%d→%d fmt=%u(%s) src_ssrc=0x%08x %s",
                         side, other, rfmt,
                         rfmt == 1 ? "PLI" : "FIR", vssrc,
                         vssrc ? "→ 重新生成 PLI 发给源腿"
                               : "（vid_ssrc 未记录，无法翻译）");
                    if (vssrc)
                        relay_video_request_pli(r, other, vssrc);
                    break;
                }
                if (rlen < 4 || roff + rlen > (size_t)n) break;
                roff += rlen;
            }
        }
        /* REQ-ORIGINATE-003 &echo：环回模式把包发回同一侧（side→side）。
         * 其余 latch 检查、peer 选择、sendto fd 下标全部复用 other 变量
         * —— 不引入新变量、不分支，零额外热路径开销。 */
        if (r->echo_mode)
            other = side;
        /* REQ-NAT-001 comedia：对侧是否就绪看 latched OR have_peer
         * （have_peer 来自 SDP，可公网直达；latched 来自 comedia 学习）。
         *  只有对侧也是明文腿（有独立视频 socket）时才查视频 latch/peer；
         *  WebRTC 腿没有视频通道，视频与音频共用地址，必须回退到 audio
         *  就绪状态，否则 A→B 视频会被永久判"对侧未就绪"丢弃。 */
        const int other_ready =
            (vid_fd && r->fd_vrtp[other] >= 0)
                ? (r->vlatched[other] || r->vhave_peer[other])
                : (r->latched[other] || r->have_peer[other]);
        if (!other_ready) {
            /* 对侧尚未就绪：丢弃早期突发 */
            if ((r->drops + r->pkts) % 100 == 0) {
                llog(LOG_DEBUG, "relay", " DROP other latch not ready "
                     "side=%d other=%d latched[0]=%d latched[1]=%d have_peer[0]=%d have_peer[1]=%d",
                     side, other,
                     r->latched[0], r->latched[1],
                     r->have_peer[0], r->have_peer[1]);
            }
            ++r->drops;
            continue;
        }

        /*  RFC 3312 Precondition 前提条件门控：
         * offer_hook 解析 offer SDP 置 precond_active=1，
         * answer_hook 解析 answer SDP 置 precond_met=1。
         * active 但未 met → 所有 RTP/RTCP 丢包（gate=LOCKED）。 */
        if (r->node && r->node->precond_active && !r->node->precond_met) {
            ++r->drops;
            continue;
        }

        /*  re-INVITE media_update 方向门控：
         * side X 发来的 RTP，只有 dir[X] 表明 X 可以发送时才放行。
         * SENDONLY(3)=只能发不能收 → 能发 RTP → 允许
         * SENDRECV(2)=收发均可 → 能发 → 允许
         * RECVONLY(4)=只能收不能发 → 不能发 RTP → 丢
         * INACTIVE(5)=hold → 丢 */
        if (r->node) {
            sdp_dir_t d = r->node->dir[side];
            if (d == SDP_DIR_INACTIVE || d == SDP_DIR_RECVONLY) {
                ++r->drops;
                continue;
            }
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
            static __thread int mb_cnt = 0;
            if (mb_cnt++ < 5 || mb_cnt % 1000 == 0)
                llog(LOG_DEBUG, "relay", "MEDIA_BUG_DISPATCH side=%d call=%p n=%zu",
                     side, r->node->call, (size_t)n);
            media_bug_dispatch_rtp_raw(r->node->call, side,
                buf, (size_t)n, s_ip, s_port, d_ip, d_port);
        } else {
            static __thread int mb_skip = 0;
            if (mb_skip++ < 5)
                llog(LOG_WARN, "relay", "MEDIA_BUG_SKIP is_rtcp=%d node=%p call=%p",
                     is_rtcp, (void*)r->node,
                     r->node ? (void*)r->node->call : NULL);
        }

        if (!is_rtcp) {
            rtp_pkt_t pkt;
            if (rtp_parse(buf, (size_t)n, &pkt) != NSBC_OK ||
                pkt.version != 2) {
                ++r->drops;
                continue;
            }
            /*  REQ-VQ-001：RTP seq-gap 丢包计数——按 SSRC 分槽
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
                /*  诊断日志：ssrc_policy 丢弃了这个 SSRC。
                 * 如果大量出现 pt=96-127（视频 PT 范围），说明 ssrc_policy 默认策略
                 * 只认第一个到达的 SSRC（音频），后到的视频 SSRC 全被 drop。 */
                static __thread int drop_log_burst = 0;
                if (drop_log_burst < 50 || ++drop_log_burst % 1000 == 0) {
                    llog(LOG_DEBUG, "relay",
                         "SSRC_DROP side=%d ssrc=%08x pt=%u total_drops=%u",
                         side, pkt.ssrc, (unsigned)pkt.pt, (unsigned)r->drops);
                }
                ++r->drops;
                /*  REQ-REC-009：被 ssrc_policy 丢弃的 G.711 音频包也要喂录音。
                 * 录音是旁路采集，不该被转发面门控截断（与视频 vid_reasm hook
                 * 放在门控之前同理）。WebRTC 断线重连/换设备会换 SSRC —— policy
                 * 未学习新源期间丢的包，之前直接导致"这边说话声没录上"。
                 * 只放行静态 G.711 PT（0/8）：rec_feed_raw 只解 G.711；
                 * 动态 PT 的解码依赖后级 transcode，被丢的包没有后级。
                 * 转发与录音互斥（这里喂完就 continue，不会双重写）。 */
                /*  2026-11-17 修复：SSRC_DROP 也要喂动态 rec_hooks 链表（ai_dialog）。
                 * 之前只喂了 cfg.rec_feed_fn 主槽，动态 hook 完全漏掉。 */
                if (r->m->cfg.rec_feed_fn && r->node && r->node->call &&
                    ((uint8_t)pkt.pt == 0 || (uint8_t)pkt.pt == 8)) {
                    r->m->cfg.rec_feed_fn(r->m->cfg.rec_feed_ud,
                                          r->node->call,
                                          pkt.payload, pkt.payload_len,
                                          side, (int)pkt.pt);
                }
                /*  动态 hooks（ai_dialog）：SSRC 被 drop 但 G.711 音频仍然喂 */
                if ((uint8_t)pkt.pt == 0 || (uint8_t)pkt.pt == 8) {
                    void *hhead = __atomic_load_n(&r->m->rec_hooks, __ATOMIC_ACQUIRE);
                    if (hhead) {
                        static __thread int dh_cnt = 0;
                        if (dh_cnt++ < 3)
                            llog(LOG_INFO, "relay", "SSRC_DROP_REC_HOOK m=%p hooks=%p side=%d pt=%u len=%zu",
                                 (void *)r->m, hhead, side, (unsigned)pkt.pt, pkt.payload_len);
                        mb_rec_hook_t *local[MB_REC_HOOK_MAX];
                        int nh = 0;
                        pthread_mutex_lock(&r->m->rec_hook_lock);
                        /* 必须在锁内重新读链表头：锁外的 hhead 只是“是否为空”的提示，
                         * 期间注销方可能已摘链并 free 节点，直接遍历 hhead 会 UAF。 */
                        for (mb_rec_hook_t *h =
                                 __atomic_load_n(&r->m->rec_hooks, __ATOMIC_RELAXED);
                             h && nh < MB_REC_HOOK_MAX; h = h->next) {
                            __atomic_add_fetch(&h->refs, 1, __ATOMIC_RELAXED);
                            local[nh++] = h;
                        }
                        pthread_mutex_unlock(&r->m->rec_hook_lock);
                        ++rec_hook_dispatch_depth;
                        for (int hi = 0; hi < nh; ++hi)
                            local[hi]->fn(local[hi]->ud,
                                          r->node ? r->node->call : NULL,
                                          pkt.payload, pkt.payload_len,
                                          side, (int)pkt.pt);
                        --rec_hook_dispatch_depth;
                        /* P1-2：引用计数原子回退；仅当确有注销等待者时才加锁唤醒。
                         * 用 SEQ_CST 与注销侧构成 Dekker 配对，避免弱内存序下漏唤醒。 */
                        if (nh > 0) {
                            int wake = 0;
                            for (int hi = 0; hi < nh; ++hi) {
                                if (__atomic_sub_fetch(&local[hi]->refs, 1,
                                                       __ATOMIC_SEQ_CST) == 0 &&
                                    __atomic_load_n(&r->m->rec_hook_waiters,
                                                    __ATOMIC_SEQ_CST) > 0)
                                    wake = 1;
                            }
                            if (wake) {
                                pthread_mutex_lock(&r->m->rec_hook_lock);
                                pthread_cond_broadcast(&r->m->rec_hook_cond);
                                pthread_mutex_unlock(&r->m->rec_hook_lock);
                            }
                        }
                    }
                }
                continue;
            }

            /*  DTMF RFC4733 拦截：本 side 的 telephone-event PT → 消费不转发。
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
                        if (b2b_call_is_alive(r->node->call)) {
                            b2b_call_t *call = (b2b_call_t *)r->node->call;
                            if (call->ivr_sess) {
                                ivr_action_t act;
                                int rc = ivr_on_dtmf(call->ivr_sess, ev.digit, &act);
                                static __thread int di_cnt = 0;
                                ++di_cnt;
                                if (di_cnt < 20 || di_cnt % 500 == 0)
                                    llog(LOG_INFO, "dtmf",
                                         "DTMF rx side=%d digit='%c' act=%d ivr_rc=%d call=%p",
                                         side, ev.digit, (int)act.act, rc, call);
                            }
                        }
                    }
                }
                continue;  /* 丢弃 DTMF RTP 包 — 对端不需要收到 RFC4733 */
            }

            /*  视频转码（VP9↔H264）：本腿视频 PT 且已规划 video_xcode。
             *  完全独立于音频转码器——video PT 永不进入 transcode_sess。
             *  收齐完整帧后解码/重编码，按 mtu 分片发往对侧。 */
            if (r->node) {
                video_xcode_t *vx = __atomic_load_n(&r->node->vxcode[side],
                                                    __ATOMIC_ACQUIRE);
                int is_vpt = r->node->video_pt_allowed_count[side] > 0 &&
                             pt_in_list((uint8_t)pkt.pt,
                                        r->node->video_pt_allowed[side],
                                        r->node->video_pt_allowed_count[side]);
                /*  记录入向视频的原始 SSRC（SSRC 重写前），供 PLI 翻译：
                 *  对端 PLI 引用的是 SBC 出向改写后的 SSRC，源端不认。
                 *  WebRTC 腿视频与音频共用 socket（vid_fd=0），且浏览器常
                 *  按自己的 PT（如 96）发视频而不是信令里的 97，is_vpt 也
                 *  不成立——需用"动态 PT + 大 payload"兜底识别，否则
                 *  vid_ssrc[1] 恒为 0，A→B 的 PLI 无法翻译。 */
                const int looks_video =
                    is_vpt || vid_fd ||
                    (r->node->neg_v_valid && pkt.pt >= 96 &&
                     pkt.pt != r->node->dtmf_pt[side] &&
                     pkt.payload_len > 200);
                if (looks_video && pkt.pt < 128) {
                    __atomic_store_n(&r->vid_ssrc[side], pkt.ssrc,
                                     __ATOMIC_RELAXED);
                    /* §3.4 snapshot 请求的关键帧：由媒体线程发送 PLI，
                     * 与 SRTP/发送路径同线程，避免跨线程使用 SRTP 会话。 */
                    if (__atomic_exchange_n(&r->pli_req[side], 0,
                                            __ATOMIC_ACQ_REL)) {
                        relay_video_request_pli(r, side, pkt.ssrc);
                    }
                    /*  视频 seq 缺口 → 代接收端向源腿补发 PLI。
                     *  Linphone 不发 PLI/FIR，WebRTC 源端又只在收到 PLI 时
                     *  重出 IDR；中继链路丢包后接收端参考帧链断裂，画面就
                     *  永久定格（本抓包：11.2s 起 10 处缺口，SBC→Linphone
                     *  之后再无任何关键帧）。检测到缺口立刻要一帧 IDR。
                     *  只更新前向 seq（负 diff 视为乱序，不改基线）；
                     *  SSRC 变化视为换源，重新同步。 */
                    if (r->vgap_primed[side] &&
                        r->vgap_ssrc[side] == pkt.ssrc) {
                        int16_t vd = (int16_t)(pkt.seq - r->vgap_last_seq[side]);
                        if (vd > 0) {
                            if (vd > 1) {
                                uint64_t vnow = nsbc_now_ms();
                                if (vnow - r->vgap_pli_ms[side] >= 2000) {
                                    r->vgap_pli_ms[side] = vnow;
                                    if (r->diag_vgap[side] < 10 ||
                                        r->diag_vgap[side] % 100 == 0)
                                        llog(LOG_NOTICE, "relay",
                                             "VIDEO_GAP side=%d ssrc=0x%08x "
                                             "lost=%d → 代接收端请求关键帧(n=%u)",
                                             side, pkt.ssrc, (int)(vd - 1),
                                             r->diag_vgap[side] + 1);
                                    ++r->diag_vgap[side];
                                    relay_video_request_pli(r, side, pkt.ssrc);
                                }
                            }
                            r->vgap_last_seq[side] = pkt.seq;
                        }
                    } else {
                        r->vgap_primed[side] = 1;
                        r->vgap_ssrc[side] = pkt.ssrc;
                        r->vgap_last_seq[side] = pkt.seq;
                    }
                }
                /*  视频流起始主动向源腿要一次关键帧：bridge_confirm 之前的
                 *  I 帧没有 latch/计划会被丢弃，而 VP8/H264 若首帧不是 I 帧
                 *  就永远黑屏（Linphone 显示 no video，浏览器也不会自发重发
                 *  I 帧）。限频 500ms，每侧最多 2 次，代价仅一个 RTCP PSFB。 */
                if (looks_video && r->node->neg_v_valid &&
                    r->diag_vkf[side] < 2) {
                    ++r->diag_vkf[side];
                    llog(LOG_INFO, "relay",
                         "VIDEO_KF_REQ side=%d pt=%u ssrc=0x%08x（首包主动请求关键帧）",
                         side, (unsigned)pkt.pt, pkt.ssrc);
                    relay_video_request_pli(r, side, pkt.ssrc);
                }
                /*  视频载荷指纹：每侧前 3 包打印首 8 字节。
                 *  信令里的 codec 名不可全信（客户端会改写 answer 文本），
                 *  用载荷首字节区分 H.264（bit6=1，单 NAL/FU-A/STAP-A 的
                 *  NRI 位）与 VP8（bit6 恒 0，描述符 R 位保留为 0）。 */
                if (looks_video && pkt.payload_len >= 8 &&
                    r->diag_vsniff[side] < 3) {
                    const uint8_t *pp = pkt.payload;
                    char hx[3 * 8 + 1];
                    int hp = 0;
                    for (size_t hi = 0; hi < 8; ++hi)
                        hp += snprintf(hx + (size_t)hp,
                                       sizeof(hx) - (size_t)hp,
                                       "%02x", pp[hi]);
                    llog(LOG_NOTICE, "relay",
                         "VIDEO_SNIFF side=%d pt=%u len=%zu hex=%s guess=%s",
                         side, (unsigned)pkt.pt, pkt.payload_len, hx,
                         (pp[0] & 0x40) ? "H264" : "VP8/other");
                    ++r->diag_vsniff[side];
                }
                if (!is_vpt && r->node->neg_v_valid && pkt.pt >= 96 &&
                    pkt.pt != r->node->dtmf_pt[side] && pkt.payload_len > 200) {
                    /*  信令 answer 与实际发送 PT 不一致时（浏览器客户端白名单
                     *  删码但内部仍发原 PT），只做诊断计数，不贸然转码——
                     *  把未知编码喂给固定编码的解码器会污染输出。
                     *  根因修复在 web 端 VIDEO_KEEP_MAIN。
                     *  注：这种情况下若对端也接受该 PT，包会按原 PT 透传
                     *  （下方 out_vid 判定），并不是丢弃。 */
                    if (r->diag_vrx_nox[side] < 3 ||
                        r->diag_vrx_nox[side] % 500 == 0)
                        llog(LOG_WARN, "relay",
                             "VIDEO_PT_MISMATCH side=%d pt=%u len=%zu "
                             "allowed_count=%d（信令与实际 PT 不一致，不转码/按原 PT 透传）",
                             side, (unsigned)pkt.pt, pkt.payload_len,
                             r->node->video_pt_allowed_count[side]);
                    ++r->diag_vrx_nox[side];
                }
                if (vx && is_vpt) {
                    if (r->diag_vrx[side] < 5 || r->diag_vrx[side] % 500 == 0)
                        llog(LOG_INFO, "relay",
                             "VIDEO_RX side=%d pt=%u len=%zu marker=%d ts=%u",
                             side, (unsigned)pkt.pt, pkt.payload_len,
                             pkt.marker, (unsigned)pkt.ts);
                    ++r->diag_vrx[side];
                    relay_video_ctx_t vc;
                    vc.r = r;
                    vc.side = side;
                    vc.other = other;
                    (void)video_xcode_input(vx, pkt.payload, pkt.payload_len,
                                            pkt.marker, pkt.ts,
                                            relay_video_emit, &vc);
                    if (video_xcode_take_pli(vx))
                        relay_video_request_pli(r, side, pkt.ssrc);
                    ++r->pkts;
                    continue;
                }
            }

            /*  诊断（不丢包）：本腿 audio PT 既不在协商集合、也不是
             *  DTMF / 视频 PT，且没走下面任何 PT-rewrite / 转码分支 →
             *  原样转发会让对端收到未协商的 PT（对端通常直接丢弃 = 静音）。
             *  典型场景：信令协商 PCMA(8)，浏览器实际发 PCMU(0)，
             *  按 PCMA 建的转码器永远不命中。 */
            if (r->node && r->node->audio_n[side] > 0 &&
                pkt.pt != r->node->dtmf_pt[side] &&
                !pt_in_list((uint8_t)pkt.pt, r->node->audio_pt[side],
                            r->node->audio_n[side]) &&
                !pt_in_list((uint8_t)pkt.pt, r->node->video_pt_allowed[side],
                            r->node->video_pt_allowed_count[side])) {
                int *bad = &r->node->audio_badpt_diag[side];
                if (*bad < 3 || (*bad % 500) == 0) {
                    llog(LOG_WARN, "relay",
                         "AUDIO_PT_UNNEGOTIATED side=%d pt=%u len=%zu "
                         "ssrc=0x%08x negotiated_n=%d pt0=%u pt1=%u "
                         "— 信令与媒体不一致，已原样转发",
                         side, (unsigned)pkt.pt, pkt.payload_len, pkt.ssrc,
                         r->node->audio_n[side],
                         (unsigned)r->node->audio_pt[side][0],
                         (unsigned)r->node->audio_pt[side][1]);
                }
                ++(*bad);
            }

            /*  视频同 codec 不同 PT：只改 RTP header PT 字节（不转码）。 */
            if (r->node && r->node->video_rewrite_active[side] &&
                (uint8_t)pkt.pt == r->node->video_rewrite_src_pt[side]) {
                uint8_t vnew = r->node->video_rewrite_dst_pt[side];
                buf[1] = (uint8_t)((buf[1] & 0x80) | (vnew & 0x7F));
                pkt.pt = vnew;
            }

            /*  PT rewrite fast-path：同 codec 不同 PT
             * 例：WebRTC G729 PT=96 → SIP G729 PT=18
             * 只改 RTP header PT 字节，payload 不动，零开销。 */
            if (r->node && r->node->pt_rewrite_active[side] &&
                (uint8_t)pkt.pt == r->node->pt_rewrite[side].src_pt) {
                uint8_t new_pt = r->node->pt_rewrite[side].dst_pt;
                buf[1] = (buf[1] & 0x80) | (new_pt & 0x7F);
                pkt.pt = new_pt;
            }
            /*  旁路载荷直转（§2.2）：CN/red/rtx 等 is_event payload 不进入
             *  PCM 转码器，只按目标腿协商的 PT 重写后转发。telephone-event
             *  已在上方 DTMF 分支消费，不会到这里。 */
            if (r->node) {
                uint8_t ept = peer_event_pt(r->node, side, (int)pkt.pt);
                if (ept != 0xFF && ept != (uint8_t)pkt.pt) {
                    buf[1] = (uint8_t)((buf[1] & 0x80) | (ept & 0x7F));
                    pkt.pt = ept;
                }
            }
            /*  REQ-TRANS-004 P0：运行时 G.711 转码
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
                /*  2026-10-16 转码入口守卫：说明为何没进转码（静态在函数内，
                 * relay per-call 所以等价于 per-call，不会跨呼叫泄漏）
                 *  2026-10-16 v2：去掉 transcode[side] 条件 — side=1(WebRTC)
                 * 即使没有 transcode 也要打，才能看到 Chrome 是否持续发 RTP。 */
                //static int guard_seen[2] = {0};
                if (r->node) {
                    // if (guard_seen[side] < 5 || ++guard_seen[side] % 100 == 0) {
                    //     int in_list = pt_in_list((uint8_t)pkt.pt,
                    //                               r->node->pt_allowed[other],
                    //                               r->node->pt_allowed_count[other]);
                    //     llog(LOG_INFO, "relay",
                    //          "TC-GUARD side=%d in_pt=%u transcode=%p "
                    //          "pt_allowed[%d]=(n=%d) in_list=%d WILL_XCODE=%d",
                    //          side, (unsigned)pkt.pt,
                    //          (void *)r->node->transcode[side],
                    //          other, r->node->pt_allowed_count[other],
                    //          in_list, !in_list);
                    // }
                }
            }
            transcode_sess_t *tc =
                r->node ? __atomic_load_n(&r->node->transcode[side],
                                          __ATOMIC_ACQUIRE) : NULL;
            if (r->node && tc &&
                !peer_accepts_payload(r->node, side, pkt.pt) &&
                /*  视频 PT 属于源腿 → 绝不能进音频转码器。 */
                !pt_in_list((uint8_t)pkt.pt,
                            r->node->video_pt_allowed[side],
                            r->node->video_pt_allowed_count[side]) &&
                /*  入口校验：入包 PT 必须属于该腿音频 codec，且与转码源一致。 */
                leg_audio_pt_to_cid(r->node, side, pkt.pt) ==
                    transcode_src_codec_id(tc)) {
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
                if (rc == NSBC_OK && new_out_len == 0) {
                    /* 编码器尚未产出（缓冲/DTX），保留原包不发送空载荷 */
                    rc = NSBC_AGAIN;
                }
                if (rc == NSBC_OK) {
                    /* 转码成功 — 更新 payload_len + 总包长 n */
                    pkt.payload_len = new_out_len;
                    n = (ssize_t)((pkt.payload - buf) + new_out_len);
                    /* RTP header byte 1：M(bit7) + PT(bit6-0)
                     * 优先使用协商出的目标 PT（xcode_dst_pt），
                     * 未设置时才退回 codec default_pt。 */
                    codec_id_t dst_id = transcode_dst_codec_id(tc);
                    uint8_t new_pt = transcode_get_dst_pt(tc);
                    if (new_pt == 0xFF) {
                        const codec_ops_t *ops_dst = codec_registry_find(dst_id);
                        if (ops_dst && ops_dst->default_pt >= 0 &&
                            ops_dst->default_pt < 256)
                            new_pt = (uint8_t)ops_dst->default_pt;
                        else
                            new_pt = (uint8_t)(dst_id & 0x7F);
                    }
                    /* 保留 marker bit (bit7)，替换低 7 位为新 PT */
                    buf[1] = (buf[1] & 0x80) | (new_pt & 0x7F);
                    pkt.pt = new_pt;
                    path = "transcode";   /* 音频做了编解码转换 */
                    /*  诊断：用 cnode->transcode_seen_diag[side]（per-call），
                     * 从 static __thread 改过来——避免媒体线程跨呼叫累积
                     * 导致短通话（~400 包）碰不到 %1000 静默。周期改 %100 更易观察。 */
                    if (r->node) {
                        int *ts = &r->node->transcode_seen_diag[side];
                        ++(*ts);  /*  无条件先递增，避免 || 短路导致计数器永远停在 0 */
                        if (*ts <= 10 || *ts % 100 == 0) {
                            llog(LOG_DEBUG, "relay",
                                 "TRANSCODE side=%d pt_in=%u→%u "
                                 "bytes=%zu call=%p",
                                 side, (unsigned)old_pt, (unsigned)new_pt,
                                 pkt.payload_len,
                                 (void *)(r->node ? r->node->call : NULL));
                        }
                    }
                } else if (rc == NSBC_AGAIN) {
                    /* 编码器缓冲中，尚未产出新载荷；不记错误 */
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
            if (r->m->cfg.rec_feed_fn && pkt.payload && pkt.payload_len &&
                /*  video PT 不进录音 raw 通道（G.711 sidecar）——
                 * H.264 走 vid_reasm → rec_feed_video 专用链路，
                 * 喂进来只会污染 raw 文件 + 触发无意义的 PT 告警。
                 * 独立视频 socket 上到达的包同样一律不喂。 */
                !vid_fd &&
                !(r->node && pt_in_list((uint8_t)pkt.pt,
                                        r->node->video_pt_allowed[side],
                                        r->node->video_pt_allowed_count[side]))) {
                r->m->cfg.rec_feed_fn(r->m->cfg.rec_feed_ud,
                                      r->node ? r->node->call : NULL,
                                      pkt.payload, pkt.payload_len,
                                      side, (int)pkt.pt);
            }
            /* REQ-AI-001：动态 hook 链表（ai_dialog 等可选模块运行时注册）。
             * cfg.rec_feed_fn 是 signaling 创建时设的录音主槽；rec_hooks 是
             * 模块 dlopen 后追加的 hook。锁内拷贝 fn+ud 到栈、解锁后执行 ——
             * Phase 0: 锁内执行外部 hook 会嵌套 recording.c 的 sess.lock（之前 P0-2 锁内套锁）。*/
            {
                void *hooks_head = __atomic_load_n(&r->m->rec_hooks, __ATOMIC_ACQUIRE);
                static __thread int rh_cnt = 0;
                if (rh_cnt++ < 5)
                    llog(LOG_INFO, "relay", "REC_HOOK_CHECK m=%p hooks=%p side=%d pt=%u len=%zu payload=%p",
                         (void *)r->m, hooks_head, side, (unsigned)pkt.pt, pkt.payload_len, pkt.payload);
                if (pkt.payload && pkt.payload_len && hooks_head) {
                    mb_rec_hook_t *local[MB_REC_HOOK_MAX];
                    int nh = 0;
                    pthread_mutex_lock(&r->m->rec_hook_lock);
                    for (mb_rec_hook_t *h =
                             __atomic_load_n(&r->m->rec_hooks, __ATOMIC_RELAXED);
                         h && nh < MB_REC_HOOK_MAX; h = h->next) {
                        __atomic_add_fetch(&h->refs, 1, __ATOMIC_RELAXED);
                        local[nh++] = h;
                    }
                    pthread_mutex_unlock(&r->m->rec_hook_lock);
                    /* 锁外遍历执行 hook —— 各自内部锁互不嵌套 */
                    ++rec_hook_dispatch_depth;
                    for (int hi = 0; hi < nh; ++hi)
                        local[hi]->fn(local[hi]->ud, r->node ? r->node->call : NULL,
                                      pkt.payload, pkt.payload_len, side, (int)pkt.pt);
                    --rec_hook_dispatch_depth;
                    /* P1-2：引用计数原子回退；仅当确有注销等待者时才加锁唤醒。
                     * 用 SEQ_CST 与注销侧构成 Dekker 配对，避免弱内存序下漏唤醒。 */
                    if (nh > 0) {
                        int wake = 0;
                        for (int hi = 0; hi < nh; ++hi) {
                            if (__atomic_sub_fetch(&local[hi]->refs, 1,
                                                   __ATOMIC_SEQ_CST) == 0 &&
                                __atomic_load_n(&r->m->rec_hook_waiters,
                                                __ATOMIC_SEQ_CST) > 0)
                                wake = 1;
                        }
                        if (wake) {
                            pthread_mutex_lock(&r->m->rec_hook_lock);
                            pthread_cond_broadcast(&r->m->rec_hook_cond);
                            pthread_mutex_unlock(&r->m->rec_hook_lock);
                        }
                    }
                }
            }
            /* ← 关 line 1141 REC_HOOK_CHECK 外层块 */
            /* 出向 SSRC 重写（固定头偏移 8，4 字节网络序）
             * 之前 Bug：只改 p[3] 一个字节，导致 SSRC 变成 orig[0..2] | (new & 0xFF)。
             *
             *  REQ-MEDIA-SSRC-002 双 WebRTC 透传优化：
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
                /*  mixed-mode（一端明文、一端 WebRTC）：音频固定使用 relay_start
                 *  预分配的出向 SSRC —— answer SDP 用 a=ssrc 宣告的就是同一个
                 *  值（JSEP/SKILL §4.2），Chrome 才能按 a=ssrc 建立接收 sink。
                 *  视频仍走 map（video_xcode / 独立 vid 通道自行维护 SSRC，
                 *  未在 answer 里宣告，不能套用音频的固定值）。 */
                int is_vid = vid_fd ||
                    (r->node && pt_in_list((uint8_t)pkt.pt,
                                           r->node->video_pt_allowed[side],
                                           r->node->video_pt_allowed_count[side]));
                int dest_ws = (r->node && r->node->dtls[other] != NULL);
                int src_plain = (r->node == NULL || r->node->dtls[side] == NULL);
                if (!is_vid && dest_ws && src_plain && r->plan_out_ssrc[side])
                    newssrc = r->plan_out_ssrc[side];
                else
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
            if (pkt.payload && pkt.payload_len && r->node &&
                b2b_call_is_alive(r->node->call)) {
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
        /*  出向流种类：改完 PT 后按"目标腿的视频 PT 表"判定。
         *  不能直接用入向 socket 判断——B(WebRTC) 发来的视频收在共享
         *  socket 上，却要发往 A(明文) 的独立视频端口。
         *  但入向若是明文腿的独立视频 socket，包必为视频，直接放行，
         *  不再受目标腿 PT 表收窄的影响。 */
        int out_vid = !is_rtcp && r->node &&
                      (vid_fd ||
                       (r->node->video_pt_allowed_count[other] > 0 &&
                        pt_in_list((uint8_t)(buf[1] & 0x7F),
                                   r->node->video_pt_allowed[other],
                                   r->node->video_pt_allowed_count[other])));
        /* === REQ-NAT-001 comedia：sendto 目标地址 ===
         * 与上方 dst_hint 同逻辑：优先 comedia 学到的真实 latch。
         *  只有目标腿拥有独立视频 socket 时才可用视频 latch——
         *  WebRTC 腿的视频与音频共用同一地址，必须走 audio latch
         *  （那是 ICE 打洞后学到的真实可达地址）。 */
        /*  视频 RTCP 的目的地：明文腿有独立视频 socket 时，PLI/FIR/SR/RR
         *  必须发到它的 RTCP 端口并从 RTCP socket 发出（正值错误见
         *  relay_video_request_pli 注释）。判定：PSFB(206) 必属视频；
         *  SR/RR 用报文首个子包的发送方 SSRC 与本腿已记录的视频 SSRC 比对，
         *  对不上就按音频流走原路径（WebRTC BUNDLE 腿 fd_vrtcp<0 也走原路径）。 */
        int rtcp_vid_dst = 0;
        if (is_rtcp && r->fd_vrtcp[other] >= 0) {
            /*  RTCP 的 byte[1] 是完整 8 位 PT（没有 Marker 位），不能 &0x7F */
            if (buf[1] == 206) {
                rtcp_vid_dst = 1;
            } else if (n >= 8) {
                uint32_t rtcp_sender = 0;
                memcpy(&rtcp_sender, buf + 4, 4);
                rtcp_sender = ntohl(rtcp_sender);
                if (rtcp_sender &&
                    rtcp_sender ==
                        __atomic_load_n(&r->vid_ssrc[side], __ATOMIC_RELAXED))
                    rtcp_vid_dst = 1;
            }
        }
        /*  对端是否协商了 rtcp-mux（RFC 5761）：WebRTC 腿恒 mux（SBC 在
         *  answer 里声明 a=rtcp-mux，音视频共用一条传输）；明文腿 SBC 已
         *  剥掉 a=rtcp-mux，RTCP 必须按 RFC 3550 §11 发到 RTP 端口 + 1，
         *  否则对端把 SR/RR 丢弃（PLI 也到不了，视频定格）。 */
        const int peer_mux = (!r->node) ? 1
                           : ((other == 0) ? (int)r->node->a_ws
                                           : (int)r->node->b_ws);
        int dst_is_rtcp_port = 0;   /* dst 已经是 RTCP 端口（comedia 学到的 RTCP 源） */
        nsbc_sa_t rtcp_dst_fix;
        const nsbc_sa_t *dst;
        if (rtcp_vid_dst) {
            if (r->vrlatched[other]) {
                dst = &r->vrlatch[other];
                dst_is_rtcp_port = 1;
            } else {
                dst = r->vhave_peer[other] ? &r->vpeer[other] : &r->latch[other];
            }
        } else if (out_vid && r->fd_vrtp[other] >= 0) {
            /*  明文腿视频：vlatch > SDP 视频 peer > 音频 latch；
             *  视频 peer 是私网时在 helper 内回退到音频 latch 的公网 IP。 */
            dst = relay_video_dst_sel(r, other);
        } else {
            dst = r->latched[other]
                  ? &r->latch[other]
                  : (r->have_peer[other] ? &r->peer[other] : &r->latch[other]);
        }
        /*  RTCP 目的地是"RTP 端口"（latch/SDP）时，未协商 rtcp-mux 的对端
         *  实际监听端口是 RTP+1 —— 上面所有 latch 都只从 RTP 包学习
         *  （音频/视频 RTCP 源另有 vrlatch 单独记录），这里统一补 +1。 */
        if (is_rtcp && !peer_mux && !dst_is_rtcp_port) {
            uint16_t dp = nsbc_sa_port(dst);
            if (dp > 0 && dp < 65535) {
                rtcp_dst_fix = *dst;
                nsbc_sa_set_port(&rtcp_dst_fix, (uint16_t)(dp + 1));
                dst = &rtcp_dst_fix;
            }
        }
        /*  REQ-MEDIA-SSRC-004：mixed-mode 下把转发的 RTCP 发送方 SSRC 改成
         *  与 RTP 一致的出向 SSRC。位置必须在 SRTCP 加密之前（认证标签覆盖
         *  整包），且在 rtcp_vid_dst 判定之后（那里要用原始 sender SSRC
         *  比对视频源）。 */
        if (is_rtcp && !r->echo_mode && r->node &&
            r->node->dtls[other] && !r->node->dtls[side] &&
            r->plan_out_ssrc[side])
            relay_rtcp_rewrite_ssrc(buf, (size_t)n, r->plan_out_ssrc[side],
                __atomic_load_n(&r->d_in_ssrc[side], __ATOMIC_RELAXED),
                __atomic_load_n(&r->vid_ssrc[side], __ATOMIC_RELAXED));
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
        /*  诊断：sendto 目标地址（前 5 包 + 每 500 包；P2-6/P3-16 降为 DEBUG）
         * 确认 SBC 把 SRTP 发到 Chrome ICE 候选的真实地址
         *  2026-11-22 per-call 计数 */
        {
            /* per-call 计数：前 5 包 + 每 500 包一条（短通话也可见） */
            uint32_t seen = r->diag_sendto[side]++;
            if ((seen < 5 || seen % 500 == 0) && log_get_level() >= LOG_DEBUG) {
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
            /* (diag_sendto 已在进入本分支前自增) */
                llog(LOG_DEBUG, "relay",
                     " SENDTO side=%d→%d pt=%u vid=%d rtcp=%d path=%s ssrc=0x%08x dst=%s:%u send_len=%zu "
                     "latched[other]=%d have_peer[other]=%d srtp_protect=%d",
                     side, other, (unsigned)(buf[1] & 0x7F), out_vid, is_rtcp, path, ssrc,
                     ipa, (unsigned)dport, send_len,
                     r->latched[other], r->have_peer[other],
                     !!(r->node && r->node->dtls[other] && r->srtp[other]));
            }
        }
        /*  明文腿的视频必须从独立视频端口发出：源端口与对端发送目标一致，
         *  避免严格 symmetric-RTP 的 UA 丢弃源端口不匹配的视频包。
         *  非 rtcp-mux 对端的 RTCP 从通告的 RTCP socket（RTP+1）发出，
         *  与 SDP 告知对端的端口一致（RFC 3550 §11 / 对称 RTCP）。 */
        int out_fd = rtcp_vid_dst ? r->fd_vrtcp[other]
                     : ((out_vid && r->fd_vrtp[other] >= 0)
                        ? r->fd_vrtp[other] : r->fd_rtp[other]);
        if (is_rtcp && !rtcp_vid_dst && !peer_mux && r->fd_rtcp[other] >= 0)
            out_fd = r->fd_rtcp[other];
        ssize_t sret = sendto(out_fd, buf, send_len, MSG_NOSIGNAL,
                              (const struct sockaddr *)&dst->ss, dst->len);
        if (sret != (ssize_t)send_len) {
            llog(LOG_WARN, "relay", "sendto FAIL side=%d→%d ret=%zd errno=%d "
                 "expected=%zd dst=%s", side, other, sret, errno, send_len, "?");
        }
        ++r->pkts;
        /*  诊断：每 100 包打一条（P3-16：降为 DEBUG，避免默认级别下每通话刷屏） */
        if (r->pkts % 100 == 0) {
            llog(LOG_DEBUG, "relay", " FWD pkts=%llu drops=%u "
                 "latched[0]=%d latched[1]=%d srtp[0]=%p srtp[1]=%p",
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
    if (!r) return;
    /* P0-3 RACE-FIX: dead flag 入口守卫 —— 防止 reactor batch 内已 dispatch
     * 的 callback 在 rtp_relay_stop 执行 free(r) 后还访问 r 成员（UAF）。
     * dead 是在 reactor_call 之前就置 1 的，所以即使 reactor_del 还没来得及
     * 清 slots[fd].cb，这里也能拦截。 */
    if (__atomic_load_n(&r->dead, __ATOMIC_ACQUIRE))
        return;
    /* DEBUG 2026-11-13: relay_cb 是否被 reactor 回调？
     * P0-3 FIX: hits 是函数级 static，跨所有 relay 累计，所以新 relay 启动
     * 时可能 hits 已经 > 10，导致前 10 次都不打印。改成 per-relay hits 存
     * 在 relay 内部成员里（见 rtp_relay_t 的 cb_hits），但 struct 里没这个字段
     * —— 简化：只要 hits <= 10 || hits % 1000 == 0 都打，确保生产首次 arm 时
     * 能看到前几条。 */
    {
        static int hits = 0;
        if (hits <= 10 || hits % 1000 == 0) {
            fprintf(stderr, "[RELAY_CB] fd=%d ev=0x%x r=%p rtp=[%d,%d] rtcp=[%d,%d]\n",
                    fd, (unsigned)ev, (void*)r,
                    r->fd_rtp[0], r->fd_rtp[1], r->fd_rtcp[0], r->fd_rtcp[1]);
        }
        ++hits;
    }
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
        /* 明文腿独立视频通道（与 audio 同一套 PT 分流逻辑） */
        if (fd == r->fd_vrtp[side]) {
            relay_recv_one(r, side, 0, fd);
            return;
        }
        if (fd == r->fd_vrtcp[side]) {
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
    if (h)
        __atomic_add_fetch(&m->rec_hook_waiters, 1, __ATOMIC_SEQ_CST);
    for (mb_rec_hook_t *it = h; it; it = it->next) {
        while (__atomic_load_n(&it->refs, __ATOMIC_SEQ_CST) > 0)
            pthread_cond_wait(&m->rec_hook_cond, &m->rec_hook_lock);
    }
    if (h)
        __atomic_sub_fetch(&m->rec_hook_waiters, 1, __ATOMIC_SEQ_CST);
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

/*  REQ-MEDIA-SEC-004：从 SDP 里提取 a=fingerprint:sha-256
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
        /*  WebRTC answer 清理：剥掉 B 侧（普通 SIP）私货属性，
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
         *  注意 101 必须带 " rtx" 后缀：裸 "a=rtpmap:101" 会误伤音频段的
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

/*  PLEN 方向预扫描：收集 m=video 段里最终会带 a=rtpmap 输出的 PT 集合。
 *
 *  2026-11-24 抓包确认的缺陷：attr_is_webrtc_only() 会把浏览器大把
 *  a=rtpmap:<pt> 行剥掉（只留 VP8/rtx/H264 + 后面追加的 VP9），
 *  而 plen_slim_fmt_list() 之前把 m=video 的 20 个 PT 全部透传，
 *  结果 offer 里 m=video 列 21 个 PT、只有 4 条 rtpmap → 非法 SDP，
 *  Linphone 直接丢弃整个视频段（200 OK 只剩 m=audio），
 *  连媒体会话都没起来（抓包零 RTP/RTCP）。
 *  这里按"真正会输出的 rtpmap"构建允许集合，供 slim 收窄 fmt-list。 */
static void sdp_plain_video_pt_set(const char *sdp, size_t len, uint8_t ok[128])
{
    memset(ok, 0, 128);
    const char *p = sdp, *end = sdp + len;
    int in_video = 0;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t ll = (size_t)(le - p);
        while (ll > 0 && p[ll - 1] == '\r') --ll;

        if (ll >= 2 && p[0] == 'm' && p[1] == '=') {
            in_video = (ll >= 7 && memcmp(p + 2, "video ", 6) == 0);
        } else if (in_video && ll >= 9 &&
                   memcmp(p, "a=rtpmap:", 9) == 0 &&
                   !attr_is_webrtc_only(p, ll)) {
            int pt = atoi(p + 9);
            if (pt >= 0 && pt < 128) ok[pt] = 1;
        }
        p = eol ? eol + 1 : end;
    }
}

/* 精简 m= 行的 fmt-list —— PLEN 模式只保留原始 offer 里已有的子集：
 *   audio 段：PCMA(8) / PCMU(0) + telephone-event 8k(126)
 *   video 段：只保留最终会输出 a=rtpmap 的 PT（见 sdp_plain_video_pt_set）
 * 关键：从原始 fmt-list 挑子集，**不塞新 PT**（否则后面没 rtpmap 会让 SIP 端困惑）。
 * 原始 m=audio "8 110 126" → 精简成 "8 126"
 * 原始 m=video "96 97 102 ...(20 个)" → 只留 96/97/102（其余 rtpmap 已被剥掉）
 *
 * 2026-11-24 视频不再硬编码 PT=102 为 H264：
 *   WebRTC/浏览器对 PT 分配是动态的（同一次 offer，H264 可能是 PT=96 也可能是 PT=102），
 *   把非 H264 PT 当作 H264 发给 Linphone 会让对方 answer 带 video_port=0 退化成纯音频。
 *   现在把 m=video 的 fmt-list 原样透传给 SIP 端点，让它自己从 rtpmap 里挑支持的 codec。 */
static int plen_slim_fmt_list(char *mline, size_t mline_cap,
                              const uint8_t *video_pt_ok)
{
    /* m=<type> <port> <proto> <pt1> <pt2> ... → 只留前三个 token + 精简后的 PT 子集 */
    const char *sp1 = strchr(mline, ' ');
    if (!sp1) return 0;
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return 0;
    const char *sp3 = strchr(sp2 + 1, ' ');
    if (!sp3) return 0;  /* 已经没 fmt-list，不用精 */
    /*  media_type 要跳过 "m=" 前缀！mline="m=audio 59996..." → media_type="audio" */
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
            /*  只保留最终会带 rtpmap 输出的 PT：m=video 里列了 PT 却没有
             *  对应 a=rtpmap 属非法 SDP，Linphone 会丢弃整个视频段。 */
            int ptv = atoi(tok);
            keep_it = (video_pt_ok && ptv >= 0 && ptv < 128 && video_pt_ok[ptv]);
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
             "ENSURE_VIDEO: m=video already present (skip append)");
        return NSBC_OK;   /* 已经有 video m-line，什么都不做 */
    }

    llog(LOG_WARN, "sdp",
         "ENSURE_VIDEO: NO m=video found! Appending port=0 placeholder...");

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
        int fix_rejected_video_pt = 0;  /*  提到外层作用域——在 m= 块内设置，块外复制时使用 */

        /*  在 session 级（还没遇到任何 m= 行）跳过 a=group: 和 a=sendrecv
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
            /*  检测本段是否被拒：m=<media> 0 <proto> ...
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
                     "ANSWER_MLINE_REJECTED: audio port=0 at_idx=%d", media_idx);
            }

            /*  REQ-MEDIA-VIDEO-002：rejected video m-line 必须带合法 PT 列表。
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
        /*  复制当前行——如果是 rejected video m-line 且 PT 需要修复，
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

/*  RFC 8843(BUNDLE)/5761(rtcp-mux)：WebRTC 腿的多条 m= 段共用同一个传输
 *  端口（bridge_rewrite_sdp 在 dir=WS 且 video_port==0 时让 video 复用
 *  audio 端口）。共用端口必须在 answer 的每个未拒绝 m= 段显式声明
 *  a=rtcp-mux，否则对端按 RFC 3550 认为 RTCP 在 RTP 端口 + 1，SR/RR/PLI
 *  全部落空（视频定格、质量统计失效）。
 *  answer 骨架通常由 sdp_build_webrtc_answer 补齐；本函数是最终兜底：
 *  只在确实存在"两个非零端口相同"时工作，给缺少 a=rtcp-mux 的段补一行，
 *  已声明则不重复。返回新长度；0=未改动（调用方保留原长度）。 */
static int sdp_ensure_rtcp_mux(char *buf, size_t cap, size_t len)
{
    if (!buf || len == 0 || len >= cap) return 0;
    /* 1) 是否存在非零端口复用 */
    uint16_t seen[8];
    int nseen = 0, shared = 0;
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t ll = (size_t)(le - p);
        if (ll >= 8 && p[0] == 'm' && p[1] == '=') {
            const char *q = p + 2;
            while (q < le && *q != ' ') ++q;
            while (q < le && *q == ' ') ++q;
            unsigned v = 0;
            while (q < le && *q >= '0' && *q <= '9')
                v = v * 10u + (unsigned)(*q++ - '0');
            if (v > 0 && v <= 65535u) {
                for (int i = 0; i < nseen; ++i)
                    if (seen[i] == (uint16_t)v) shared = 1;
                if (nseen < 8) seen[nseen++] = (uint16_t)v;
            }
        }
        p = eol ? eol + 1 : end;
    }
    if (!shared) return 0;

    /* 2) 逐段复制，段尾补 a=rtcp-mux（已有则不补） */
    static const char MUX[] = "a=rtcp-mux\r\n";
    const size_t mux_len = sizeof(MUX) - 1;
    char *nb = (char *)malloc(cap);
    if (!nb) return 0;
    size_t used = 0;
    int have_sect = 0, sect_port = 0, sect_mux = 0, overflow = 0;
    p = buf;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t ll = (size_t)(le - p);
        size_t cl = ll;
        while (cl > 0 && p[cl - 1] == '\r') --cl;

        int flush_mux = 0;
        if (cl >= 8 && p[0] == 'm' && p[1] == '=') {
            if (have_sect && sect_port > 0 && !sect_mux) flush_mux = 1;
            const char *q = p + 2;
            while (q < p + cl && *q != ' ') ++q;
            while (q < p + cl && *q == ' ') ++q;
            unsigned v = 0;
            while (q < p + cl && *q >= '0' && *q <= '9')
                v = v * 10u + (unsigned)(*q++ - '0');
            sect_port = (int)v;
            sect_mux = 0;
            have_sect = 1;
        } else if (have_sect && cl >= 10 && memcmp(p, "a=rtcp-mux", 10) == 0) {
            sect_mux = 1;
        }
        if (flush_mux) {
            if (used + mux_len >= cap) { overflow = 1; break; }
            memcpy(nb + used, MUX, mux_len);
            used += mux_len;
        }
        if (used + ll >= cap) { overflow = 1; break; }
        memcpy(nb + used, p, ll);
        used += ll;
        if (eol) {
            if (used >= cap) { overflow = 1; break; }
            nb[used++] = '\n';
        }
        p = eol ? eol + 1 : end;
    }
    if (!overflow && have_sect && sect_port > 0 && !sect_mux) {
        /* 末行可能没有换行符（非标准但防御）：先补 CRLF 再落属性 */
        if (used > 0 && nb[used - 1] != '\n') {
            if (used + 2 >= cap) {
                overflow = 1;
            } else {
                nb[used++] = '\r';
                nb[used++] = '\n';
            }
        }
        if (!overflow) {
            if (used + mux_len < cap) {
                memcpy(nb + used, MUX, mux_len);
                used += mux_len;
            } else {
                overflow = 1;
            }
        }
    }
    if (overflow || used == 0 || used >= cap) {
        free(nb);
        return 0;
    }
    memcpy(buf, nb, used);
    buf[used] = '\0';
    free(nb);
    return (int)used;
}

/*  RFC 3264 段数对齐：answer 的 m= 段数必须等于 A 原始 offer 的段数。
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
                       uint16_t video_port, mb_sdp_dir_t dir)
{
    size_t used = 0;
    const char *p = in;
    const char *end = in + in_len;
    uint16_t media_port = rtp_port;
    int media_idx = 0;
    /*  PLEN 方向：先算出 m=video 里最终保留 rtpmap 的 PT 集合，
     *  避免 fmt-list 保留全部 PT 而 rtpmap 只输出少数几条（非法 SDP）。 */
    uint8_t vpt_ok[128];
    if (dir == MB_SDP_PLAIN)
        sdp_plain_video_pt_set(in, in_len, vpt_ok);

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        size_t llen = (size_t)(line_end - p);

        /*  FIX: 统一回剥行尾 \r。
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
            /*  o= 行改写后 fall-through 到通用写入，
             * 通用路径现在统一补 \r\n，不再丢 \r */
        }

        /* ---- c= 行改写 ---- */
        if (media_port != 0 && llen >= 2 && p[0] == 'c' && p[1] == '=') {
            int n = snprintf(tmp, sizeof(tmp), "c=IN IP4 %s", ip);
            if (n > 0) {
                outp = tmp;
                outlen = (size_t)n;
            }
            /*  c= 行独立写入 + \r\n，不 fall-through */
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
                 "REWRITE_MLINE: type=%.*s media_idx=%d media_port=%u video_port=%u dir=%d",
                 (int)(mt_end - mt), mt, media_idx,
                 (unsigned)media_port, (unsigned)video_port, dir);

            /*  PLAIN 方向（发给硬 SIP 端）：
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
                         "REWRITE_BRANCH[%d]: orig_port=%u media_port=0 → BYPASS keep port=%u",
                         media_idx, orig_port, this_port);
                } else if (orig_port == 0) {
                    /*  RFC 3264 语义：m=xxx 0 = 拒绝/关闭该媒体流
                     *   - answer 里 port=0 → 对端明确拒绝（如 MicroSIP 拒 video）
                     *   - offer 里 port=0 → hold 或关闭该媒体
                     *    必须保持原样，绝对不能改写成 relay 真实端口——
                     *     改写 = 硬启用被拒绝的媒体 → WebRTC setRemoteDescription
                     *     检测到 answer 声称启用但 offer ICE 不可达 → reject → BYE 立挂。
                     *   后续 this_port==0 分支（L2239+L2256）已正确处理：
                     *     - proto 保持原始（不做 WS/SAVPF 注入）
                     *     - tail 只保留第一个 fmt token */
                    this_port = 0;
                    llog(LOG_INFO, "media",
                         "REWRITE_BRANCH[%d]: orig_port=0 (media REJECTED/CLOSED) → keep port=0 (no rewrite)",
                         media_idx);
                } else {
                    /*  明文腿：video m-line 用独立的 video_port（专用 socket）。
                     *  WebRTC 腿（video_port==0）保持 BUNDLE 语义，所有 m-line
                     *  共用第一条 ICE/DTLS 传输的端口。
                     *  不能给明文腿 two m-lines 同端口——对端（linphone）会把
                     *  video 自行挪到 port+2，正好落到对侧 socket 上被当 SRTP
                     *  解密而全丢（音频正常、视频零流量）。 */
                    this_port = (uint16_t)media_port;
                    if (video_port != 0 &&
                        mt_end - mt == 5 && memcmp(mt, "video", 5) == 0)
                        this_port = video_port;
                    (void)media_idx;
                }

                const char *ps = port_end;
                while (ps < p + llen && *ps == ' ') ++ps;
                const char *pe = ps;
                while (pe < p + llen && *pe != ' ') ++pe;

                const char *proto_token;
                if (this_port == 0) {
                    /*  port=0 的 rejected m-line：保持原始 proto（如 RTP/AVP），
                     * 不做 WS/SAVPF 注入——对方明确说拒绝，不需要给它注入 DTLS 语义。 */
                    proto_token = (pe > ps) ? ps : "RTP/AVP";
                } else if (dir != MB_SDP_ASIS && pe > ps) {
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
                    /*  FIX: tail 不回剥 \r（已在循环开头统一回剥），
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
                        int slim = plen_slim_fmt_list(tmp, sizeof(tmp), vpt_ok);
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
                         "REWRITE_MLINE_OUT: %.*s",
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
        /*  FIX: 统一输出 \r\n。
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

/*  明文腿 H.264 fmtp 的 profile-level-id 必须描述本机编码器实际发出的码流
 * （RFC 6184 §8.1：SDP 参数与码流不一致时，严格实现按 SDP 预置解码器）。
 *
 * 现场（2026-11-24）：SBC→Linphone 的 offer 里 H.264 fmtp 是从 WebRTC 腿
 * 原样透传的 "level-asymmetry-allowed=1;packetization-mode=1;
 * profile-level-id=42001f"，而本机 x264（av_codec.c：profile=baseline,
 * level=3.1）实际发出的 SPS 是 67 42 C0 1F，即 profile-level-id=42c01f。
 * 本次抓包中 Linphone 收到后每 5s 发 SIP INFO picture_fast_update 要关键帧、
 * 视频始终不出图；SDP 宣告的 profile 与码流不一致是首要疑点。
 *
 * 只改 fmtp 中 "profile-level-id=" 后的 6 个十六进制字符（等长原地替换），
 * packetization-mode 等其余参数与其它行一律不动；非 H.264 的 fmtp 不动。
 * 只处理 Baseline 系（profile_idc=0x42）：Main/High 等码流若是对端透传的，
 * 改 profile 反而会与真实码流不符，故不动。
 * 返回改写的 fmtp 行数（0=无需改动）。
 *
 * ★ NSBC_H264_LOCAL_PROFILE_ID 必须与 mod/av/av_codec.c 的 x264 参数
 *   （profile=baseline / level=3.1）实际输出的 SPS 保持一致。 */
#define NSBC_H264_LOCAL_PROFILE_ID "42c01f"

static int sdp_name_is(const char *s, size_t n, const char *want)
{
    size_t wl = strlen(want);
    if (n < wl) return 0;
    for (size_t i = 0; i < wl; ++i) {
        char a = s[i], b = want[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

static int sdp_align_h264_profile(char *sdp, size_t len)
{
    if (sdp == NULL || len == 0) return 0;
    uint8_t h264_pt[128];
    memset(h264_pt, 0, sizeof(h264_pt));

    /* 第 1 遍：a=rtpmap:<pt> H264/90000 → 标记 H.264 的 PT */
    size_t pos = 0;
    while (pos < len) {
        const char *nl = memchr(sdp + pos, '\n', len - pos);
        size_t line_end = nl ? (size_t)(nl - sdp) : len;
        size_t ll = line_end - pos;
        if (ll > 9 && memcmp(sdp + pos, "a=rtpmap:", 9) == 0) {
            const char *q = sdp + pos + 9;
            int pt = atoi(q);
            const char *sp = memchr(q, ' ', ll - 9);
            if (sp != NULL && pt >= 0 && pt < 128) {
                size_t navail = (size_t)(sdp + line_end - (sp + 1));
                if (sdp_name_is(sp + 1, navail, "H264") &&
                    (navail == 4 || sp[1 + 4] == '/'))
                    h264_pt[pt] = 1;
            }
        }
        if (nl == NULL) break;
        pos = line_end + 1;
    }

    /* 第 2 遍：a=fmtp:<pt> ... profile-level-id=XXXXXX → 换成编码器实际值 */
    int fixed = 0;
    pos = 0;
    while (pos < len) {
        const char *nl = memchr(sdp + pos, '\n', len - pos);
        size_t line_end = nl ? (size_t)(nl - sdp) : len;
        size_t ll = line_end - pos;
        if (ll > 7 && memcmp(sdp + pos, "a=fmtp:", 7) == 0) {
            int pt = atoi(sdp + pos + 7);
            if (pt >= 0 && pt < 128 && h264_pt[pt]) {
                char *key = memmem(sdp + pos, ll, "profile-level-id=", 17);
                if (key != NULL) {
                    char *val = key + 17;
                    size_t vavail = (size_t)(sdp + line_end - val);
                    int hex6 = (vavail >= 6);
                    for (int i = 0; hex6 && i < 6; ++i)
                        if (!isxdigit((unsigned char)val[i])) hex6 = 0;
                    if (hex6 && vavail > 6 &&
                        isxdigit((unsigned char)val[6]))
                        hex6 = 0;  /* 非标准的长值，不动 */
                    /* 只对齐 Baseline 系（profile_idc=0x42） */
                    if (hex6 && !(val[0] == '4' && val[1] == '2'))
                        hex6 = 0;
                    if (hex6 &&
                        memcmp(val, NSBC_H264_LOCAL_PROFILE_ID, 6) != 0) {
                        memcpy(val, NSBC_H264_LOCAL_PROFILE_ID, 6);
                        ++fixed;
                    }
                }
            }
        }
        if (nl == NULL) break;
        pos = line_end + 1;
    }
    return fixed;
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
    /*  mixed-mode 出向音频 SSRC 预分配：answer SDP 要在此之前宣告 a=ssrc，
     *  故不能等到首包再随机分配。两方向都预置，值互不相同且非 0。 */
    r->plan_out_ssrc[0] = nsbc_ssrc_random();
    r->plan_out_ssrc[1] = nsbc_ssrc_random();
    if (r->plan_out_ssrc[0] == 0)
        r->plan_out_ssrc[0] = 0x4e534243u; /* "NSBC" */
    if (r->plan_out_ssrc[1] == 0 || r->plan_out_ssrc[1] == r->plan_out_ssrc[0])
        r->plan_out_ssrc[1] = r->plan_out_ssrc[0] ^ 0x00000001u;
    for (int i = 0; i < 2; ++i) {
        r->fd_rtp[i] = r->fd_rtcp[i] = -1;
        r->fd_vrtp[i] = r->fd_vrtcp[i] = -1;
        ssrc_policy_cfg_t pc = {0};
        ssrc_policy_init(&r->pol[i], &pc);
    }
    uint16_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    uint16_t v0 = 0, vc0 = 0, v1 = 0, vc1 = 0;
    int want_vid_a = (ports && (ports->want_video & 1)) != 0;
    int want_vid_b = (ports && (ports->want_video & 2)) != 0;
    int quad = 0;
    /*  A 腿视频端口：与 A 腿 audio 一起做连续四口分配（audio=r0/r0+1、
     *  video=r0+2/r0+3），满足 linphone 等 UA "video 挪到 audio+2" 的习惯；
     *  段内放不下连续四口时降级为仅分配音频对（视频与音频同端口）。
     *  quad 分配内部保证 video RTP 不越出 range_hi。 */
    if (want_vid_a) {
        if (ptable_alloc_quad_range(m->cfg.ptable, m->cfg.local_ip, r,
                                    range_lo, range_hi,
                                    &r0, &c0, &v0, &vc0) == NSBC_OK) {
            quad = 1;
            llog(LOG_INFO, "media",
                 "relay_start: quad #0 r0=%u c0=%u rv0=%u cv0=%u",
                 (unsigned)r0, (unsigned)c0, (unsigned)v0, (unsigned)vc0);
        } else {
            llog(LOG_WARN, "media",
                 "relay_start: alloc quad#0 FAILED (降级: 视频与音频同端口)");
            want_vid_a = 0;
        }
    }
    if (!quad) {
        llog(LOG_INFO, "media", "relay_start: before alloc pair #0");
        if (ptable_alloc_pair_range(m->cfg.ptable, m->cfg.local_ip, r,
                                    range_lo, range_hi, &r0, &c0) != NSBC_OK) {
            llog(LOG_ERR, "media", "relay_start: alloc #0 FAILED");
            free(r);
            return NSBC_NOSPC;
        }
        llog(LOG_INFO, "media", "relay_start: pair #0 r0=%u c0=%u", (unsigned)r0, (unsigned)c0);
    }
    llog(LOG_INFO, "media", "relay_start: before alloc pair #1");
    if (ptable_alloc_pair_range(m->cfg.ptable, m->cfg.local_ip, r,
                                range_lo, range_hi, &r1, &c1) != NSBC_OK) {
        llog(LOG_ERR, "media", "relay_start: alloc #1 FAILED, releasing #0");
        ptable_release_pair(m->cfg.ptable, m->cfg.local_ip, r0);
        if (want_vid_a)
            ptable_release_pair(m->cfg.ptable, m->cfg.local_ip, v0);
        free(r);
        return NSBC_NOSPC;
    }
    llog(LOG_INFO, "media", "relay_start: pair #1 r1=%u c1=%u", (unsigned)r1, (unsigned)c1);
    if (want_vid_b) {
        if (ptable_alloc_pair_range(m->cfg.ptable, m->cfg.local_ip, r,
                                    range_lo, range_hi, &v1, &vc1) != NSBC_OK) {
            llog(LOG_WARN, "media", "relay_start: alloc video#1 FAILED (降级: 视频与音频同端口)");
            want_vid_b = 0;
        } else {
            llog(LOG_INFO, "media", "relay_start: video#1 rv1=%u cv1=%u", (unsigned)v1, (unsigned)vc1);
        }
    }
    r->rtp_port[0] = r0;
    r->rtp_port[1] = r1;
    r->vrtp_port[0] = want_vid_a ? v0 : 0;
    r->vrtp_port[1] = want_vid_b ? v1 : 0;
    /* P0 relay 纯 IPv4：dualstack=0 避免 v4-mapped family 不匹配
     * （recvfrom 返回 sockaddr_in6 但 peer 为 sockaddr_in → nsbc_sa_eq_addr
     * family 比较直接失败，所有 RTP 被 drop）。 */
    net_udp_cfg_t uc = {0, 0, 0, 0, 0};
    llog(LOG_INFO, "media", "relay_start: before udp_open rtp0");
    r->fd_rtp[0] = net_udp_open(m->cfg.local_ip, r0, &uc);
    llog(LOG_INFO, "media", "relay_start: rtp0 fd=%d", r->fd_rtp[0]);
    r->fd_rtcp[0] = net_udp_open(m->cfg.local_ip, c0, &uc);
    llog(LOG_INFO, "media", "relay_start: rtcp0 fd=%d", r->fd_rtcp[0]);
    if (want_vid_a) {
        r->fd_vrtp[0] = net_udp_open(m->cfg.local_ip, v0, &uc);
        r->fd_vrtcp[0] = net_udp_open(m->cfg.local_ip, vc0, &uc);
        llog(LOG_INFO, "media", "relay_start: vrtp0 fd=%d vrtcp0 fd=%d",
             r->fd_vrtp[0], r->fd_vrtcp[0]);
    }
    r->fd_rtp[1] = net_udp_open(m->cfg.local_ip, r1, &uc);
    llog(LOG_INFO, "media", "relay_start: rtp1 fd=%d", r->fd_rtp[1]);
    r->fd_rtcp[1] = net_udp_open(m->cfg.local_ip, c1, &uc);
    llog(LOG_INFO, "media", "relay_start: rtcp1 fd=%d", r->fd_rtcp[1]);
    if (want_vid_b) {
        r->fd_vrtp[1] = net_udp_open(m->cfg.local_ip, v1, &uc);
        r->fd_vrtcp[1] = net_udp_open(m->cfg.local_ip, vc1, &uc);
        llog(LOG_INFO, "media", "relay_start: vrtp1 fd=%d vrtcp1 fd=%d",
             r->fd_vrtp[1], r->fd_vrtcp[1]);
    }
    if (r->fd_rtp[0] < 0 || r->fd_rtp[1] < 0) {
        llog(LOG_WARN, "media",
             "relay_start: UDP open FAIL rtp0=%d rtp1=%d",
             r->fd_rtp[0], r->fd_rtp[1]);
        rtp_relay_stop(r);
        return NSBC_ERR;
    }
    /*  视频 socket 打开失败 → 该腿降级为与音频共用端口，
     *  绝不能广告一个没有 socket 的视频端口。 */
    for (int i = 0; i < 2; ++i) {
        int want = (i == 0) ? want_vid_a : want_vid_b;
        if (want && r->fd_vrtp[i] < 0) {
            llog(LOG_WARN, "media",
                 "relay_start: video socket#%d open FAIL → 降级共用音频端口", i);
            if (r->fd_vrtcp[i] >= 0) {
                close(r->fd_vrtcp[i]);
                r->fd_vrtcp[i] = -1;
            }
            ptable_release_pair(m->cfg.ptable, m->cfg.local_ip, r->vrtp_port[i]);
            r->vrtp_port[i] = 0;
            if (i == 0) want_vid_a = 0; else want_vid_b = 0;
        }
    }
    llog(LOG_DEBUG, "media",
         "relay_start: OK r0=%d r1=%d fd0=%d fd1=%d local_ip=%s (defer arm)",
         r0, r1, r->fd_rtp[0], r->fd_rtp[1],
         m->cfg.local_ip ? m->cfg.local_ip : "(null)");
    /*  RACE-FIX: 延迟 reactor_add — 先构造 relay（open fd + alloc port），
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
    ports->a_vrtp = r->vrtp_port[0];
    ports->a_vrtcp = want_vid_a ? vc0 : 0;
    ports->b_vrtp = r->vrtp_port[1];
    ports->b_vrtcp = want_vid_b ? vc1 : 0;
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

    /* DEBUG 2026-11-13: 诊断 reactor_add 是否真的执行 + 返回值 */
    llog(LOG_NOTICE, "media",
         "relay_batch_arm_fn ENTER r=%p target=%p home_reactor=%p armed=%d "
         "fd_rtp[0]=%d fd_rtp[1]=%d fd_rtcp[0]=%d fd_rtcp[1]=%d",
         (void *)r, (void *)target, (void *)r->home_reactor, r->armed,
         r->fd_rtp[0], r->fd_rtp[1], r->fd_rtcp[0], r->fd_rtcp[1]);

    int rc;
    int ok_count = 0;
    int ok_fds[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };  /* 记录哪些 fd 注册成功用于回滚 */

    rc = reactor_add(target, r->fd_rtp[0], EPOLLIN, relay_cb, r);
    llog(LOG_NOTICE, "media", "reactor_add rtp[0]=%d rc=%d", r->fd_rtp[0], rc);
    if (rc != NSBC_OK) goto rollback;
    ok_fds[ok_count++] = r->fd_rtp[0];

    rc = reactor_add(target, r->fd_rtp[1], EPOLLIN, relay_cb, r);
    llog(LOG_NOTICE, "media", "reactor_add rtp[1]=%d rc=%d", r->fd_rtp[1], rc);
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
    /* 明文腿的独立视频 RTP/RTCP 通道 */
    for (int i = 0; i < 2; ++i) {
        if (r->fd_vrtp[i] >= 0) {
            rc = reactor_add(target, r->fd_vrtp[i], EPOLLIN, relay_cb, r);
            if (rc != NSBC_OK) goto rollback;
            ok_fds[ok_count++] = r->fd_vrtp[i];
        }
        if (r->fd_vrtcp[i] >= 0) {
            rc = reactor_add(target, r->fd_vrtcp[i], EPOLLIN, relay_cb, r);
            if (rc != NSBC_OK) goto rollback;
            ok_fds[ok_count++] = r->fd_vrtcp[i];
        }
    }

    r->armed = 1;
    return NSBC_OK;

rollback:
    llog(LOG_WARN, "media",
         "relay_batch_arm: reactor_add failed rc=%d — rolling back %d succeeded",
         rc, ok_count);
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
    for (int i = 0; i < 2; ++i) {
        if (r->fd_vrtp[i] >= 0)
            reactor_del(target, r->fd_vrtp[i]);
        if (r->fd_vrtcp[i] >= 0)
            reactor_del(target, r->fd_vrtcp[i]);
    }
    return NSBC_OK;
}

/*  RACE-FIX: 延迟 arm — 所有 dtls 创建 + admit_ssrc 完成后才注册 reactor fd。
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

    /* P0-3 RACE-FIX: armed 提前清 0 做幂等 guard —— 防止第二次调用
     * （通过 DTLS 降级路径或 bridge_release_call 重入）再次进 reactor_call
     * 或 reactor_del 路径，操作已 close/free 的资源。 */
    if (r->armed == 0 && r->dead) {
        llog(LOG_WARN, "media", "relay_stop: SKIP (already stopped) r=%p", (void *)r);
        return;
    }
    r->armed = 0;

    /*  P0-2 RACE-FIX: 先置 dead=1 再 reactor_call。
     * relay_recv_one for(;;) 循环每次迭代检查 dead，检测到立即 return。
     * 这样 reactor_call 返回时即使已进入的 relay_recv_one 还在内核队列里有
     * 残留数据包，也会在下一次迭代迅速退出，不会在 bridge_release_call free(r/e)
     * 后继续访问 r->node 等成员。 */
    __atomic_store_n(&r->dead, 1, __ATOMIC_RELEASE);

    /* P3: 通过 reactor_call 在 reactor owner 线程里批量删除所有 fd。
     * reactor_call 同步等待——owner 执行完 reactor_del 才返回，
     * 之后 close fd / free relay 安全（reactor 不会再触发 relay_cb）。
     *
     * ★ P0-3 BUGFIX: reactor_call 失败时（task_closing 重载 / !task_started /
     * 队列满）fallback 直接 reactor_del —— 不能跳过！否则 reactor slots 里
     * 还保留着 cb/arg 指向本 relay，free(r) 后 reactor 继续 dispatch → UAF！ */
    int need_fallback = 0;
    if (r->home_reactor && m && m->cfg.mgr) {
        int rc = reactor_call(r->home_reactor, relay_batch_stop_fn, r);
        if (rc != NSBC_OK) {
            llog(LOG_WARN, "media",
                 "relay_stop: reactor_call rc=%d — fallback direct reactor_del", rc);
            need_fallback = 1;
        }
    } else if (r->home_reactor) {
        /* 单 reactor 模式，home_reactor 就是 m->cfg.re */
        int rc = reactor_call(r->home_reactor, relay_batch_stop_fn, r);
        if (rc != NSBC_OK) {
            llog(LOG_WARN, "media",
                 "relay_stop: reactor_call rc=%d — fallback direct reactor_del", rc);
            need_fallback = 1;
        }
    } else {
        /* 无 home_reactor — reactor 还没 start 或已销毁，直接删 */
        need_fallback = 1;
    }
    if (need_fallback && m && m->cfg.re) {
        for (int i = 0; i < 2; ++i) {
            if (r->fd_rtp[i] >= 0)
                reactor_del(m->cfg.re, r->fd_rtp[i]);
            if (r->fd_rtcp[i] >= 0)
                reactor_del(m->cfg.re, r->fd_rtcp[i]);
            if (r->fd_vrtp[i] >= 0)
                reactor_del(m->cfg.re, r->fd_vrtp[i]);
            if (r->fd_vrtcp[i] >= 0)
                reactor_del(m->cfg.re, r->fd_vrtcp[i]);
        }
    } else if (need_fallback && r->home_reactor) {
        /* reactor_call 失败但 home_reactor 还在 —— 直接调 reactor_del（非 owner
         * 线程路径，生产构建 owner check 关闭；此时 reactor 大概率也在 shutdown） */
        for (int i = 0; i < 2; ++i) {
            if (r->fd_rtp[i] >= 0)
                reactor_del(r->home_reactor, r->fd_rtp[i]);
            if (r->fd_rtcp[i] >= 0)
                reactor_del(r->home_reactor, r->fd_rtcp[i]);
            if (r->fd_vrtp[i] >= 0)
                reactor_del(r->home_reactor, r->fd_vrtp[i]);
            if (r->fd_vrtcp[i] >= 0)
                reactor_del(r->home_reactor, r->fd_vrtcp[i]);
        }
    }

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
        if (r->fd_vrtp[i] >= 0) {
            close(r->fd_vrtp[i]);
            r->fd_vrtp[i] = -1;
        }
        if (r->fd_vrtcp[i] >= 0) {
            close(r->fd_vrtcp[i]);
            r->fd_vrtcp[i] = -1;
        }
        if (r->rtp_port[i])
            ptable_release_pair(m->cfg.ptable, m->cfg.local_ip,
                                r->rtp_port[i]);
        if (r->vrtp_port[i])
            ptable_release_pair(m->cfg.ptable, m->cfg.local_ip,
                                r->vrtp_port[i]);
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

/*  视频 m-line 的对端地址（明文腿专用）。与 set_peer_a/b 分开记录，
 *  避免 audio 的端口被当成 video 的目标端口（反之亦然）。 */
void rtp_relay_set_peer_video(rtp_relay_t *r, int side, const nsbc_sa_t *sa)
{
    if (!r || side < 0 || side > 1 || !sa) return;
    r->vpeer[side] = *sa;
    r->vhave_peer[side] = 1;
}

/*  HA P3-2: failover rebuild 预填 SSRC 重写映射
 * 让 STANDBY→ACTIVE 新建的 relay 对同一路 UA 发同样的 out SSRC —
 * UA 侧（软电话/WebRTC）不感知 SSRC 漂移，RTP session 不 drop。
 *
 * 预填只占 map[side].e[0] 一个槽（hint 指它），后续自动学习的新 SSRC
 * 会占 e[1..15]，不冲突。
 *
 * ⚠️ 必须在 rtp_relay_arm 之前调用 — 否则 relay thread 并发读写 smap。 */
void rtp_relay_preset_ssrc(rtp_relay_t *r, int side,
                           uint32_t in_ssrc, uint32_t out_ssrc)
{
    if (!r || side < 0 || side > 1 || !in_ssrc || !out_ssrc) return;
    smap_t *m = &r->map[side];
    m->e[0].used = 1;
    m->e[0].in   = in_ssrc;
    m->e[0].out  = out_ssrc;
    m->hint = 0;
    /* OAM view 也反映预填值 — UA-A 发来的 SSRC 是 in_ssrc；
     * relay 发往对侧的是 out_ssrc */
    __atomic_store_n(&r->d_in_ssrc[side],  in_ssrc,  __ATOMIC_RELEASE);
    __atomic_store_n(&r->d_out_ssrc[side], out_ssrc, __ATOMIC_RELEASE);
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

/*  HA P3-3: failover rebuild — 把 ACTIVE 侧 SRTP session 挂进新 relay
 * 调用者（b2b_repl_rebuild_from_buf）保证 side ∈ [0,1] 且在 arm relay 之前 */
void rtp_relay_set_srtp(rtp_relay_t *r, int side, nsbc_srtp_sess_t *sr)
{
    if (!r || side < 0 || side > 1) return;
    r->srtp[side] = sr;
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

/*  RFC1918/CGNAT/回环判断 —— 用于"对端 SDP 宣告的是私网地址，服务器不可达"
 *  的兜底（offer_hook 里改用信令源 IP 作为 RTP 目标）。 */
static int mb_ipv4_is_private(const char *ip)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!ip || !ip[0])
        return 0;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    if (a == 0 || a == 10 || a == 127)
        return 1;
    if (a == 100 && b >= 64 && b <= 127)   /* CGNAT 100.64/10 */
        return 1;
    if (a == 169 && b == 254)              /* link-local */
        return 1;
    if (a == 172 && b >= 16 && b <= 31)
        return 1;
    if (a == 192 && b == 168)
        return 1;
    return 0;
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
    e->xcode_src_pt[0] = e->xcode_src_pt[1] = 0xFF;
    e->xcode_dst_pt[0] = e->xcode_dst_pt[1] = 0xFF;
    e->neg_a_pt = e->neg_b_pt = 0xFF;
    e->dir[0] = e->dir[1] = SDP_DIR_SENDRECV;
    e->next = s->head;
    s->head = e;
    pthread_mutex_unlock(&s->lock);
    return e;
}

/* ------------------------------------------------------------------ */
/* 每腿独立 SDP 终结（第 1 项）                                          */
/* ------------------------------------------------------------------ */
/*  SBC 作为 B2BUA，对每条腿独立终结 SDP，而不是把一腿的 fmt-list 原样
 *  丢给另一腿：
 *   1) offer → B：在 A 的 m=audio fmt-list 尾部追加 SBC 注册表里
 *      "既能解码又能编码"的静态 PT（A 未提供的），让 B 有选择 SBC 支持
 *      codec 的机会（A=G729-only、B=PCMA-only 时转码才可能成立）；
 *   2) answer → A：只向 A 宣告定型后的 A 侧音频 codec 与 A 自己的
 *      telephone-event PT，绝不把 B 腿的 PT 泄漏给 A（否则 A 收到
 *      未 offer 过的 PT 会拒绝 SDP → 488/BYE）。
 *  视频：编码名与 PT 完全一致才保留；否则视频段 port=0 降级纯音频
 *  （视频转码尚未实现，不能伪造 PT）。BYPASS 完全不改写。 */
#define SDP_XC_MAXPT 32   /* 2026-11-24 从 16 提到 32：WebRTC m=video 可有 20+ 个 PT
                              (含 rtx/red/ulpfec 等冗余 codec)，sdp_xc_parse 截断到 16 会
                              把 VP9 PT=98 这类 codec 漏掉 → present 检查未命中 →
                              sdp_xc_add_offer_audio 追加一个新 PT=98（fmt-list 重复），
                              Linphone answer 里整条 m=video section 消失。 */

typedef struct {
    int found;      /* 是否存在该类型媒体段 */
    int port;       /* m= 端口（0=被拒绝） */
    int n;
    struct {
        int  pt;
        char enc[24];
        char rate[12];
        int  ch;
        char fmtp[512];  /* H.265 sprop-vps/sps/pps 较长，96 已不够 */
        int  te;    /* telephone-event */
    } c[SDP_XC_MAXPT];
} sdp_xc_mline_t;

/* 前置声明：定义在文件后部（bridge_confirm 附近），此处终结逻辑复用 */
static int sdp_extract_audio_codecs(const char *sdp, size_t len,
                                    uint8_t *out_pts, codec_id_t *out_cids,
                                    int out_cap);
static uint8_t find_dtmf_pt(const char *sdp, size_t len);

static void tok_copy(char *dst, size_t cap, sip_tok_t t)
{
    if (dst == NULL || cap == 0) return;
    size_t n = t.p ? t.len : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, t.p, n);
    dst[n] = '\0';
}

/* 编码名归一化比较：忽略大小写与 '.'/'-'/'_'（G.722 == g722 等） */
static char xc_norm_ch(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == '.' || c == '-' || c == '_') return 0;
    return c;
}

static int xc_enc_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return 0;
    for (;;) {
        char ca = 0, cb = 0;
        while (*a && (ca = xc_norm_ch(*a)) == 0) ++a;
        while (*b && (cb = xc_norm_ch(*b)) == 0) ++b;
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
        ++a;
        ++b;
    }
}

/* 解析指定类型的第一个媒体段（含 codecs 构建） */
static void sdp_xc_parse(const char *sdp, size_t len, sdp_mtype_t type,
                         sdp_xc_mline_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sdp == NULL || len == 0) return;
    sdp_t s;
    memset(&s, 0, sizeof(s));
    if (sdp_parse(&s, sdp, len) != NSBC_OK) return;
    for (int i = 0; i < s.nmedia; ++i) {
        sdp_media_t *m = &s.media[i];
        if (m->type != type) continue;
        out->found = 1;
        out->port = m->port;
        if (!m->codecs_built)
            sdp_media_build_codecs(&s, m);
        for (int j = 0; j < m->ncodecs && out->n < SDP_XC_MAXPT; ++j) {
            sdp_codec_t *c = &m->codecs[j];
            int k = out->n++;
            out->c[k].pt = c->pt;
            tok_copy(out->c[k].enc, sizeof(out->c[k].enc), c->enc);
            tok_copy(out->c[k].rate, sizeof(out->c[k].rate), c->rate);
            out->c[k].ch = 0;
            if (c->channels.p && c->channels.len) {
                char tmp[8];
                tok_copy(tmp, sizeof(tmp), c->channels);
                out->c[k].ch = atoi(tmp);
            }
            tok_copy(out->c[k].fmtp, sizeof(out->c[k].fmtp), c->fmtp);
            out->c[k].te = c->telephone_event;
        }
        return;
    }
}

/* 视频 codec 是否可双向处理：目前只支持 H.264 / VP9（video_xcode 组合）。
 * 返回 CODEC_NONE 表示该编码名不能作为视频转码的一端。 */
static codec_id_t xc_video_cid(const char *enc)
{
    if (enc == NULL || enc[0] == '\0') return CODEC_NONE;
    size_t nlen = strlen(enc);
    const codec_ops_t *ops = codec_find_by_name_kind(enc, nlen, CODEC_KIND_VIDEO);
    if (ops == NULL) {
        /*  兜底：按名称查一次再校验类别。
         * 部分第三方 SDP 的 rtpmap 名与注册表 sdp_name 不完全一致（别名/大小写），
         * 且按 kind 查找会因 kind 未填而漏掉，这里补一条不限 kind 的查找。 */
        const codec_ops_t *o2 = codec_registry_find_name(enc, nlen);
        if (o2 && o2->kind == CODEC_KIND_VIDEO) ops = o2;
    }
    if (ops == NULL) return CODEC_NONE;
    if (ops->id != CODEC_H264 && ops->id != CODEC_VP9 &&
        ops->id != CODEC_VP8 && ops->id != CODEC_H265)
        return CODEC_NONE;
    /* H.265：常见于“只透传 / 仅录制（H265→H264）”场景，只需要解码器；
     * 是否具备编码器由具体转码方向在 video_xcode_create 时决定并记录告警。 */
    if (ops->id == CODEC_H265)
        return codec_can_decode(ops->id) ? ops->id : CODEC_NONE;
    if (!codec_can_decode(ops->id) || !codec_can_encode(ops->id))
        return CODEC_NONE;
    return ops->id;
}

typedef struct {
    int        ok;
    int        port;
    int        pt;
    codec_id_t cid;
    char       enc[24];
    char       rate[12];
    char       fmtp[512];  /* H.265 sprop-* 可能较长 */
} sdp_vsel_t;

/* 从 SDP 的视频段里挑选第一个 SBC 可双向处理的 codec（H264/VP9）。 */
static int sdp_video_select(const char *sdp, size_t len, sdp_vsel_t *out)
{
    if (out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    sdp_xc_mline_t vm;
    sdp_xc_parse(sdp, len, SDP_M_VIDEO, &vm);
    if (!vm.found) return 0;
    out->port = vm.port;
    for (int i = 0; i < vm.n; ++i) {
        if (vm.c[i].te) continue;
        codec_id_t cid = xc_video_cid(vm.c[i].enc);
        if (cid == CODEC_NONE) continue;
        out->ok   = 1;
        out->pt   = vm.c[i].pt;
        out->cid  = cid;
        snprintf(out->enc, sizeof(out->enc), "%s", vm.c[i].enc);
        snprintf(out->rate, sizeof(out->rate), "%s",
                 vm.c[i].rate[0] ? vm.c[i].rate : "90000");
        snprintf(out->fmtp, sizeof(out->fmtp), "%s", vm.c[i].fmtp);
        return 1;
    }
    return 0;
}

/* 从 m= 行里定位 fmt-list 起点（跳过 "<type> <port> <proto>" 三个字段） */
static const char *xc_mline_fmt_start(const char *p, size_t cl)
{
    const char *end = p + cl;
    const char *q = p + 2;
    int field = 0;
    while (q < end && field < 3) {
        while (q < end && *q != ' ' && *q != '\t') ++q;
        if (q >= end) return end;
        ++field;
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
    }
    return q;
}

/* 定位 m= 行的 port 字段 [start,end) */
static void xc_mline_port_span(const char *p, size_t cl,
                               const char **ps, const char **pe)
{
    const char *end = p + cl;
    const char *q = p + 2;
    while (q < end && *q != ' ' && *q != '\t') ++q;   /* type */
    while (q < end && (*q == ' ' || *q == '\t')) ++q;
    *ps = q;
    while (q < end && *q >= '0' && *q <= '9') ++q;
    *pe = q;
}

/* offer → B：追加 SBC 可双向处理的静态音频 PT，以及视频 H264/VP9 动态 PT。
 * 视频两侧 codec 不同时由 video_xcode 转码（VP9↔H264）。
 * 返回追加个数（0=未改动，缓冲区不动）。 */
static int sdp_xc_add_offer_audio(char *sdp, size_t *lenp, size_t cap)
{
    if (sdp == NULL || lenp == NULL || *lenp == 0) return 0;
    size_t len = *lenp;
    sdp_xc_mline_t am, vm;
    sdp_xc_parse(sdp, len, SDP_M_AUDIO, &am);
    sdp_xc_parse(sdp, len, SDP_M_VIDEO, &vm);

    int add_pt[SDP_XC_MAXPT];
    int nadd = 0;
    /*  只追加 RFC 3551 静态音频 PT 的保守白名单，且必须在 registry 中
     *  "可解码 + 可编码"。不扫全静态区间——避免把 G722/16000 之类
     *  SDP 时钟与采样率不一致的 codec 强塞给对端。
     *
     *  ★ 顺序与取舍很关键：PCMA 优先，且 PCMA 可用时不再追加 PCMU。
     *  典型故障（2026-11-22 实测）：offer 尾部是 "0 8" 时，Chrome 对
     *  G.711 的偏好会选 PCMU(0)；而本仓库 WebRTC 页的 codec 白名单只保留
     *  PCMA 并据此改写"信令里的 answer"→ 信令宣告 8(PCMA)、实际媒体却是
     *  0(PCMU)。SBC 按 PCMA 建的转码器永远不命中，未协商的 PT=0 被原样
     *  转发给 G729 对端 → 对方完全静音（本端却能听到对方）。
     *  只给 PCMA 可从源头消除这个二义性：所有 G.711 端点都支持 PCMA。 */
    static const int cand[] = { 8 /* PCMA */, 18 /* G729 */, 0 /* PCMU */ };
    int have_pcma = 0;
    if (am.found && am.port > 0) {
        for (size_t ci = 0; ci < sizeof(cand) / sizeof(cand[0]); ++ci) {
            int pt = cand[ci];
            const codec_ops_t *ops = codec_find_by_pt_kind(pt, CODEC_KIND_AUDIO);
            if (!ops) continue;
            if (!codec_can_decode(ops->id) || !codec_can_encode(ops->id))
                continue;
            /*  PCMA 已可协商 → 不再给对端 PCMU，避免它选到 0。 */
            if (pt == 0 && have_pcma) continue;
            int present = 0;
            for (int j = 0; j < am.n; ++j) {
                if (am.c[j].pt == pt ||
                    xc_enc_eq(am.c[j].enc, ops->name) ||
                    (ops->sdp_name && xc_enc_eq(am.c[j].enc, ops->sdp_name))) {
                    present = 1;
                    break;
                }
            }
            if (pt == 8) have_pcma = 1;
            if (!present)
                add_pt[nadd++] = pt;
        }
    }

    /* 视频候选：H264 / VP9（video_xcode 支持的两个方向） */
    int add_vpt[4];
    codec_id_t add_vcid[4];
    int nvadd = 0;
    if (vm.found && vm.port > 0) {
        /* 扫原始 m=video 行所有 PT token（跳过 "m=video <port> <proto>" 后），
         * 构建全量 used PT 位图，弥补 vm.c[] 截断（SDP_XC_MAXPT 上限）的漏洞。
         * 这是 present 命中 + PT used 检查的主数据源。 */
        int video_used[128] = {0};  /* PT 范围 0..127 */
        const char *scan = sdp;
        int safety1 = 0;
        while (scan < sdp + len && safety1++ < 4096) {
            if ((size_t)(sdp + len - scan) >= 5 &&
                memcmp(scan, "m=vid", 5) == 0) break;  /* 找到第一个 m=video  */
            scan = memchr(scan, '\n', (size_t)(sdp + len - scan));
            if (!scan) break;
            ++scan;
        }
        if (scan && (size_t)(sdp + len - scan) >= 5) {
            /* scan 指向 "m=video ..." 行 */
            const char *lend = memchr(scan, '\n', (size_t)(sdp + len - scan));
            if (!lend) lend = sdp + len;
            const char *tok = scan;
            /* 跳过前三个 token: "m=video", port, proto */
            for (int skip = 0; skip < 3; ++skip) {
                const char *sp = memchr(tok, ' ', (size_t)(lend - tok));
                if (!sp) break;
                tok = sp + 1;
            }
            /* 扫剩余的 PT token，全部标记 used */
            int safety2 = 0;
            while (tok < lend && safety2++ < 128) {
                while (tok < lend && (*tok == ' ' || *tok == '\r' || *tok == '\n')) ++tok;
                if (tok >= lend) break;
                const char *next = tok;
                while (next < lend && *next != ' ' && *next != '\r' && *next != '\n') ++next;
                if (next == tok) break;  /* 零长度 token → 死循环保护 */
                char pend = *next;
                *(char *)next = '\0';
                int pt_val = atoi(tok);
                *(char *)next = pend;
                if (pt_val >= 0 && pt_val < 128) video_used[pt_val] = 1;
                tok = next;
            }
        }
        char vubuf[256] = ""; size_t vubp = 0;
        for (int j = 0; j < 128 && vubp < 250; j++)
            if (video_used[j])
                vubp += snprintf(vubuf + vubp, sizeof(vubuf) - vubp,
                                 "%s%d", vubp ? "," : "", j);
        llog(LOG_INFO, "media", "sdp_xc_add: vm.n=%d vm.port=%d video_used=[%s]",
             vm.n, vm.port, vubuf);

        static const codec_id_t vcand[] = { CODEC_H264, CODEC_VP8, CODEC_VP9 };
        for (size_t ci = 0; ci < sizeof(vcand) / sizeof(vcand[0]); ++ci) {
            const codec_ops_t *ops = codec_registry_find(vcand[ci]);
            if (!ops || ops->kind != CODEC_KIND_VIDEO) continue;
            if (!codec_can_decode(ops->id) || !codec_can_encode(ops->id))
                continue;
            int present = 0;
            for (int j = 0; j < vm.n; ++j) {
                if (xc_enc_eq(vm.c[j].enc, ops->name) ||
                    (ops->sdp_name && xc_enc_eq(vm.c[j].enc, ops->sdp_name))) {
                    present = 1;
                    break;
                }
            }
            llog(LOG_INFO, "media", "sdp_xc_add: vcand[%zu] id=%d name='%s' present=%d",
                 ci, (int)ops->id, ops->name ? ops->name : "(null)", present);
            if (present) continue;

            /* 选一个未使用的动态 PT（96..127），用 video_used 全量位图 */
            int pt = -1;
            for (int cand_pt = CODEC_PT_DYN_MIN; cand_pt <= 127; ++cand_pt) {
                if (video_used[cand_pt]) continue;
                int dup = 0;
                for (int j = 0; j < nvadd; ++j) if (add_vpt[j] == cand_pt) dup = 1;
                for (int j = 0; j < am.n; ++j) if (am.c[j].pt == cand_pt) dup = 1;
                if (!dup) { pt = cand_pt; break; }
            }
            if (pt < 0) break;
            llog(LOG_INFO, "media", "sdp_xc_add: append video codec id=%d name='%s' pt=%d",
                 (int)ops->id, ops->name, pt);
            video_used[pt] = 1;
            add_vcid[nvadd] = ops->id;
            add_vpt[nvadd++] = pt;
        }
    }

    if (nadd == 0 && nvadd == 0) return 0;
    llog(LOG_INFO, "media", "sdp_xc_add_offer_audio: ENTER nadd=%d nvadd=%d len=%zu",
         nadd, nvadd, len);

    char *nb = (char *)malloc(cap + 1);
    if (nb == NULL) return 0;
    size_t used = 0;
    const char *p = sdp, *end = sdp + len;
    int overflow = 0;
    int loop_safety = 0;
    /*  追加的 a=rtpmap/a=fmtp 必须落到媒体段末尾，不能紧跟 m= 行。
     *  RFC 4566 的媒体描述顺序是 m= → i= → c= → b= → k= → a=，
     *  紧跟 m= 输出会得到 "m=audio .../a=rtpmap:18/c=IN IP4 ..." 的非法
     *  顺序（2026-11-24 实测：Linphone/belle-sip 只认 c= 之前的属性，
     *  于是只回 PT=18、video 段整个消失，且因远端地址解析失败而不发 RTP）。
     *  这里先缓存，遇到下一个 m= 或 SDP 结束再输出。 */
    char pend[2048];
    size_t pendl = 0;
    while (p < end && loop_safety++ < 65536) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t ll = (size_t)(le - p);
        size_t cl = ll;
        while (cl > 0 && p[cl - 1] == '\r') --cl;
        const int is_m = (cl >= 2 && p[0] == 'm' && p[1] == '=');
        const int is_audio_m =
            is_m && cl >= 8 && memcmp(p + 2, "audio", 5) == 0 &&
            (p[7] == ' ' || p[7] == '\t');
        const int is_video_m =
            is_m && cl >= 8 && memcmp(p + 2, "video", 5) == 0 &&
            (p[7] == ' ' || p[7] == '\t');

        /* 段尾冲刷：上一个媒体段的追加属性在这里落盘 */
        if (is_m && pendl > 0) {
            if (put_line(nb, cap, &used, pend, pendl) != NSBC_OK) {
                overflow = 1;
                break;
            }
            pendl = 0;
        }

        if (is_audio_m && nadd > 0) {
            /* parse + 去重 + 重建 fmt-list，与 video 分支保持一致的防御性处理 */
            char rebuilt[1024]; size_t rb = 0;
            int first_tok = 1;
            const char *tok = p, *line_end = p + cl;
            for (int skip = 0; skip < 3 && tok < line_end; ++skip) {
                while (tok < line_end && *tok == ' ') ++tok;
                if (tok >= line_end) break;
                const char *next = tok;
                while (next < line_end && *next != ' ') ++next;
                if (!first_tok) rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, " ");
                rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, "%.*s",
                               (int)(next - tok), tok);
                tok = next;
                first_tok = 0;
            }
            int seen[128] = {0};
            int safety3 = 0;
            while (tok < line_end && safety3++ < 128) {
                while (tok < line_end && (*tok == ' ' || *tok == '\r' || *tok == '\n')) ++tok;
                if (tok >= line_end) break;
                const char *next = tok;
                while (next < line_end && *next != ' ' && *next != '\r' && *next != '\n') ++next;
                if (next == tok) break;
                char pend = *next;
                *(char *)next = '\0';
                int pv = atoi(tok);
                *(char *)next = pend;
                if (pv >= 0 && pv < 128 && !seen[pv]) {
                    seen[pv] = 1;
                    rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, " %d", pv);
                }
                tok = next;
            }
            for (int i = 0; i < nadd; ++i) {
                if (seen[add_pt[i]]) continue;
                seen[add_pt[i]] = 1;
                rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, " %d", add_pt[i]);
            }
            rebuilt[sizeof(rebuilt) - 1] = '\0';
            if (put_line(nb, cap, &used, rebuilt, rb) != NSBC_OK) {
                overflow = 1;
                break;
            }
            if (overflow) break;
            if (put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                overflow = 1;
                break;
            }
            /* 在段内补 rtpmap（追加格式项必须有 rtpmap，否则对端无法识别）。
             * 先缓存到 pend，等本段 c=/a= 行输出完，下一个 m= 前再写。 */
            for (int i = 0; i < nadd; ++i) {
                const codec_ops_t *ops =
                    codec_find_by_pt_kind(add_pt[i], CODEC_KIND_AUDIO);
                if (!ops) continue;
                const char *nm = ops->sdp_name ? ops->sdp_name : ops->name;
                /* G.722 等：SDP 时钟率必须用 RTP 时钟率（8000），
                 * 不能用 PCM 采样率（16000），否则写出非标准 G722/16000 */
                int rate = codec_rtp_clock_rate(ops);
                int an = snprintf(pend + pendl, sizeof(pend) - pendl,
                                  "a=rtpmap:%d %s/%d\r\n",
                                  add_pt[i], nm, rate);
                if (an <= 0 || (size_t)an >= sizeof(pend) - pendl) {
                    overflow = 1;
                    break;
                }
                pendl += (size_t)an;
            }
            if (overflow) break;
            p = eol ? eol + 1 : end;
            continue;
        }

        if (is_video_m && nvadd > 0) {
            /* 先 parse 原始 m=video 行，重建去重后的 fmt-list，
             * 再追加 add_vpt（跳过已存在的），**绝对不允许重复 PT 出现在输出里**。
             * 这是最终防御——任何上游（vcand 循环、原始 SDP）导致的重复在这里被抹平。 */
            char rebuilt[1024]; size_t rb = 0;
            int first_tok = 1;
            const char *tok = p, *line_end = p + cl;
            /* 1. 先复制前三个 token（m=video、port、proto）不去重 */
            for (int skip = 0; skip < 3 && tok < line_end; ++skip) {
                while (tok < line_end && *tok == ' ') ++tok;
                if (tok >= line_end) break;
                const char *next = tok;
                while (next < line_end && *next != ' ') ++next;
                if (!first_tok) rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, " ");
                rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, "%.*s",
                               (int)(next - tok), tok);
                tok = next;
                first_tok = 0;
            }
            /* 2. 去重复制原始 fmt-list PT */
            int seen2[128] = {0};
            int safety4 = 0;
            while (tok < line_end && safety4++ < 128) {
                while (tok < line_end && (*tok == ' ' || *tok == '\r' || *tok == '\n')) ++tok;
                if (tok >= line_end) break;
                const char *next = tok;
                while (next < line_end && *next != ' ' && *next != '\r' && *next != '\n') ++next;
                if (next == tok) break;
                char pend = *next;
                *(char *)next = '\0';
                int pv = atoi(tok);
                *(char *)next = pend;
                if (pv >= 0 && pv < 128 && !seen2[pv]) {
                    seen2[pv] = 1;
                    rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, " %d", pv);
                }
                tok = next;
            }
            /* 3. 追加 add_vpt 里未出现的 */
            for (int i = 0; i < nvadd; ++i) {
                if (seen2[add_vpt[i]]) continue;  /* 原始 fmt-list 里已经有了，跳过 */
                seen2[add_vpt[i]] = 1;
                rb += snprintf(rebuilt + rb, sizeof(rebuilt) - rb, " %d", add_vpt[i]);
            }
            rebuilt[sizeof(rebuilt) - 1] = '\0';
            if (put_line(nb, cap, &used, rebuilt, rb) != NSBC_OK) {
                overflow = 1;
                break;
            }
            if (overflow) break;
            if (put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                overflow = 1;
                break;
            }
            for (int i = 0; i < nvadd; ++i) {
                /* 编码名以注册表为准（H264 / H265 / VP8 / VP9） */
                const codec_ops_t *vops = codec_registry_find(add_vcid[i]);
                const char *nm = "H264";
                if (vops) {
                    if (vops->sdp_name && vops->sdp_name[0])
                        nm = vops->sdp_name;
                    else if (vops->name[0])
                        nm = vops->name;
                }
                char abuf[768];
                int an = snprintf(abuf, sizeof(abuf), "a=rtpmap:%d %s/90000",
                                  add_vpt[i], nm);
                an = (an > 0 && (size_t)an < sizeof(abuf)) ? an : -1;
                if (an < 0) {
                    overflow = 1;
                    break;
                }
                int pn = snprintf(pend + pendl, sizeof(pend) - pendl,
                                  "%s\r\n", abuf);
                if (pn <= 0 || (size_t)pn >= sizeof(pend) - pendl) {
                    overflow = 1;
                    break;
                }
                pendl += (size_t)pn;
                /* fmtp 由 codec 元数据提供（desc->default_fmtp）：
                 *   H.264 packetization-mode/profile-level-id
                 *   H.265 sprop-max-don-diff=0;sprop-vps/sps/pps
                 *   VP8/VP9 为空 → 不输出 fmtp */
                const char *xf =
                    (vops && vops->default_fmtp && vops->default_fmtp[0])
                        ? vops->default_fmtp : NULL;
                if (!xf && add_vcid[i] == CODEC_H264)
                    xf = "packetization-mode=1;profile-level-id=42e01f";
                if (xf) {
                    an = snprintf(abuf, sizeof(abuf), "a=fmtp:%d %s",
                                  add_vpt[i], xf);
                    if (an <= 0 || (size_t)an >= sizeof(abuf)) {
                        overflow = 1;
                        break;
                    }
                    pn = snprintf(pend + pendl, sizeof(pend) - pendl,
                                  "%s\r\n", abuf);
                    if (pn <= 0 || (size_t)pn >= sizeof(pend) - pendl) {
                        overflow = 1;
                        break;
                    }
                    pendl += (size_t)pn;
                }
            }
            if (overflow) break;
            p = eol ? eol + 1 : end;
            continue;
        }

        if (put_line(nb, cap, &used, p, ll) != NSBC_OK) {
            overflow = 1;
            break;
        }
        if (eol && put_line(nb, cap, &used, "\n", 1) != NSBC_OK) {
            overflow = 1;
            break;
        }
        p = eol ? eol + 1 : end;
    }
    /* SDP 结束时冲刷最后一段的追加属性 */
    if (!overflow && pendl > 0 &&
        put_line(nb, cap, &used, pend, pendl) != NSBC_OK)
        overflow = 1;
    if (overflow || used >= cap) {
        free(nb);
        return 0;
    }
    nb[used] = '\0';
    memcpy(sdp, nb, used);
    sdp[used] = '\0';
    *lenp = used;
    free(nb);
    return nadd + nvadd;
}

/* ==================================================================
 * answer 媒体段方向终结（REQ-MEDIA-DIR-002）
 * ==================================================================
 *  B2BUA 每条腿的 SDP 必须独立终结：SBC 自身的媒体能力恒为 sendrecv，
 *  回给对端的方向只能由"该对端 offer 声明的方向"决定，绝不能继承另一条腿
 *  （B 腿）answer 里的方向属性。
 *  现场故障：Linphone 的 200 OK 在 m=video 里写 a=recvonly（只收不发），
 *  该行被原样透传进回给 Chrome 的 answer → Chrome 的 video transceiver
 *  退化成 sendonly：1000 只发不收、不产生远端 video track（控制台只有
 *  ONTRACK: audio），也永远不会发出 PLI/关键帧请求。
 *  这里按 RFC 3264 §6.1 取交集重算（answer 方向以 SBC 视角表达）：
 *    offer sendrecv/未声明 → a=sendrecv
 *    offer recvonly        → a=sendonly
 *    offer sendonly        → a=recvonly
 *    offer inactive        → a=inactive
 *  端口为 0 的拒绝段统一 a=inactive。
 *  返回改写后的长度；失败返回原长度，ans 内容保持不变。
 */
static const char *sdp_dir_attr(sdp_dir_t d)
{
    switch (d) {
    case SDP_DIR_SENDONLY: return "a=sendonly";
    case SDP_DIR_RECVONLY: return "a=recvonly";
    case SDP_DIR_INACTIVE: return "a=inactive";
    default:               return "a=sendrecv";
    }
}

/* 应答方向 = 对端 offer 方向 ∩ SBC(sendrecv) */
static sdp_dir_t sdp_answer_dir(sdp_dir_t offer_dir)
{
    switch (offer_dir) {
    case SDP_DIR_RECVONLY: return SDP_DIR_SENDONLY;
    case SDP_DIR_SENDONLY: return SDP_DIR_RECVONLY;
    case SDP_DIR_INACTIVE: return SDP_DIR_INACTIVE;
    default:               return SDP_DIR_SENDRECV;
    }
}

/* 写出本段的方向属性，并记录偏移（供 a=inactive 回溯改写） */
static int sdp_put_dir_line(char *nb, size_t cap, size_t *used,
                            size_t *dir_off, sdp_dir_t d)
{
    char db[24];
    int dn = snprintf(db, sizeof(db), "%s\r\n", sdp_dir_attr(d));
    if (dn <= 0) return NSBC_ERR;
    size_t off = *used;
    if (put_line(nb, cap, used, db, (size_t)dn) != NSBC_OK) return NSBC_ERR;
    if (dir_off) *dir_off = off;
    return NSBC_OK;
}

static size_t sdp_answer_fix_directions(char *ans, size_t len, size_t cap,
                                        const char *offer, size_t offer_len)
{
    if (!ans || len == 0 || len > cap || !offer || offer_len == 0) return len;

    sdp_t os;
    memset(&os, 0, sizeof(os));
    if (sdp_parse(&os, offer, offer_len) != NSBC_OK) return len;

    char *nb = (char *)malloc(cap + 1);
    if (nb == NULL) return len;

    size_t used = 0;
    int seen[SDP_M_IMAGE + 1];
    memset(seen, 0, sizeof(seen));
    int sect_fix = 0;    /* 1=当前段是 audio/video，需要重算方向 */
    int dir_pending = 0; /* 1=方向待写（放到本段 c= 之后，保持 RFC 4566 字段顺序） */
    sdp_dir_t pending_dir = SDP_DIR_SENDRECV;
    size_t dir_off = 0;  /* 本段已写入方向属性的偏移（用于 a=inactive 回溯修正） */
    int failed = 0;      /* 1=缓冲溢出/写失败，整段放弃 */

    const char *p = ans, *end = ans + len;
    while (p < end && !failed) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t ll = (size_t)(le - p);
        size_t cl = ll;
        while (cl > 0 && p[cl - 1] == '\r') --cl;
        const int is_m = (cl >= 2 && p[0] == 'm' && p[1] == '=');
        const int is_c = (cl >= 2 && p[0] == 'c' && p[1] == '=');
        const int is_a = (cl >= 2 && p[0] == 'a' && p[1] == '=');
        const int is_dir = (cl == 10 &&
                            (memcmp(p, "a=sendrecv", 10) == 0 ||
                             memcmp(p, "a=sendonly", 10) == 0 ||
                             memcmp(p, "a=recvonly", 10) == 0 ||
                             memcmp(p, "a=inactive", 10) == 0));

        if (is_m) {
            /* 上一段没有 c= 行时在这里补写方向 */
            if (dir_pending) {
                if (sdp_put_dir_line(nb, cap, &used, &dir_off, pending_dir)
                    != NSBC_OK) {
                    failed = 1;
                    break;
                }
                dir_pending = 0;
            }
            int t = -1;
            if (cl >= 8 && memcmp(p + 2, "audio", 5) == 0)
                t = (int)SDP_M_AUDIO;
            else if (cl >= 8 && memcmp(p + 2, "video", 5) == 0)
                t = (int)SDP_M_VIDEO;
            sect_fix = (t >= 0);

            /* 原样写出 m= 行 */
            if (put_line(nb, cap, &used, p, ll) != NSBC_OK ||
                (eol && put_line(nb, cap, &used, "\n", 1) != NSBC_OK)) {
                failed = 1;
                break;
            }
            if (!sect_fix) {   /* 非 audio/video 段：方向原样保留 */
                p = eol ? eol + 1 : end;
                continue;
            }

            int port_zero = 0;
            {
                const char *ps = NULL, *pe = NULL;
                xc_mline_port_span(p, cl, &ps, &pe);
                if (ps && pe && pe > ps && *ps == '0') port_zero = 1;
            }
            /* 按媒体类型 + 出现序号匹配 offer 的对应段 */
            sdp_dir_t od = SDP_DIR_SENDRECV;
            {
                int occ = seen[t]++;
                for (int i = 0; i < os.nmedia; ++i) {
                    if ((int)os.media[i].type != t) continue;
                    if (occ-- > 0) continue;
                    od = sdp_media_effective_dir(&os, &os.media[i]);
                    break;
                }
            }
            pending_dir = port_zero ? SDP_DIR_INACTIVE : sdp_answer_dir(od);
            dir_pending = 1;
            p = eol ? eol + 1 : end;
            continue;
        }

        /* 媒体段内原有的方向属性一律丢弃（由本函数重算的结果取代） */
        if (sect_fix && is_a && is_dir) {
            if (dir_pending) {
                /*  B 腿在 answer 里声明 a=inactive（呼叫建立即 hold / 拒绝媒体），
                 *  这是有语义的状态，不能被 sendrecv 覆盖掉。 */
                if (cl == 10 && memcmp(p, "a=inactive", 10) == 0)
                    pending_dir = SDP_DIR_INACTIVE;
            } else if (cl == 10 && memcmp(p, "a=inactive", 10) == 0 &&
                       dir_off + 12 <= used) {
                /* 方向属性都是 12 字节，原位改写 */
                memcpy(nb + dir_off, "a=inactive\r\n", 12);
            }
            p = eol ? eol + 1 : end;
            continue;
        }

        if (put_line(nb, cap, &used, p, ll) != NSBC_OK ||
            (eol && put_line(nb, cap, &used, "\n", 1) != NSBC_OK)) {
            failed = 1;
            break;
        }
        /* 方向属性放在本段 c= 行之后（RFC 4566 字段顺序 m=,i=,c=,b=,k=,a=） */
        if (sect_fix && dir_pending && is_c) {
            if (sdp_put_dir_line(nb, cap, &used, &dir_off, pending_dir)
                != NSBC_OK) {
                failed = 1;
                break;
            }
            dir_pending = 0;
        }
        p = eol ? eol + 1 : end;
    }

    /* 段尾（最后一段没有 c= 行）补写方向 */
    if (!failed && dir_pending) {
        if (sdp_put_dir_line(nb, cap, &used, &dir_off, pending_dir) != NSBC_OK)
            failed = 1;
    }

    if (failed || used >= cap) {
        free(nb);
        return len;
    }
    memcpy(ans, nb, used);
    ans[used] = '\0';
    free(nb);
    return used;
}

/*  REQ-MEDIA-SSRC-004（JSEP / SKILL.md §4.2）：向 WebRTC 腿的 answer 里每个
 *  未拒绝的 audio m= 段末尾补 a=ssrc:<ssrc> cname:<cname>。
 *  追加位置在该段其它所有 a= 之后（§1.1：追加属性写在段末，绝不插到 c= 前）。
 *  段内已存在同 SSRC 的 a=ssrc 则不重复注入。
 *  成功返回新长度；任何失败返回原长度（缓冲区不动）。 */
static size_t sdp_answer_add_ssrc(char *ans, size_t len, size_t cap,
                                  uint32_t ssrc)
{
    if (ans == NULL || ssrc == 0 || len == 0 || len >= cap)
        return len;
    char line[80];
    int ll = snprintf(line, sizeof(line), "a=ssrc:%u cname:nsbc%08x\r\n",
                      (unsigned)ssrc, (unsigned)ssrc);
    if (ll <= 0 || (size_t)ll >= sizeof(line))
        return len;

    char *nb = (char *)malloc(cap + 1);
    if (nb == NULL)
        return len;

    size_t used = 0;
    int in_audio = 0, dup = 0;
    const char *p = ans, *end = ans + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t cl = (size_t)(le - p);
        size_t tl = cl;
        if (tl > 0 && p[tl - 1] == '\r') --tl;

        if (tl >= 2 && p[0] == 'm' && p[1] == '=') {
            /* 上一段结束：若它是有效 audio 段且未重复声明，追加在段末 */
            if (in_audio && !dup) {
                if (used + (size_t)ll >= cap) { free(nb); return len; }
                memcpy(nb + used, line, (size_t)ll);
                used += (size_t)ll;
            }
            in_audio = 0;
            dup = 0;
            /* 只处理未拒绝（port != 0）的 audio 段 */
            if (tl >= 8 && memcmp(p + 2, "audio", 5) == 0) {
                const char *ps = NULL, *pe = NULL;
                xc_mline_port_span(p, tl, &ps, &pe);
                int port = 0;
                if (ps && pe && pe > ps && (size_t)(pe - ps) < 12) {
                    char pb[12];
                    size_t pl = (size_t)(pe - ps);
                    memcpy(pb, ps, pl);
                    pb[pl] = '\0';
                    port = atoi(pb);
                }
                in_audio = (port != 0);
            }
        } else if (in_audio && tl > 7 && memcmp(p, "a=ssrc:", 7) == 0) {
            unsigned v = 0;
            const char *q = p + 7;
            while (q < p + tl && *q >= '0' && *q <= '9') {
                v = v * 10u + (unsigned)(*q - '0');
                ++q;
            }
            if (v == ssrc)
                dup = 1;
        }

        if (used + cl + (eol ? 1u : 0u) >= cap) { free(nb); return len; }
        memcpy(nb + used, p, cl);
        used += cl;
        if (eol) {
            nb[used++] = '\n';
            p = eol + 1;
        } else {
            p = end;
        }
    }
    /* SDP 末尾的 audio 段（后面没有下一个 m=）同样追加 */
    if (in_audio && !dup) {
        /* 兜底：SDP 末行若没有以换行结束，先补 CRLF，避免与新属性粘连 */
        if (used > 0 && nb[used - 1] != '\n') {
            if (used + 2 >= cap) { free(nb); return len; }
            nb[used++] = '\r';
            nb[used++] = '\n';
        }
        if (used + (size_t)ll >= cap) { free(nb); return len; }
        memcpy(nb + used, line, (size_t)ll);
        used += (size_t)ll;
    }
    if (used >= cap) { free(nb); return len; }

    memcpy(ans, nb, used);
    ans[used] = '\0';
    free(nb);
    return used;
}

/* answer → A：只保留 A 侧选定的音频 codec + A 的 telephone-event。
 * 失败/不适用返回 0（缓冲区不动）。 */
static int sdp_xc_answer_for_a(char *ans, size_t *lenp, size_t cap,
                               const char *a_offer, size_t a_off_len,
                               int a_pt)
{
    if (ans == NULL || lenp == NULL || a_offer == NULL || a_pt < 0)
        return 0;
    size_t len = *lenp;
    sdp_xc_mline_t aa, av, ba, bv;
    sdp_xc_parse(a_offer, a_off_len, SDP_M_AUDIO, &aa);
    sdp_xc_parse(a_offer, a_off_len, SDP_M_VIDEO, &av);
    sdp_xc_parse(ans, len, SDP_M_AUDIO, &ba);
    sdp_xc_parse(ans, len, SDP_M_VIDEO, &bv);
    if (!aa.found || !ba.found || ba.port == 0) return 0;

    /* A 侧 a_pt 的 rtpmap 元数据（重建 answer 属性用） */
    const char *aenc = NULL, *arate = NULL, *afmtp = NULL;
    int ach = 0, a_te = 0;
    for (int i = 0; i < aa.n; ++i) {
        if (aa.c[i].te && a_te == 0 && aa.c[i].pt > 0)
            a_te = aa.c[i].pt;
        if (aa.c[i].pt == a_pt) {
            aenc = aa.c[i].enc;
            arate = aa.c[i].rate;
            ach = aa.c[i].ch;
            afmtp = aa.c[i].fmtp;
        }
    }
    if (aenc == NULL || aenc[0] == '\0') return 0;  /* 无 rtpmap 无法重建 */

    /* 视频逐腿终结：
     *   - 两侧都有 SBC 可双向处理的视频 codec（H264/VP9）→ 回给 offer 侧
     *     的视频段只保留 offer 侧选定 codec（对侧由 video_xcode 转码）；
     *   - 否则 port=0 降级纯音频（绝不伪造 PT）。 */
    int vport_zero = 0;
    int v_rewrite = 0;
    int a_vidx = -1;
    if (av.found && bv.found && bv.port > 0) {
        for (int i = 0; i < av.n; ++i) {
            if (av.c[i].te) continue;
            if (xc_video_cid(av.c[i].enc) != CODEC_NONE) {
                a_vidx = i;
                break;
            }
        }
        int b_ok = 0;
        for (int i = 0; i < bv.n; ++i) {
            if (bv.c[i].te) continue;
            if (xc_video_cid(bv.c[i].enc) != CODEC_NONE) {
                b_ok = 1;
                break;
            }
        }
        if (a_vidx >= 0 && b_ok) v_rewrite = 1;
        else vport_zero = 1;
        if (vport_zero) {
            llog(LOG_WARN, "media",
                 "sdp_xc_answer_for_a: video rejected — A[found=%d port=%d "
                 "n=%d a_vidx=%d] B[found=%d port=%d n=%d b_ok=%d]",
                 av.found, av.port, av.n, a_vidx,
                 bv.found, bv.port, bv.n, b_ok);
        }
    } else {
        llog(LOG_INFO, "media",
             "sdp_xc_answer_for_a: no video section A[found=%d port=%d] "
             "B[found=%d port=%d]",
             av.found, av.port, bv.found, bv.port);
    }

    char *nb = (char *)malloc(cap + 1);
    if (nb == NULL) return 0;
    size_t used = 0;
    const char *p = ans, *end = ans + len;
    int overflow = 0, sect_audio = 0, sect_video = 0;
    int in_media = 0;
    /*  183 的形状是"会话级 c= + 媒体段内没有 c="，Linphone 接受并正常收发。
     *  200 OK 的会话级 c= 被写在了 t=0 0 之后（RFC 4566 会话字段顺序是
     *  v o s i u e p c b t r z k a，c= 必须在 t= 之前），Linphone 判定
     *  answer SDP 非法 → ACK 后同毫秒 BYE。这里统一成 183 的形状：会话级
     *  c= 提前到 t= 之前，段内与之一致的 c= 去掉。 */
    char cinip[64] = {0};
    int has_sess_c = 0;   /* 1=已由本函数写出会话级 c= */
    {
        const char *q = ans, *qe = ans + len;
        while (q < qe) {
            const char *nl = memchr(q, '\n', (size_t)(qe - q));
            const char *le2 = nl ? nl : qe;
            if ((size_t)(le2 - q) > 9 && q[0] == 'c' && q[1] == '=' &&
                memcmp(q, "c=IN IP4 ", 9) == 0) {
                const char *v = q + 9, *ve = le2;
                while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ')) --ve;
                if ((size_t)(ve - v) < sizeof(cinip)) {
                    memcpy(cinip, v, (size_t)(ve - v));
                    cinip[ve - v] = '\0';
                }
                break;
            }
            q = nl ? nl + 1 : qe;
        }
    }
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        size_t ll = (size_t)(le - p);
        size_t cl = ll;
        while (cl > 0 && p[cl - 1] == '\r') --cl;
        const int is_m = (cl >= 2 && p[0] == 'm' && p[1] == '=');
        int mtype = -1;                    /* 0=audio 1=video */
        if (is_m) {
            if (cl >= 8 && memcmp(p + 2, "audio", 5) == 0)
                mtype = 0;
            else if (cl >= 8 && memcmp(p + 2, "video", 5) == 0)
                mtype = 1;
            sect_audio = (mtype == 0);
            sect_video = (mtype == 1);
            in_media = 1;
            /* 兜底：SDP 里没有 t= 行时，退化为在首个 m= 之前补会话级 c= */
            if (!has_sess_c && cinip[0]) {
                char cbuf[96];
                int cn = snprintf(cbuf, sizeof(cbuf),
                                  "c=IN IP4 %s\r\n", cinip);
                if (cn <= 0 ||
                    put_line(nb, cap, &used, cbuf, (size_t)cn) != NSBC_OK) {
                    overflow = 1;
                    break;
                }
                has_sess_c = 1;
            }
        }

        if (sect_audio && is_m) {
            /* 音频 m= 行：替换整个 fmt-list 为 A 选定 PT（+ TE） */
            const char *fs = xc_mline_fmt_start(p, cl);
            if (put_line(nb, cap, &used, p, (size_t)(fs - p)) != NSBC_OK) {
                overflow = 1;
                break;
            }
            char tmp[32];
            int tn = snprintf(tmp, sizeof(tmp), "%d", a_pt);
            if (tn <= 0 ||
                put_line(nb, cap, &used, tmp, (size_t)tn) != NSBC_OK) {
                overflow = 1;
                break;
            }
            if (a_te > 0 && a_te != a_pt) {
                tn = snprintf(tmp, sizeof(tmp), " %d", a_te);
                if (tn <= 0 ||
                    put_line(nb, cap, &used, tmp, (size_t)tn) != NSBC_OK) {
                    overflow = 1;
                    break;
                }
            }
            if (put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                overflow = 1;
                break;
            }
            /* 紧随 m= 行重建 rtpmap / fmtp —— 原 B 腿载荷属性在下方被丢弃 */
            char abuf[192];
            int an = snprintf(abuf, sizeof(abuf), "a=rtpmap:%d %s/%s%s",
                              a_pt, aenc, (arate && arate[0]) ? arate : "8000",
                              (ach > 1) ? "/2" : "");
            if (an <= 0 ||
                put_line(nb, cap, &used, abuf, (size_t)an) != NSBC_OK ||
                put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                overflow = 1;
                break;
            }
            if (afmtp && afmtp[0]) {
                /* 协商阶段把 fmtp 交给 codec（AMR octet-align / Opus bitrate 等），
                 * codec 未实现协商器时原样透传 */
                char nfmt[192];
                const codec_ops_t *fo = codec_find_by_name_kind(
                    aenc, aenc ? strlen(aenc) : 0, CODEC_KIND_AUDIO);
                codec_id_t fcid = fo ? fo->id : pt_to_codec(a_pt);
                int fn = 0;
                nfmt[0] = '\0';
                if (codec_fmtp_negotiate(fcid, afmtp, NULL,
                                         nfmt, sizeof(nfmt)) != NSBC_OK ||
                    nfmt[0] == '\0') {
                    size_t fl = strlen(afmtp);
                    if (fl >= sizeof(nfmt)) fl = sizeof(nfmt) - 1;
                    memcpy(nfmt, afmtp, fl);
                    nfmt[fl] = '\0';
                }
                fn = snprintf(abuf, sizeof(abuf), "a=fmtp:%d %s", a_pt, nfmt);
                if (fn <= 0 ||
                    put_line(nb, cap, &used, abuf, (size_t)fn) != NSBC_OK ||
                    put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                    overflow = 1;
                    break;
                }
            }
            if (a_te > 0) {
                const char *terate = "8000";
                const char *tefmtp = NULL;
                for (int i = 0; i < aa.n; ++i) {
                    if (aa.c[i].pt == a_te) {
                        if (aa.c[i].rate[0]) terate = aa.c[i].rate;
                        tefmtp = aa.c[i].fmtp;
                        break;
                    }
                }
                an = snprintf(abuf, sizeof(abuf),
                              "a=rtpmap:%d telephone-event/%s", a_te, terate);
                if (an <= 0 ||
                    put_line(nb, cap, &used, abuf, (size_t)an) != NSBC_OK ||
                    put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                    overflow = 1;
                    break;
                }
                if (tefmtp && tefmtp[0]) {
                    an = snprintf(abuf, sizeof(abuf), "a=fmtp:%d %s",
                                  a_te, tefmtp);
                    if (an <= 0 ||
                        put_line(nb, cap, &used, abuf, (size_t)an) != NSBC_OK ||
                        put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                        overflow = 1;
                        break;
                    }
                }
            }
            p = eol ? eol + 1 : end;
            continue;
        }

        if (sect_audio && !is_m && cl >= 2 && p[0] == 'a' && p[1] == '=') {
            /* 丢弃 B 腿的载荷相关属性，避免与 A 的 PT 冲突 */
            if ((cl >= 9 && memcmp(p, "a=rtpmap:", 9) == 0) ||
                (cl >= 7 && memcmp(p, "a=fmtp:", 7) == 0) ||
                (cl >= 10 && memcmp(p, "a=rtcp-fb:", 10) == 0)) {
                p = eol ? eol + 1 : end;
                continue;
            }
        }

        if (sect_video && is_m && v_rewrite) {
            /* 视频 m= 行：保留对侧端口/proto，fmt-list 收敛到 offer 侧选定 PT，
             * 紧随其后重建 rtpmap/fmtp（原对侧载荷属性在下方被丢弃）。 */
            const char *fs = xc_mline_fmt_start(p, cl);
            size_t pre = (size_t)(fs - p);
            /* fs 已跳过 proto 后的空白；若前缀最后一个字符已是空白，
             * 追加 PT 时不能再加前导空格，否则会出现 "RTP/AVP  96"
             * 这类双空格，严格 SDP 解析器会判为无效 fmt-list。 */
            int sp = (pre > 0 && (p[pre - 1] == ' ' || p[pre - 1] == '\t'));
            char tmp[32];
            char abuf[768];
            int tn = snprintf(tmp, sizeof(tmp), sp ? "%d" : " %d",
                              av.c[a_vidx].pt);
            if (put_line(nb, cap, &used, p, pre) != NSBC_OK ||
                tn <= 0 ||
                put_line(nb, cap, &used, tmp, (size_t)tn) != NSBC_OK ||
                put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                overflow = 1;
                break;
            }
            int an = snprintf(abuf, sizeof(abuf), "a=rtpmap:%d %s/%s",
                              av.c[a_vidx].pt, av.c[a_vidx].enc,
                              av.c[a_vidx].rate[0] ? av.c[a_vidx].rate
                                                   : "90000");
            if (an <= 0 ||
                put_line(nb, cap, &used, abuf, (size_t)an) != NSBC_OK ||
                put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                overflow = 1;
                break;
            }
            if (av.c[a_vidx].fmtp[0]) {
                /* 视频 fmtp（H.265 sprop-vps/sps/pps 可超 200 字节）：
                 * 缓冲已按 fmtp[512] + abuf[768] 放大，仍超长则判失败，
                 * 绝不发出被截断的 sprop 参数 */
                an = snprintf(abuf, sizeof(abuf), "a=fmtp:%d %s",
                              av.c[a_vidx].pt, av.c[a_vidx].fmtp);
                if (an <= 0 || (size_t)an >= sizeof(abuf) ||
                    put_line(nb, cap, &used, abuf, (size_t)an) != NSBC_OK ||
                    put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                    overflow = 1;
                    break;
                }
            }
            /*  RFC 4585：answer 必须显式列出本端接受的反馈消息，否则对端
             * "无权"发 PLI。之前这里把源头 offer 的 a=rtcp-fb 全丢了，导致
             * Linphone 收到无 rtcp-fb 的 answer → 丢包后从不请求关键帧 →
             * 远端画面永久定格（实测 A 侧 RTCP_FB=0，只有 SR 没有 PLI）。
             * SBC 已实现 PLI 中继（relay_video_request_pli + 入向 PLI/FIR
             * 翻译，见 relay_recv_one），所以只宣告 nack pli —— 这是 RFC 4585
             * 的必选反馈，Linphone/Chrome 都认；ccm fir 是可选扩展，不宣告。 */
            an = snprintf(abuf, sizeof(abuf), "a=rtcp-fb:%d nack pli",
                          av.c[a_vidx].pt);
            if (an <= 0 ||
                put_line(nb, cap, &used, abuf, (size_t)an) != NSBC_OK ||
                put_line(nb, cap, &used, "\r\n", 2) != NSBC_OK) {
                overflow = 1;
                break;
            }
            p = eol ? eol + 1 : end;
            continue;
        }

        if (sect_video && !is_m && v_rewrite && cl >= 2 &&
            p[0] == 'a' && p[1] == '=') {
            if ((cl >= 9 && memcmp(p, "a=rtpmap:", 9) == 0) ||
                (cl >= 7 && memcmp(p, "a=fmtp:", 7) == 0) ||
                (cl >= 10 && memcmp(p, "a=rtcp-fb:", 10) == 0) ||
                (cl >= 7 && memcmp(p, "a=ssrc:", 7) == 0) ||
                (cl >= 13 && memcmp(p, "a=ssrc-group:", 13) == 0)) {
                p = eol ? eol + 1 : end;
                continue;
            }
        }

        if (sect_video && is_m && vport_zero) {
            /* 视频无法安全映射 → port=0（拒绝该媒体流，降级纯音频） */
            const char *ps = NULL, *pe = NULL;
            xc_mline_port_span(p, cl, &ps, &pe);
            if (ps && pe && put_line(nb, cap, &used, p,
                                     (size_t)(ps - p)) == NSBC_OK &&
                put_line(nb, cap, &used, "0", 1) == NSBC_OK &&
                put_line(nb, cap, &used, pe, (size_t)(p + cl - pe)) == NSBC_OK &&
                put_line(nb, cap, &used, "\r\n", 2) == NSBC_OK) {
                p = eol ? eol + 1 : end;
                continue;
            }
            overflow = 1;
            break;
        }

        /* 会话级已有 c=，段内重复的同地址 c= 去掉（183 就是这个形状，
         * Linphone 接受；同时避免 c= 落到 a= 之后违反 RFC 4566 顺序）。 */
        if (in_media && !is_m && cl >= 2 && p[0] == 'c' && p[1] == '=' &&
            cinip[0]) {
            const char *v = p + 2, *ve = p + cl;
            if ((size_t)(ve - v) > 7 && memcmp(v, "IN IP4 ", 7) == 0) {
                v += 7;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\r')) --ve;
                if ((size_t)(ve - v) == strlen(cinip) &&
                    memcmp(v, cinip, (size_t)(ve - v)) == 0) {
                    p = eol ? eol + 1 : end;
                    continue;
                }
            }
        }

        /*  会话级 c= 必须在 t= 之前（RFC 4566 会话字段顺序
         *  v o s i u e p c b t r z k a）。ans 里原位置的会话级 c=
         *  一律丢弃，统一在 t= 行前写出；没有 t= 行时由 is_m
         *  兜底分支在首个 m= 前补。 */
        if (!in_media && cl >= 2) {
            if (!has_sess_c && cinip[0] && p[0] == 't' && p[1] == '=') {
                char cbuf[96];
                int cn = snprintf(cbuf, sizeof(cbuf),
                                  "c=IN IP4 %s\r\n", cinip);
                if (cn <= 0 ||
                    put_line(nb, cap, &used, cbuf, (size_t)cn) != NSBC_OK) {
                    overflow = 1;
                    break;
                }
                has_sess_c = 1;
            }
            if (cinip[0] && p[0] == 'c' && p[1] == '=') {
                p = eol ? eol + 1 : end;
                continue;
            }
        }

        if (put_line(nb, cap, &used, p, ll) != NSBC_OK) {
            overflow = 1;
            break;
        }
        if (eol && put_line(nb, cap, &used, "\n", 1) != NSBC_OK) {
            overflow = 1;
            break;
        }
        p = eol ? eol + 1 : end;
    }
    if (overflow || used >= cap) {
        free(nb);
        return 0;
    }
    nb[used] = '\0';
    memcpy(ans, nb, used);
    ans[used] = '\0';
    *lenp = used;
    free(nb);
    return 1;
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
    /*  第 1 项：保存 A 原始 offer —— answer_hook 逐腿终结时据此重建
     * "回给 A 的 answer"。每次 offer（含 re-INVITE）都覆盖；malloc 失败
     * 则清空，绝不让 answer_hook 用到陈旧 SDP。同时复位上一轮的终结结果。 */
    e->neg_valid = 0;
    e->neg_a_pt = e->neg_b_pt = 0xFF;
    e->neg_v_valid = 0;
    e->neg_v_a_pt = e->neg_v_b_pt = 0xFF;
    e->neg_v_a_cid = e->neg_v_b_cid = CODEC_NONE;
    {
        char *cp = NULL;
        if (in && inlen) {
            cp = (char *)malloc(inlen + 1);
            if (cp) {
                memcpy(cp, in, inlen);
                cp[inlen] = '\0';
            }
        }
        free(e->a_offer_sdp);
        e->a_offer_sdp = cp;
        e->a_offer_len = cp ? inlen : 0;
    }
    /*  RFC 3312 Precondition 前提条件门控：
     * profile->enable_precondition=0（企业 PBX 等场景关闭）→ 永远置 0，gate 常开；
     * profile=NULL（无归属 profile）→ 默认关闭（普通 PSTN/SIP 中继场景）。
     * 只有显式 enable_precondition=1 的 profile 才解析 a=des/a=curr。 */
    e->precond_active = 0;
    e->precond_met = 0;
    {
        /*  三层守卫：cfg（无）→ force_precondition (dialplan 三态) → profile.enable_precondition
         *  REQ-ARCH-011：call_key 由 signaling 创建、media 仅消费。异步 resolve
         *  期间呼叫可能已被 call_destroy 归还 slab 并复用，此时强转会读到垃圾
         *  （实测 prof 读到 SDP 文本 "a=rtcp-f" 导致 SIGSEGV）。必须先做 magic
         *  存活校验，未通过则视为"无呼叫上下文"，按默认关处理。 */
        b2b_call_t *bc = b2b_call_is_alive(call) ? (b2b_call_t *)call : NULL;
        if (!bc && call) {
            /* 诊断：call_key 非空但 magic 不匹配 → 陈旧/被复用指针。
             * 这里绝不解引用，仅限频告警，便于定位呼叫生命周期问题。 */
            static uint32_t stale_n = 0;
            uint32_t sn = __atomic_add_fetch(&stale_n, 1, __ATOMIC_RELAXED);
            if (sn == 1 || sn % 100 == 0)
                llog(LOG_WARN, "media",
                     "offer_hook: call_key stale (total=%u) — 跳过 precondition 解析", sn);
        }
        sip_profile_t *pr = bc ? bc->prof : NULL;
        int prof_en = (pr && pr->enable_precondition) ? 1 : 0;  /* profile NULL 默认关 */
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
    /*  REQ-NAT-002：per-call NAT 公网 RTP IP —— offer 阶段查一次缓存到 cnode，
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

    /*  REQ-DTLS-001 方向感知 + REQ-MEDIA-DP-001 rule override
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

    /*  存端点 WebRTC 标志到 cnode——answer_hook 需要读取，
     * 独立于 dtls[]（Phase C SRTP_RELAY 下双侧都创建 DTLS server，
     * 不再能从 dtls[] 反推端点类型）。 */
    e->a_ws = (uint8_t)a_ws;
    e->b_ws = (uint8_t)b_ws;

    /*  数 A 原始 offer 的 m= 段数（RFC 3264 answer 段数对齐用）。
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

    /* ① 硬约束
     * mixed_ws (混合 WebRTC + SIP 明文) 强制 MB_RELAY：
     *   - WebRTC 侧 DTLS-SRTP 由 bridge 终结
     *   - SIP 侧走 RTP/AVP 明文
     *   - bridge 做 SRTP↔RTP 加解密转换
     *   - 绝不能用 MB_SRTP_RELAY —— SRTP_RELAY 要求两端都是 WebRTC/DTLS-SRTP，
     *     SIP 明文端不支持 DTLS，会导致 media bridge 卡死在等待 ClientHello。
     * both_ws (两端都是 WebRTC) 才强制 MB_SRTP_RELAY。 */
    if (mixed_ws) {
        if (rule_mm == 1)
            llog(LOG_WARN, "media", "offer_hook: rule=bypass but mixed-leg WebRTC (ignored, forcing RELAY)");
        if (mode != MB_RELAY) {
            mode = MB_RELAY;
            llog(LOG_INFO, "media", "offer_hook: mixed WebRTC leg → force RELAY (WebRTC DTLS + SIP RTP/AVP via SRTP↔RTP bridge)");
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
            /* 混合腿 rule=bypass 已在上面打 WARN，此处不动 mode（已被强制 RELAY） */
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

    /*  NAT 兜底（2026-11-23）：端点换到 NAT 后面后，SDP 的 c= 变成私网地址
     *  （如 10.0.0.137），服务器直接往这个地址发 RTP 是必然不可达的；
     *  而端点若又不先发 RTP（现场日志 latched[0]=0 持续 30s+），comedia
     *  latch 永远学不到真实地址 → 双向无声、无图。
     *  规则：SDP 私网 + 信令源是公网 → 用信令源 IP 替代（端口保留 SDP 值，
     *  家宽/4G NAT 普遍保端口）。一旦对端发出 RTP，latch 仍会覆盖成实测地址，
     *  因此这是纯兜底，不改变既有 comedia 行为。 */
    {
        /* magic 存活校验：call_key 陈旧时不解引用（见 offer_hook 注释）。 */
        b2b_call_t *bc0 = b2b_call_is_alive(call) ? (b2b_call_t *)call : NULL;
        char sip_src[64] = "";
        if (bc0 && bc0->leg[0].dst.ss.ss_family == AF_INET) {
            const struct sockaddr_in *sin0 =
                (const struct sockaddr_in *)&bc0->leg[0].dst.ss;
            inet_ntop(AF_INET, &sin0->sin_addr, sip_src, sizeof(sip_src));
        }
        if (sip_src[0] && mb_ipv4_is_private(e->a_ip) &&
            !mb_ipv4_is_private(sip_src)) {
            llog(LOG_WARN, "media",
                 "offer_hook: A SDP 私网地址 %s 不可达（信令源 %s，NAT 后）"
                 " → RTP 目标改用信令源 IP，端口保留 %u",
                 e->a_ip, sip_src, (unsigned)e->a_port);
            snprintf(e->a_ip, sizeof(e->a_ip), "%s", sip_src);
        }
    }

    /* 3.3：队列 hold 期已由 bridge_local_start 建 relay，A 腿 183 SDP 已通告
     * 该 relay 的 A 侧端口。此时建 B 腿必须复用同一 relay；若降级 BYPASS，
     * B 会拿到 A 的原始地址，与 183 通告不一致 → 媒体断流。 */
    if (e->relay && mode == MB_BYPASS) {
        llog(LOG_WARN, "media",
             "offer_hook: hold relay exists → force RELAY (bypass disabled) call=%p",
             call);
        mode = MB_RELAY;
        e->mode = mode;
    }

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
    memset(&ports, 0, sizeof(ports));
    /* 明文 A 腿 audio/video 各自独立端口；WebRTC 腿 BUNDLE 复用同一端口 */
    ports.want_video = (uint8_t)((a_ws ? 0 : 1) | (b_ws ? 0 : 2));
    if (e->relay) {
        /* 3.3：复用 hold 期 relay（端口已在 183 SDP 中通告给 A 腿） */
        ports.a_rtp  = rtp_relay_port(e->relay, 0);
        ports.b_rtp  = rtp_relay_port(e->relay, 1);
        ports.a_rtcp = (uint16_t)(ports.a_rtp ? ports.a_rtp + 1 : 0);
        ports.b_rtcp = (uint16_t)(ports.b_rtp ? ports.b_rtp + 1 : 0);
        ports.a_vrtp = rtp_relay_video_port(e->relay, 0);
        ports.b_vrtp = rtp_relay_video_port(e->relay, 1);
        e->local_media = 0;   /* 交接给 B 腿：不再按单腿 AI/discard 处理 */
        llog(LOG_INFO, "media",
             "offer_hook: reuse existing relay (hold/pre_answer/queue) call=%p "
             "a_rtp=%u b_rtp=%u",
             call, (unsigned)ports.a_rtp, (unsigned)ports.b_rtp);
    } else {
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
    }
    if (!e->relay) {
        llog(LOG_WARN, "media", "offer_hook: relay_start FAIL");
        return NSBC_NOSPC;
    }
    e->relay->node = e;
    llog(LOG_INFO, "media", "offer_hook: relay_start OK rtp0=%d rtp1=%d",
         ports.a_rtp, ports.b_rtp);
    /*  REQ-MEDIA-SSRC-003③：offer SDP 的 SSRC 预登记到 side=0（offer 来自 A 侧）。
     * 必须在 bridge_rewrite_sdp 之前（rewrite 剥 a=ssrc: 行）。 */
    admit_ssrc_from_sdp(e->relay, 0, in, inlen);
    if (e->a_ip[0] && !a_ws) {
        /* A=WebRTC 时不设固定 peer：ICE 实际源地址/端口与 SDP c=/m=
         * 可能不同（浏览器任选候选），该侧纯 latch 首包学习。 */
        nsbc_sa_t sa;
        if (nsbc_sa_from(&sa, e->a_ip, e->a_port, AF_UNSPEC) == NSBC_OK)
            rtp_relay_set_peer_a(e->relay, &sa);
        /* 视频 m-line 是独立端口，单独记录，避免与 audio 端口混用 */
        sdp_vsel_t va;
        if (sdp_video_select(in, inlen, &va) && va.port > 0 &&
            nsbc_sa_from(&sa, e->a_ip, (uint16_t)va.port, AF_UNSPEC) == NSBC_OK)
            rtp_relay_set_peer_video(e->relay, 0, &sa);
    }
    /* B 侧 INVITE 重写：offer SDP 告诉 B 把 RTP 发到 SBC 的 B 侧 relay 端口
     * （b_rtp=fd_rtp[1]）。方向决定改写形态——最终根据 mode+dtls 结果选：
     *   MB_BYPASS  → ASIS：透传原始 SDP（不改地址/proto）
     *   B=WebRTC   → WS：proto→SAVPF，剥 A 旧凭证后下方注入本侧新凭证；
     *   A=WebRTC   → PLAIN：消毒（proto→RTP/AVP + 剥 ICE/DTLS）给明文端；
     *   双明文     → ASIS：仅改地址端口。 */

    /* REQ-DTLS-001：SRTP_RELAY 模式 → 只给**端点是 WebRTC** 的那一侧建 DTLS server。
     *
     *  2026-10-16 关键修复：之前双侧无条件建 DTLS，导致 mixed_ws 下明文 SIP 侧
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
    if ((mode == MB_SRTP_RELAY || mode == MB_RELAY) && m->cfg.re) {
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
        /*  降级条件：只有**需要 DTLS 的那侧**真的建失败才降级。
         *   both_plain（need_dtls=[0,0]）→ 明文 RELAY，**不降级**！
         *   mixed_ws 下一侧 DTLS 成功就够了。
         *
         *  2026-11-13 修复：之前 !any_dtls_ok 把 both_plain 也降级了，
         *   导致 sipp↔sipp 全明文场景永远走 BYPASS → relay 不 arm →
         *   rec_feed_hook 不触发 → recording bytes=0。 */
        const int need_any_dtls = need_dtls_on_side[0] || need_dtls_on_side[1];
        if (!any_dtls_ok && need_any_dtls) {
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
    /*  RACE-FIX: dtls 创建 + node 赋值 + admit_ssrc 全部完成后，
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
    /* B 腿视频端口：只有明文 B 腿才有独立端口；WebRTC B 腿 BUNDLE 复用 b_rtp */
    uint16_t rewrite_vport = 0;
    if (!is_bypass && dir != MB_SDP_WS && e->relay)
        rewrite_vport = rtp_relay_video_port(e->relay, 1);
    int n = bridge_rewrite_sdp(in, inlen, out, outsz,
                               (e->external_rtp_ip[0]) ? e->external_rtp_ip : m->cfg.local_ip,
                               rewrite_port, rewrite_vport, dir);
    llog(LOG_INFO, "media",
         "REWRITE_CALL: rewrite_port=%u video_port=%u dir=%d mode=%d is_bypass=%d a_ws=%d b_ws=%d n=%d",
         (unsigned)rewrite_port, (unsigned)rewrite_vport, dir, mode, is_bypass, a_ws, b_ws, n);
    if (n < 0) return NSBC_NOSPC;

    /*  明文腿 offer：把 H.264 fmtp 的 profile-level-id 对齐到本机编码器
     * 实际发出的码流（WebRTC 腿透传的 42e01f/42001f 与 x264 的 42c01f
     * 不一致，Linphone 会反复 picture_fast_update）。详见
     * sdp_align_h264_profile() 上方注释。
     * 只改 offer，不改 answer —— answer 的 fmtp 必须与对端 offer 兼容
     * （RFC 3264 §6.1 / RFC 6184 §8.2.2）。 */
    if (!is_bypass && dir == MB_SDP_PLAIN) {
        int h264fix = sdp_align_h264_profile(out, (size_t)n);
        if (h264fix > 0)
            llog(LOG_INFO, "media",
                 "offer_hook: H.264 profile-level-id aligned to local encoder "
                 "(%s) in %d fmtp line(s)", NSBC_H264_LOCAL_PROFILE_ID, h264fix);
    }

    /*  暴力 dump：rewrite 输出逐行打，确认 video m= 行到底是什么！ */
    {
        const char *dp = out; const char *dend = out + n;
        for (int di = 0; dp && dp < dend; ++di) {
            const char *de = memchr(dp, '\n', (size_t)(dend - dp));
            size_t dl = de ? (size_t)(de - dp) : strlen(dp);
            if (dl >= 2 && ((dp[0] == 'm' && dp[1] == '=') ||
                dp[0] == 'c')) {
                llog(LOG_INFO, "media", "REWRITE_OUT[%d]: %.*s", di, (int)dl, dp);
            }
            dp = de ? de + 1 : NULL;
        }
    }

    /*  第 1 项：offer → B 时追加 SBC 可双向处理的静态音频 PT。
     * A 只提供 G729 而 B 只会 PCMA 时，B 必须能在 offer 里看到 PCMA，
     * 否则永远无法协商出可转码的目标。BYPASS / 双 WebRTC 不改写。 */
    if (!is_bypass && !(a_ws && b_ws)) {
        size_t xl = (size_t)n;
        int xadd = sdp_xc_add_offer_audio(out, &xl, outsz);
        if (xadd > 0) {
            n = (int)xl;
            llog(LOG_INFO, "media",
                 "offer_hook: per-leg SDP termination — appended %d SBC audio PT(s), n=%d",
                 xadd, n);
        }
    }

    /*  向 WebRTC 侧发送的 SDP，如果源 offer 只有 audio 一段没有 video，
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
                /*  和 answer_hook 对称的 guard：
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
    /*  诊断：原始 A 的 offer + 最终 offer（改写后） */
    {
        const char *lp = in;
        for (int li = 0; lp && lp < in + inlen; ++li) {
            const char *le = memchr(lp, '\n', (size_t)(in + inlen - lp));
            size_t ll = le ? (size_t)(le - lp) : strlen(lp);
            llog(LOG_DEBUG, "dtls", "A_OFFER_RAW[%d]: %.*s", li, (int)ll, lp);
            lp = le ? le + 1 : NULL;
        }
    }
    {
        FILE *f = fopen("/tmp/nsbc_offer.sdp", "wb");
        if (f) { fwrite(out, 1, (size_t)n, f); fclose(f); }
        /*  日志也打印完整 SDP（每一行单独 INFO，grep "OFFER" 即可） */
        {
            const char *lp = out;
            for (int li = 0; lp && lp < out + n; ++li) {
                const char *le = memchr(lp, '\n', (size_t)(out + n - lp));
                size_t ll = le ? (size_t)(le - lp) : strlen(lp);
                llog(LOG_DEBUG, "dtls", "OFFER_OUT[%d]: %.*s", li, (int)ll, lp);
                lp = le ? le + 1 : NULL;
            }
        }
        /* 专项诊断：video m-line 检查 */
        {
            const char *vid = memmem(out, (size_t)n, "m=video", 7);
            if (vid) {
                const char *eol = memchr(vid, '\n', (size_t)(out + n - vid));
                size_t vlen = eol ? (size_t)(eol - vid) : 80;
                llog(LOG_DEBUG, "dtls", " OFFER VIDEO LINE: %.*s", (int)vlen, vid);
            } else {
                llog(LOG_WARN, "dtls", " OFFER MISSING m=video!");
            }
        }
    }
    return n;
}

/*  单腿本地媒体（AI 内循环）：A 腿单独接通，无 B 腿。
 * 与 offer_hook 的差别：
 *   - 不重写"给 B 的 offer"，而是生成"回给 A 的 200 OK SDP"（指向 relay A 侧端口）
 *   - B 侧设一个 discard sink（127.0.0.1:9），只为让 relay 转发路径/rec hook
 *     （ai rx）正常触发；AI 的音频注入走 bridge_send_raw → cnode->local_media
 *     强制 dst_side=0 发回 A，不会产生回声。
 *   - WebRTC A 腿（需 DTLS 终结）暂不支持，返回 NSBC_NOTSUP。 */
int bridge_local_start(void *call_key, const char *offer, size_t offer_len,
                       char *out, size_t outsz, void *ud)
{
    bridge_mgr_t *m = ud;
    if (!m || !call_key || !offer || !offer_len || !out || !outsz)
        return NSBC_INVAL;

    /* WebRTC SDP 特征：UDP/TLS / fingerprint / ice-ufrag。
     * local:ai 单腿模式本来只支持明文 RTP（media_bridge 内部是明文 relay，
     * 没有 DTLS-SRTP 终结逻辑）。
     * 但 488 NOTSUP 会直接把用户拒在门外，更糟。此处改 warn 后继续走
     * cleartext relay（bridge_rewrite_sdp 把 SDP 里的 UDP/TLS/RTP/SAVPF
     * 改写成 RTP/AVP，剥掉 fingerprint/crypto 行），用户可能听不到声音
     * （WebRTC 客户端等待 SRTP 收不到就静默），但至少不会被拒。
     * P1 规划：在 signaling 层 local_media 分支前置 mixed_ws 检测，
     * 让 WebRTC offer 先走 DTLS 终结再把明文凭据传给 local_start。 */
    if (is_webrtc_sdp(offer, offer_len)) {
        llog(LOG_WARN, "media",
             "local_start: WebRTC A-leg detected — warn only, force cleartext relay "
             "(P1: add DTLS-SRTP termination for local:ai)");
    }

    cnode_t *e = entry_get(m, call_key);
    if (!e) {
        llog(LOG_WARN, "media", "local_start: entry_get NOMEM call=%p", call_key);
        return NSBC_NOMEM;
    }
    e->mode = MB_RELAY;
    e->local_media = 1;
    e->a_ws = 0;
    e->b_ws = 0;
    e->precond_active = 0;
    e->precond_met = 0;

    /* per-call NAT 公网 RTP IP（与 offer_hook 同源） */
    e->external_rtp_ip[0] = '\0';
    if (m->cfg.call_external_rtp_ip) {
        const char *nip = m->cfg.call_external_rtp_ip(
            m->cfg.call_external_rtp_ip_ud, call_key);
        if (nip && nip[0])
            snprintf(e->external_rtp_ip, sizeof(e->external_rtp_ip), "%s", nip);
    }
    parse_media_addr(offer, offer_len, e->a_ip, sizeof(e->a_ip), &e->a_port);

    uint16_t rlo = 0, rhi = 0;
    if (m->cfg.call_port_range)
        m->cfg.call_port_range(m->cfg.call_port_range_ud, call_key, &rlo, &rhi);
    relay_ports_t ports;
    memset(&ports, 0, sizeof(ports));
    /* 单腿本地媒体（hold/AI 放音）：A 腿按明文 SIP 处理，需要独立视频端口 */
    ports.want_video = 1;
    if (rtp_relay_start(m, &e->relay, &ports, rlo, rhi) != NSBC_OK &&
        (rlo != 0 || rhi != 0)) {
        llog(LOG_WARN, "media",
             "local_start: port range %u-%u exhausted, fallback default",
             (unsigned)rlo, (unsigned)rhi);
        if (rtp_relay_start(m, &e->relay, &ports, 0, 0) != NSBC_OK)
            return NSBC_NOSPC;
    }
    if (!e->relay) return NSBC_NOSPC;
    e->relay->node = e;

    admit_ssrc_from_sdp(e->relay, 0, offer, offer_len);

    if (e->a_ip[0]) {
        nsbc_sa_t sa;
        if (nsbc_sa_from(&sa, e->a_ip, e->a_port, AF_UNSPEC) == NSBC_OK)
            rtp_relay_set_peer_a(e->relay, &sa);
        sdp_vsel_t va;
        if (sdp_video_select(offer, offer_len, &va) && va.port > 0 &&
            nsbc_sa_from(&sa, e->a_ip, (uint16_t)va.port, AF_UNSPEC) == NSBC_OK)
            rtp_relay_set_peer_video(e->relay, 0, &sa);
    }
    /* B 侧 discard sink：让 relay 认为对侧就绪（rec hook 正常触发）；
     * 转发的包打到 127.0.0.1:9，不会形成回声。 */
    {
        nsbc_sa_t sink;
        if (nsbc_sa_from(&sink, "127.0.0.1", 9, AF_UNSPEC) == NSBC_OK)
            rtp_relay_set_peer_b(e->relay, &sink);
    }
    rtp_relay_arm(m, e->relay);

    int n = bridge_rewrite_sdp(offer, offer_len, out, outsz,
                               e->external_rtp_ip[0] ? e->external_rtp_ip
                                                     : m->cfg.local_ip,
                               ports.a_rtp, ports.a_vrtp, MB_SDP_ASIS);
    if (n < 0) return NSBC_NOSPC;
    llog(LOG_INFO, "media",
         "local_start: call=%p a_rtp=%u b_rtp=%u a_ip=%s a_port=%u sdp=%dB",
         call_key, (unsigned)ports.a_rtp, (unsigned)ports.b_rtp,
         e->a_ip[0] ? e->a_ip : "(null)", (unsigned)e->a_port, n);
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
        /*  ★ B 腿 codec/PT 的权威来源：in 是本轮 B 的原始 200 OK（后续
         *  hook 会把它改写成回给 A 的 answer，PT 换成 A 侧编号）。
         *  bridge_confirm 必须基于原始 SDP 建 pmap[1]/answer_pts，否则
         *  A→B 入包会误判"对端接受"而跳过转码。 */
        if (e && e->relay && in && inlen) {
            char *cp = (char *)malloc(inlen + 1);
            if (cp) {
                memcpy(cp, in, inlen);
                cp[inlen] = '\0';
                free(e->b_answer_sdp);
                e->b_answer_sdp = cp;
                e->b_answer_len = inlen;
                e->b_answer_fresh = 1;
            }
        }
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
        /*  第 1 项后转码已由 bridge_confirm 逐腿规划执行，
         *  不再在此拦截成 NSBC_NOTSUP（旧 P0 限制）。 */
        llog(LOG_INFO, "media",
             "answer_hook: transcode required — handled by per-leg plan");
    }
    /*  RFC 3312 Precondition met 检测：
     * 三层守卫同 offer_hook：force_precondition (dialplan) → profile.enable_precondition。
     * 同样先做 magic 存活校验，call_key 陈旧时不强转（见 offer_hook 注释）。 */
    {
        b2b_call_t *bc = b2b_call_is_alive(call) ? (b2b_call_t *)call : NULL;
        sip_profile_t *pr = bc ? bc->prof : NULL;
        int prof_en = (pr && pr->enable_precondition) ? 1 : 0;
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
    memset(&ports, 0, sizeof(ports)); /* 已分配：取 fd 实际端口 */
    /* B 对端地址 */
    char ip[64];
    uint16_t port = 0;
    parse_media_addr(in, inlen, ip, sizeof(ip), &port);
    llog(LOG_INFO, "media",
         "answer_hook: B media addr %s:%u (dtls=%d) call=%p",
         ip[0] ? ip : "(null)", (unsigned)port, e->dtls[1] ? 1 : 0, call);

    /*  NAT 兜底（与 offer_hook A 腿同口径）：SDP c= 私网 + 信令源公网 →
     *  用信令源 IP 替代（端口保留 SDP 值）。否则 RTP 全部发往私网地址，
     *  对端收不到也就不会回包，comedia 永远学不到真实地址 → 双向无声。
     *  纯兜底：对端一旦回包，latch 仍会覆盖成实测地址。 */
    if (ip[0] && !e->dtls[1]) {
        b2b_call_t *bc1 = b2b_call_is_alive(call) ? (b2b_call_t *)call : NULL;
        char sip_src[64] = "";
        if (bc1 && bc1->leg[1].dst.ss.ss_family == AF_INET) {
            const struct sockaddr_in *sin1 =
                (const struct sockaddr_in *)&bc1->leg[1].dst.ss;
            inet_ntop(AF_INET, &sin1->sin_addr, sip_src, sizeof(sip_src));
        }
        if (sip_src[0] && mb_ipv4_is_private(ip) &&
            !mb_ipv4_is_private(sip_src)) {
            llog(LOG_WARN, "media",
                 "answer_hook: B SDP 私网地址 %s 不可达（信令源 %s，NAT 后）"
                 " → RTP 目标改用信令源 IP，端口保留 %u",
                 ip, sip_src, (unsigned)port);
            snprintf(ip, sizeof(ip), "%s", sip_src);
        }
    }

    if (ip[0] && !e->dtls[1]) {
        /* B=WebRTC（dtls[1] 非空）时不设固定 peer：ICE 实际源地址/端口
         * 与 SDP c=/m= 可能不同（浏览器任选候选），该侧纯 latch 首包
         * 学习——与 offer_hook 的 set_peer_a(!a_ws) 同口径。 */
        nsbc_sa_t sa;
        int rc = nsbc_sa_from(&sa, ip, port, AF_UNSPEC);
        if (rc == NSBC_OK)
            rtp_relay_set_peer_b(e->relay, &sa);
        /* 明文 B 腿：视频 m-line 独立端口，单独记录 */
        sdp_vsel_t vb;
        if (sdp_video_select(in, inlen, &vb) && vb.port > 0 &&
            nsbc_sa_from(&sa, ip, (uint16_t)vb.port, AF_UNSPEC) == NSBC_OK)
            rtp_relay_set_peer_video(e->relay, 1, &sa);
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
    /*  REQ-MEDIA-SSRC-003③：answer SDP 的 SSRC 预登记到 side=1（answer 来自 B 侧）。
     * 必须在 bridge_rewrite_sdp 之前（rewrite 剥 a=ssrc: 行）。 */
    admit_ssrc_from_sdp(e->relay, 1, in, inlen);
    /*  2026-10-16 P0 修复补漏：offer_hook 按 b_ws 条件创建 side=1 DTLS，但 b_ws 是
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
    int a_bypass = 0;
    {
        mb_mode_t fmode;
        bridge_decide(m, NULL, 0, in, inlen, &fmode);
        /*  关键 guard：只要 offer_hook 已经决定了 SRTP_RELAY（dtls[0] 或 dtls[1]
         * 已创建），answer_hook 就绝对不能降级 BYPASS。
         * 根因：MicroSIP 内网普通 SIP answer → bridge_decide(answer-only) 会因
         * trust_private + 内网 IP 返回 MB_BYPASS，导致 adir=ASIS + port=0 透传
         * B 的原始 RTP/AVP SDP，然后叠加 SBC 的 ICE/DTLS → 四不像被 JsSIP 拒绝。
         * offer_hook 已有对称 guard（line 1325-1329），answer_hook 之前缺。 */
        const int web_rtc_any = (e->dtls[0] || e->dtls[1]);
        a_bypass = !web_rtc_any &&
            (fmode == MB_BYPASS || e->mode == MB_BYPASS
             || (!e->dtls[0] && !e->dtls[1]
                 && (bridge_decide(m, NULL, 0, NULL, 0, &fmode),
                     fmode == MB_BYPASS)));
        if (a_bypass) {
            adir = MB_SDP_ASIS;
            a_rewrite_port = 0;
        } else {
            /*  用端点标志而非 dtls[]——Phase C 后 SRTP_RELAY 双侧都建 DTLS server，
             * dtls[] 不再能区分端点类型。A 侧端点是 WebRTC → WS 模式保留 WebRTC
             * SDP 属性；否则 PLAIN 模式消毒传统 SIP answer。 */
            adir = e->a_ws ? MB_SDP_WS : MB_SDP_PLAIN;
        }
    }
    /* A 腿视频端口：只有明文 A 腿才有独立端口；WebRTC A 腿 BUNDLE 复用 a_rtp */
    uint16_t a_rewrite_vport = 0;
    if (!a_bypass && adir != MB_SDP_WS && e->relay)
        a_rewrite_vport = rtp_relay_video_port(e->relay, 0);
    int n = bridge_rewrite_sdp(in, inlen, out, outsz,
                               (e->external_rtp_ip[0]) ? e->external_rtp_ip : m->cfg.local_ip,
                               a_rewrite_port, a_rewrite_vport, adir);
    if (n < 0) return NSBC_NOSPC;
    llog(LOG_INFO, "media",
         "ANSWER_REWRITE: media_port=%u video_port=%u dir=%d a_ws=%d b_ws=%d",
         (unsigned)a_rewrite_port, (unsigned)a_rewrite_vport, adir,
         (int)e->a_ws, (int)e->b_ws);

    /*  RFC 3264 段数对齐：把 answer 截断到 A 原始 offer 的 m= 段数。
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

    /*  第 1 项：每腿独立 SDP 终结 —— 只向 A 宣告双方最终协商的 A 侧
     *  音频 codec（以及 A 自己的 telephone-event PT），绝不把 B 腿的
     *  PT 泄漏给 A；同时把定型结果写入 cnode，供 bridge_confirm 直接采用
     *  （两侧各自启发式推导会得到不一致的收发 codec）。
     *  双 WebRTC 保持 Opus/P2P 语义，BYPASS 不改写。 */
    if (!a_bypass && !(e->a_ws && e->b_ws) &&
        e->a_offer_sdp && e->a_offer_len && inlen > 0) {
        e->neg_valid = 0;   /* 先失效，只有重建成功才重新置位 */
        e->pmap_n[0] = e->pmap_n[1] = 0;   /* payload map 随协商失效 */
        e->neg_v_valid = 0;
        e->neg_v_a_pt = e->neg_v_b_pt = 0xFF;
        e->neg_v_a_cid = e->neg_v_b_cid = CODEC_NONE;
        uint8_t apts[8], bpts[8];
        codec_id_t acids[8], bcids[8];
        int an = sdp_extract_audio_codecs(e->a_offer_sdp, e->a_offer_len,
                                          apts, acids, 8);
        int bn = sdp_extract_audio_codecs(in, inlen, bpts, bcids, 8);
        int ai = -1, bi = -1;
        for (int i = 0; i < an; ++i) {
            if (acids[i] != CODEC_NONE && codec_can_decode(acids[i]) &&
                codec_can_encode(acids[i])) {
                ai = i;
                break;
            }
        }
        for (int i = 0; i < bn; ++i) {
            if (bcids[i] != CODEC_NONE && codec_can_decode(bcids[i]) &&
                codec_can_encode(bcids[i])) {
                bi = i;
                break;
            }
        }
        if (ai >= 0 && bi >= 0) {
            size_t xl = (size_t)n;
            if (sdp_xc_answer_for_a(out, &xl, outsz,
                                    e->a_offer_sdp, e->a_offer_len,
                                    (int)apts[ai])) {
                const codec_ops_t *ao = codec_registry_find(acids[ai]);
                const codec_ops_t *bo = codec_registry_find(bcids[bi]);
                n = (int)xl;
                e->neg_a_pt = apts[ai];
                e->neg_a_cid = acids[ai];
                e->neg_b_pt = bpts[bi];
                e->neg_b_cid = bcids[bi];
                e->neg_a_te = find_dtmf_pt(e->a_offer_sdp, e->a_offer_len);
                e->neg_b_te = find_dtmf_pt(in, inlen);
                e->neg_valid = 1;
                llog(LOG_INFO, "media",
                     "answer_hook: per-leg SDP termination A=%s(pt=%u) "
                     "B=%s(pt=%u) te=%u/%u",
                     ao ? ao->name : "?", (unsigned)e->neg_a_pt,
                     bo ? bo->name : "?", (unsigned)e->neg_b_pt,
                     (unsigned)e->neg_a_te, (unsigned)e->neg_b_te);
            } else {
                e->neg_a_pt = e->neg_b_pt = 0xFF;
            }
        }

        /* 视频逐腿终结：A offer / B answer 都存在 SBC 可双向处理的
         * H264/VP9 时记录映射；两侧 codec 不同由 video_xcode 转码。 */
        {
            sdp_vsel_t va, vb;
            int va_ok = sdp_video_select(e->a_offer_sdp, e->a_offer_len, &va);
            int vb_ok = sdp_video_select(in, inlen, &vb);
            if (va_ok && va.port > 0 && vb_ok && vb.port > 0) {
                /*  浏览器端白名单会改写 answer 的 SDP 文本：它删掉 offer 里
                 *  它实际仍在发送的 PT（实测 offer "96 97 98"=VP8/H264/VP9，
                 *  answer 文本只剩 "97 98"=H264/VP9，而 Chrome 实际仍按
                 *  offer 首项 96(VP8) 发流）。
                 *  若按 answer 的 PT/codec 定计划，会建出永不命中的
                 *  video_xcode(H264 97→VP8 96)，并把 96 判成"未协商 PT"
                 *  原样透传。判定：answer 的视频 PT 不在 offer 视频 fmt-list
                 *  里 → 回退为 offer 侧 codec/PT（同 codec 同 PT，纯透传）。 */
                sdp_xc_mline_t ovm;
                int in_offer = 0;
                sdp_xc_parse(e->a_offer_sdp, e->a_offer_len, SDP_M_VIDEO, &ovm);
                for (int i = 0; i < ovm.n; ++i)
                    if (ovm.c[i].pt == vb.pt) { in_offer = 1; break; }
                if (!in_offer) {
                    llog(LOG_WARN, "media",
                         "answer_hook: video B pt=%u 不在 offer fmt-list → "
                         "回退 offer codec %s(pt=%d)（answer 文本被客户端改写）",
                         (unsigned)vb.pt, va.enc, va.pt);
                    vb = va;
                }
                e->neg_v_a_pt  = (uint8_t)va.pt;
                e->neg_v_a_cid = va.cid;
                e->neg_v_b_pt  = (uint8_t)vb.pt;
                e->neg_v_b_cid = vb.cid;
                e->neg_v_valid = 1;
                llog(LOG_INFO, "media",
                     "answer_hook: video A=%s(pt=%u) B=%s(pt=%u)",
                     va.enc, (unsigned)va.pt, vb.enc, (unsigned)vb.pt);
            } else {
                llog(LOG_WARN, "media",
                     "answer_hook: video NOT negotiated — A[ok=%d port=%d "
                     "enc=%s pt=%d cid=%d] B[ok=%d port=%d enc=%s pt=%d cid=%d]",
                     va_ok, va.port, va.enc, va.pt, (int)va.cid,
                     vb_ok, vb.port, vb.enc, vb.pt, (int)vb.cid);
            }
        }
    }

    /*  answer 方向同理：A=WebRTC（a_ws=1）时，B 侧普通 SIP answer
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
            /*  关键 guard：只在 B 侧是普通 SIP（!b_ws）时补全 answer 骨架。
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
    /*  RFC 5761 最终兜底：WebRTC 腿音视频共用同一媒体端口（BUNDLE）时，
     *  answer 每个未拒绝 m= 段都必须显式带 a=rtcp-mux。骨架缺失/被上游
     *  重建丢掉时在这里补齐；已有则不重复（函数内部判断端口是否复用）。 */
    if (e->a_ws) {
        int mn = sdp_ensure_rtcp_mux(out, outsz, (size_t)n);
        if (mn > 0) {
            llog(LOG_INFO, "media",
                 "answer_hook: injected a=rtcp-mux for shared BUNDLE port "
                 "(n=%d)", mn);
            n = mn;
        }
    }
    /*  REQ-MEDIA-DIR-002：answer 的媒体段方向按 A 的 offer 重算，
     *  绝不把 B 腿 answer 的方向属性透传给 A（B 可能是 a=recvonly）。
     *  逐腿独立终结后，SBC 对 A 恒为 sendrecv 能力。 */
    if (!a_bypass && e->a_offer_sdp && e->a_offer_len) {
        size_t dl = sdp_answer_fix_directions(out, (size_t)n, outsz,
                                              e->a_offer_sdp,
                                              e->a_offer_len);
        if (dl > 0 && dl != (size_t)n) {
            llog(LOG_INFO, "media",
                 "answer_hook: per-leg media direction rewritten (%d→%zu B)",
                 n, dl);
            n = (int)dl;
        }
    }
    /*  REQ-MEDIA-SSRC-004（JSEP / SKILL.md §4.2）：A 是 WebRTC、B 是明文
     *  （mixed-mode）时，answer 的 audio 段必须有 a=ssrc 宣告 SBC 实际发出的
     *  那个 SSRC。值直接取 relay 预分配并固定使用的 plan_out_ssrc[1]
     *  （B→A 方向），与实际 RTP 头里的 SSRC 严格一致，否则 Chrome 会把
     *  收不到 sink 的包丢掉。双 WebRTC 时 SSRC 透传 B 的原值，由 B 的
     *  answer 自带 a=ssrc，这里不重复注入。 */
    if (e->a_ws && !e->b_ws && e->relay && e->relay->plan_out_ssrc[1]) {
        size_t sl = sdp_answer_add_ssrc(out, (size_t)n, outsz,
                                        e->relay->plan_out_ssrc[1]);
        if (sl > (size_t)n) {
            llog(LOG_INFO, "media",
                 "answer_hook: injected a=ssrc:%u cname:nsbc%08x for audio "
                 "section (n=%d→%zu)",
                 (unsigned)e->relay->plan_out_ssrc[1],
                 (unsigned)e->relay->plan_out_ssrc[1], n, sl);
            n = (int)sl;
        }
    }
    /* B=WebRTC：answer SDP 来自 B（此时 answer_hook 的 in 就是 B 的 answer），
     * 提取 B 的 fingerprint 注入 dtls[1]，完成后握手自动校验。
     * 注意：dtls[1] 早在 offer_hook 就已创建，当时 B 的 answer SDP 还没来，
     * 所以 B 的 fingerprint 必须推迟到这里才注入——这就是 dtls_server_set_peer_fp
     * 存在的唯一理由。 */
    if (e->dtls[1]) {
        inject_peer_fp(e->dtls[1], in, inlen);
    }
    /*  诊断：①原始 B 的 answer（改写前）②最终 answer（改写后） */
    {
        const char *lp = in;
        for (int li = 0; lp && lp < in + inlen; ++li) {
            const char *le = memchr(lp, '\n', (size_t)(in + inlen - lp));
            size_t ll = le ? (size_t)(le - lp) : strlen(lp);
            llog(LOG_INFO, "dtls", "B_ANSWER_RAW[%d]: %.*s", li, (int)ll, lp);
            lp = le ? le + 1 : NULL;
        }
    }
    {
        const char *lp = out;
        for (int li = 0; lp && lp < out + n; ++li) {
            const char *le = memchr(lp, '\n', (size_t)(out + n - lp));
            size_t ll = le ? (size_t)(le - lp) : strlen(lp);
            llog(LOG_INFO, "dtls", "ANSWER_OUT[%d]: %.*s", li, (int)ll, lp);
            lp = le ? le + 1 : NULL;
        }
    }
    return n;
}

/* ==================================================================
 * B 发起 re-INVITE 的对称逐腿终结（第 1 项 · 反向）
 * ==================================================================
 * 角色约定（与正向 hook 参数顺序保持一致：in = 收到的 SDP，out = 发出去的 SDP）：
 *   bridge_offer_hook_rev  : in = B 的原始 offer，out = 发往 A 的 offer
 *   bridge_answer_hook_rev : in = A 的原始 answer，out = 发往 B 的 answer
 *
 * 原始终态 SDP 仍由 b2bua 保管（leg[1].offer / leg[0].answer），
 * bridge_confirm 从中提取每腿 codec；本层只额外保存 B 原始 offer，
 * 用于 A 的 answer 回来时重建"回给 B 的 answer"。
 * BYPASS / 任一腿 WebRTC 保持原样（WebRTC 的 ICE/DTLS 重协商不在本期范围）。 */
int bridge_offer_hook_rev(void *call, const char *in, size_t inlen,
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
    if (!e || !in || !inlen) {
        /* 无 relay 记录 / 无 body：原样透传（不制造 cnode） */
        if (inlen >= outsz) return NSBC_NOSPC;
        if (inlen) memcpy(out, in, inlen);
        out[inlen] = '\0';
        return (int)inlen;
    }

    /* 复位上一轮终结结果，并保存 B 原始 offer（覆盖式，失败清空） */
    e->neg_valid = 0;
    e->rev_pending = 0;
    e->neg_a_pt = e->neg_b_pt = 0xFF;
    e->neg_v_valid = 0;
    e->neg_v_a_pt = e->neg_v_b_pt = 0xFF;
    e->neg_v_a_cid = e->neg_v_b_cid = CODEC_NONE;
    {
        char *cp = (char *)malloc(inlen + 1);
        if (cp) {
            memcpy(cp, in, inlen);
            cp[inlen] = '\0';
        }
        free(e->b_offer_sdp);
        e->b_offer_sdp = cp;
        e->b_offer_len = cp ? inlen : 0;
        e->rev_pending = cp ? 1 : 0;
    }

    if (inlen >= outsz) return NSBC_NOSPC;
    memcpy(out, in, inlen);
    out[inlen] = '\0';
    int n = (int)inlen;

    /* BYPASS / 任一腿 WebRTC：不改写（保持既有透传语义） */
    if (!e->relay || e->mode == MB_BYPASS || e->a_ws || e->b_ws)
        return n;

    /* 双明文：先把 B 的 offer 锚定到 relay 的 A 侧端口（对称于正向
     * offer → B 用 b_rtp），否则 A 会拿到 B 的原始地址，re-INVITE 后
     * 两端直连绕过 SBC，转码与录音同时失效。dir=ASIS 只改端口/c=。 */
    {
        int rn = bridge_rewrite_sdp(in, inlen, out, outsz,
                                    (e->external_rtp_ip[0]) ? e->external_rtp_ip
                                                            : m->cfg.local_ip,
                                    rtp_relay_port(e->relay, 0),
                                    rtp_relay_video_port(e->relay, 0),
                                    MB_SDP_ASIS);
        if (rn < 0) return NSBC_NOSPC;
        n = rn;
        out[n] = '\0';
    }

    /* B → A：在 B 的 fmt-list 尾部追加 SBC 可双向处理的静态音频 PT，
     * 让 A 有机会选中 SBC 支持且可转码的 codec。 */
    size_t xl = (size_t)n;
    int xadd = sdp_xc_add_offer_audio(out, &xl, outsz);
    if (xadd > 0) {
        n = (int)xl;
        llog(LOG_INFO, "media",
             "offer_rev_hook: per-leg SDP termination — appended %d SBC audio PT(s), n=%d",
             xadd, n);
    }
    return n;
}

int bridge_answer_hook_rev(void *call, const char *in, size_t inlen,
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
    if (!e || !e->relay || !in || !inlen) {
        if (inlen >= outsz) return NSBC_NOSPC;
        if (inlen) memcpy(out, in, inlen);
        out[inlen] = '\0';
        return (int)inlen;
    }
    if (e->mode == MB_BYPASS || e->a_ws || e->b_ws ||
        !e->rev_pending || !e->b_offer_sdp || !e->b_offer_len) {
        if (inlen >= outsz) return NSBC_NOSPC;
        memcpy(out, in, inlen);
        out[inlen] = '\0';
        return (int)inlen;
    }
    e->rev_pending = 0;   /* 只处理这一轮 offer 对应的 answer */

    e->neg_valid = 0;   /* 先失效，只有重建成功才重新置位 */
    e->pmap_n[0] = e->pmap_n[1] = 0;   /* payload map 随协商失效 */
    e->neg_v_valid = 0;
    e->neg_v_a_pt = e->neg_v_b_pt = 0xFF;
    e->neg_v_a_cid = e->neg_v_b_cid = CODEC_NONE;
    uint8_t apts[8], bpts[8];
    codec_id_t acids[8], bcids[8];
    int an = sdp_extract_audio_codecs(in, inlen, apts, acids, 8);
    int bn = sdp_extract_audio_codecs(e->b_offer_sdp, e->b_offer_len,
                                      bpts, bcids, 8);
    int ai = -1, bi = -1;
    for (int i = 0; i < an; ++i) {
        if (acids[i] != CODEC_NONE && codec_can_decode(acids[i]) &&
            codec_can_encode(acids[i])) {
            ai = i;
            break;
        }
    }
    for (int i = 0; i < bn; ++i) {
        if (bcids[i] != CODEC_NONE && codec_can_decode(bcids[i]) &&
            codec_can_encode(bcids[i])) {
            bi = i;
            break;
        }
    }
    if (ai < 0 || bi < 0) {
        e->neg_a_pt = e->neg_b_pt = 0xFF;
        if (inlen >= outsz) return NSBC_NOSPC;
        memcpy(out, in, inlen);
        out[inlen] = '\0';
        return (int)inlen;
    }

    /* 回给 B 的 answer 先锚定到 relay 的 B 侧端口（对称于正向 answer → A
     * 用 a_rtp），否则 B 会把远端地址改成 A，re-INVITE 后绕过 SBC。 */
    int rn = bridge_rewrite_sdp(in, inlen, out, outsz,
                                (e->external_rtp_ip[0]) ? e->external_rtp_ip
                                                        : m->cfg.local_ip,
                                rtp_relay_port(e->relay, 1),
                                rtp_relay_video_port(e->relay, 1),
                                MB_SDP_PLAIN);
    if (rn < 0) return NSBC_NOSPC;
    out[rn] = '\0';

    /* 回给 B 的 answer 只保留 B 侧最终音频 codec 与 B 的 telephone-event PT，
     * 绝不把 A 腿的 PT 泄漏给 B（否则 B 收到未 offer 过的 PT → 488/BYE）。 */
    size_t xl = (size_t)rn;
    if (!sdp_xc_answer_for_a(out, &xl, outsz, e->b_offer_sdp, e->b_offer_len,
                             (int)bpts[bi])) {
        e->neg_a_pt = e->neg_b_pt = 0xFF;
        if (inlen >= outsz) return NSBC_NOSPC;
        memcpy(out, in, inlen);
        out[inlen] = '\0';
        return (int)inlen;
    }
    e->neg_a_pt = apts[ai];
    e->neg_a_cid = acids[ai];
    e->neg_b_pt = bpts[bi];
    e->neg_b_cid = bcids[bi];
    e->neg_a_te = find_dtmf_pt(in, inlen);
    e->neg_b_te = find_dtmf_pt(e->b_offer_sdp, e->b_offer_len);
    e->neg_valid = 1;
    {
        const codec_ops_t *ao = codec_registry_find(acids[ai]);
        const codec_ops_t *bo = codec_registry_find(bcids[bi]);
        llog(LOG_INFO, "media",
             "answer_rev_hook: per-leg SDP termination A=%s(pt=%u) "
             "B=%s(pt=%u) te=%u/%u",
             ao ? ao->name : "?", (unsigned)e->neg_a_pt,
             bo ? bo->name : "?", (unsigned)e->neg_b_pt,
             (unsigned)e->neg_a_te, (unsigned)e->neg_b_te);
    }

    /* 视频逐腿终结（B 发起 re-INVITE）：B offer / A answer 的映射。
     * 注意从 in（A 原始 answer）取 A 侧视频 codec，不能从 out 取——
     * out 已被 sdp_xc_answer_for_a 改写成 B 侧 codec。 */
    {
        sdp_vsel_t vb_off, va_ans;
        if (sdp_video_select(e->b_offer_sdp, e->b_offer_len, &vb_off) &&
            vb_off.port > 0 &&
            sdp_video_select(in, inlen, &va_ans) && va_ans.port > 0) {
            e->neg_v_b_pt  = (uint8_t)vb_off.pt;
            e->neg_v_b_cid = vb_off.cid;
            e->neg_v_a_pt  = (uint8_t)va_ans.pt;
            e->neg_v_a_cid = va_ans.cid;
            e->neg_v_valid = 1;
            llog(LOG_INFO, "media",
                 "answer_rev_hook: video B=%s(pt=%u) A=%s(pt=%u)",
                 vb_off.enc, (unsigned)vb_off.pt,
                 va_ans.enc, (unsigned)va_ans.pt);
        }
    }
    return (int)xl;
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
            /*  转码会话清理（REQ-TRANS-004）：当前 + re-INVITE 退役的旧会话 */
            for (int si = 0; si < 2; ++si) {
                transcode_sess_t *tc =
                    __atomic_load_n(&e->transcode[si], __ATOMIC_ACQUIRE);
                if (tc) transcode_close(tc);
                e->transcode[si] = NULL;
            }
            for (int i = 0; i < e->n_xcode_retired; ++i) {
                if (e->xcode_retired[i])
                    transcode_close(e->xcode_retired[i]);
                e->xcode_retired[i] = NULL;
            }
            e->n_xcode_retired = 0;
            /*  视频转码会话清理（VP9↔H264）：当前 + re-INVITE 退役的旧会话 */
            for (int si = 0; si < 2; ++si) {
                video_xcode_t *vx =
                    __atomic_load_n(&e->vxcode[si], __ATOMIC_ACQUIRE);
                if (vx) video_xcode_destroy(vx);
                e->vxcode[si] = NULL;
            }
            for (int i = 0; i < e->n_vxcode_retired; ++i) {
                if (e->vxcode_retired[i])
                    video_xcode_destroy(e->vxcode_retired[i]);
                e->vxcode_retired[i] = NULL;
            }
            e->n_vxcode_retired = 0;
            /*  DTMF RFC4733 去重器清理 */
            for (int si = 0; si < 2; ++si) {
                if (e->dtmf_rx[si]) {
                    dtmf_rx_destroy((dtmf_rx_t *)e->dtmf_rx[si]);
                    e->dtmf_rx[si] = NULL;
                }
                e->dtmf_pt[si] = 0;
            }
            /* M1 视频重组器清理（rtp_relay_stop 已置 dead=1，
             * relay_recv_one 循环退出后才走到这里 —— 无并发 feed） */
            for (int si = 0; si < 2; ++si) {
                if (e->vid_reasm[si]) {
                    vid_reasm_destroy(e->vid_reasm[si]);
                    e->vid_reasm[si] = NULL;
                }
            }
            /* M2 录制定向转码清理（VP8/VP9/H.265 → H.264） */
            for (int si = 0; si < 2; ++si) {
                video_xcode_t *rx =
                    __atomic_load_n(&e->rec_xcode[si], __ATOMIC_ACQUIRE);
                if (rx) video_xcode_destroy(rx);
                e->rec_xcode[si] = NULL;
            }
            /* 观察实例（tap）清理：只解包，无解码/编码资源 */
            for (int si = 0; si < 2; ++si) {
                video_xcode_t *tx =
                    __atomic_load_n(&e->tap_xcode[si], __ATOMIC_ACQUIRE);
                if (tx) video_xcode_destroy(tx);
                e->tap_xcode[si] = NULL;
            }
            free(e->a_offer_sdp);
            e->a_offer_sdp = NULL;
            e->a_offer_len = 0;
            free(e->b_offer_sdp);
            e->b_offer_sdp = NULL;
            e->b_offer_len = 0;
            free(e->b_answer_sdp);
            e->b_answer_sdp = NULL;
            e->b_answer_len = 0;
            free(e);
            return;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s->lock);
    llog(LOG_INFO, "media", "bridge_release_call: call=%p NOT FOUND in shard", call);
}

/*  HA P1: 反向绑定 — 给定新 call_key 挂入 bridge_mgr shard。
 * 和 bridge_release_call 对称：同 ptr_shard → 同 shard 锁 → 同 cnode 生命周期。
 * rebuild 侧负责 rtp_relay_start + set_peer 把 relay 内部状态建好再调此函数。 */
int bridge_register_call(bridge_mgr_t *m, void *call_key,
                         rtp_relay_t *relay, mb_mode_t mode)
{
    if (!m || !call_key || !relay) return NSBC_INVAL;

    br_shard_t *s = &m->sh[ptr_shard(call_key)];
    pthread_mutex_lock(&s->lock);

    /* 幂等: 已存在就跳过。FULL resync 重发同一 dialog 多次时这里会 hit */
    cnode_t *dup = entry_find(m, call_key);
    if (dup) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_AGAIN;
    }

    cnode_t *ne = malloc(sizeof(*ne));
    if (!ne) { pthread_mutex_unlock(&s->lock); return NSBC_NOMEM; }
    memset(ne, 0, sizeof(*ne));
    ne->call  = call_key;
    ne->relay = relay;
    ne->mode  = mode;
    ne->next  = s->head;
    s->head   = ne;

    pthread_mutex_unlock(&s->lock);
    llog(LOG_INFO, "media", "HA rebuild: bridge_register_call call=%p relay=%p mode=%d",
         call_key, (void *)relay, mode);
    return NSBC_OK;
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
    /*  单腿本地媒体：只有 A 侧（side=0），AI 注入一律发回 A，
     * 否则 1-leg 会算到不存在对侧 → -ENODEV（"听不到 AI"根因之一）。 */
    if (r->node && r->node->local_media)
        dst_side = 0;
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
            __atomic_add_fetch(&m->rec_hook_waiters, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&target->refs, __ATOMIC_SEQ_CST) > 0)
                pthread_cond_wait(&m->rec_hook_cond, &m->rec_hook_lock);
            __atomic_sub_fetch(&m->rec_hook_waiters, 1, __ATOMIC_SEQ_CST);
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
        /*  防御性 fallback：sdp_parse 因 attr 超限/未知 type 行等失败时，
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
        /*  关键：sdp_parse 只填 fmt[]，codecs[] 是延迟构建的
         * 不调 build_codecs → ncodecs=0 → pt_allowed=(n=0) → 所有 RTP 包被静默丢弃 */
        if (!m->codecs_built)
            sdp_media_build_codecs(&s, m);
        llog(LOG_INFO, "media",
             "sdp_extract_audio_codecs: after build_codecs ncodecs=%d", m->ncodecs);
        int n = 0;
        for (int j = 0; j < m->ncodecs && n < out_cap; ++j) {
            const sdp_codec_t *c = &m->codecs[j];
            int rtp_rate = 0;
            codec_id_t cid = CODEC_NONE;
            out_pts[n] = (uint8_t)c->pt;
            if (c->rate.len > 0 && c->rate.p) {
                int v = 0;
                for (size_t k = 0; k < c->rate.len &&
                                   c->rate.p[k] >= '0' && c->rate.p[k] <= '9'; ++k)
                    v = v * 10 + (c->rate.p[k] - '0');
                rtp_rate = v;
            }
            llog(LOG_INFO, "media",
                 "sdp_extract_audio_codecs: codec[%d] pt=%u enc.len=%zu enc=[%.*s]",
                 j, (unsigned)c->pt, c->enc.len,
                 (int)c->enc.len, c->enc.p ? c->enc.p : "(null)");

            /* 名字做主键：rtpmap enc name → codec（覆盖动态 PT）；
             * 命中后校验 RTP 时钟率，G.722 特判 8000/16000 */
            if (c->enc.len > 0 && c->enc.p) {
                const codec_ops_t *ops =
                    codec_registry_find_name(c->enc.p, c->enc.len);
                if (ops != NULL) {
                    int ok = 1;
                    if (rtp_rate > 0) {
                        if (ops->id == CODEC_G722)
                            ok = (rtp_rate == 8000 || rtp_rate == 16000);
                        else
                            ok = (rtp_rate == codec_rtp_clock_rate(ops));
                    }
                    if (ok) {
                        cid = ops->id;
                    } else {
                        llog(LOG_WARN, "media",
                             "codec name=[%.*s] rate=%d 与注册表 rtp_rate=%d 不符，"
                             "按 PT 回落 pt=%u",
                             (int)c->enc.len, c->enc.p, rtp_rate,
                             codec_rtp_clock_rate(ops), (unsigned)c->pt);
                    }
                }
            }
            /* 名字未命中 / 校验失败 → 静态 PT 表 */
            if (cid == CODEC_NONE)
                cid = pt_to_codec(c->pt);
            out_cids[n] = cid;
            ++n;
        }
        llog(LOG_INFO, "media", "sdp_extract_audio_codecs: audio media return n=%d", n);
        return n;
    }
    llog(LOG_WARN, "media", "sdp_extract_audio_codecs: no audio m-line, nmedia=%d", s.nmedia);
    return 0;
}

/* REQ-MEDIA-VIDEO-003：从 SDP 所有 m=video 行的 fmt-list 提取 PT。
 * 复用 audio 版本的 sdp_parse + fallback raw scan 双路径，
 * 但只返回 PT（不需要 codec_id——video 转码没注册）。
 * 多次 m=video（rare but 合法）的 PT 合并去重。 */
static int sdp_extract_video_codecs(const char *sdp, size_t len,
                                    uint8_t *out_pts, int out_cap)
{
    int n = 0;
    sdp_t s;
    if (sdp_parse(&s, sdp, len) == NSBC_OK) {
        for (int i = 0; i < s.nmedia && n < out_cap; ++i) {
            sdp_media_t *m = &s.media[i];
            if (m->type != SDP_M_VIDEO) continue;
            /* m=video 0（含 SBC 为纯音频补的 m=video 0 99）表示媒体被拒绝/停用，
             * 不能算该腿有视频；否则抓图会空等关键帧直到 NO_KEYFRAME。 */
            if (m->rejected || m->port <= 0) continue;
            if (!m->codecs_built)
                sdp_media_build_codecs(&s, m);
            for (int j = 0; j < m->ncodecs && n < out_cap; ++j) {
                uint8_t pt = (uint8_t)m->codecs[j].pt;
                /* 去重 */
                int dup = 0;
                for (int k = 0; k < n; ++k)
                    if (out_pts[k] == pt) { dup = 1; break; }
                if (!dup) {
                    out_pts[n++] = pt;
                    llog(LOG_INFO, "media",
                         "sdp_extract_video_codecs: video media[%d] codec[%d] pt=%u",
                         i, j, (unsigned)pt);
                }
            }
        }
    }
    if (n > 0) {
        llog(LOG_INFO, "media", "sdp_extract_video_codecs: return n=%d PTs (via sdp_parse)", n);
        return n;
    }

    /* Fallback：sdp_parse 失败或没找到 video m-line —— raw scan */
    const char *p = sdp;
    const char *end = sdp + len;
    while (p < end && n < out_cap) {
        if ((size_t)(end - p) >= 8 &&
            p[0] == 'm' && p[1] == '=' &&
            strncmp(p + 2, "video", 5) == 0 &&
            (p[7] == ' ' || p[7] == '\t')) {
            const char *q = p + 8; /* 跳过 "m=video " */
            /* 先取 port：0 = 媒体被拒绝/停用，跳过本行（与 sdp_parse 路径一致） */
            char *pe = NULL;
            long vport = strtol(q, &pe, 10);
            if (pe == q || vport <= 0) {
                while (p < end && *p != '\n') ++p;
                if (p < end && *p == '\n') ++p;
                continue;
            }
            /* 跳过 port 和 proto */
            int field = 0;
            while (q < end && field < 2) {
                while (q < end && *q != ' ' && *q != '\r' && *q != '\n') ++q;
                ++field;
                if (q < end && *q == ' ') ++q;
            }
            /* fmt-list */
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
                    int dup = 0;
                    for (int k = 0; k < n; ++k)
                        if (out_pts[k] == (uint8_t)pt) { dup = 1; break; }
                    if (!dup) out_pts[n++] = (uint8_t)pt;
                }
            }
        }
        while (p < end && *p != '\n') ++p;
        if (p < end && *p == '\n') ++p;
    }
    if (n > 0)
        llog(LOG_INFO, "media",
             "sdp_extract_video_codecs: FALLBACK raw scan got n=%d PTs", n);
    else
        llog(LOG_DEBUG, "media",
             "sdp_extract_video_codecs: no video m-line, nmedia=%zu",
             sdp_parse(&s, sdp, len) == NSBC_OK ? (size_t)s.nmedia : 0);
    return n;
}

/* 选择转码目标：候选必须来自"目标腿"的 codec 列表（不是源腿），
 * 且该 codec 必须能编码。优先选源腿也支持的 codec（便于对侧 re-INVITE）。
 * out_pt 返回目标腿 SDP 中该 codec 的协商 PT。 */
static codec_id_t find_xcode_dst(codec_id_t src,
                                 const uint8_t *dst_pts,
                                 const codec_id_t *dst_cids, int dst_n,
                                 const codec_id_t *src_cids, int src_n,
                                 uint8_t *out_pt)
{
    if (out_pt) *out_pt = 0xFF;
    /* 第一优先级：目标腿 codec ∩ 源腿 codec */
    for (int i = 0; i < dst_n; ++i) {
        if (dst_cids[i] == CODEC_NONE || dst_cids[i] == src) continue;
        if (!codec_can_encode(dst_cids[i])) continue;
        for (int j = 0; j < src_n; ++j) {
            if (src_cids[j] == dst_cids[i]) {
                if (out_pt) *out_pt = dst_pts[i];
                return dst_cids[i];
            }
        }
    }
    /* 第二优先级：目标腿第一个可编码 codec */
    for (int i = 0; i < dst_n; ++i) {
        if (dst_cids[i] == CODEC_NONE || dst_cids[i] == src) continue;
        if (!codec_can_encode(dst_cids[i])) continue;
        if (out_pt) *out_pt = dst_pts[i];
        return dst_cids[i];
    }
    return CODEC_NONE;
}

/* 安全替换转码会话：旧会话可能仍被媒体线程读取，延迟到 release 释放 */
static int cnode_install_xcode(cnode_t *e, int side, transcode_sess_t *tc,
                               uint8_t src_pt, uint8_t dst_pt)
{
    transcode_sess_t *old;
    if (e == NULL || side < 0 || side > 1) return NSBC_INVAL;
    old = __atomic_load_n(&e->transcode[side], __ATOMIC_ACQUIRE);
    if (old != NULL) {
        if (e->n_xcode_retired >= (int)(sizeof(e->xcode_retired) /
                                        sizeof(e->xcode_retired[0]))) {
            llog(LOG_WARN, "media",
                 "xcode retire list full (call=%p side=%d) — keep old session",
                 e->call, side);
            return NSBC_BUSY;
        }
        e->xcode_retired[e->n_xcode_retired++] = old;
    }
    transcode_set_dst_pt(tc, dst_pt);
    e->xcode_src_pt[side] = src_pt;
    e->xcode_dst_pt[side] = dst_pt;
    e->xcode_sig[side] = ((int)transcode_dst_codec_id(tc) << 16) | (int)dst_pt;
    __atomic_store_n(&e->transcode[side], tc, __ATOMIC_RELEASE);
    return NSBC_OK;
}

static void cnode_drop_xcode(cnode_t *e, int side)
{
    transcode_sess_t *old;
    if (e == NULL || side < 0 || side > 1) return;
    old = __atomic_load_n(&e->transcode[side], __ATOMIC_ACQUIRE);
    if (old == NULL) {
        e->xcode_dst_pt[side] = 0xFF;
        e->xcode_sig[side] = 0;
        return;
    }
    if (e->n_xcode_retired >= (int)(sizeof(e->xcode_retired) /
                                    sizeof(e->xcode_retired[0]))) {
        llog(LOG_WARN, "media", "xcode retire list full — cannot drop call=%p",
             e->call);
        return;
    }
    __atomic_store_n(&e->transcode[side], NULL, __ATOMIC_RELEASE);
    e->xcode_retired[e->n_xcode_retired++] = old;
    e->xcode_src_pt[side] = 0xFF;
    e->xcode_dst_pt[side] = 0xFF;
    e->xcode_sig[side] = 0;
}

/* 视频转码会话退役：只摘指针，真正的 destroy 在 bridge_release_call
 * （媒体线程可能仍持有旧指针，必须等呼叫结束再释放）。 */
static void cnode_retire_vxcode(cnode_t *e, int side, video_xcode_t *old)
{
    if (!e || !old) return;
    if (e->n_vxcode_retired >= (int)(sizeof(e->vxcode_retired) /
                                    sizeof(e->vxcode_retired[0]))) {
        /* 退役槽满：宁可继续用新会话，旧会话泄漏到 release 时统一清理 */
        llog(LOG_WARN, "media", "vxcode retire list full call=%p", e->call);
        return;
    }
    e->vxcode_retired[e->n_vxcode_retired++] = old;
    (void)side;
}

static void cnode_drop_vxcode(cnode_t *e, int side)
{
    if (!e || side < 0 || side > 1) return;
    video_xcode_t *old = __atomic_load_n(&e->vxcode[side], __ATOMIC_ACQUIRE);
    if (old) {
        __atomic_store_n(&e->vxcode[side], NULL, __ATOMIC_RELEASE);
        cnode_retire_vxcode(e, side, old);
    }
    e->vxcode_sig[side] = 0;
    e->vxcode_src_pt[side] = 0xFF;
    e->vxcode_dst_pt[side] = 0xFF;
    e->video_rewrite_active[side] = 0;
}

/* 录制定向转码会话退役：与 vxcode 一样只摘指针，destroy 留到 release，
 * 避免媒体线程仍在使用旧实例。 */
static void cnode_drop_rec_xcode(cnode_t *e, int side)
{
    if (!e || side < 0 || side > 1) return;
    video_xcode_t *old = __atomic_load_n(&e->rec_xcode[side], __ATOMIC_ACQUIRE);
    if (old) {
        __atomic_store_n(&e->rec_xcode[side], NULL, __ATOMIC_RELEASE);
        cnode_retire_vxcode(e, side, old);
    }
    e->rec_xcode_sig[side] = 0;
    e->rec_xcode_pt[side] = 0xFF;
}

/* 观察实例退役：同 rec_xcode，destroy 延后到 release */
static void cnode_drop_tap(cnode_t *e, int side)
{
    if (!e || side < 0 || side > 1) return;
    video_xcode_t *old = __atomic_load_n(&e->tap_xcode[side], __ATOMIC_ACQUIRE);
    if (old) {
        __atomic_store_n(&e->tap_xcode[side], NULL, __ATOMIC_RELEASE);
        cnode_retire_vxcode(e, side, old);
    }
    e->tap_xcode_sig[side] = 0;
    e->tap_xcode_pt[side] = 0xFF;
}


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

/* 构建单腿 payload map（§2.2）：只收录该腿最终接受的 PT，名字做主键，
 * fmtp/rate/channels 一并保存；SDP 解析失败时按协商 PT+cid 列表兜底。 */
static void build_leg_pmap(cnode_t *e, int side, const char *sdp, size_t len,
                           const uint8_t *allow, const codec_id_t *allow_cids,
                           int allow_n)
{
    const int cap = (int)NSBC_ARRAY_SIZE(e->pmap[0]);
    sdp_t s;
    int n = 0;
    if (e == NULL || side < 0 || side > 1) return;
    e->pmap_n[side] = 0;
    if (sdp == NULL || len == 0 || allow == NULL || allow_n <= 0) return;
    memset(&s, 0, sizeof(s));
    if (sdp_parse(&s, sdp, len) == NSBC_OK) {
        for (int i = 0; i < s.nmedia; ++i) {
            sdp_media_t *m = &s.media[i];
            if (m->type != SDP_M_AUDIO) continue;
            if (!m->codecs_built) sdp_media_build_codecs(&s, m);
            for (int j = 0; j < m->ncodecs && n < cap; ++j) {
                const sdp_codec_t *c = &m->codecs[j];
                char chbuf[8], fbuf[128];
                int rate = 0, ch = 0;
                if (!pt_in_list((uint8_t)c->pt, allow, allow_n)) continue;
                if (c->rate.p && c->rate.len) {
                    for (size_t k = 0; k < c->rate.len &&
                                       c->rate.p[k] >= '0' &&
                                       c->rate.p[k] <= '9'; ++k)
                        rate = rate * 10 + (c->rate.p[k] - '0');
                }
                if (c->channels.p && c->channels.len) {
                    tok_copy(chbuf, sizeof(chbuf), c->channels);
                    ch = atoi(chbuf);
                }
                tok_copy(fbuf, sizeof(fbuf), c->fmtp);
                n = codec_payload_map_add(e->pmap[side], cap, n,
                                          c->pt, c->enc.p, c->enc.len,
                                          rate, ch, fbuf[0] ? fbuf : NULL);
            }
            break;   /* 只看第一个 audio m-line，与 sdp_extract 同口径 */
        }
    }
    /* 兜底：名字未命中/SDP 不可解析时，用协商 PT + codec_id 补齐条目，
     * 保证 relay 判定与旁路载荷识别不依赖 SDP 文本。 */
    for (int i = 0; i < allow_n && n < cap; ++i) {
        leg_payload_t *x;
        const codec_ops_t *ops;
        if (codec_payload_map_find(e->pmap[side], n, allow[i]) != NULL)
            continue;
        if (allow_cids == NULL || allow_cids[i] == CODEC_NONE) continue;
        ops = codec_registry_find(allow_cids[i]);
        x = &e->pmap[side][n++];
        memset(x, 0, sizeof(*x));
        x->pt = allow[i];
        x->cid = allow_cids[i];
        x->rtp_rate = ops ? codec_rtp_clock_rate(ops) : 8000;
        x->pcm_rate = ops ? codec_pcm_sample_rate(ops) : x->rtp_rate;
        x->channels = (ops && ops->channels > 0) ? ops->channels : 1;
        x->ptime_hint = (ops && ops->ptime_ms > 0) ? ops->ptime_ms : 20;
        x->is_event = (ops && (ops->caps & CODEC_CAP_EVENT)) ? 1 : 0;
        if (x->cid == CODEC_TELEPHONE_EVENT || x->cid == CODEC_CN)
            x->is_event = 1;
    }
    e->pmap_n[side] = n;
    llog(LOG_INFO, "media", "payload map side=%d n=%d", side, n);
}

int bridge_confirm(bridge_mgr_t *m, void *call, const char *offer,
                   size_t offer_len, const char *answer, size_t answer_len)
{
    if (!m || !call || !offer || !offer_len || !answer || !answer_len)
        return NSBC_INVAL;

    /*  完整 SDP dump：offer=A 腿原始 INVITE SDP；answer 是调用方传入的
     *  SDP（入呼场景为 answer hook 回给 A 的改写 answer，B 原始 200 OK
     *  另由 b_answer_sdp stash，见下方解析）
     * 分段打（每 256B 一行），避免单行过长被日志截断 */
    llog(LOG_INFO, "media",
         "BRIDGE_CONFIRM call=%p offer_len=%zu answer_len=%zu",
         call, offer_len, answer_len);
    llog(LOG_INFO, "media", "OFFER_DUMP_BEGIN total=%zu", offer_len);
    for (size_t off = 0; off < offer_len; off += 256) {
        size_t chunk = offer_len - off;
        if (chunk > 256) chunk = 256;
        llog(LOG_INFO, "media", "OFFER[%zu..%zu]: %.*s",
             off, off + chunk, (int)chunk, offer + off);
    }
    llog(LOG_INFO, "media", "OFFER_DUMP_END");
    llog(LOG_INFO, "media", "ANSWER_DUMP_BEGIN total=%zu", answer_len);
    for (size_t off = 0; off < answer_len; off += 256) {
        size_t chunk = answer_len - off;
        if (chunk > 256) chunk = 256;
        llog(LOG_INFO, "media", "ANSWER[%zu..%zu]: %.*s",
             off, off + chunk, (int)chunk, answer + off);
    }
    llog(LOG_INFO, "media", "ANSWER_DUMP_END");

    /* bridge_decide 全路径返回 NSBC_OK（与 offer/answer hook 同口径，
     * 不检查返回值）；初值兜底，决策结果必被其写入。 */
    mb_mode_t final_mode = MB_BYPASS;

    /*  A 腿（side 0）codec 列表：offer 就是 A 的原始 INVITE SDP。
     *  完整逐腿 PT/cid 列表（payload map 输入）——逐腿终结会把 offer_n/answer_n
     *  收窄到 1 条最终 codec，但 map 需要保留全部协商 PT（含 telephone-event/
     *  CN 旁路载荷），否则事件包无法识别与转发。 */
    uint8_t offer_pts[8];
    codec_id_t offer_cids[8];
    int offer_n = sdp_extract_audio_codecs(offer, offer_len, offer_pts, offer_cids, 8);
    uint8_t offer_all_pts[8];
    codec_id_t offer_all_cids[8];
    int offer_all_n = offer_n;
    if (offer_all_n > 0) {
        memcpy(offer_all_pts, offer_pts, (size_t)offer_all_n);
        memcpy(offer_all_cids, offer_cids,
               sizeof(codec_id_t) * (size_t)offer_all_n);
    }

    /*  REQ-MEDIA-VIDEO-003：video PT 提取 —— 独立于 audio codec，
     * 因为 pt_allowed[8] 槽给 audio 用刚好够。video PT 单独存 video_pt_allowed。 */
    uint8_t offer_video_pts[8];
    int offer_video_n = sdp_extract_video_codecs(offer, offer_len, offer_video_pts, 8);

    br_shard_t *s = &m->sh[ptr_shard(call)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call);
    if (!e) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_NOENT;
    }

    /*  ★ B 腿（side 1）的 codec/PT 必须基于 B 的原始 200 OK：
     *  answer 参数是 answer hook 回给 A 的改写 SDP（A 侧 PT 编号），
     *  拿它建 pmap[1]/answer_pts 会让"A→B 的入包"误判对端接受同 codec，
     *  运行时跳过转码直接 relay（浏览器收到 G.729 无声）。
     *  bridge_answer_hook 已把 B 原始 200 OK stash 到 b_answer_sdp 并置
     *  fresh；没有 fresh（B 发起 re-INVITE 方向反转）时回退 answer 参数。 */
    const char *b_sdp = answer;
    size_t b_sdp_len = answer_len;
    if (e->b_answer_fresh && e->b_answer_sdp && e->b_answer_len) {
        b_sdp = e->b_answer_sdp;
        b_sdp_len = e->b_answer_len;
    }
    e->b_answer_fresh = 0;
    llog(LOG_INFO, "media",
         "BRIDGE_B_SDP call=%p source=%s len=%zu",
         call, (b_sdp == answer) ? "param" : "raw200", b_sdp_len);

    bridge_decide(m, offer, offer_len, b_sdp, b_sdp_len, &final_mode);

    /*  通用 codec 列表提取（PT + codec_id）
     * 无论 bridge_decide 返回什么，都解析拿到两侧 audio codec 列表，
     * 为运行时通用转码做准备（G.711 互转 + G.729 互转 + 跨 codec）。 */
    uint8_t answer_pts[8];
    codec_id_t answer_cids[8];
    int answer_n = sdp_extract_audio_codecs(b_sdp, b_sdp_len, answer_pts, answer_cids, 8);
    uint8_t answer_all_pts[8];
    codec_id_t answer_all_cids[8];
    int answer_all_n = answer_n;
    if (answer_all_n > 0) {
        memcpy(answer_all_pts, answer_pts, (size_t)answer_all_n);
        memcpy(answer_all_cids, answer_cids,
               sizeof(codec_id_t) * (size_t)answer_all_n);
    }
    uint8_t answer_video_pts[8];
    int answer_video_n = sdp_extract_video_codecs(b_sdp, b_sdp_len, answer_video_pts, 8);

    if (e->relay) {
        /*  第 1 项：逐腿 SDP 终结后两侧音频 codec 已在 answer_hook 定型，
         *  直接用这对 (A,B) 做转码计划，保证与回给 A 的 answer 完全一致。
         *  offer_n==0（如 originate 无 A 腿 offer）不采用，保持原行为。 */
        if (e->neg_valid && offer_n > 0 && answer_n > 0 &&
            e->neg_a_pt != 0xFF && e->neg_b_pt != 0xFF) {
            offer_pts[0] = e->neg_a_pt;
            offer_cids[0] = e->neg_a_cid;
            offer_n = 1;
            answer_pts[0] = e->neg_b_pt;
            answer_cids[0] = e->neg_b_cid;
            answer_n = 1;
            llog(LOG_INFO, "media",
                 "BRIDGE_TERMINATED call=%p A_pt=%u A_cid=%d B_pt=%u B_cid=%d",
                 call, (unsigned)offer_pts[0], (int)offer_cids[0],
                 (unsigned)answer_pts[0], (int)answer_cids[0]);
        }
        /* 媒体面已锚定：不降级 bypass
         *
         *  pt_allowed[side] 语义：转发到 side 这一侧时，对端接受的 PT 列表
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

        /* 逐腿音频 codec 快照（运行时入包校验 + 出包 PT 反查）*/
        e->audio_n[0] = offer_n;
        memcpy(e->audio_pt[0], offer_pts, (size_t)offer_n);
        memcpy(e->audio_cid[0], offer_cids, sizeof(codec_id_t) * (size_t)offer_n);
        e->audio_n[1] = answer_n;
        memcpy(e->audio_pt[1], answer_pts, (size_t)answer_n);
        memcpy(e->audio_cid[1], answer_cids, sizeof(codec_id_t) * (size_t)answer_n);

        /*  逐腿终结后每腿只协商一个音频 codec（+ telephone-event）：
         *  pmap 收窄到该 PT，否则 peer_accepts_payload 会拿"offer/answer
         *  里列过但未选中"的 codec 误判可零转码中继（例如 A 的 offer 里
         *  也列了 PCMA，但最终协商是 G.729）。 */
        const uint8_t *map_pts0 = offer_all_pts;
        const codec_id_t *map_cids0 = offer_all_cids;
        int map_n0 = offer_all_n;
        const uint8_t *map_pts1 = answer_all_pts;
        const codec_id_t *map_cids1 = answer_all_cids;
        int map_n1 = answer_all_n;
        uint8_t narrow0[2], narrow1[2];
        codec_id_t narrow_c0[2], narrow_c1[2];
        if (e->neg_valid && e->neg_a_pt != 0xFF && e->neg_b_pt != 0xFF) {
            int n0 = 0, n1 = 0;
            narrow0[n0] = e->neg_a_pt;
            narrow_c0[n0] = e->neg_a_cid;
            ++n0;
            if (e->neg_a_te) {
                narrow0[n0] = e->neg_a_te;
                narrow_c0[n0] = CODEC_TELEPHONE_EVENT;
                ++n0;
            }
            narrow1[n1] = e->neg_b_pt;
            narrow_c1[n1] = e->neg_b_cid;
            ++n1;
            if (e->neg_b_te) {
                narrow1[n1] = e->neg_b_te;
                narrow_c1[n1] = CODEC_TELEPHONE_EVENT;
                ++n1;
            }
            map_pts0 = narrow0;
            map_cids0 = narrow_c0;
            map_n0 = n0;
            map_pts1 = narrow1;
            map_cids1 = narrow_c1;
            map_n1 = n1;
        }

        /* 每腿 payload map（§2.2）：名字做主键、PT 做索引，保存 rate/fmtp。
         * 运行时 relay/transcode 判定与旁路载荷转发都查这张表。 */
        build_leg_pmap(e, 0, offer, offer_len,
                       map_pts0, map_cids0, map_n0);
        build_leg_pmap(e, 1, b_sdp, b_sdp_len,
                       map_pts1, map_cids1, map_n1);



        /*  保存 B 的 answer：leg 1 的 fmtp（如 H.264 sprop-parameter-sets）
         *  仅存在于该 SDP 中。b_sdp 已经是 answer hook stash 的原始 200 OK
         *  时无需重复拷贝；否则（如 B 发起 re-INVITE 方向反转）覆盖。 */
        if (b_sdp != e->b_answer_sdp) {
            char *cp = NULL;
            if (b_sdp && b_sdp_len) {
                cp = (char *)malloc(b_sdp_len + 1);
                if (cp) {
                    memcpy(cp, b_sdp, b_sdp_len);
                    cp[b_sdp_len] = '\0';
                }
            }
            free(e->b_answer_sdp);
            e->b_answer_sdp = cp;
            e->b_answer_len = cp ? b_sdp_len : 0;
        }

        e->video_pt_allowed_count[0] = offer_video_n;
        memcpy(e->video_pt_allowed[0], offer_video_pts, (size_t)offer_video_n);
        e->video_pt_allowed_count[1] = answer_video_n;
        memcpy(e->video_pt_allowed[1], answer_video_pts, (size_t)answer_video_n);
        /* 抓图/录制用完整集合（见 cnode_t.vid_feed_pt 注释），下面逐腿终结
         * 只会收窄 video_pt_allowed，不影响这里。 */
        e->vid_feed_pt_n[0] = offer_video_n;
        memcpy(e->vid_feed_pt[0], offer_video_pts, (size_t)offer_video_n);
        e->vid_feed_pt_n[1] = answer_video_n;
        memcpy(e->vid_feed_pt[1], answer_video_pts, (size_t)answer_video_n);

        /*  视频转码规划（VP9↔H264）：
         *    - 优先采用 answer hook 的逐腿选择（neg_v_*），否则从原始
         *      offer/answer 各挑第一个 H264/VP9；
         *    - 两侧 codec 不同 → video_xcode 转码；
         *    - 两侧相同但 PT 不同 → 仅改 RTP 头 PT；
         *    - 任一侧不可用 → 两个会话都退役，保持原透传。 */
        {
            uint8_t v_apt = 0xFF, v_bpt = 0xFF;
            codec_id_t v_acid = CODEC_NONE, v_bcid = CODEC_NONE;
            /*  双 WebRTC 才保持原透传：浏览器之间按 ICE/DTLS-SRTP 语义
             *  直接协商同一 codec，SBC 不介入转码；混合腿（SIP↔WebRTC）
             *  必须转码——SRTP 已由 relay 终结，转码输出经
             *  relay_video_emit 重新加密后发往 WebRTC 侧。 */
            if (e->a_ws && e->b_ws) {
                for (int side = 0; side < 2; ++side)
                    cnode_drop_vxcode(e, side);
                goto video_plan_done;
            }
            if (e->neg_v_valid) {
                v_apt  = e->neg_v_a_pt;
                v_acid = e->neg_v_a_cid;
                v_bpt  = e->neg_v_b_pt;
                v_bcid = e->neg_v_b_cid;
            } else {
                sdp_vsel_t va, vb;
                if (sdp_video_select(offer, offer_len, &va) && va.port > 0 &&
                    sdp_video_select(b_sdp, b_sdp_len, &vb) && vb.port > 0) {
                    v_apt  = (uint8_t)va.pt;
                    v_acid = va.cid;
                    v_bpt  = (uint8_t)vb.pt;
                    v_bcid = vb.cid;
                }
            }

            if (v_apt != 0xFF && v_bpt != 0xFF &&
                v_acid != CODEC_NONE && v_bcid != CODEC_NONE) {
                /* 逐腿终结后每侧只接收自己 codec 的 PT */
                e->video_pt_allowed_count[0] = 1;
                e->video_pt_allowed[0][0] = v_apt;
                e->video_pt_allowed_count[1] = 1;
                e->video_pt_allowed[1][0] = v_bpt;

                for (int side = 0; side < 2; ++side) {
                    uint8_t    spt  = (side == 0) ? v_apt : v_bpt;
                    uint8_t    dpt  = (side == 0) ? v_bpt : v_apt;
                    codec_id_t scid = (side == 0) ? v_acid : v_bcid;
                    codec_id_t dcid = (side == 0) ? v_bcid : v_acid;

                    if (scid == dcid) {
                        cnode_drop_vxcode(e, side);
                        if (spt != dpt) {
                            e->video_rewrite_src_pt[side] = spt;
                            e->video_rewrite_dst_pt[side] = dpt;
                            e->video_rewrite_active[side] = 1;
                        } else {
                            /* 同 codec 同 PT：清掉上一轮 re-INVITE 的 PT 改写 */
                            e->video_rewrite_active[side] = 0;
                        }
                        continue;
                    }

                    int sig = ((int)scid << 24) ^ ((int)dcid << 16) ^
                              ((int)spt << 8) ^ (int)dpt;
                    video_xcode_t *cur =
                        __atomic_load_n(&e->vxcode[side], __ATOMIC_ACQUIRE);
                    if (cur && e->vxcode_sig[side] == sig &&
                        e->vxcode_src_pt[side] == spt &&
                        e->vxcode_dst_pt[side] == dpt) {
                        continue;   /* 已按同一组合建好 */
                    }

                    uint32_t ssrc = 0x4E534200u ^
                                    (uint32_t)((uintptr_t)e & 0xFFFFu) ^
                                    ((uint32_t)side << 1);
                    video_xcode_t *nv =
                        video_xcode_create(scid, dcid, spt, dpt, ssrc);
                    if (nv == NULL) {
                        llog(LOG_WARN, "media",
                             "VIDEO_XCODE call=%p side=%d CREATE FAIL %d->%d — 保持透传",
                             call, side, (int)scid, (int)dcid);
                        continue;
                    }
                    if (cur) cnode_retire_vxcode(e, side, cur);
                    e->vxcode_sig[side]    = sig;
                    e->vxcode_src_pt[side] = spt;
                    e->vxcode_dst_pt[side] = dpt;
                    e->video_rewrite_active[side] = 0;
                    __atomic_store_n(&e->vxcode[side], nv, __ATOMIC_RELEASE);
                    llog(LOG_INFO, "media",
                         "VIDEO_XCODE call=%p side=%d %d(pt=%u) -> %d(pt=%u)",
                         call, side, (int)scid, (unsigned)spt,
                         (int)dcid, (unsigned)dpt);
                }
            } else {
                for (int side = 0; side < 2; ++side)
                    cnode_drop_vxcode(e, side);
            }
            /* 抓图排障：把逐腿视频计划与入包 PT 白名单一起打出来。
             * 只出一张图（另一条腿没建 job）时，看这里就能判断是
             * 白名单为空（has_video=0）还是 codec/PT 认错。 */
            if (v_apt != 0xFF || v_bpt != 0xFF) {
                llog(LOG_INFO, "media",
                     "VIDEO_PLAN call=%p A=%d(pt=%u) B=%d(pt=%u) "
                     "allowed A=%d(first=%u) B=%d(first=%u) feed A=%d B=%d",
                     call, (int)v_acid, (unsigned)v_apt,
                     (int)v_bcid, (unsigned)v_bpt,
                     e->video_pt_allowed_count[0],
                     (unsigned)(e->video_pt_allowed_count[0] > 0
                                ? e->video_pt_allowed[0][0] : 0xFF),
                     e->video_pt_allowed_count[1],
                     (unsigned)(e->video_pt_allowed_count[1] > 0
                                ? e->video_pt_allowed[1][0] : 0xFF),
                     e->vid_feed_pt_n[0], e->vid_feed_pt_n[1]);
            }
video_plan_done: ;
        }
        if (offer_video_n > 0 || answer_video_n > 0) {
            llog(LOG_INFO, "media",
                 "BRIDGE_VIDEO_PTS call=%p offer_video_n=%d answer_video_n=%d",
                 call, offer_video_n, answer_video_n);
        }

        /*  M1+/M2 视频录制：按每条腿实际协商的视频 codec 分别建立录制 tap。
         *    - H.264 腿：vid_reasm 重组 Annex-B → rec_feed_video（零转码）
         *    - VP8/VP9/H.265 腿：rec_xcode 定向转码到 H.264，AU sink → rec_feed_video
         *  与语音分腿录音一致：side=0 录 A 的视频，side=1 录 B 的视频。
         *  re-INVITE 改 codec / 去掉 video 时按新协商销毁或重建。 */
        {
            codec_id_t r_cid[2] = { CODEC_NONE, CODEC_NONE };
            uint8_t    r_pt[2]  = { 0xFF, 0xFF };
            if (e->neg_v_valid) {
                r_cid[0] = e->neg_v_a_cid; r_pt[0] = e->neg_v_a_pt;
                r_cid[1] = e->neg_v_b_cid; r_pt[1] = e->neg_v_b_pt;
            } else {
                sdp_vsel_t va, vb;
                if (sdp_video_select(offer, offer_len, &va) && va.port > 0) {
                    r_cid[0] = va.cid;
                    r_pt[0]  = (uint8_t)va.pt;
                }
                if (sdp_video_select(b_sdp, b_sdp_len, &vb) && vb.port > 0) {
                    r_cid[1] = vb.cid;
                    r_pt[1]  = (uint8_t)vb.pt;
                }
            }

            b2b_call_t *bc = (b2b_call_t *)call;
            for (int side = 0; side < 2; ++side) {
                /* 与原 M1 语义一致：仅当 offer/answer 双向都协商了视频才录制，
                 * 避免把“只在一侧出现的视频”误录进通话录像。 */
                int have_video = (offer_video_n > 0 && answer_video_n > 0);
                codec_id_t cid = have_video ? r_cid[side] : CODEC_NONE;

                /* H.264：用便宜的 reasm，不依赖录音 sess 是否已启动，
                 * 允许通话中 record start 后继续录到后续帧。 */
                if (cid == CODEC_H264) {
                    if (e->rec_xcode[side] != NULL)
                        cnode_drop_rec_xcode(e, side);
                    /* 重新协商回 H.264：观察实例不再需要（reasm 接管抓图） */
                    if (e->tap_xcode[side] != NULL)
                        cnode_drop_tap(e, side);
                    if (e->vid_reasm[side] == NULL) {
                        e->vid_ud[side].node = e;
                        e->vid_ud[side].side = side;
                        if (vid_reasm_create(&e->vid_reasm[side],
                                             bridge_video_frame_cb,
                                             &e->vid_ud[side]) == NSBC_OK) {
                            llog(LOG_INFO, "media",
                                 "VIDEO_REASM call=%p side=%d CREATED (H.264 relay recording)",
                                 call, side);
                        } else {
                            llog(LOG_WARN, "media",
                                 "VIDEO_REASM call=%p side=%d CREATE FAIL — video recording disabled",
                                 call, side);
                        }
                    }
                    continue;
                }

                /* 非 H.264：不再需要 H.264 重组器 */
                if (e->vid_reasm[side] != NULL) {
                    vid_reasm_destroy(e->vid_reasm[side]);
                    e->vid_reasm[side] = NULL;
                    llog(LOG_INFO, "media",
                         "VIDEO_REASM call=%p side=%d DESTROYED (renegotiated to non-H.264)",
                         call, side);
                }

                /*  观察实例（tap）：非 H.264 腿即使与对侧同编码（无通话转码）
                 *  也建立，保证抓图可用；只解包，不占解码/编码算力。 */
                int tsig = ((int)cid << 24) ^ ((int)r_pt[side] << 8);
                if (cid == CODEC_NONE) {
                    if (e->tap_xcode[side] != NULL)
                        cnode_drop_tap(e, side);
                } else if (e->tap_xcode[side] == NULL ||
                           e->tap_xcode_sig[side] != tsig) {
                    if (e->tap_xcode[side] != NULL)
                        cnode_drop_tap(e, side);
                    video_xcode_t *tv = video_xcode_create_tap(cid, r_pt[side]);
                    if (tv == NULL) {
                        llog(LOG_WARN, "media",
                             "VIDEO_TAP call=%p side=%d CREATE FAIL codec=%d(pt=%u)"
                             " — snapshot unavailable on this leg",
                             call, side, (int)cid, (unsigned)r_pt[side]);
                    } else {
                        e->tap_ud[side].node = e;
                        e->tap_ud[side].side = side;
                        e->tap_ud[side].cid  = cid;
                        video_xcode_set_au_sink(tv, bridge_tap_au_cb,
                                                &e->tap_ud[side]);
                        e->tap_xcode_sig[side] = tsig;
                        e->tap_xcode_pt[side]  = r_pt[side];
                        __atomic_store_n(&e->tap_xcode[side], tv,
                                         __ATOMIC_RELEASE);
                        llog(LOG_INFO, "media",
                             "VIDEO_TAP call=%p side=%d codec=%d(pt=%u) CREATED"
                             " (snapshot)",
                             call, side, (int)cid, (unsigned)r_pt[side]);
                    }
                }

                /* VP8/VP9/H.265：仅在该腿已存在录音 sess 时建立定向转码，
                 * 避免未录音通话白白占用解码器/编码器 CPU。 */
                rec_sess_t *rs = NULL;
                if (bc != NULL)
                    rs = (rec_sess_t *)__atomic_load_n(&bc->rec[side],
                                                       __ATOMIC_ACQUIRE);
                /* 方案 A：该腿已有通话转码、且其目标编码就是 H.264 时，
                 * 直接把录制 AU sink 挂到既有 vxcode 上，复用转码输出，
                 * 省掉一条独立的解码+编码链路（复用转码解码输出）。 */
                codec_id_t dst_cid = r_cid[1 - side];
                if (rs != NULL && cid != CODEC_NONE &&
                    e->vxcode[side] != NULL && dst_cid == CODEC_H264) {
                    if (e->rec_xcode[side] != NULL)
                        cnode_drop_rec_xcode(e, side);
                    e->rec_ud[side].node = e;
                    e->rec_ud[side].side = side;
                    video_xcode_set_au_sink(e->vxcode[side], bridge_rec_au_cb,
                                            &e->rec_ud[side]);
                    continue;
                }

                int need = (cid == CODEC_VP8 || cid == CODEC_VP9 ||
                            cid == CODEC_H265) && rs != NULL;
                if (!need) {
                    if (e->rec_xcode[side] != NULL)
                        cnode_drop_rec_xcode(e, side);
                    continue;
                }

                int sig = ((int)cid << 24) ^ ((int)r_pt[side] << 8);
                if (e->rec_xcode[side] != NULL &&
                    e->rec_xcode_sig[side] == sig &&
                    e->rec_xcode_pt[side] == r_pt[side])
                    continue;   /* 同组合已建好 */

                uint32_t ssrc = 0x4E534300u ^
                                (uint32_t)((uintptr_t)e & 0xFFFFu) ^
                                ((uint32_t)side << 1);
                video_xcode_t *nv =
                    video_xcode_create(cid, CODEC_H264, r_pt[side], 255, ssrc);
                if (nv == NULL) {
                    llog(LOG_WARN, "media",
                         "VIDEO_REC_XCODE call=%p side=%d CREATE FAIL %d->H264(pt=%u)"
                         " — 本腿视频轨将缺失（检查 mod_av/libx264）",
                         call, side, (int)cid, (unsigned)r_pt[side]);
                    if (e->rec_xcode[side] != NULL)
                        cnode_drop_rec_xcode(e, side);
                    continue;
                }
                e->rec_ud[side].node = e;
                e->rec_ud[side].side = side;
                video_xcode_set_au_sink(nv, bridge_rec_au_cb, &e->rec_ud[side]);
                if (e->rec_xcode[side] != NULL)
                    cnode_drop_rec_xcode(e, side);
                e->rec_xcode_sig[side] = sig;
                e->rec_xcode_pt[side]  = r_pt[side];
                __atomic_store_n(&e->rec_xcode[side], nv, __ATOMIC_RELEASE);
                llog(LOG_INFO, "media",
                     "VIDEO_REC_XCODE call=%p side=%d %d(pt=%u) -> H264（录制转码）",
                     call, side, (int)cid, (unsigned)r_pt[side]);
            }
        }

        /*  DTMF RFC4733：扫描 offer/answer SDP 提取 telephone-event PT
         * 顶层 static find_dtmf_pt（函数外定义）负责实际解析 */
        e->dtmf_pt[0] = find_dtmf_pt(offer, offer_len);
        e->dtmf_pt[1] = find_dtmf_pt(b_sdp, b_sdp_len);
        /*  第 1 项：answer 已改写成 A 的 PT 域 —— 不能再用它推断 B 侧的
         *  telephone-event PT，必须用终结时从 B 原始 answer 里取的值。 */
        if (e->neg_valid) {
            if (e->neg_a_te) e->dtmf_pt[0] = e->neg_a_te;
            if (e->neg_b_te) e->dtmf_pt[1] = e->neg_b_te;
        }
        if (e->dtmf_pt[0] || e->dtmf_pt[1]) {
            llog(LOG_INFO, "media",
                 "DTMF call=%p dtmf_pt[0]=%u dtmf_pt[1]=%u",
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

        /*  re-INVITE media_update：方向门控刷新
         * bridge_confirm 每次被调用（初始 + re-INVITE re-entry）都会跑这段，
         * 保证新 SDP 的 hold/unhold/codec renegotiation 立即生效。
         * side 0 = offer 侧（朝 A leg），side 1 = answer 侧（朝 B leg）。 */
        {
            sdp_dir_t nd[2] = { SDP_DIR_SENDRECV, SDP_DIR_SENDRECV };
            sdp_t sd;
            if (offer_len > 0) {
                memset(&sd, 0, sizeof(sd));
                if (sdp_parse(&sd, offer, offer_len) == NSBC_OK && sd.nmedia > 0) {
                    nd[0] = sdp_media_effective_dir(&sd, &sd.media[0]);
                    if (sdp_media_is_legacy_hold(&sd, &sd.media[0]))
                        nd[0] = SDP_DIR_INACTIVE;
                }
            }
            if (b_sdp_len > 0) {
                memset(&sd, 0, sizeof(sd));
                if (sdp_parse(&sd, b_sdp, b_sdp_len) == NSBC_OK && sd.nmedia > 0) {
                    nd[1] = sdp_media_effective_dir(&sd, &sd.media[0]);
                    if (sdp_media_is_legacy_hold(&sd, &sd.media[0]))
                        nd[1] = SDP_DIR_INACTIVE;
                }
            }
            if (nd[0] != e->dir[0] || nd[1] != e->dir[1]) {
                llog(LOG_INFO, "media",
                     "BRIDGE_DIR call=%p dir[0]: %d→%d dir[1]: %d→%d",
                     call, e->dir[0], nd[0], e->dir[1], nd[1]);
                e->dir[0] = nd[0];
                e->dir[1] = nd[1];
            }
        }

        if (final_mode == MB_TRANSCODE && e->mode != MB_TRANSCODE) {
            e->mode = MB_TRANSCODE;
            llog(LOG_INFO, "media",
                 "call %p codec mismatch SDP-level, entering TRANSCODE mode",
                 call);
        }

        /*  通用 codec 交集检查（REQ-TRANS-004 P1 泛化）
         * 对 offer 里每个 PT，检查 answer 有没有接受；没接受 → 用 find_transcode_dst
         * 找一个已注册的目标 codec → transcode_open(src→dst)。
         *
         * 覆盖场景：
         *   G.711 互转（offer=0+8, answer=8 only → 0→8）
         *   G.729 互转（offer=G729 PT=96, answer=G729 PT=18 → 96→18）
         *   跨 codec （offer=0, answer=18 → 0→18 走 G711 PCMA→PCM→G729） */

        /* --- 音频转码规划：每次 bridge_confirm 重算（支持 re-INVITE）---
         * side=0：A(offer) → B(answer)；side=1：B(answer) → A(offer)。
         * dst 候选必须来自"目标腿"列表，杜绝用源腿列表选目标。 */
        e->pt_rewrite_active[0] = 0;
        e->pt_rewrite_active[1] = 0;

        for (int side = 0; side < 2; ++side) {
            const uint8_t    *sp = (side == 0) ? offer_pts : answer_pts;
            const codec_id_t *sc = (side == 0) ? offer_cids : answer_cids;
            int sn = (side == 0) ? offer_n : answer_n;
            const uint8_t    *dp = (side == 0) ? answer_pts : offer_pts;
            const codec_id_t *dc = (side == 0) ? answer_cids : offer_cids;
            int dn = (side == 0) ? answer_n : offer_n;

            int need = 0, rewrite = 0;
            codec_id_t src = CODEC_NONE, dst = CODEC_NONE;
            uint8_t src_pt = 0xFF, dst_pt = 0xFF, rw_dst_pt = 0xFF;

            for (int i = 0; i < sn; ++i) {
                int same_cid = 0;
                uint8_t same_pt = 0xFF;
                if (sc[i] == CODEC_NONE) continue;
                if (!codec_can_decode(sc[i])) continue;
                /* 按 codec 身份判断（PT 空间两腿独立，不能只比 PT 数字）。
                 * 注：fmtp 差异交由运行时 peer_accepts_payload() 处理，
                 * 计划层保持「同 cid 即中继」以免 answer fmtp 裁剪
                 * （如 AMR mode-set）把同 codec 通话误判成需要转码。 */
                for (int j = 0; j < dn; ++j) {
                    if (dc[j] == sc[i]) { same_cid = 1; same_pt = dp[j]; break; }
                }
                if (same_cid && same_pt == sp[i])
                    continue;               /* 同 codec 同 PT：直接中继 */
                need = 1;
                src = sc[i];
                src_pt = sp[i];
                if (same_cid) {
                    rewrite = 1;
                    rw_dst_pt = same_pt;    /* 同 codec 不同 PT：只改 PT */
                } else {
                    dst = find_xcode_dst(src, dp, dc, dn, sc, sn, &dst_pt);
                }
                break;
            }

            if (!need) {
                e->src_codec[side] = CODEC_NONE;
                e->dst_codec[side] = CODEC_NONE;
                cnode_drop_xcode(e, side);
                continue;
            }

            if (rewrite) {
                cnode_drop_xcode(e, side);
                e->pt_rewrite[side].src_pt = src_pt;
                e->pt_rewrite[side].dst_pt = rw_dst_pt;
                e->pt_rewrite_active[side] = 1;
                const codec_ops_t *ops = codec_registry_find(src);
                llog(LOG_INFO, "media",
                     "call %p PT rewrite[%d] %s(pt=%u)→pt=%u (same codec)",
                     call, side, ops ? ops->name : "?",
                     (unsigned)src_pt, (unsigned)rw_dst_pt);
                continue;
            }

            if (dst == CODEC_NONE) {
                llog(LOG_WARN, "media",
                     "call %p side=%d src=%s(pt=%u) no encodable target codec "
                     "on peer leg — payload may be dropped",
                     call, side,
                     codec_registry_find(src) ? codec_registry_find(src)->name : "?",
                     (unsigned)src_pt);
                continue;
            }

            {
                const codec_ops_t *so = codec_registry_find(src);
                const codec_ops_t *eo = codec_registry_find(dst);
                int sig = ((int)dst << 16) | (int)dst_pt;
                transcode_sess_t *cur =
                    __atomic_load_n(&e->transcode[side], __ATOMIC_ACQUIRE);

                e->src_codec[side] = src;
                e->dst_codec[side] = dst;
                if (cur && e->xcode_sig[side] == sig &&
                    e->xcode_src_pt[side] == src_pt) {
                    e->mode = MB_TRANSCODE;   /* 计划未变，沿用现有会话 */
                    continue;
                }

                transcode_sess_t *tc = NULL;
                char s_fmtp[128], d_fmtp[128];
                const char *src_sdp = (side == 0) ? e->a_offer_sdp
                                                  : e->b_offer_sdp;
                size_t src_sdp_len = (side == 0) ? e->a_offer_len
                                                 : e->b_offer_len;
                /* 目标腿 SDP：side=0 目标是 B（answer），side=1 目标是 A（offer）*/
                const char *dst_sdp = (side == 0) ? b_sdp : offer;
                size_t dst_sdp_len = (side == 0) ? b_sdp_len : offer_len;
                /* fmtp 透传：优先取 payload map 里协商好的 fmtp（名字命中、
                 * 已校验 rate），缺失时回退 SDP 文本扫描。 */
                s_fmtp[0] = '\0';
                d_fmtp[0] = '\0';
                {
                    const leg_payload_t *mse = leg_map_entry(e, side, src_pt);
                    const leg_payload_t *mde =
                        leg_map_entry(e, side ^ 1, dst_pt);
                    if (mse != NULL && mse->fmtp[0] != '\0')
                        snprintf(s_fmtp, sizeof(s_fmtp), "%s", mse->fmtp);
                    else
                        sdp_fmtp_for_pt(src_sdp, src_sdp_len, src_pt,
                                        s_fmtp, sizeof(s_fmtp));
                    if (mde != NULL && mde->fmtp[0] != '\0')
                        snprintf(d_fmtp, sizeof(d_fmtp), "%s", mde->fmtp);
                    else
                        sdp_fmtp_for_pt(dst_sdp, dst_sdp_len, dst_pt,
                                        d_fmtp, sizeof(d_fmtp));
                }
                int rc = transcode_open_ex(src, dst,
                                           so ? codec_pcm_sample_rate(so) : 8000,
                                           eo ? codec_pcm_sample_rate(eo) : 8000,
                                           so ? so->channels : 1,
                                           s_fmtp[0] ? s_fmtp : NULL,
                                           d_fmtp[0] ? d_fmtp : NULL, &tc);
                if (rc == NSBC_OK) {
                    rc = cnode_install_xcode(e, side, tc, src_pt, dst_pt);
                    if (rc != NSBC_OK) {
                        transcode_close(tc);
                    } else {
                        e->mode = MB_TRANSCODE;
                        llog(LOG_INFO, "media",
                             "call %p transcode[%d]=%s(pt=%u)→%s(pt=%u) enabled",
                             call, side,
                             so ? so->name : "?", (unsigned)src_pt,
                             eo ? eo->name : "?", (unsigned)dst_pt);
                    }
                } else {
                    llog(LOG_WARN, "media",
                         "call %p transcode_open2(%s→%s) rc=%d — payload may be dropped",
                         call, so ? so->name : "?", eo ? eo->name : "?", rc);
                }
            }
        }

        if (e->mode == MB_TRANSCODE &&
            !__atomic_load_n(&e->transcode[0], __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&e->transcode[1], __ATOMIC_ACQUIRE)) {
            llog(LOG_WARN, "media",
                 "call %p codec mismatch but no transcode session created "
                 "(registry missing target codecs) — running plain relay",
                 call);
        }
    } else {
        e->mode = MB_BYPASS; /* 无 relay 实例：实际路径即透传 */
    }

    /*  bridge_confirm 最终状态快照 — 单通/静音调试时一目了然 */
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
        if (e->video_pt_allowed_count[0] || e->video_pt_allowed_count[1]) {
            llog(LOG_INFO, "media",
                 "  video_pt_allowed[0]=(n=%d)[", e->video_pt_allowed_count[0]);
            for (int i = 0; i < e->video_pt_allowed_count[0]; ++i)
                llog(LOG_INFO, "media",
                     "  video_pt_allowed[0][%d]=%u", i, e->video_pt_allowed[0][i]);
            llog(LOG_INFO, "media",
                 "  video_pt_allowed[1]=(n=%d)[", e->video_pt_allowed_count[1]);
            for (int i = 0; i < e->video_pt_allowed_count[1]; ++i)
                llog(LOG_INFO, "media",
                     "  video_pt_allowed[1][%d]=%u", i, e->video_pt_allowed[1][i]);
        }
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
        /*  REQ-VQ-001：RTP seq-gap 本地丢包推断——按 SSRC 分槽累加
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
        /*  HA P3-3: SRTP material — 每侧独立。
         * relay->srtp[i] = 该侧 DTLS-SRTP 会话（WebRTC 侧可能有，SIP 侧通常 NULL 明文）。
         * get_material 只 memcpy struct 字段 — 桥层持 shard 锁 + material 只读安全。
         * ⚠️ ROC 同步暂未做（libsrtp opaque struct 内部，无公共 API） —
         * 下一阶段在 relay 层自己维护 send_roc/recv_roc 计数器。 */
        {
            nsbc_srtp_sess_t *sr = r->srtp[i];
            if (sr) {
                nsbc_srtp_material_t m_out = {0}, m_in = {0};
                nsbc_srtp_cipher_t cipher = 0;
                if (nsbc_srtp_session_get_material(sr, &m_out, &m_in, &cipher) == NSBC_OK) {
                    v->srtp_cipher[i] = (uint8_t)cipher;
                    v->srtp_keylen[i] = (uint8_t)m_out.keylen;
                    if (m_out.keylen > 0)
                        memcpy(v->srtp_out[i], m_out.key, SRTP_KEY_MAX);
                    if (m_in.keylen > 0)
                        memcpy(v->srtp_in[i], m_in.key, SRTP_KEY_MAX);
                }
            }
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

/*  取媒体层最终定型的音频 codec 名称（side 0=A / 1=B）。
 *  信令层的 leg[1].answer 在入呼场景存的是"SBC 回给 A 的 answer"，
 *  直接解析会把 B 腿（WebRTC）显示成 A 的 codec；真实协商结果
 *  （含转码后两端各自的 codec）只有 bridge_confirm 的 neg_* 知道。 */
int bridge_negotiated_codec_name(bridge_mgr_t *m, void *call_key, int side,
                                 char *name, size_t cap)
{
    if (!m || !call_key || !name || cap == 0 || side < 0 || side > 1)
        return 0;
    name[0] = '\0';
    br_shard_t *s = &m->sh[ptr_shard(call_key)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call_key);
    int ok = 0;
    if (e && e->neg_valid) {
        const codec_ops_t *ops =
            codec_registry_find(side ? e->neg_b_cid : e->neg_a_cid);
        /*  ops->name 是内嵌数组，恒非 NULL；只有 sdp_name 是可选指针。 */
        if (ops && ops->name[0]) {
            snprintf(name, cap, "%s",
                     ops->sdp_name ? ops->sdp_name : ops->name);
            ok = 1;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return ok;
}

/*  183 早期媒体透传前置条件：两腿音频 codec 是否一致。
 *  一致（无需转码）才能把落地的早期媒体 payload 直接转给另一腿；
 *  需要转码时转码器要到 bridge_confirm（200/ACK）才建立，此时转发会把
 *  B 编码的 payload 按 A 的 PT 送出 → 主叫听到噪声，故调用方应降级本地 183。
 *  返回 1=需要转码，0=无需转码，<0=尚未定型（无从判断）。 */
int bridge_answer_needs_transcode(bridge_mgr_t *m, void *call_key)
{
    if (!m || !call_key)
        return -1;
    br_shard_t *s = &m->sh[ptr_shard(call_key)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call_key);
    int need = -1;
    /* 计划层口径：同 cid 即视为可中继（与 bridge_confirm 一致）。 */
    if (e && e->neg_valid)
        need = (e->neg_a_cid != e->neg_b_cid) ? 1 : 0;
    pthread_mutex_unlock(&s->lock);
    return need;
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

/* ---------- §3.4 snapshot：视频状态查询 / 关键帧请求 ---------- */

int media_bridge_call_has_video(bridge_mgr_t *m, void *call_key, int leg)
{
    if (!m || !call_key || leg < 0 || leg > 1) return 0;
    br_shard_t *s = &m->sh[ptr_shard(call_key)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call_key);
    int has = (e != NULL && e->video_pt_allowed_count[leg] > 0) ? 1 : 0;
    pthread_mutex_unlock(&s->lock);
    return has;
}

/* 向该腿对端请求关键帧（RTCP PLI）。与转码/录制路径共享 500ms 限频。
 * 返回 NSBC_AGAIN 表示尚未学习到视频 SSRC（对端还没发过视频包），调用方稍后重试。 */
int media_bridge_request_keyframe(bridge_mgr_t *m, void *call_key, int leg)
{
    if (!m || !call_key || leg < 0 || leg > 1) return NSBC_INVAL;

    br_shard_t *s = &m->sh[ptr_shard(call_key)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call_key);
    if (e == NULL || e->relay == NULL) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_NOENT;
    }
    uint32_t ssrc = __atomic_load_n(&e->relay->vid_ssrc[leg], __ATOMIC_RELAXED);
    if (ssrc == 0) {
        pthread_mutex_unlock(&s->lock);
        return NSBC_AGAIN;
    }
    /* 只置标志，真正的 RTCP PLI 由媒体线程在下一包视频时发送：
     * relay_video_request_pli 会走 SRTP protect + sendto，必须与媒体线程
     * 同线程，否则共享 SRTP 会话状态会被并发破坏。 */
    __atomic_store_n(&e->relay->pli_req[leg], 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s->lock);
    return NSBC_OK;
}

/* 该腿视频 fmtp：snapshot 用它取 sprop-parameter-sets 补齐 SPS/PPS。
 * leg 0 = A 的 offer；leg 1 = B 的 answer（re-INVITE 前可能尚未保存）。 */
int media_bridge_call_video_fmtp(bridge_mgr_t *m, void *call_key, int leg,
                                 char *out, size_t cap)
{
    if (!m || !call_key || !out || cap == 0 || leg < 0 || leg > 1)
        return 0;
    out[0] = '\0';

    br_shard_t *s = &m->sh[ptr_shard(call_key)];
    pthread_mutex_lock(&s->lock);
    cnode_t *e = entry_find(m, call_key);
    int ok = 0;
    if (e != NULL) {
        const char *sdp = (leg == 0) ? e->a_offer_sdp : e->b_answer_sdp;
        size_t len = (leg == 0) ? e->a_offer_len : e->b_answer_len;
        for (int i = 0; i < e->video_pt_allowed_count[leg] && !ok; ++i)
            ok = sdp_fmtp_for_pt(sdp, len, e->video_pt_allowed[leg][i], out, cap);
    }
    pthread_mutex_unlock(&s->lock);
    return ok;
}

int rtp_relay_fd(const rtp_relay_t *r, int side){
    if (!r || side < 0 || side > 1) return -1;
    return r->fd_rtp[side];
}

uint16_t rtp_relay_port(const rtp_relay_t *r, int side)
{
    if (!r || side < 0 || side > 1) return 0;
    return r->rtp_port[side];
}

uint16_t rtp_relay_video_port(const rtp_relay_t *r, int side)
{
    if (!r || side < 0 || side > 1) return 0;
    return r->vrtp_port[side];
}

const char *bridge_local_ip(const bridge_mgr_t *m)
{
    return (m && m->cfg.local_ip) ? m->cfg.local_ip : NULL;
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
