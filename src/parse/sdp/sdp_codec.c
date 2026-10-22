/* 编解码列表构建与派生判定：
 * 每个 fmt 项解析一次；rtpmap/fmtp 先查媒体段、再回落会话段；
 * 数字 PT 无 rtpmap 时按 RFC 3551 静态表补全 */

#include "sip_common.h"
#include "sdp.h"
#include "sdp_internal.h"
#include "log.h"
/* 在指定属性集中找 rtpmap 并解析 */
static nsbc_rc_t find_rtpmap(const sdp_attr_t *attrs, int n, int want_pt,
                             sip_tok_t *enc, sip_tok_t *rate,
                             sip_tok_t *channels)
{
    /* ★ DIAG：遍历 attrs 时打印每个条目 —— 确认 name/val 里到底是什么 */
    for (int i = 0; i < n; ++i) {
        llog(LOG_INFO, "sdp",
             "find_rtpmap[i=%d/%d] want_pt=%d name.len=%zu name.p=%.*s val.len=%zu val.p=%.*s",
             i, n, want_pt,
             attrs[i].name.len,
             (int)attrs[i].name.len,
             attrs[i].name.p ? attrs[i].name.p : "(null)",
             attrs[i].val.len,
             (int)attrs[i].val.len,
             attrs[i].val.p ? attrs[i].val.p : "(null)");
    }
    /* ★ P1 2026-10-16："rtpmap" 是 6 字符 (r-t-p-m-a-p)，原代码写成 7 导致所有
     * rtpmap 名称匹配失败 → build_codecs 里 find_rtpmap 恒返回 -1 → PT=110/126
     * 等无静态映射的 codec 被 SKIP。PT=8 靠 static_lookup 兜底才勉强能用。 */
    for (int i = 0; i < n; ++i) {
        if (!(attrs[i].name.len == 6 &&
              sip_ci_eq(attrs[i].name.p, "rtpmap", 6)))
            continue;
        if (attrs[i].val.p == NULL)
            continue;
        int pt;
        /* ★ 之前 return NSBC_ERR 太激进：某一条 rtpmap val 解析失败
         * 会导致整个 find_rtpmap 对所有 PT 都返回 ERR —— 即使前面的
         * rtpmap 都是好的、后面的才格式错。改成 continue 跳过。 */
        if (sdp_parse_rtpmap_val(&attrs[i].val, &pt, enc, rate,
                                 channels) != NSBC_OK) {
            /* ★ 2026-10-16 P1 修复后诊断：如果 rtpmap val 解析仍失败，
             * 说明 val 本身格式有问题（如尾部残留 \r / 无 '/' / PT 非数字），
             * 打印 val 原始内容帮定位。正常情况下修完 name 长度后不应触发。 */
            llog(LOG_DEBUG, "sdp",
                 "find_rtpmap: sdp_parse_rtpmap_val FAIL i=%d want_pt=%d "
                 "val=[%.*s]",
                 i, want_pt, (int)attrs[i].val.len,
                 attrs[i].val.p ? attrs[i].val.p : "(null)");
            continue;
        }
        llog(LOG_INFO, "sdp", "find_rtpmap: i=%d parsed pt=%d want_pt=%d %s",
             i, pt, want_pt, (pt == want_pt) ? "★MATCH" : "");
        if (pt == want_pt)
            return NSBC_OK;
    }
    return NSBC_ERR;
}

static int find_fmtp(const sdp_attr_t *attrs, int n, int want_pt,
                     sip_tok_t *params)
{
    for (int i = 0; i < n; ++i) {
        if (!(attrs[i].name.len == 4 &&
              sip_ci_eq(attrs[i].name.p, "fmtp", 4)))
            continue;
        if (attrs[i].val.p == NULL)
            continue;
        const char *p = attrs[i].val.p;
        const char *end = p + attrs[i].val.len;
        const char *sp = p;
        while (sp < end && !sip_is_sp(*sp))
            ++sp;
        long pt;
        if (sp == p || !sip_parse_ulong(p, (size_t)(sp - p), &pt))
            continue;
        if ((int)pt == want_pt) {
            const char *pv = sip_skip_lws_run(sp, end);
            params->p = pv;
            params->len = (size_t)(end - pv);
            return 1;
        }
    }
    return 0;
}

nsbc_rc_t sdp_media_build_codecs(sdp_t *s, sdp_media_t *m)
{
    if (m->codecs_built)
        return NSBC_OK;
    llog(LOG_INFO, "sdp",
         "build_codecs: ENTER nfmt=%d nattrs(media)=%d nattrs(session)=%d",
         m->nfmt, m->nattrs, s->nattrs);
    int n = 0;
    for (int i = 0; i < m->nfmt; ++i) {
        int pt = m->fmt[i].pt;
        llog(LOG_INFO, "sdp", "build_codecs: fmt[%d] pt=%d", i, pt);
        if (pt < 0)
            continue; /* 非数字 PT（如 webrtc-datachannel）非 RTP 编解码 */
        if (n >= SDP_MAX_FMT)
            return NSBC_ERR;
        sdp_codec_t *c = &m->codecs[n];
        memset(c, 0, sizeof(*c));
        c->pt = pt;

        sip_tok_t enc, rate, channels;
        nsbc_rc_t rc = find_rtpmap(m->attrs, m->nattrs, pt, &enc, &rate,
                                   &channels);
        llog(LOG_INFO, "sdp", "build_codecs: pt=%d find_rtpmap(m) rc=%d", pt, rc);
        if (rc != NSBC_OK && s->nattrs) {
            rc = find_rtpmap(s->attrs, s->nattrs, pt, &enc, &rate,
                             &channels);
            llog(LOG_INFO, "sdp", "build_codecs: pt=%d find_rtpmap(s) rc=%d", pt, rc);
        }
        if (rc != NSBC_OK) {
            /* 静态 PT 回退（sdp_attr.c） */
            sip_tok_t senc, srate;
            nsbc_rc_t src = sdp_static_pt_lookup(pt, &senc, &srate);
            llog(LOG_INFO, "sdp", "build_codecs: pt=%d static_lookup rc=%d", pt, src);
            if (src != NSBC_OK) {
                /* ★ 之前直接 return NSBC_ERR 导致整个函数退出
                 * 现在改成 skip continue——不能因为一个不认识的 PT 就丢全部 */
                llog(LOG_WARN, "sdp",
                     "build_codecs: pt=%d unknown (no rtpmap, not static) — SKIP", pt);
                continue;
            }
            c->enc = senc;
            c->rate = srate;
        } else {
            c->enc = enc;
            c->rate = rate;
            c->channels = channels;
        }

        sip_tok_t fp;
        int got_fp = find_fmtp(m->attrs, m->nattrs, pt, &fp);
        if (!got_fp && s->nattrs)
            got_fp = find_fmtp(s->attrs, s->nattrs, pt, &fp);
        if (got_fp)
            c->fmtp = fp;

        c->telephone_event =
            (c->enc.len == 15 &&
             sip_ci_eq(c->enc.p, "telephone-event", 15));
        ++n;
    }
    m->ncodecs = n;
    m->codecs_built = 1;
    llog(LOG_INFO, "sdp", "build_codecs: DONE ncodecs=%d", n);
    return NSBC_OK;
}

/* ---- 方向：媒体段声明优先，否则回落会话段，最终 sendrecv ---- */
sdp_dir_t sdp_media_effective_dir(const sdp_t *s, const sdp_media_t *m)
{
    if (m->dir != SDP_DIR_INHERIT)
        return m->dir;
    if (s->dir != SDP_DIR_INHERIT)
        return s->dir;
    return SDP_DIR_SENDRECV;
}

const sdp_conn_t *sdp_media_effective_conn(const sdp_t *s,
                                           const sdp_media_t *m)
{
    if (m->has_conn)
        return &m->conn;
    if (s->has_conn)
        return &s->conn;
    return NULL;
}

/* ---- T.38：m=image ... udptl；属性集含 T38Fax*；
 *      兼容部分网关的 m=audio ... a=rtpmap:PT T.38/rate 形式 ---- */
static int rtpmap_enc_is_t38(const sdp_attr_t *attrs, int n)
{
    for (int i = 0; i < n; ++i) {
        /* ★ 同 find_rtpmap："rtpmap" 6 字符，原代码 len==7 / sip_ci_eq(..., 7) 恒假 */
        if (attrs[i].name.len != 6 ||
            !sip_ci_eq(attrs[i].name.p, "rtpmap", 6) ||
            attrs[i].val.p == NULL)
            continue;
        const char *p = attrs[i].val.p;
        const char *end = p + attrs[i].val.len;
        const char *sp = p;
        while (sp < end && !sip_is_sp(*sp))
            ++sp;
        const char *e = sip_skip_lws_run(sp, end);
        const char *slash = e;
        while (slash < end && *slash != '/')
            ++slash;
        size_t elen = (size_t)(slash - e);
        /* "T.38" 或 "T38" */
        if ((elen == 4 && sip_ci_eq(e, "T.38", 4)) ||
            (elen == 3 && sip_ci_eq(e, "T38", 3)))
            return 1;
    }
    return 0;
}

int sdp_media_is_t38(const sdp_media_t *m)
{
    if (m->type == SDP_M_IMAGE &&
        m->proto.len == 5 && sip_ci_eq(m->proto.p, "udptl", 5))
        return 1;
    for (int i = 0; i < m->nattrs; ++i)
        if (m->attrs[i].name.len > 7 &&
            sip_ci_eq(m->attrs[i].name.p, "T38Fax", 7))
            return 1;
    if (rtpmap_enc_is_t38(m->attrs, m->nattrs))
        return 1;
    return 0;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
