/* DTLS-SRTP 媒体终结（REQ-DTLS-001）
 *
 * 在 relay 的 WebRTC 侧 UDP fd 上：
 *   1. 收到 STUN Binding Request → 回 Binding Response（ICE-Lite host candidate only）
 *   2. 收到 DTLS Client Hello → 推进 DTLS 握手
 *   3. 握手完成 → RFC5764 keying material → nsbc_srtp_session_create
 *   4. 后续 SRTP 包 → srtp_unprotect → 明文 RTP relay；明文 RTP → srtp_protect → SRTP 发回
 *
 * reactor 线程单线程运行，DTLS 非阻塞，状态机无锁。
 * OpenSSL 版本差异全由 nsbc_ssl 层封装——本文件只暴露协议状态机 + STUN 处理。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dtls_srtp.h"
#include "log.h"
#include "nsbc_common.h"
#include "net_sock.h"
#include "nsbc_ssl.h"
#include "srtp_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef NSBC_WITH_OPENSSL
#include <openssl/hmac.h>   /* STUN MESSAGE-INTEGRITY HMAC-SHA1 */
#endif

/* STUN/RFC5389 常量 */
#define STUN_MAGIC_COOKIE   0x2112A442u
#define STUN_METH_BINDING     0x0001

/* ====================================================================
 * STUN CRC32（IEEE 802.3 多项式，RFC5389 FINGERPRINT 用）
 * 前向声明——stun_resp_binding 先调，定义在后面
 * ==================================================================== */
static uint32_t stun_crc32_poly(const uint8_t *data, size_t len,
                                const uint8_t *extra, size_t extra_len);

/* TLS/STUN/SRTP 包前缀判别：relay_cb 单线程 demux 无锁 */
static int pkt_classify(const uint8_t *buf, size_t len)
{
    if (len < 2) return -1;
    uint16_t type = (uint16_t)((buf[0] << 8) | buf[1]);
    /* STUN：高 2 bit 00，方法 0-15 bit。首 2 字节后 30bit=MAGIC */
    if ((type & 0xC000) == 0x0000 && len >= 20) {
        uint32_t cookie = ntohl(*(const uint32_t *)(buf + 4));
        if (cookie == STUN_MAGIC_COOKIE) return 1; /* STUN */
    }
    /* DTLS：Content Type 20-25（ChangeCipherSpec/Handshake 等） */
    if (buf[0] >= 20 && buf[0] <= 25) return 2; /* DTLS record */
    /* SRTP/RTP：版本位 10 */
    if ((buf[0] & 0xC0) == 0x80) return 3; /* SRTP 或明文 RTP */
    return -1;
}

/* STUN Binding Success Response 构造（RFC5389）
 * 必含三属性，缺一 Chrome 都会判 ICE check 失败：
 *   XOR-MAPPED-ADDRESS — 地址按 magic cookie（不是 TID！）异或
 *   MESSAGE-INTEGRITY  — HMAC-SHA1(key=本端 ice_pwd)（short-term credential）
 *   FINGERPRINT        — CRC32 ^ 0x5354554e
 * HMAC 输入 = 消息头(长度已含 MI 属性 24B) + 前置属性 + MI 属性头 4B，
 * 不含 MI value（RFC5389 §15.4）；FINGERPRINT 的 CRC 覆盖含 FP 头、
 * FP value 置零的整条消息（长度=最终值）。 */
static int stun_resp_binding(const uint8_t *req_tid, const uint8_t *src4,
                             uint16_t src_port, const char *pwd,
                             int add_ice_controlled,
                             uint8_t *out, size_t outsz)
{
    /* XOR(12) + IC(12, 可选) + MI(24) + FP(8) — ice-lite 必须回 ICE-CONTROLLED */
    const size_t xor_len = 12;
    const size_t ic_len  = add_ice_controlled ? 12 : 0;  /* ICE-CONTROLLED attr=12B */
    const size_t mi_len  = 24;
    const size_t fp_len  = 8;
    size_t body_len = xor_len + ic_len + mi_len + fp_len;
    size_t total = 20 + body_len;
    if (outsz < total || !pwd)
        return NSBC_NOSPC;

    uint8_t *p = out;
    /* Header：Binding Success Response type=0x0101 */
    p[0] = 0x01; p[1] = 0x01;
    p[2] = (uint8_t)(body_len >> 8); p[3] = (uint8_t)body_len;
    uint32_t cookie = htonl(STUN_MAGIC_COOKIE);
    memcpy(p + 4, &cookie, 4);
    memcpy(p + 8, req_tid, 12);
    p += 20;

    /* XOR-MAPPED-ADDRESS (attr 0x0020, IPv4 value=8B: reserved(1)+family(1)+port(2)+addr(4)) */
    {
        uint16_t attr_type = htons(0x0020);
        uint16_t attr_len  = htons(8);
        memcpy(p, &attr_type, 2); p += 2;
        memcpy(p, &attr_len, 2);  p += 2;
        p[0] = 0x00; /* reserved */
        p[1] = 0x01; /* IPv4 */
        uint16_t port_xor = htons((uint16_t)(src_port ^ 0x2112));
        memcpy(p + 2, &port_xor, 2);
        p[4] = src4[0] ^ 0x21;
        p[5] = src4[1] ^ 0x12;
        p[6] = src4[2] ^ 0xA4;
        p[7] = src4[3] ^ 0x42;
        p += 8;
    }

    /* ICE-CONTROLLED (attr 0x8029, value=8B tiebreaker=0)
     * RFC 8445 §6.1.1：ice-lite 收到带 ICE-CONTROLLING 的请求时，
     * 响应必须带 ICE-CONTROLLED。tiebreaker ice-lite 不参与 role conflict，填 0 即可。 */
    if (add_ice_controlled) {
        uint16_t ic_type = htons(0x8029);
        uint16_t ic_vlen = htons(8);
        memcpy(p, &ic_type, 2); p += 2;
        memcpy(p, &ic_vlen, 2); p += 2;
        uint8_t tiebreaker[8] = {0};  /* ice-lite 不参与 role conflict */
        memcpy(p, tiebreaker, 8); p += 8;
    }

    /* MESSAGE-INTEGRITY (attr 0x0008, value 20B HMAC-SHA1)
     * RFC 5389 §15.4 原文：
     *   "HMAC covers the entire STUN message up to (but not including)
     *    the MESSAGE-INTEGRITY attribute."
     * MI attribute 是完整 TLV(type+length+value=24B)，整个都不包含在 HMAC 内！
     *
     *   Length 字段  = up to MI value 末尾（xor + ic + 4 + 20 = 48）
     *   HMAC 输入   = header(20) + XOR(12) + ICE-CONTROLLED(12) = 44
     *                 整个 MI attribute（type+length+value）都不包含
     *   body_len    = Length + FP_attr_len (4+4=8) = 56
     *   FINGERPRINT CRC 覆盖 up to but NOT including FP attr
     *
     * Bug 历史：
     *   v1: mi_header_len=28 + hmac_in=48 → 全错
     *   v2: mi_end_len=48 + hmac_in=68 → RFC 8489 draft 口径，Chrome 不认
     *   v3: mi_end_len=48 + hmac_in=44 → ✅ RFC 5389 §15.4 原文，DTLS 正常建立
     *   v4(回滚): mi_end_len=48 + hmac_in=48 → ❌ 错误地加了 MI attr header 4B，
     *             导致 Chrome 不认 Binding Success，DTLS 永远不建立 */
    {
        uint16_t mi_type = htons(0x0008);
        uint16_t mi_vlen = htons(20);
        memcpy(p, &mi_type, 2);       /* MI attr type=0x0008 */
        memcpy(p + 2, &mi_vlen, 2);   /* MI attr length=20   */
        memset(p + 4, 0, 20);         /* MI value 先填 0，HMAC 覆盖 */

        /* 临时 Length：含 MI 值（+20），不含 FP
         * = XOR(12) + IC(12) + MI_attr(4+20=24) = 48 */
        size_t mi_end_len = xor_len + ic_len + 24;   /* 12 + 12 + 24 = 48 */
        out[2] = (uint8_t)(mi_end_len >> 8);
        out[3] = (uint8_t)mi_end_len;

        /* HMAC 输入（RFC 5389 §15.4 原文）：
         * "up to (but not including) the MESSAGE-INTEGRITY attribute"
         * MI attribute 整体不包含在 HMAC 内。
         * p 当前指向 MI type（刚写好 type+length，还没往后挪），
         * 所以 hmac_in = p - out = header(20) + XOR(12) + IC(12) = 44 */
        size_t hmac_in = (size_t)(p - out);           /* 44 ✅ RFC 5389 原文 */

#ifdef NSBC_WITH_OPENSSL
        unsigned int mac_len = 0;
        if (!HMAC(EVP_sha1(), pwd, (int)strlen(pwd), out, hmac_in,
                  p + 4, &mac_len) || mac_len != 20)
            return NSBC_ERR;
#endif
        llog(LOG_DEBUG, "dtls", "STUN HMAC: len_field=%zu hmac_in=%zu",
             mi_end_len, hmac_in);
    }
    p += 4 + 20;

    /* 头部长度恢复最终值（含 FP）后算 FINGERPRINT (attr 0x8028) */
    out[2] = (uint8_t)(body_len >> 8); out[3] = (uint8_t)body_len;
    {
        uint16_t fp_type = htons(0x8028);
        uint16_t fp_vlen = htons(4);
        memcpy(p, &fp_type, 2); p += 2;
        memcpy(p, &fp_vlen, 2);  p += 2;
        /* CRC 覆盖 up to but NOT including FINGERPRINT attribute（RFC 8489 §6.4）。 */
        uint32_t crc = stun_crc32_poly(out, (size_t)(p - 4 - out), NULL, 0);
        uint32_t fp = htonl(crc ^ 0x5354554eu); /* "STUN" */
        memcpy(p, &fp, 4); p += 4;
    }
    return (int)(p - out);
}

/* STUN CRC32（IEEE 802.3）
 * ★ P1-3.8 fix: 多 reactor 线程并发首次调用时 if(!inited) 非原子 → 数据竞争。
 * 改用 pthread_once — C 标准一次性初始化，线程安全。 */
static uint32_t g_stun_crc32_tbl[256];
static pthread_once_t g_stun_crc32_once = PTHREAD_ONCE_INIT;
static void g_stun_crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_stun_crc32_tbl[i] = c;
    }
}

static uint32_t stun_crc32_poly(const uint8_t *data, size_t len,
                                const uint8_t *extra, size_t extra_len)
{
    pthread_once(&g_stun_crc32_once, g_stun_crc32_init);

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = g_stun_crc32_tbl[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    for (size_t i = 0; i < extra_len; i++) crc = g_stun_crc32_tbl[(crc ^ extra[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ========================================================================
 * dtls_server —— OpenSSL 差异全走 nsbc_ssl
 * ======================================================================*/

struct dtls_server {
    int fd;
    dtls_role_t role;
    dtls_state_t state;
    dtls_cfg_t cfg;
    /* 对端 ICE 凭证 */
    char remote_ufrag[24];
    char remote_pwd[26];
    /* SRTP master key（DTLS exporter 导出；take_srtp 消费后置空） */
    nsbc_srtp_sess_t *srtp;
    /* OpenSSL DTLS 不透明句柄（nsbc_ssl 持有内部 SSL_CTX/SSL/BIO） */
    nsbc_dtls_hdl_t ssl_hdl;
};

int dtls_server_create(int fd, dtls_role_t role, const dtls_cfg_t *cfg,
                      dtls_server_t **out)
{
    (void)role;
    if (!out) return NSBC_INVAL;
    llog(LOG_INFO, "dtls", "dtls_server_create: ENTER fd=%d", fd);

    dtls_server_t *d = calloc(1, sizeof(*d));
    if (!d) return NSBC_NOMEM;
    d->fd = fd;
    d->role = role;
    d->state = DTLS_ST_WAIT_CLIENT_HELLO;
    if (cfg) d->cfg = *cfg;

    llog(LOG_INFO, "dtls", "dtls_server_create: before nsbc_ssl_dtls_server_new fd=%d", fd);
    /* 生成 DTLS Server（自签证书 + SSL_CTX + BIO_dgram）——OpenSSL 版本
     * 差异全封装在 nsbc_ssl.c 内部。证书只生成一次，fp_sha256 直接输出。
     *
     * SRTP profiles：优先 AEAD_AES_128_GCM（RFC7714 现代浏览器唯一支持），
     * 同时加 CM_SHA1_80/32 fallback 供 OpenSSL 1.0.x（CentOS 7）协商到旧套件。
     * 注：Chrome/Firefox 新版本已不再支持 CM_SHA1，1.0.x 上 DTLS-SRTP
     * 实际会因 profile 不匹配握手失败——offer_hook 检测到后降级 BYPASS。 */
    /* ★★ 强制 ICM+HMAC 优先 — libsrtp2 2.5.0 Debian 包没编 GCM 支持，
     * srtp_create GCM rc=5 (bad_param)。先让业务跑通，后续自编译 libsrtp2
     * 加 --enable-openssl 开 GCM 后再把 GCM 放回第一位。 */
    static const char *profiles =
        "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32:SRTP_AEAD_AES_128_GCM";
    if (nsbc_ssl_dtls_server_new(fd, &d->ssl_hdl, profiles,
                                  d->cfg.fp_sha256,
                                  sizeof(d->cfg.fp_sha256)) != NSBC_OK) {
        llog(LOG_ERR, "dtls", "DTLS server create FAIL fd=%d", fd);
        free(d);
        return NSBC_ERR;
    }
    llog(LOG_INFO, "dtls", "dtls_server_create: ssl_new OK fp=%s", d->cfg.fp_sha256);

    /* 生成 ICE 凭证（无 cfg 时） */
    if (d->cfg.ice_ufrag[0] == '\0') {
        snprintf(d->cfg.ice_ufrag, sizeof(d->cfg.ice_ufrag),
                 "nsbc%08x", (unsigned)rand());
    }
    if (d->cfg.ice_pwd[0] == '\0') {
        /* ICE-Lite 场景：用系统 rand() 足够（非密码学安全的 ICE
         * 密码在 LAN 内可接受；Phase 2 NAT 穿透时换 OpenSSL RAND_bytes） */
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 22; i++)
            d->cfg.ice_pwd[i] = b64[rand() & 0x3F];
        d->cfg.ice_pwd[22] = '\0';
    }

    *out = d;
    return NSBC_OK;
}

void dtls_server_destroy(dtls_server_t *d)
{
    if (!d) return;
    if (d->ssl_hdl) nsbc_ssl_dtls_destroy(d->ssl_hdl);
    if (d->srtp) nsbc_srtp_session_destroy(d->srtp);
    free(d);
}

int dtls_server_fd(const dtls_server_t *d)
{
    return d ? d->fd : -1;
}

dtls_state_t dtls_server_state(const dtls_server_t *d)
{
    return d ? d->state : DTLS_ST_FAILED;
}

const dtls_cfg_t *dtls_server_cfg(const dtls_server_t *d)
{
    return d ? &d->cfg : NULL;
}

const char *dtls_server_remote_ufrag(const dtls_server_t *d)
{
    return d ? d->remote_ufrag : NULL;
}
const char *dtls_server_remote_pwd(const dtls_server_t *d)
{
    return d ? d->remote_pwd : NULL;
}

nsbc_srtp_sess_t *dtls_server_take_srtp(dtls_server_t *d)
{
    if (!d) return NULL;
    nsbc_srtp_sess_t *s = d->srtp;
    d->srtp = NULL;
    return s;
}

int dtls_server_set_peer_fp(dtls_server_t *d, const char *fp)
{
    if (!d) return NSBC_INVAL;
    if (!fp || !fp[0]) {
        d->cfg.peer_fp_sha256[0] = '\0';
        __atomic_thread_fence(__ATOMIC_RELEASE); /* 信令线程写 → reactor 线程读 */
        llog(LOG_DEBUG, "dtls", "peer_fp_sha256 cleared fd=%d (relaxed mode)", d->fd);
        return NSBC_OK;
    }
    size_t n = strlen(fp);
    if (n >= sizeof(d->cfg.peer_fp_sha256))
        n = sizeof(d->cfg.peer_fp_sha256) - 1;
    memcpy(d->cfg.peer_fp_sha256, fp, n);
    d->cfg.peer_fp_sha256[n] = '\0';
    __atomic_thread_fence(__ATOMIC_RELEASE); /* 保证 reactor 线程看到完整 memcpy */
    llog(LOG_DEBUG, "dtls", "peer_fp_sha256 set fd=%d expected=%s",
         d->fd, d->cfg.peer_fp_sha256);
    return NSBC_OK;
}

/* 内部：握手完成后校验 peer cert fingerprint == SDP 声明的 peer_fp_sha256。
 * peer_fp 为空串 → 宽松模式直接 OK（兼容不规范的 WebRTC 实现）。
 * 校验失败 / peer cert 不可得 → FAILED，调用方不再调用 dtls_export_srtp。 */
static int dtls_verify_peer_fp(dtls_server_t *d)
{
    if (!d || !d->ssl_hdl) return NSBC_INVAL;

    /* reactor 线程读：与 set_peer_fp（信令线程写）配对 */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (!d->cfg.peer_fp_sha256[0]) {
        llog(LOG_DEBUG, "dtls", "peer_fp not set fd=%d — SKIP verification (relaxed mode)",
             d->fd);
        return NSBC_OK;
    }

    char actual[NSBC_SSL_FP_SHA256_SIZE];
    if (nsbc_ssl_dtls_peer_fp(d->ssl_hdl, actual, sizeof(actual)) != NSBC_OK) {
        llog(LOG_ERR, "dtls", "peer cert unavailable fd=%d — handshake aborted",
             d->fd);
        return NSBC_ERR;
    }
    if (strcasecmp(d->cfg.peer_fp_sha256, actual) != 0) {
        llog(LOG_ERR, "dtls", "FINGERPRINT MISMATCH fd=%d expected=%s actual=%s",
             d->fd, d->cfg.peer_fp_sha256, actual);
        return NSBC_ERR;
    }
    llog(LOG_DEBUG, "dtls", "peer fingerprint VERIFIED fd=%d fp=%s", d->fd, actual);
    return NSBC_OK;
}

/* RFC5764 exporter → nsbc_srtp_session_create（通过 nsbc_ssl）
 * cipher/keylen 必须与 nsbc_ssl_dtls_export_srtp 协商到的 profile 匹配——
 * 两者都动态选择，确保 AEAD_AES_128_GCM / CM_SHA1_80 各自正确。 */
static int dtls_export_srtp(dtls_server_t *d)
{
    nsbc_srtp_material_t sending = {0};   /* 我们发出的 SRTP */
    nsbc_srtp_material_t receiving = {0};  /* 我们收到的 SRTP */
    if (nsbc_ssl_dtls_export_srtp(d->ssl_hdl, &sending, &receiving) != NSBC_OK) {
        llog(LOG_ERR, "dtls", "SSL_export_keying_material FAIL fd=%d", d->fd);
        return NSBC_ERR;
    }

    /* 根据 exporter 决定的 keylen 反推协商到的 profile：
     *   28B → SRTP_AEAD_AES_128_GCM (0x0007)
     *   30B → SRTP_AES128_CM_SHA1_80 (0x0001) / CM_SHA1_32 (0x0002)
     *   44B → SRTP_AEAD_AES_256_GCM (0x0008)
     * nsbc_srtp_session_create 会根据 cipher 选正确的密钥派生方式。 */
    nsbc_srtp_cipher_t cipher;
    switch (sending.keylen) {
    case 28: cipher = SRTP_CIPHER_AEAD_AESGCM128; break;
    case 30: cipher = SRTP_CIPHER_AESCM128; break;
    case 44: cipher = SRTP_CIPHER_AEAD_AESGCM256; break;
    default:
        llog(LOG_ERR, "dtls", "unknown exporter keylen=%d, cannot pick cipher",
             sending.keylen);
        return NSBC_ERR;
    }
    nsbc_srtp_policy_t sp = {
        .cipher = cipher,
        .keylen = sending.keylen,
        .kdr = 0,
    };
    llog(LOG_DEBUG, "dtls", "nsbc_srtp_session_create cipher=%d keylen=%d", cipher, sp.keylen);
    /* RFC5764：DTLS Server 的 sending material 对应出站 SRTP（Server→Client），
     * receiving material 对应入站 SRTP（Client→Server）。libsrtp2 的
     * inbound=对端来（我们解密），outbound=我们发（我们加密）。所以：
     *   inbound  = receiving（Client 发来的 SRTP 用 Client 的 sending material 加密 →
     *             等于我们 exporter 的 receiving material）
     *   outbound = sending（我们发的 SRTP 用我们的 sending material 加密） */
    /* RFC5764：DTLS Server 的 exporter material 交错格式：
     *   bytes[0..15]   = client_write_key  (Client 加密用)
     *   bytes[16..31]  = server_write_key  (Server 加密用)
     *   bytes[32..43]  = client_write_salt
     *   bytes[44..55]  = server_write_salt
     *
     * nsbc_ssl_dtls_export_srtp 解析后：
     *   receiving = client_write_key || client_write_salt (我们收 → 解密 Chrome 发来的包)
     *   sending   = server_write_key || server_write_salt (我们发 → 加密给 Chrome 的包)
     *
     * libsrtp2 的 srtp_session_create(inbound, outbound)：
     *   inbound  = 对端来的 SRTP 解密 (我们用 client_write key 解 Chrome 的包)
     *   outbound = 我们发出的 SRTP 加密 (我们用 server_write key 加密给 Chrome)
     *
     * ★ 正确接线：inbound=receiving, outbound=sending */
    return nsbc_srtp_session_create(&sp, &receiving /*inbound*/,
                                &sending /*outbound*/, &d->srtp);
}

/* OpenSSL < 1.1.1 fallback：dtls_server_on_read 只调 nsbc_ssl_dtls_handshake。
 * 没有 BIO_CTRL_DGRAM_SET_DATA 时，**绝对不能自己 recvfrom**——
 * 任何外部 recvfrom 会取走 DTLS record，BIO_dgram 的 SSL_accept 就拿不到了。
 *
 * 所以此函数只做：循环调 nsbc_ssl_dtls_handshake（BIO_dgram 内部 recvfrom 读 DTLS）。
 * ICE STUN 在握手前由 BIO_dgram 取走后交给 SSL 层 → 被丢弃
 * （非 DTLS record SSL 层会忽略/丢弃）。ICE-Lite host-only 场景 WebRTC 仍能发起 DTLS。
 * 握手后 relay_cb 不再调此函数，转 relay_recv_one 统一 recvfrom + demux（STUN/SRTP 全处理）。 */
int dtls_server_on_read(dtls_server_t *d)
{
    if (!d || d->state == DTLS_ST_FAILED || d->state == DTLS_ST_ESTABLISHED)
        return NSBC_INVAL;

    d->state = DTLS_ST_HANDSHAKING;

    /* 循环调 SSL_accept —— 每次 SSL_accept 通过 BIO_dgram 从 fd 读 DTLS record。
     * 如果 fd 上还没有完整的 Client Hello，返回 WANT_READ → NSBC_AGAIN。
     * reactor 会在下次 EPOLLIN 时再调进来。 */
    int rc = nsbc_ssl_dtls_handshake(d->ssl_hdl);
    if (rc == NSBC_OK) {
        d->state = DTLS_ST_ESTABLISHED;
        llog(LOG_INFO, "dtls", "handshake OK fd=%d", d->fd);
        if (dtls_verify_peer_fp(d) != NSBC_OK) {
            d->state = DTLS_ST_FAILED;
            return NSBC_ERR;
        }
        if (dtls_export_srtp(d) != NSBC_OK) {
            d->state = DTLS_ST_FAILED;
            return NSBC_ERR;
        }
        return NSBC_OK;
    }
    if (rc == NSBC_AGAIN) {
        /* 继续等，reactor 会再回调 */
        return NSBC_OK;
    }
    llog(LOG_WARN, "dtls", "handshake FAIL fd=%d", d->fd);
    d->state = DTLS_ST_FAILED;
    return NSBC_ERR;
}

/* 外部把一个 dgram 包喂进 DTLS 状态机（1.1.1+ feed 模型）。relay_recv_one 统一 recvfrom 后
 * 按首字节判定：
 *   class == 1 (STUN) → 走 dtls_ice_handle_stun（不碰 DTLS 状态机）
 *   class == 2 (DTLS) → 走 dtls_server_feed（喂 + 推进握手）
 *   class == 3 (SRTP/RTP) → 返回 NSBC_AGAIN（给调用方处理 unprotect）
 * 返回：NSBC_OK 已处理；NSBC_AGAIN SRTP/RTP（等调用方处理）；NSBC_ERR 致命 */
int dtls_server_feed(dtls_server_t *d, const uint8_t *buf, size_t len)
{
    if (!d || !buf || len == 0) return NSBC_INVAL;
    if (d->state == DTLS_ST_FAILED) return NSBC_ERR;

    int cls = pkt_classify(buf, len);
    if (cls == 1) { /* STUN —— 即时回 Binding Response */
        /* 注意：这里没 src 地址信息——relay_recv_one 直接调 dtls_ice_handle_stun 时
         * 才知道 src。dtls_server_feed 只管 DTLS 包，STUN 由调用方直接调。 */
        return NSBC_OK;
    }
    if (cls == 3) { /* SRTP/RTP */
        if (d->state == DTLS_ST_ESTABLISHED)
            return NSBC_AGAIN; /* 让调用方走 srtp_unprotect → relay */
        return NSBC_OK; /* DTLS 未完成前的 SRTP 忽略（可能是乱序） */
    }
    if (cls != 2) return NSBC_OK; /* 未知类型忽略 */

    /* DTLS record —— 喂进 BIO + 推进握手 */
    if (nsbc_ssl_dtls_feed(d->ssl_hdl, buf, len) != NSBC_OK)
        return NSBC_ERR;

    int rc = nsbc_ssl_dtls_handshake(d->ssl_hdl);
    if (rc == NSBC_OK) {
        if (d->state != DTLS_ST_ESTABLISHED) {
            d->state = DTLS_ST_ESTABLISHED;
            llog(LOG_DEBUG, "dtls", "handshake OK fd=%d", d->fd);
            if (dtls_verify_peer_fp(d) != NSBC_OK) {
                d->state = DTLS_ST_FAILED;
            } else if (dtls_export_srtp(d) != NSBC_OK) {
                d->state = DTLS_ST_FAILED;
            }
        }
    } else if (rc == NSBC_AGAIN) {
        d->state = DTLS_ST_HANDSHAKING;
    } else {
        llog(LOG_WARN, "dtls", "handshake FAIL fd=%d", d->fd);
        d->state = DTLS_ST_FAILED;
    }
    return NSBC_OK;
}

int dtls_ice_handle_stun(dtls_server_t *d, const uint8_t *pkt, size_t pkt_len,
                         const nsbc_sa_t *src)
{
    if (!d || !pkt || pkt_len < 20) return NSBC_INVAL;
    uint16_t type = (uint16_t)((pkt[0] << 8) | pkt[1]);
    uint16_t meth = type & 0xFFF;
    if (meth != STUN_METH_BINDING) return NSBC_ERR; /* 只处理 Binding */

    /* 解析 USERNAME attribute（"ufrag:rag" — rag 是我们的 ufrag） */
    uint16_t body_len = (uint16_t)((pkt[2] << 8) | pkt[3]);
    const uint8_t *body = pkt + 20;
    const uint8_t *end  = body + body_len;
    const char *req_ufrag = NULL;
    const char *req_rag   = NULL;
    int has_controlling = 0;  /* 请求里是否带 ICE-CONTROLLING */
    for (const uint8_t *a = body; a + 4 <= end; ) {
        uint16_t atype = (uint16_t)((a[0] << 8) | a[1]);
        uint16_t alen  = (uint16_t)((a[2] << 8) | a[3]);
        const uint8_t *val = a + 4;
        if (val + alen > end) break;
        if (atype == 0x0006 && alen >= 2 && alen < 48) { /* USERNAME */
            static char ubuf[48];
            memcpy(ubuf, val, alen); ubuf[alen] = '\0';
            char *colon = strchr(ubuf, ':');
            if (colon) {
                /* RFC5245 §7.1.2：USERNAME="<收方 ufrag>:<发方 ufrag>"
                 * 第一段是我们（被 check 的一端）的 ufrag，第二段是浏览器（发起方）。 */
                *colon = '\0';
                req_ufrag = ubuf;   /* 我们（收方）的 ufrag */
                req_rag   = colon + 1; /* 浏览器（发方）的 ufrag */
            }
        }
        if (atype == 0x802A) has_controlling = 1;  /* ICE-CONTROLLING（诊断用） */
        a += 4 + alen;
        a += (4 - (alen % 4)) % 4; /* 4B align */
    }
    (void)has_controlling;  /* ice-lite 强制加 ICE-CONTROLLED，不依赖请求里有没有 */

    /* 验证第一段 == our ufrag（不是给我们的 STUN 忽略） */
    if (req_ufrag && d->cfg.ice_ufrag[0] &&
        strcmp(req_ufrag, d->cfg.ice_ufrag) != 0)
        return NSBC_OK;

    if (req_rag)
        strncpy(d->remote_ufrag, req_rag, sizeof(d->remote_ufrag) - 1);

    /* 构造 Binding Response */
    uint8_t resp[512];
    int n;
    uint8_t src4[4] = {0};
    uint16_t port = 0;
    if (src && src->ss.ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&src->ss;
        memcpy(src4, &sin->sin_addr, 4);
        port = ntohs(sin->sin_port);
    }
    /* ice-lite 强制加 ICE-CONTROLLED — RFC 8445 §6.1.1：ice-lite 永久扮演
     * controlled 角色，无论请求里有没有 ICE-CONTROLLING 属性，响应都必须带
     * ICE-CONTROLLED。之前只在 has_controlling=1 时才加，Chrome 某些场景下
     * Binding Request 不带 ICE-CONTROLLING → 响应缺 ICE-CONTROLLED → 64 字节
     * → Chrome 拒绝响应 → ICE 永远 checking 不 complete。 */
    n = stun_resp_binding(pkt + 8, src4, port, d->cfg.ice_pwd,
                          1 /* ICE-CONTROLLED 强制 */, resp, sizeof(resp));
    if (n <= 0) return NSBC_ERR;

    /* ===== STUN HEX DUMP — 请求入 + 响应出 ===== */
    {
        llog(LOG_INFO, "dtls", "STUN RX fd=%d len=%zu ufrag=%s pwd=%s",
             d->fd, pkt_len, d->cfg.ice_ufrag, d->cfg.ice_pwd ? d->cfg.ice_pwd : "(null)");
        for (int i = 0; i < (int)pkt_len; i += 16) {
            char hex[128] = {0}; int off = 0;
            for (int j = i; j < i + 16 && j < (int)pkt_len; ++j)
                off += snprintf(hex + off, sizeof(hex) - off, "%02x ", pkt[j]);
            llog(LOG_DEBUG, "dtls", "  RX %04x: %s", i, hex);
        }
        llog(LOG_DEBUG, "dtls", "STUN TX fd=%d len=%d (Binding Success + ICE-CONTROLLED)", d->fd, n);
        for (int i = 0; i < n; i += 16) {
            char hex[128] = {0}; int off = 0;
            for (int j = i; j < i + 16 && j < n; ++j)
                off += snprintf(hex + off, sizeof(hex) - off, "%02x ", resp[j]);
            llog(LOG_DEBUG, "dtls", "  TX %04x: %s", i, hex);
        }
    }

    if (!src) return NSBC_ERR;
    /* src->len 若为 sizeof(sockaddr_storage)=128 会导致 sendto 行为异常，
     * 按 ss_family 显式选正确长度 */
    socklen_t salen = src->len;
    if (src->ss.ss_family == AF_INET)  salen = sizeof(struct sockaddr_in);
    if (src->ss.ss_family == AF_INET6) salen = sizeof(struct sockaddr_in6);
    ssize_t sret = sendto(d->fd, resp, (size_t)n, 0,
                          (const struct sockaddr *)&src->ss, salen);
    if (sret != (ssize_t)n) {
        llog(LOG_WARN, "dtls", "STUN resp sendto ret=%zd errno=%d "
             "expected=%d salen=%d", sret, errno, n, (int)salen);
    }
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
