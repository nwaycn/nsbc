/* H.264 RTP 重组器（RFC 6184）— M1 视频录制前置
 *
 * 职责：把 RTP 里的 H.264 NAL（单包 / FU-A 分片 / STAP-A 聚合）
 *       重组为完整 access unit，加 Annex-B start code 后回调。
 * 位置：media_bridge relay_recv_one（SRTP unprotect 之后）调用。
 *
 * 快路径零 malloc：buffer 在 create 时一次性分配（bridge_confirm 控制路径）。
 * 帧判定：marker=1 或 ts 变化；100ms 无 marker 超时强制 flush；
 *         512KB 帧上限 + seq 回退/丢失/重复 → 丢整帧（dropping 模式等下一个 marker/新 ts）。
 * ★ P0 修复：seq 丢失（gap>1）检测 —— 无重传/无 FEC 链路上半截 NAL 落盘
 *   会打断裂码器参考链（花屏 + 后续画面冻结），必须丢整帧。 */

#include "rtp_reasm.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

struct vid_reasm {
    uint8_t       *buf;
    size_t         len;
    size_t         cap;
    uint32_t       ts;
    int            have_ts;
    uint16_t       last_seq;
    int            have_seq;
    uint64_t       last_ms;
    int            dropping;   /* 溢出/乱序后丢弃当前帧，等下一个 marker/新 ts */
    int            in_fu;      /* 1 = FU-A 分片进行中（S 已见 E 未到） */
    vid_frame_cb_t cb;
    void          *ud;
};

/* ---------- 内部 append helpers（返回 0=OK, -1=溢出） ---------- */

/* 帧边界标记 NAL：原始 RTP payload 已带这些类型 → 不需要重组器再注入 AUD。
 * type=6 SEI, type=7 SPS, type=8 PPS, type=9 AUD, type=12 EOB,
 * type=13 Prefix(SVC), type=14 Suffix(SVC) */
static inline int is_boundary_nal(uint8_t t)
{
    return t == 6 || t == 7 || t == 8 || t == 9 || t == 12 || t == 13 || t == 14;
}

static int raw_append(vid_reasm_t *r, const uint8_t *p, size_t n)
{
    if (r->len + n > r->cap) return -1;
    if (n > 0) {
        memcpy(r->buf + r->len, p, n);
        r->len += n;
    }
    return 0;
}

static int annexb_append(vid_reasm_t *r, const uint8_t *nal, size_t n)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    /* ★ AUD 条件注入：仅当帧首 NAL 本身不是帧边界标记时才补 AUD。
     * Chrome SVC 原生 RTP payload 已带 AUD/EOB/Prefix 等边界标记，
     * 若重组器再无条件注入一份 → 每帧 2× AUD → ffmpeg demux 帧边界混乱 → 花屏。
     * 取证实锤：935 帧侧车有 1873 个 AUD，正是此因。 */
    if (r->len == 0 && !is_boundary_nal(nal[0] & 0x1F)) {
        static const uint8_t aud[5] = { 0, 0, 0, 1, 9 };
        if (r->cap < sizeof(aud) + 4 + n) return -1;
        if (raw_append(r, aud, sizeof(aud)) != 0) return -1;
    }
    if (r->len + 4 + n > r->cap) return -1;
    if (raw_append(r, sc, 4) != 0) return -1;
    return raw_append(r, nal, n);
}

/* ---------- 帧完成 ---------- */

static void frame_flush(vid_reasm_t *r)
{
    if (r->len > 0 && r->cb)
        r->cb(r->ud, r->buf, r->len, r->ts);
    r->len      = 0;
    r->have_ts  = 0;
    r->dropping = 0;
    r->in_fu    = 0;
}

static void frame_drop(vid_reasm_t *r)
{
    r->len      = 0;
    r->dropping = 1;
    r->in_fu    = 0;
    /* ★ have_ts/r->ts 刻意保留：dropping 恢复靠"新 timestamp = 新帧"检测，
     * 清掉会把恢复路径降级成"只能等 marker"——丢尾包时整条流冻结。 */
}

/* ---------- lifecycle ---------- */

int vid_reasm_create(vid_reasm_t **out, vid_frame_cb_t cb, void *ud)
{
    if (out == NULL || cb == NULL) return NSBC_INVAL;
    vid_reasm_t *r = (vid_reasm_t *)calloc(1, sizeof(*r));
    if (r == NULL) return NSBC_NOMEM;
    r->buf = (uint8_t *)malloc(VID_REASM_MAX_FRAME);
    if (r->buf == NULL) {
        free(r);
        return NSBC_NOMEM;
    }
    r->cap = VID_REASM_MAX_FRAME;
    r->cb  = cb;
    r->ud  = ud;
    *out = r;
    return NSBC_OK;
}

void vid_reasm_destroy(vid_reasm_t *r)
{
    if (r == NULL) return;
    free(r->buf);
    free(r);
}

/* ---------- 喂包 ---------- */

void vid_reasm_feed(vid_reasm_t *r, const uint8_t *payload, size_t len,
                    int marker, uint16_t seq, uint32_t ts, uint64_t now_ms)
{
    if (r == NULL || payload == NULL || len < 2) return;

    /* dropping 恢复：marker = 坏帧的尾包（连同它一起丢）；
     * 新 timestamp = 新帧开始（从这包重新积累）。
     * ★ 恢复时必须重置 have_seq：last_seq 停在坏帧附近，残留旧基准会把
     *   恢复帧的首包再判成 gap>1 → 再次 frame_drop → 无限循环冻结。 */
    if (r->dropping) {
        if (marker) {
            r->dropping = 0;
            r->len      = 0;
            r->have_ts  = 0;
            r->have_seq = 0;
            return;  /* 尾包也丢弃，避免半截 NAL 混进下一帧 */
        }
        if (!r->have_ts) {  /* 坏帧首片也丢了 —— 记下坏帧 ts 供新 ts 检测 */
            r->ts      = ts;
            r->have_ts = 1;
            return;
        }
        if (ts != r->ts) {
            r->dropping = 0;
            r->len      = 0;
            r->have_ts  = 0;
            r->have_seq = 0;
            /* 落到正常路径处理这包（新帧第一片） */
        } else {
            return;  /* 同一坏帧的后续碎片 —— 继续丢 */
        }
    }

    /* ★ P0：seq 丢失/乱序/重复检测（花屏根因）。
     *   diff > 1 —— 中间包丢失（无重传/无 FEC），当前帧必然缺片，
     *              半截 NAL 落盘 → 解码器参考链断裂 → 花屏/画面冻结。
     *   diff < 0 —— 乱序/重传：当前帧按缺片处理，丢整帧。
     *   diff == 0 —— 重复包：静默丢弃（重复分片落盘同样花屏）。 */
    if (r->have_seq) {
        int16_t diff = (int16_t)(seq - r->last_seq);
        if (diff == 0)
            return;              /* 重复包 —— 不落盘不丢帧 */
        if (diff < 0 || diff > 1) {
            frame_drop(r);
            return;
        }
    }
    r->last_seq = seq;
    r->have_seq = 1;

    /* marker 超时：100ms 无 marker 强制收尾（网络丢尾包兜底）。
     * FU-A 还在进行中 → 半截 NAL 只能丢，不能 flush 进文件。 */
    if (r->len > 0 && r->have_ts &&
        now_ms >= r->last_ms &&
        now_ms - r->last_ms > VID_REASM_MARKER_TIMEOUT_MS) {
        if (r->in_fu)
            frame_drop(r);
        else
            frame_flush(r);
    }

    /* timestamp 变化 = 新 access unit 开始（上个 FU-A 未完 → 丢，不 flush 半截 NAL） */
    if (r->have_ts && ts != r->ts) {
        if (r->in_fu)
            frame_drop(r);
        else
            frame_flush(r);
    }

    r->ts      = ts;
    r->have_ts = 1;
    r->last_ms = now_ms;

    const uint8_t nal_type = payload[0] & 0x1F;

    /* type=12 EOB —— Chrome SVC 专用 End of Bitstream 标记，不属于 access unit 内容。
     * 取证侧车有 4 个 EOB 混在帧数据里，落盘会打断 ffmpeg demux 帧边界。 */
    if (nal_type == 12)
        return;

    /* FU-A 未结束（S 见 E 未到）却来了其他 NAL 类型 —— 上个 NAL 被截断，丢整帧 */
    if (r->in_fu && nal_type != 28) {
        frame_drop(r);
        return;
    }

    if (nal_type >= 1 && nal_type <= 23) {
        /* 单包 NAL —— 整个 payload 就是一个 NAL */
        if (annexb_append(r, payload, len) != 0) {
            frame_drop(r);
            return;
        }
    } else if (nal_type == 24) {
        /* STAP-A —— [STAP-A hdr][2B size][NAL][2B size][NAL]... */
        size_t off = 1;
        while (off + 2 <= len) {
            uint16_t n = (uint16_t)((payload[off] << 8) | payload[off + 1]);
            off += 2;
            if (n == 0 || off + n > len) break;
            if (annexb_append(r, payload + off, n) != 0) {
                frame_drop(r);
                return;
            }
            off += n;
        }
    } else if (nal_type == 28) {
        /* FU-A —— [FU indicator][FU header][payload...]
         * S=1：新 NAL 开始，重构 NAL header = (indicator & 0xE0) | (fu_type) */
        const uint8_t fu_hdr  = payload[1];
        const int     start   = (fu_hdr & 0x80) != 0;
        const int     end     = (fu_hdr & 0x40) != 0;
        if (!start && !r->in_fu) {  /* 中间分片但 S 丢了 —— 无法重构 NAL 头 */
            frame_drop(r);
            return;
        }
        if (start) {
            if (r->in_fu) {  /* 上个 FU-A 还没 E 就来了新 S —— 截断，丢整帧 */
                frame_drop(r);
                return;
            }
            /* ★ 先重构 NAL header 再判断是否需要 AUD（同 annexb_append 条件注入逻辑） */
            const uint8_t nal_hdr = (uint8_t)((payload[0] & 0xE0) | (fu_hdr & 0x1F));
            if (r->len == 0 && !is_boundary_nal(nal_hdr & 0x1F)) {
                static const uint8_t aud[5] = { 0, 0, 0, 1, 9 };
                if (raw_append(r, aud, sizeof(aud)) != 0) {
                    frame_drop(r);
                    return;
                }
            }
            static const uint8_t sc[4] = { 0, 0, 0, 1 };
            if (raw_append(r, sc, 4) != 0 || raw_append(r, &nal_hdr, 1) != 0) {
                frame_drop(r);
                return;
            }
            r->in_fu = 1;
        }
        if (len > 2 && raw_append(r, payload + 2, len - 2) != 0) {
            frame_drop(r);
            return;
        }
        if (end)
            r->in_fu = 0;  /* FU-A 完整 —— 下个 NAL 可以是任意类型 */
    } else {
        /* type 0 / 25-27 / 29-31（FU-B/MTAP 等）不支持 —— 丢弃 */
        return;
    }

    if (marker) {
        if (r->in_fu)  /* FU-A 尾包（E 位）丢了 —— 半截 NAL 不能进文件 */
            frame_drop(r);
        else
            frame_flush(r);
    }
}
