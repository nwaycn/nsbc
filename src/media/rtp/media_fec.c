/* RED/FEC——粗骨架（RFC 2198/5109/8627，REQ-MEDIA-FEC，P1/P2）
 *
 * 接线注意：RED 块头为 PT(7b)+时间戳差(14b)+块长(10b) 变长编码；
 * ULP FEC 级 0 保护 RTP 头+载荷 XOR；FlexFEC 按 k 位掩码分组，
 * repair RTP 流独立 SSRC（出向重写 REQ-MEDIA-SEC-003）。
 * 本模块不碰 RTCP NACK（RFC4585），那是 rtp_session 重传路径。
 */
#include "media_fec.h"

struct fec_enc {
    fec_cfg_t cfg;
};

struct fec_dec {
    fec_cfg_t cfg;
};

int fec_enc_create(const fec_cfg_t *cfg, fec_enc_t **out)
{
    fec_enc_t *e;
    if (cfg == NULL || out == NULL || cfg->scheme == FEC_SCHEME_NONE)
        return NSBC_INVAL;
    e = (fec_enc_t *)calloc(1, sizeof(*e));
    if (e == NULL)
        return NSBC_NOMEM;
    e->cfg = *cfg;
    *out = e;
    return NSBC_OK;
}

void fec_enc_destroy(fec_enc_t *e)
{
    free(e);
}

int fec_enc_on_packet(fec_enc_t *e, const uint8_t *payload, size_t len, uint16_t seq, uint32_t ts, uint8_t *out, size_t cap, size_t *out_len, int *need)
{
    if (e == NULL || payload == NULL || out == NULL || out_len == NULL ||
        need == NULL)
        return NSBC_INVAL;
    *need = 0;
    *out_len = 0;
    (void)len;
    (void)seq;
    (void)ts;
    (void)cap;
    /* TODO(P1)：RED 拼级联块；ULP 组内 XOR 上下文滚动；Flex 组末出 repair */
    return NSBC_NOTSUP;
}

int fec_dec_create(const fec_cfg_t *cfg, fec_dec_t **out)
{
    fec_dec_t *d;
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    d = (fec_dec_t *)calloc(1, sizeof(*d));
    if (d == NULL)
        return NSBC_NOMEM;
    d->cfg = *cfg;
    *out = d;
    return NSBC_OK;
}

void fec_dec_destroy(fec_dec_t *d)
{
    free(d);
}

int fec_dec_on_red(fec_dec_t *d, const uint8_t *buf, size_t len, const uint8_t **primary, size_t *primary_len, uint32_t *ts_out)
{
    if (d == NULL || buf == NULL || primary == NULL || primary_len == NULL)
        return NSBC_INVAL;
    (void)len;
    (void)ts_out;
    /* TODO(P1)：逐块解析 RED 头（1/2/3 字节变长判定），
     * 最末块=主载荷，前序冗余块入恢复缓存（按 ts 索引）。 */
    return NSBC_NOTSUP;
}

int fec_dec_on_repair(fec_dec_t *d, const uint8_t *buf, size_t len, uint8_t *restored, size_t cap, size_t *out_len, uint16_t *restored_seq, int *need)
{
    if (d == NULL || buf == NULL || restored == NULL || out_len == NULL ||
        need == NULL)
        return NSBC_INVAL;
    *need = 0;
    *out_len = 0;
    (void)len;
    (void)cap;
    (void)restored_seq;
    /* TODO(P1/P2)：组缓存齐套检查，缺 1 包时 XOR 恢复，缺多包按
     * FlexFEC 掩码跨组解；恢复后回填 SSRC/seq 交 jitter_buf。 */
    return NSBC_NOTSUP;
}

int fec_sdp_negotiate(fec_scheme_t want, const char *offer_fmtp,
                      const char *answer_fmtp, fec_cfg_t *out)
{
    if (out == NULL)
        return NSBC_INVAL;
    memset(out, 0, sizeof(*out));
    (void)want;
    (void)offer_fmtp;
    (void)answer_fmtp;
    return NSBC_NOTSUP;
}
