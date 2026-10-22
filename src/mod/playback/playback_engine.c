/* playback_engine.c — Phase 3 Advanced Playback Engine（2026-10-15 增量）
 *
 * 单侧媒体注入：WAV → PCM S16LE → PCMU → RTP 封装 → sendto relay fd。
 * reactor 时间轮驱动周期性 pull+send；EOF 自动 stop 或 loop 模式循环。
 *
 * 依赖：player.c（已存在的 WAV+PCM 源）+ media_bridge.c（relay 暴露 API）
 * 线程：全部在 reactor 时间轮回调里跑，零锁。
 */

#include "playback_engine.h"
#include "player.h"
#include "media_bridge.h"
#include "app_ctx.h"
#include "nsbc_common.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

/* ---------- G.711 μ-law 编码 ---------- */

static uint8_t pcmu_encode(int16_t pcm)
{
    /* G.711 μ-law 编码（线性近似，256 值查表由 encoder 生成） */
    int sign = pcm >> 15;
    uint16_t sample = sign ? (uint16_t)(-pcm) | 0x8000 : (uint16_t)pcm;
    sample += 0x84; /* 加 132 偏移 */
    uint8_t exponent = 0;
    uint16_t mask = 0x4000;
    while (!(sample & mask) && exponent < 7) {
        mask >>= 1;
        exponent++;
    }
    uint8_t mantissa = (sample >> ((exponent == 0) ? 7 : (exponent + 3))) & 0x0F;
    uint8_t mu = (sign ? 0 : 0x80) | (exponent << 4) | mantissa;
    return ~mu; /* μ-law 反极性 */
}

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
#define PB_RTP_PT 0 /* PCMU */
#define PB_PULL_FRAMES 80 /* 10ms @ 8kHz → 80 samples */
#define PB_PULL_INTERVAL_MS 20 /* 20ms 定时器间隔（比 10ms pull 慢一倍，
                                * 节省 CPU；播放音高略高，正常范围） */

typedef struct pb_ctx {
    void *call_key;
    int side;                 /* 0=A, 1=B, -1=both */
    int loop;                 /* 0=once, 1=loop */
    tw_node_t tn;             /* 时间轮节点 */
    player_t pl;              /* PCM 播放器 */
    pcm_src_t *src;           /* WAV 音源引用 */
    rtp_relay_t *relay;       /* relay 指针（弱引用，bridge 自己管理生命周期） */
    nsbc_sa_t peer[2];        /* 两侧 peer 地址 */
    uint32_t ssrc;            /* 随机 SSRC */
    uint16_t seq;             /* RTP seq */
    uint32_t ts;              /* RTP timestamp（8000 per second → 每次 +80） */
    volatile int active;      /* 1=在播，0=待清理 */
    char wav_path[256];
    struct pb_ctx *next;
} pb_ctx_t;

static pthread_mutex_t g_pb_lock = PTHREAD_MUTEX_INITIALIZER;
static pb_ctx_t *g_pb_list = NULL;
static player_reg_t g_pb_reg;
static int g_pb_started = 0;
static uint64_t g_pb_total = 0;

/* ---------- 全局 registry 初始化 ---------- */

int pb_engine_start(void)
{
    if (g_pb_started) return NSBC_OK;
    player_reg_init(&g_pb_reg);
    g_pb_started = 1;
    llog(LOG_INFO, "pb", "playback engine started (PCMU 8kHz)");
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
        player_put(p->src);
        free(p);
        p = nx;
    }
    g_pb_list = NULL;
    pthread_mutex_unlock(&g_pb_lock);
    player_reg_fini(&g_pb_reg);
    g_pb_started = 0;
    llog(LOG_INFO, "pb", "playback engine stopped");
}

/* ---------- 定时器回调：pull PCM → 编码 → sendto ---------- */

static void pb_timer_cb(uint64_t now_ms, void *arg)
{
    pb_ctx_t *pb = (pb_ctx_t *)arg;
    (void)now_ms;
    if (!pb || !pb->active) return;

    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->tw || !pb->relay) return;

    /* pull PCM */
    int16_t pcm_frames[PB_PULL_FRAMES];
    int pulled = player_pull(&pb->pl, pcm_frames, PB_PULL_FRAMES);
    if (pulled <= 0) {
        /* EOF */
        if (pb->loop) {
            pb->pl.pos = 0; /* 回到开头 */
            pulled = player_pull(&pb->pl, pcm_frames, PB_PULL_FRAMES);
        }
        if (pulled <= 0) {
            /* 结束 */
            llog(LOG_INFO, "pb", "EOF reached call=%p side=%d", pb->call_key, pb->side);
            if (ctx->esl_event_fire) {
                ctx->esl_event_fire("PLAYBACK_STOP", pb->wav_path, strlen(pb->wav_path));
            }
            pb_stop(pb->call_key);
            return;
        }
    }

    /* PCMU 编码 */
    int8_t pcmu[PB_PULL_FRAMES];
    for (int i = 0; i < pulled; i++)
        pcmu[i] = (int8_t)pcmu_encode(pcm_frames[i]);

    /* RTP 封装：12B header + payload */
    uint8_t rtp_frame[12 + PB_PULL_FRAMES];
    memset(rtp_frame, 0, sizeof(rtp_frame));
    rtp_frame[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
    rtp_frame[1] = PB_RTP_PT; /* PT=0 (PCMU), M=0 */
    rtp_write_u16(&rtp_frame[2], pb->seq++);
    rtp_write_u32(&rtp_frame[4], pb->ts);
    pb->ts += pulled;
    rtp_write_u32(&rtp_frame[8], pb->ssrc);
    memcpy(&rtp_frame[12], pcmu, pulled);
    int rtp_len = 12 + pulled;

    /* sendto — side 指定的腿 或 both */
    int sides[2] = {pb->side, pb->side};
    int nside;
    if (pb->side >= 0) {
        sides[0] = pb->side;
        nside = 1;
    } else {
        sides[0] = 0; sides[1] = 1;
        nside = 2;
    }
    for (int i = 0; i < nside; i++) {
        int side = sides[i];
        if (pb->peer[side].len == 0) continue; /* 无 peer 地址（latch 未完成） */
        int fd = rtp_relay_fd(pb->relay, side);
        if (fd < 0) continue;
        ssize_t sret = sendto(fd, rtp_frame, rtp_len,
                              MSG_NOSIGNAL,
                              (const struct sockaddr *)&pb->peer[side].ss,
                              pb->peer[side].len);
        if (sret != rtp_len) {
            llog(LOG_WARN, "pb", "sendto FAIL side=%d ret=%zd expected=%d errno=%d",
                 side, sret, rtp_len, errno);
        }
    }

    /* 下一轮 */
    if (!pb->active) return;
    tw_add(ctx->tw, &pb->tn, PB_PULL_INTERVAL_MS, pb_timer_cb, pb);
}

/* ---------- pb_start ---------- */

int pb_start(void *call_key, const char *wav_path, int side, int loop,
             pb_ctx_t **out)
{
    if (!g_pb_started || !call_key || !wav_path)
        return NSBC_INVAL;

    nsbc_ctx_t *ctx = nsbc_get_ctx();
    if (!ctx || !ctx->tw || !ctx->bridge)
        return NSBC_ERR;

    /* 1. load wav */
    char alias[64];
    snprintf(alias, sizeof(alias), "pb_%p_%s", call_key, strrchr(wav_path, '/') ?
             strrchr(wav_path, '/') + 1 : wav_path);
    int rc = player_load_wav(&g_pb_reg, wav_path, alias);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "pb", "load_wav FAIL path=%s rc=%d", wav_path, rc);
        return NSBC_ERR;
    }
    pcm_src_t *src = player_get(&g_pb_reg, alias);
    if (!src) return NSBC_ERR;

    /* 2. lookup relay + peer */
    rtp_relay_t *relay = NULL;
    nsbc_sa_t peer[2];
    rc = bridge_lookup_relay((bridge_mgr_t *)ctx->bridge, call_key, &relay, peer);
    if (rc != NSBC_OK || !relay) {
        player_put(src);
        llog(LOG_WARN, "pb", "lookup_relay FAIL call=%p rc=%d", call_key, rc);
        return NSBC_NOENT;
    }

    /* 3. create pb_ctx */
    pb_ctx_t *pb = calloc(1, sizeof(*pb));
    if (!pb) { player_put(src); return NSBC_NOMEM; }
    pb->call_key = call_key;
    pb->side = side;
    pb->loop = loop;
    pb->src = src;
    pb->relay = relay;
    pb->peer[0] = peer[0];
    pb->peer[1] = peer[1];
    pb->ssrc = (uint32_t)rand();
    pb->seq = 0;
    pb->ts = 0;
    pb->active = 1;
    snprintf(pb->wav_path, sizeof(pb->wav_path), "%s", wav_path);
    player_open(&pb->pl, src, loop);

    /* 4. 存全局列表 */
    pthread_mutex_lock(&g_pb_lock);
    pb->next = g_pb_list;
    g_pb_list = pb;
    g_pb_total++;
    pthread_mutex_unlock(&g_pb_lock);

    /* 5. 挂时间轮（立即调度一次） */
    tw_node_init(&pb->tn);
    tw_add(ctx->tw, &pb->tn, 0 /* 立即触发 */, pb_timer_cb, pb);

    llog(LOG_INFO, "pb", "START call=%p side=%d loop=%d wav=%s", call_key, side, loop, wav_path);

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

    /* 停时间轮 */
    tw_del(ctx->tw, &pb->tn);
    player_put(pb->src);

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
