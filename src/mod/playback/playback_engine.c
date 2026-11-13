/* playback_engine.c — Phase 3 Advanced Playback Engine（2026-10-15 增量）
 *
 * 单侧媒体注入：WAV → PCM S16LE → G.711（PCMU/PCMA 按协商 PT）→ RTP
 * 封装 → sendto relay fd。reactor 时间轮驱动周期性 pull+send；EOF 自动
 * stop 或 loop 模式循环。
 *
 * 依赖：player.c（WAV+PCM 源）+ media_bridge.c（relay 暴露 API）
 *       + g711.c（共享 G.711 编码，非本地副本）
 * 线程：全部在 reactor 时间轮回调里跑，零锁。
 */

#include "playback_engine.h"
#include "snd_file.h"
#include "media_bridge.h"
#include "g711.h"
#include "app_ctx.h"
#include "nsbc_common.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

/* ---------- RTP 封装 ---------- */

static inline void rtp_write_u16(uint8_t *p, uint16_t v)
{ p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }

static inline void rtp_write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}

/* ---------- pb_ctx：每个播放会话 ---------- */

#define PB_MAX_PARALLEL 256
#define PB_RTP_PT 8 /* PCMU（pb_start 旧签名的默认值） */
#define PB_MAX_FRAMES 800 /* 100ms @ 8kHz → 适配 timer_wheel 100ms 粒度
                            * （tw_add 被 round 到 tick_ms 整数倍，
                            *  PB_PULL_INTERVAL_MS=20 实际 ≈ 100ms 触发一次） */

typedef struct pb_ctx {
    void *call_key;
    int side;                 /* 0=A, 1=B, -1=both */
    int loop;                 /* 0=once, 1=loop */
    uint8_t pt;               /* RTP payload type：0=PCMU, 8=PCMA */
    int stop_on_answer;       /* 1=B 腿 LEG_CONFIRMED 时 pb_stop_by_flag(c,1) 停
                                 * (ringback 行为)，
                                 * 0=不受接通影响 (playback / endless_playback / MoH) */
    tw_node_t tn;             /* 时间轮节点 */
    snd_src_t *snd;           /* 音频文件解码 + 重采样 → 8k/mono/S16LE 统一输出 */
    rtp_relay_t *relay;       /* relay 指针（弱引用，bridge 自己管理生命周期） */
    nsbc_sa_t peer[2];        /* 两侧 peer 地址 */
    uint32_t ssrc;            /* 随机 SSRC */
    uint16_t seq;             /* RTP seq */
    uint32_t ts;              /* RTP timestamp（8000 per second） */
    volatile int active;      /* 1=在播，0=待清理 */
    uint64_t lazy_start_ms;   /* lazy start 计时起点（relay 还没创建时等待） */
    uint64_t last_tick_ms;    /* ★ 上次 timer_cb 时刻，用于动态计算 pull 量 */
    int total_pulled;         /* ★ 累计 pull 帧数（诊断用） */
    int total_sent;           /* ★ 累计 sendto 次数（诊断用） */
    char wav_path[256];
    struct pb_ctx *next;
} pb_ctx_t;

static pthread_mutex_t g_pb_lock = PTHREAD_MUTEX_INITIALIZER;
static pb_ctx_t *g_pb_list = NULL;
static int g_pb_started = 0;
static uint64_t g_pb_total = 0;

/* ---------- 全局 registry 初始化 ---------- */

int pb_engine_start(void)
{
    if (g_pb_started) return NSBC_OK;
    g_pb_started = 1;
    llog(LOG_INFO, "pb", "playback engine started (snd_file + G.711 encode)");
    return NSBC_OK;
}

void pb_engine_stop(void)
{
    pthread_mutex_lock(&g_pb_lock);
    pb_ctx_t *p = g_pb_list;
    while (p) {
        pb_ctx_t *nx = p->next;
        p->active = 0;
        tw_del(nsbc_get_ctx()->tw, &p->tn);
        if (p->snd) { snd_file_close(p->snd); p->snd = NULL; }
        free(p);
        p = nx;
    }
    g_pb_list = NULL;
    pthread_mutex_unlock(&g_pb_lock);
    g_pb_started = 0;
    llog(LOG_INFO, "pb", "playback engine stopped");
}

/* ---------- 定时器回调：pull PCM → 编码 → sendto ---------- */

#define PB_FRAMES_PER_MS 8

/* ★ 标准 RTP ptime = 20ms → 160 样本 @ 8kHz。
 * timer_wheel tick_ms=100ms（main.c:684），tw_add 会把 expire_ms
 * 向上取整到下一个 tick 边界（timer_wheel.c:98），所以 timer_cb 实际
 * 每 100ms 触发一次。正确做法：每次 tick 拉满 100ms 数据（800 帧），
 * 但拆成 5 个标准 RTP 包发送（每包 20ms/160 帧），保证 RTP timestamp
 * 连续，MicroSIP jitter buffer 能正确恢复音频节奏。 */
#define PB_RTP_PTIME_MS 20
#define PB_RTP_FRAMES_PER_PACKET 160 /* 20ms × 8kHz */
#define PB_TICK_MS 100                /* timer_wheel tick_ms */
#define PB_PACKETS_PER_TICK 5         /* 100 / 20 = 5 个 RTP 包/次 tick */

static void pb_timer_cb(uint64_t now_ms, void *arg)
{
    pb_ctx_t *pb = (pb_ctx_t *)arg;
    if (!pb || !pb->active) return;

    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->tw) return;

    /* lazy start：relay 还没创建时继续等 */
    if (!pb->relay) {
        if (!pb->lazy_start_ms) pb->lazy_start_ms = now_ms;
        if (now_ms - pb->lazy_start_ms > 5000) {
            llog(LOG_WARN, "pb", "lazy start TIMEOUT call=%p", pb->call_key);
            pb->active = 0;
            if (pb->snd) { snd_file_close(pb->snd); pb->snd = NULL; }
            return;
        }
        rtp_relay_t *relay = NULL;
        nsbc_sa_t peer[2] = {{0}};
        if (bridge_lookup_relay((bridge_mgr_t *)ctx->bridge, pb->call_key, &relay, peer) == NSBC_OK && relay) {
            pb->relay = relay;
            pb->peer[0] = peer[0];
            pb->peer[1] = peer[1];
            pb->lazy_start_ms = 0;
            llog(LOG_INFO, "pb", "lazy start READY call=%p relay=%p", pb->call_key, (void *)relay);
        } else {
            tw_add(ctx->tw, &pb->tn, now_ms + 1, pb_timer_cb, pb);
            return;
        }
    }

    /* peer latch refresh（每 tick 都查一次） */
    {
        int need_refresh = 0;
        if (pb->side >= 0) {
            if (pb->peer[pb->side].len == 0) need_refresh = 1;
        } else {
            if (pb->peer[0].len == 0 || pb->peer[1].len == 0) need_refresh = 1;
        }
        if (need_refresh) {
            rtp_relay_t *relay2 = NULL;
            nsbc_sa_t peer2[2] = {{0}};
            if (bridge_lookup_relay((bridge_mgr_t *)ctx->bridge, pb->call_key, &relay2, peer2) == NSBC_OK && relay2) {
                int old_len0 = pb->peer[0].len;
                int old_len1 = pb->peer[1].len;
                pb->relay = relay2;
                pb->peer[0] = peer2[0];
                pb->peer[1] = peer2[1];
                if (old_len0 == 0 && pb->peer[0].len > 0)
                    llog(LOG_INFO, "pb", "peer[0] LATCHED call=%p len=%d", pb->call_key, pb->peer[0].len);
                if (old_len1 == 0 && pb->peer[1].len > 0)
                    llog(LOG_INFO, "pb", "peer[1] LATCHED call=%p len=%d", pb->call_key, pb->peer[1].len);
            }
        }
    }

    /* ★ 每 tick 拉满 100ms 数据，拆成 5 个 20ms RTP 包 */
    const int total_frames = PB_RTP_FRAMES_PER_PACKET * PB_PACKETS_PER_TICK; /* 160×5 = 800 */
    int16_t pcm_buf[PB_MAX_FRAMES];
    int pulled = snd_file_pull(pb->snd, pcm_buf, total_frames, pb->loop);
    pb->total_pulled += pulled;
    if (pulled <= 0) {
        llog(LOG_INFO, "pb", "EOF call=%p", pb->call_key);
        pb_stop(pb->call_key);
        return;
    }

    /* peer / fd 检查 */
    int side = pb->side >= 0 ? pb->side : 0;
    if (pb->peer[side].len == 0) {
        tw_add(ctx->tw, &pb->tn, now_ms + 1, pb_timer_cb, pb);
        return;
    }
    int fd = rtp_relay_fd(pb->relay, side);
    if (fd < 0) {
        tw_add(ctx->tw, &pb->tn, now_ms + 1, pb_timer_cb, pb);
        return;
    }

    /* ★ 诊断：前 3 tick 打印链路状态 */
    if (pb->total_sent < 3) {
        llog(LOG_INFO, "pb", "tick call=%p pulled=%d total_pulled=%d peer[%d].len=%d fd=%d",
             pb->call_key, pulled, pb->total_pulled, side, pb->peer[side].len, fd);
    }

    /* 拆包发送：每次 160 帧（20ms） */
    int remaining = pulled;
    int offset = 0;
    int pkts = 0;
    while (remaining > 0) {
        int pkt_frames = remaining >= PB_RTP_FRAMES_PER_PACKET ? PB_RTP_FRAMES_PER_PACKET : remaining;
        uint8_t payload[PB_RTP_FRAMES_PER_PACKET];
        size_t enc = (pb->pt == 8)
            ? g711_alaw_encode(pcm_buf + offset, pkt_frames, payload)
            : g711_ulaw_encode(pcm_buf + offset, pkt_frames, payload);
        if (enc == 0) break;

        uint8_t rtp_frame[12 + PB_RTP_FRAMES_PER_PACKET];
        memset(rtp_frame, 0, sizeof(rtp_frame));
        rtp_frame[0] = 0x80;
        rtp_frame[1] = pb->pt;
        rtp_write_u16(&rtp_frame[2], pb->seq++);
        rtp_write_u32(&rtp_frame[4], pb->ts);
        pb->ts += pkt_frames;
        rtp_write_u32(&rtp_frame[8], pb->ssrc);
        memcpy(&rtp_frame[12], payload, enc);
        int rtp_len = 12 + (int)enc;

        ssize_t sret = sendto(fd, rtp_frame, rtp_len, MSG_NOSIGNAL,
                              (const struct sockaddr *)&pb->peer[side].ss,
                              pb->peer[side].len);
        pb->total_sent++;
        pkts++;

        if (sret != rtp_len) {
            llog(LOG_WARN, "pb", "sendto FAIL side=%d ret=%zd expect=%d errno=%d",
                 side, sret, rtp_len, errno);
            if (errno == ENOTSOCK || errno == EBADF) {
                llog(LOG_WARN, "pb", "fd invalid self-stop call=%p", pb->call_key);
                pb->active = 0;
                return;
            }
        }

        offset += pkt_frames;
        remaining -= pkt_frames;
    }

    if (pb->total_sent <= 5 || pb->total_sent % 100 == 0) {
        llog(LOG_INFO, "pb", "sendto OK tick call=%p pkts=%d total=%d ts=%lu",
             pb->call_key, pkts, pb->total_sent, (unsigned long)pb->ts);
    }

    if (!pb->active) return;
    tw_add(ctx->tw, &pb->tn, now_ms + 1, pb_timer_cb, pb);
}

/* ---------- pb_start ---------- */

int pb_start(void *call_key, const char *wav_path, int side, int loop,
             pb_ctx_t **out)
{
    /* 兼容入口：默认 PCMU + stop_on_answer=0（旧调用方——OAM playback 命令 /
     * in-dialog MoH；early media 路径用 pb_start_ex 传协商 PT 和明确 flag） */
    return pb_start_ex(call_key, wav_path, side, loop, PB_RTP_PT, 0, out);
}

int pb_start_ex(void *call_key, const char *wav_path, int side, int loop,
                 int pt, int stop_on_answer, pb_ctx_t **out)
{
    if (!g_pb_started || !call_key || !wav_path)
        return NSBC_INVAL;

    /* P0 编码器只有 G.711（查表）；opus/G.729 等待 codec 引擎 P1 */
    if (pt != 0 && pt != 8) {
        llog(LOG_WARN, "pb", "pb_start_ex: unsupported pt=%d (P0: PCMU=0/PCMA=8)", pt);
        return NSBC_NOTSUP;
    }

    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->tw || !ctx->bridge)
        return NSBC_ERR;

    /* 1. 开音频文件（snd_file 负责：格式探测 + 重采样 + 声道合并 →
     * 统一 8k/mono/S16LE 输出；零依赖 PCM WAV backend 已覆盖 alice.wav 等） */
    snd_src_t *snd = NULL;
    int backend = 0;
    int rc = snd_file_open(wav_path, &snd, 0 /* auto fallback */, &backend);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "pb", "snd_file_open FAIL path=%s rc=%d", wav_path, rc);
        return NSBC_ERR;
    }
    llog(LOG_INFO, "pb", "snd_file_open OK path=%s backend=%d orig_rate=%d ch=%d",
         wav_path, backend, snd_file_rate(snd), snd_file_channels(snd));

    /* 2. lookup relay + peer（lazy start —— relay 可能还没创建，
     * ringback 在 bridge 之前执行时 relay 还不存在） */
    rtp_relay_t *relay = NULL;
    nsbc_sa_t peer[2] = {{0}};
    rc = bridge_lookup_relay((bridge_mgr_t *)ctx->bridge, call_key, &relay, peer);
    if (rc != NSBC_OK || !relay) {
        llog(LOG_INFO, "pb", "lookup_relay pending call=%p (lazy start, will retry)", call_key);
        relay = NULL;
    }

    /* 3. create pb_ctx */
    pb_ctx_t *pb = calloc(1, sizeof(*pb));
    if (!pb) { snd_file_close(snd); return NSBC_NOMEM; }
    pb->call_key = call_key;
    pb->side = side;
    pb->loop = loop;
    pb->pt = (uint8_t)pt;
    pb->stop_on_answer = stop_on_answer ? 1 : 0;
    pb->snd = snd;
    pb->relay = relay;
    pb->peer[0] = peer[0];
    pb->peer[1] = peer[1];
    pb->ssrc = (uint32_t)rand();
    pb->seq = 0;
    pb->ts = 0;
    pb->active = 1;
    snprintf(pb->wav_path, sizeof(pb->wav_path), "%s", wav_path);

    /* 4. 存全局列表 */
    pthread_mutex_lock(&g_pb_lock);
    pb->next = g_pb_list;
    g_pb_list = pb;
    g_pb_total++;
    pthread_mutex_unlock(&g_pb_lock);

    /* 5. 挂时间轮（★ tw_add 第三参是【绝对 expire_ms】而非 delay——
     * timer_wheel.c:94 拒绝 expire_ms<=now；之前传 1/20 这类相对小值
     * 等于"过去时刻"被静默拒绝，pb_timer_cb 从未执行（lll.pcap 实证：
     * 服务器 0 帧 RTP 外发）。必须 now+1。 */
    tw_node_init(&pb->tn);
    {
        uint64_t expire = nsbc_now_ms() + 1;
        int rc = tw_add(ctx->tw, &pb->tn, expire, pb_timer_cb, pb);
        llog(LOG_INFO, "pb", "tw_add expire=%lu now=%lu rc=%d",
             (unsigned long)expire,
             (unsigned long)nsbc_now_ms(), rc);
    }

    llog(LOG_INFO, "pb", "START call=%p side=%d loop=%d pt=%u wav=%s",
         call_key, side, loop, pb->pt, wav_path);

    /* 6. fire ESL PLAYBACK_START */
    if (ctx->esl_event_fire) {
        char jbuf[512];
        int n = snprintf(jbuf, sizeof(jbuf),
                         "{\"call_key\":\"%p\",\"side\":%d,\"wav\":\"%s\",\"loop\":%d}",
                         call_key, side, wav_path, loop);
        ctx->esl_event_fire("PLAYBACK_START", jbuf, (size_t)n);
    }

    if (out) *out = pb;
    return NSBC_OK;
}

/* ---------- pb_stop ---------- */

int pb_stop(void *call_key)
{
    if (!call_key) return NSBC_INVAL;
    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx) return NSBC_ERR;

    pthread_mutex_lock(&g_pb_lock);
    pb_ctx_t **pp = &g_pb_list;
    while (*pp && (*pp)->call_key != call_key) pp = &(*pp)->next;
    if (!*pp) {
        pthread_mutex_unlock(&g_pb_lock);
        return NSBC_NOENT;
    }
    pb_ctx_t *pb = *pp;
    *pp = pb->next; /* 从链表摘除 */
    pb->active = 0;
    pthread_mutex_unlock(&g_pb_lock);

    /* 停时间轮 + 关 snd_file */
    tw_del(ctx->tw, &pb->tn);
    if (pb->snd) { snd_file_close(pb->snd); pb->snd = NULL; }

    llog(LOG_INFO, "pb", "STOP call=%p side=%d", call_key, pb->side);

    if (ctx->esl_event_fire) {
        ctx->esl_event_fire("PLAYBACK_STOP", pb->wav_path, strlen(pb->wav_path));
    }

    free(pb);
    return NSBC_OK;
}

/* ---------- pb_status ---------- */

int pb_status(void *call_key, int *side_out, int *loop_out)
{
    if (!call_key) return 0;
    pthread_mutex_lock(&g_pb_lock);
    pb_ctx_t *p = g_pb_list;
    while (p && p->call_key != call_key) p = p->next;
    pthread_mutex_unlock(&g_pb_lock);
    if (!p) return 0;
    if (side_out) *side_out = p->side;
    if (loop_out) *loop_out = p->loop;
    return 1;
}

int pb_stop_if_playing(void *call_key)
{
    if (!call_key) return 0;
    return pb_stop(call_key) == NSBC_OK ? 1 : 0;
}

/* ---------- pb_stop_by_flag ----------
 *
 * ★ 只停止 stop_on_answer == flag 值的播放上下文。
 * B 腿 LEG_CONFIRMED 时传 flag=1 → 只停 ringback，不影响正在播的
 * 工号提示音 / MoH / endless_playback。
 *
 * 实现：扫描 g_pb_list 找 call_key 匹配 + stop_on_answer==flag 的 ctx，
 * 按 pb_stop 标准流程停（摘链 + 删时间轮 + 关 snd_file）。 */
int pb_stop_by_flag(void *call_key, int stop_on_answer)
{
    if (!call_key) return 0;
    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx) return 0;

    int stopped = 0;
    pthread_mutex_lock(&g_pb_lock);
    pb_ctx_t **pp = &g_pb_list;
    while (*pp) {
        pb_ctx_t *pb = *pp;
        if (pb->call_key == call_key && pb->stop_on_answer == (stop_on_answer ? 1 : 0)) {
            *pp = pb->next; /* 从链表摘除 */
            pb->active = 0;
            tw_del(ctx->tw, &pb->tn);
            if (pb->snd) { snd_file_close(pb->snd); pb->snd = NULL; }
            llog(LOG_INFO, "pb", "pb_stop_by_flag stop call=%p flag=%d wav=%s",
                 call_key, stop_on_answer, pb->wav_path);
            free(pb);
            stopped++;
            /* 注意：摘链后继续扫——同一个 call_key 理论上只有一个 ctx，
             * 但 pb_start_ex 之前 pb_stop_if_playing 会先停旧的，
             * 这里保险起见不 break */
        } else {
            pp = &pb->next;
        }
    }
    pthread_mutex_unlock(&g_pb_lock);
    return stopped;
}

/* pb_start_chain：管道符拼接。P0 简化实现——内部只播第一个，
 * 后续文件由调用方（Dialplan 执行器）按顺序依次调 pb_start。
 * P1 扩展：把 chain 存在 pb_ctx_t 里，EOF 时自动 start 下一个。 */
int pb_start_chain(void *call_key, const char **wav_list, int n,
                  int side, pb_ctx_t **out)
{
    if (!call_key || !wav_list || n <= 0) return NSBC_INVAL;
    if (n == 1) return pb_start(call_key, wav_list[0], side, 0, out);
    /* P0 简化：先停可能在播的，依次调 pb_start（后续 EOF 自动 stop）。
     * 这符合 Dialplan app_chain 语义——每个 playback app 独立执行。 */
    pb_stop_if_playing(call_key);
    int rc = NSBC_OK;
    for (int i = 0; i < n; ++i) {
        rc = pb_start(call_key, wav_list[i], side, 0,
                      (i == n - 1) ? out : NULL);
        if (rc != NSBC_OK) break;
        /* 注意：pb_start 内部挂时间轮，当前 pb_timer_cb 只推一个文件。
         * 多文件串联需要扩展 pb_ctx_t 存 chain + next_idx，EOF 时 start 下一个。
         * P0 先按 Dialplan app_chain 模式走——每个文件一个 playback app 调用。 */
    }
    return rc;
}

/* ---------- pb_active_count / pb_total_players ---------- */
uint64_t pb_total_players(void) { return g_pb_total; }

/* ---------- OAM/ESL 命令注册 ---------- */
/* 命令注册在 oam 模块里做（需要 cmd_reg），这里只导出帮助函数。
 * 实际命令绑定在 oam/oam_module.c start 里。 */

int pb_register_commands(void) { return NSBC_OK; } /* 占位 */

/* ---------- §8 ctx 槽位绑定 ----------
 * 让 oam 层（call_commands）能调 pb_stop_if_playing 而不反向 include
 * media 层头文件。由 core/main 在模块 start 时调一次。 */
#include "app_ctx.h"
void pb_bind_ctx_slots(nsbc_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->pb_stop_if_playing = pb_stop_if_playing;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
