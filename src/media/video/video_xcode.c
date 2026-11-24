/* video_xcode：RTP 视频拆包 → 解码 → 重编码 → 打包 */
#include "video_xcode.h"
#include "rtp_video.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

struct video_xcode {
    codec_id_t        src;
    codec_id_t        dst;
    const codec_ops_t *sops;
    const codec_ops_t *dops;

    codec_sess_t     *dec;
    codec_sess_t     *enc;

    int               src_kind;      /* 1=H264, 2=VP9, 3=VP8, 4=H265 */
    union {
        rtp_h264_depack_t h264;
        rtp_vp9_depack_t  vp9;
        rtp_vp8_depack_t  vp8;
        rtp_h265_depack_t h265;
    } dep;

    uint8_t           src_pt;
    uint8_t           dst_pt;
    uint16_t          out_seq;
    uint32_t          out_ts;
    uint32_t          out_ssrc;
    uint32_t          last_in_ts;
    int               have_in_ts;

    int               got_key;       /* 已成功处理过关键帧 */
    int               need_pli;      /* 等待上游关键帧 */
    int               force_key;     /* 下一个编码帧强制 IDR */

    uint8_t          *enc_buf;
    size_t            enc_cap;

    /* 录制旁路：重编码 AU 回调（可为 NULL） */
    video_xcode_au_fn au_fn;
    void             *au_ud;
};

#define VIDEO_XCODE_ENC_CAP (512 * 1024)

typedef struct {
    video_xcode_t     *vx;
    video_xcode_emit_fn emit;
    void              *u;
} pack_ctx_t;

static void emit_rtp(video_xcode_t *vx,
                     video_xcode_emit_fn emit, void *ud,
                     const uint8_t *payload, size_t len, int marker)
{
    uint8_t pkt[12 + RTP_VIDEO_MTU_DEFAULT + 16];
    if (len + 12 > sizeof(pkt)) return;

    pkt[0] = 0x80;                                   /* V=2 */
    pkt[1] = (uint8_t)((marker ? 0x80 : 0x00) | (vx->dst_pt & 0x7F));
    pkt[2] = (uint8_t)(vx->out_seq >> 8);
    pkt[3] = (uint8_t)(vx->out_seq & 0xFF);
    pkt[4] = (uint8_t)(vx->out_ts >> 24);
    pkt[5] = (uint8_t)(vx->out_ts >> 16);
    pkt[6] = (uint8_t)(vx->out_ts >> 8);
    pkt[7] = (uint8_t)(vx->out_ts);
    pkt[8]  = (uint8_t)(vx->out_ssrc >> 24);
    pkt[9]  = (uint8_t)(vx->out_ssrc >> 16);
    pkt[10] = (uint8_t)(vx->out_ssrc >> 8);
    pkt[11] = (uint8_t)(vx->out_ssrc);
    memcpy(pkt + 12, payload, len);

    ++vx->out_seq;
    emit(ud, pkt, len + 12);
}

static void pack_cb(void *ud, const uint8_t *payload, size_t len, int marker)
{
    pack_ctx_t *c = (pack_ctx_t *)ud;
    emit_rtp(c->vx, c->emit, c->u, payload, len, marker);
}

static void xcode_close(video_xcode_t *vx)
{
    if (!vx) return;
    if (vx->dec && vx->sops && vx->sops->fini) vx->sops->fini(vx->dec);
    if (vx->enc && vx->dops && vx->dops->fini) vx->dops->fini(vx->enc);
    if (vx->src_kind == 1) rtp_h264_depack_free(&vx->dep.h264);
    else if (vx->src_kind == 2) rtp_vp9_depack_free(&vx->dep.vp9);
    else if (vx->src_kind == 3) rtp_vp8_depack_free(&vx->dep.vp8);
    else if (vx->src_kind == 4) rtp_h265_depack_free(&vx->dep.h265);
    free(vx->enc_buf);
    free(vx);
}

video_xcode_t *video_xcode_create(codec_id_t src, codec_id_t dst,
                                  uint8_t src_pt, uint8_t dst_pt,
                                  uint32_t out_ssrc)
{
    if (src == CODEC_NONE || dst == CODEC_NONE || src == dst) return NULL;

    const codec_ops_t *so = codec_registry_find(src);
    const codec_ops_t *dop = codec_registry_find(dst);
    if (!so || !dop) return NULL;
    if (so->kind != CODEC_KIND_VIDEO || dop->kind != CODEC_KIND_VIDEO) return NULL;
    if (!so->vid_decode || !dop->vid_encode) return NULL;

    int sk = 0;
    if (so->id == CODEC_H264) sk = 1;
    else if (so->id == CODEC_VP9) sk = 2;
    else if (so->id == CODEC_VP8) sk = 3;
    else if (so->id == CODEC_H265) sk = 4;
    else return NULL;   /* 目前只支持 H.264 / H.265 / VP9 / VP8 */

    video_xcode_t *vx = (video_xcode_t *)calloc(1, sizeof(*vx));
    if (!vx) return NULL;

    vx->src = src;
    vx->dst = dst;
    vx->sops = so;
    vx->dops = dop;
    vx->src_kind = sk;
    vx->src_pt = src_pt;
    vx->dst_pt = dst_pt;
    vx->out_ssrc = out_ssrc ? out_ssrc : 0x4E534243u;
    vx->out_seq = (uint16_t)((out_ssrc >> 8) ^ (out_ssrc >> 24));
    vx->need_pli = 1;
    vx->force_key = 1;

    vx->enc_cap = VIDEO_XCODE_ENC_CAP;
    vx->enc_buf = (uint8_t *)malloc(vx->enc_cap);
    if (!vx->enc_buf) { xcode_close(vx); return NULL; }

    if (sk == 1) rtp_h264_depack_init(&vx->dep.h264);
    else if (sk == 2) rtp_vp9_depack_init(&vx->dep.vp9);
    else if (sk == 3) rtp_vp8_depack_init(&vx->dep.vp8);
    else rtp_h265_depack_init(&vx->dep.h265);

    if (so->vid_dec_init(NULL, &vx->dec) != NSBC_OK) { xcode_close(vx); return NULL; }
    if (dop->vid_enc_init(NULL, &vx->enc) != NSBC_OK) { xcode_close(vx); return NULL; }

    llog(LOG_INFO, "xcode", "video_xcode created src=%s dst=%s pt=%u->%u",
         so->sdp_name ? so->sdp_name : so->name,
         dop->sdp_name ? dop->sdp_name : dop->name,
         (unsigned)src_pt, (unsigned)dst_pt);
    return vx;
}

void video_xcode_destroy(video_xcode_t *vx)
{
    xcode_close(vx);
}

int video_xcode_take_pli(video_xcode_t *vx)
{
    if (!vx || !vx->need_pli) return 0;
    vx->need_pli = 0;
    return 1;
}

void video_xcode_set_au_sink(video_xcode_t *vx, video_xcode_au_fn fn, void *ud)
{
    if (!vx) return;
    /* 可能由信令线程在 re-INVITE 时调用，媒体线程同时在读：原子发布。
     * 先写 ud 再写 fn；撤销时先清 fn。 */
    if (fn == NULL) {
        __atomic_store_n(&vx->au_fn, (video_xcode_au_fn)NULL, __ATOMIC_RELEASE);
        __atomic_store_n(&vx->au_ud, (void *)NULL, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&vx->au_ud, ud, __ATOMIC_RELAXED);
        __atomic_store_n(&vx->au_fn, fn, __ATOMIC_RELEASE);
    }
}

/* 完整帧 → 解码 → 重编码 → 打包发送 */
static int xcode_frame(video_xcode_t *vx, const uint8_t *au, size_t au_len,
                       uint32_t in_ts, int in_key,
                       video_xcode_emit_fn emit, void *ud)
{
    /* 诊断：每个媒体线程独立计数，只打前几次，避免刷屏 */
    static __thread int diag_wait_key, diag_dec_fail, diag_enc_fail, diag_ok;

    if (!vx->got_key) {
        if (!in_key) {
            vx->need_pli = 1;
            if (diag_wait_key++ < 3)
                llog(LOG_INFO, "xcode", "wait keyframe: drop non-key au_len=%zu",
                     au_len);
            return 0;
        }
    }

    /* 输出时间戳：沿用输入 90kHz 增量 */
    if (vx->have_in_ts)
        vx->out_ts += (uint32_t)(in_ts - vx->last_in_ts);
    else
        vx->out_ts = in_ts;
    vx->last_in_ts = in_ts;
    vx->have_in_ts = 1;

    video_frame_t f;
    memset(&f, 0, sizeof(f));
    int rc = vx->sops->vid_decode(vx->dec, au, au_len, &f);
    if (rc != NSBC_OK) {
        if (!vx->got_key) vx->need_pli = 1;
        if (diag_dec_fail++ < 3)
            llog(LOG_WARN, "xcode", "vid_decode FAIL rc=%d au_len=%zu key=%d",
                 rc, au_len, in_key);
        return 0;   /* AGAIN/ERR：丢弃该帧，等待下一个关键帧 */
    }

    f.pts = vx->out_ts;
    if (!vx->got_key) f.keyframe = 1;      /* 首帧强制 IDR */
    if (vx->force_key) { f.keyframe = 1; vx->force_key = 0; }

    size_t out_len = 0;
    int key = 0;
    rc = vx->dops->vid_encode(vx->enc, &f, vx->enc_buf, vx->enc_cap,
                              &out_len, &key);
    if (vx->sops->vid_frame_free)
        vx->sops->vid_frame_free(vx->dec, &f);

    if (rc != NSBC_OK || out_len == 0) {
        if (!vx->got_key) vx->need_pli = 1;
        if (diag_enc_fail++ < 3)
            llog(LOG_WARN, "xcode", "vid_encode FAIL rc=%d out_len=%zu",
                 rc, out_len);
        return 0;
    }

    /* 录制旁路：直接交付重编码后的 H.264 Annex-B AU。
     * 编码器未开 GLOBAL_HEADER，libx264 输出带起始码（Annex-B），
     * 且 repeat-headers=1 保证每个 IDR 自带 SPS/PPS —— 可直接写 .h264。 */
    video_xcode_au_fn afn = __atomic_load_n(&vx->au_fn, __ATOMIC_ACQUIRE);
    if (afn)
        afn(__atomic_load_n(&vx->au_ud, __ATOMIC_RELAXED),
            vx->enc_buf, out_len, vx->out_ts, key);

    /* emit=NULL 表示只做录制、不转发（录制定向转码），跳过打包 */
    if (emit) {
        pack_ctx_t c;
        c.vx = vx; c.emit = emit; c.u = ud;

        if (vx->dops->payload == RTP_PAYLOAD_VP9)
            rtp_vp9_pack(vx->enc_buf, out_len, RTP_VIDEO_MTU_DEFAULT, pack_cb, &c);
        else if (vx->dops->payload == RTP_PAYLOAD_VP8)
            rtp_vp8_pack(vx->enc_buf, out_len, RTP_VIDEO_MTU_DEFAULT, pack_cb, &c);
        else if (vx->dops->payload == RTP_PAYLOAD_H265)
            rtp_h265_pack(vx->enc_buf, out_len, RTP_VIDEO_MTU_DEFAULT, pack_cb, &c);
        else
            rtp_h264_pack(vx->enc_buf, out_len, RTP_VIDEO_MTU_DEFAULT, pack_cb, &c);
    }

    vx->got_key = 1;
    if (diag_ok++ < 5)
        llog(LOG_INFO, "xcode", "FRAME ok in=%zu out=%zu key=%d ts=%u",
             au_len, out_len, key, (unsigned)vx->out_ts);
    return 0;
}

int video_xcode_input(video_xcode_t *vx,
                      const uint8_t *payload, size_t len,
                      int marker, uint32_t rtp_ts,
                      video_xcode_emit_fn emit, void *ud)
{
    /* emit 可为 NULL：此时只走 AU sink（录制），不产出 RTP 包 */
    if (!vx || !payload || len == 0) return NSBC_INVAL;

    const uint8_t *au = NULL;
    size_t au_len = 0;
    uint32_t au_ts = 0;
    int key = 0;
    int r;

    if (vx->src_kind == 1)
        r = rtp_h264_depack_feed(&vx->dep.h264, payload, len, marker, rtp_ts,
                                 &au, &au_len, &au_ts, &key);
    else if (vx->src_kind == 2)
        r = rtp_vp9_depack_feed(&vx->dep.vp9, payload, len, marker, rtp_ts,
                                &au, &au_len, &au_ts, &key);
    else if (vx->src_kind == 3)
        r = rtp_vp8_depack_feed(&vx->dep.vp8, payload, len, marker, rtp_ts,
                                &au, &au_len, &au_ts, &key);
    else
        r = rtp_h265_depack_feed(&vx->dep.h265, payload, len, marker, rtp_ts,
                                 &au, &au_len, &au_ts, &key);

    if (r < 0) return r;
    if (r == 0) return 0;
    return xcode_frame(vx, au, au_len, au_ts, key, emit, ud);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
