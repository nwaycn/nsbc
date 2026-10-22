#ifndef NSBC_DTLS_SRTP_H
#define NSBC_DTLS_SRTP_H

/* DTLS-SRTP 媒体终结（REQ-DTLS-001，P0）
 *
 * 用途：WebRTC 浏览器与 NSBC 媒体桥互通。WebRTC 侧走 DTLS-SRTP 加密，
 * NSBC 在 relay fd 上用 OpenSSL DTLS 握手终结，导出 SRTP master key
 * 后喂 libsrtp2 做 protect/unprotect，MicrosIP 侧保持明文 RTP relay。
 *
 * 构建：NSBC_WITH_OPENSSL + NSBC_WITH_SRTP（DTLS 需 OpenSSL，protect/unprotect 需 libsrtp2）。
 *      缺任何一个 → 全接口 NOTSUP。
 *
 * reactor 集成：relay_cb 发现 rtp_relay_t 有 dtls_ctx → 收包后按首字节前缀
 * 分 STUN / DTLS / SRTP（未完成前不会有 SRTP）；DTLS 包喂 BIO，STUN 回 Binding Response。
 *
 * 状态机：IDLE → WAIT_CLIENT_HELLO → HANDSHAKING → ESTABLISHED
 */
#include "nsbc_common.h"
#include "nsbc_ssl.h"     /* NSBC_SSL_FP_SHA256_SIZE */
#include "srtp_session.h"
#include "net_sock.h"

/* OpenSSL BIO_CTRL_DGRAM_SET_DATA 可用性
 * openssl_compat.h 会在 1.1.1+ 上定义为 1；否则默认 0
 * （relay_recv_one 统一 demux 路径 + bridge_decide dtls_server 创建都依赖此宏） */
#ifndef NSBC_OPENSSL_HAS_DTLS_FEED
#  define NSBC_OPENSSL_HAS_DTLS_FEED 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtls_server dtls_server_t;

/* role：我们在 WebRTC 方向的 DTLS 角色——SBC 作为 DTLS Server 等浏览器 Client Hello。
 *        MICROSIP 侧是纯明文 RTP，不走 DTLS。 */
typedef enum {
    DTLS_ROLE_SERVER = 0,
} dtls_role_t;

typedef enum {
    DTLS_ST_IDLE = 0,
    DTLS_ST_WAIT_CLIENT_HELLO,
    DTLS_ST_HANDSHAKING,
    DTLS_ST_ESTABLISHED,
    DTLS_ST_FAILED,
} dtls_state_t;

typedef struct {
    /* DTLS 套件选择（RFC5764 §4.1）——浏览器默认 SRTP_AEAD_AES_128_GCM (0x0007)。
     * 我们 DTLS Server 端只支持这个套件即可（协商阶段双方必须一致）。 */
    unsigned srtp_profile_id;   /* 0x0007 = SRTP_AEAD_AES_128_GCM */
    /* ICE 凭证（ICE-Lite：host candidate only，server-reflexive/relay P1） */
    char ice_ufrag[24];
    char ice_pwd[26];
    /* SDP a=fingerprint: 自签证书的 SHA-256 指纹（小写 hex，冒号分隔，
     * RFC 4572 格式 95 字符 + NUL） */
    char fp_sha256[NSBC_SSL_FP_SHA256_SIZE];
    /* 期望的对端 fingerprint（SDP a=fingerprint 提取后注入；
     * 小写/大写 hex + 冒号，strcasecmp 比对）。空串不校验（降级宽松）。 */
    char peer_fp_sha256[NSBC_SSL_FP_SHA256_SIZE];
} dtls_cfg_t;

/* 在一个 UDP fd 上启动 DTLS Server（非阻塞）：
 * - 生成自签 ECDSA 证书（Prime256v1 曲线，现代浏览器兼容）
 * - SSL_CTX_new(DTLS_method()) + SSL_new + BIO_new_dgram(fd, BIO_NOCLOSE)
 * - BIO 配置非阻塞
 *
 * 返回的 dtls_server_t 持 fd（BIO_NOCLOSE 不拥有 fd，调用方继续管理）；
 * reactor 回调通过 dtls_on_read 推进状态机。 */
int dtls_server_create(int fd, dtls_role_t role, const dtls_cfg_t *cfg,
                      dtls_server_t **out);
void dtls_server_destroy(dtls_server_t *d);

/* reactor 事件入口——relay_recv_one 统一 recvfrom 后调：
 *   STUN 包 → dtls_ice_handle_stun（立即回 Binding Response）
 *   DTLS 包 → dtls_server_feed（喂 BIO + 推进握手）
 *   SRTP/RTP 包 → dtls_server_feed 返回 NSBC_AGAIN 让调用方处理 unprotect
 *
 * 这样避免 dtls_server 内部 recvfrom 与 relay_recv_one 竞争同 fd。 */
int dtls_server_feed(dtls_server_t *d, const uint8_t *buf, size_t len);

/* OpenSSL < 1.1.1 (BIO_CTRL_DGRAM_SET_DATA 不存在) 的 fallback 路径：
 * 握手前内部自己 recvfrom 循环（STUN + DTLS Client Hello），BIO_dgram 从 fd 读
 * DTLS record 推进握手。握手完成后不再调此函数——relay_recv_one 统一处理。
 *
 * relay_cb 里按 dtls 状态分流：
 *   未 ESTABLISHED → dtls_server_on_read（1.0.x 必须；1.1.1+ 也能跑，只是多一次 recvfrom）
 *   已 ESTABLISHED → relay_recv_one（统一 recvfrom + demux） */
int dtls_server_on_read(dtls_server_t *d);

/* 查询当前状态（reactor 回调、OAM 诊断用） */
dtls_state_t dtls_server_state(const dtls_server_t *d);

/* 取底层 fd（diagnostic / 日志用） */
int dtls_server_fd(const dtls_server_t *d);

/* 取创建时的配置（NSBC 侧 ice_ufrag/ice_pwd/fp_sha256）——
 * answer_hook 往 WebRTC 方向 answer 注入 ICE/DTLS 属性用 */
const dtls_cfg_t *dtls_server_cfg(const dtls_server_t *d);

/* 握手完成后取 SRTP 会话（SSL_export_keying_material → nsbc_srtp_session_create）。
 * ESTABLISHED 之前返回 NULL。调用方用完 destroy。 */
nsbc_srtp_sess_t *dtls_server_take_srtp(dtls_server_t *d);

/* 注入期望的对端 fingerprint（dtls_cfg_t 里是创建时的 cfg 副本，
 * 但 dtls[1] 创建时 B 的 answer SDP 还没到，必须 setter 延迟注入）。
 * fp 为 NULL 或空串 → 清空（降级宽松不校验）。 */
int dtls_server_set_peer_fp(dtls_server_t *d, const char *fp);

/* 取对端 ICE username/password（浏览器 STUN Binding Request 验证用） */
const char *dtls_server_remote_ufrag(const dtls_server_t *d);
const char *dtls_server_remote_pwd(const dtls_server_t *d);

/* ICE-Lite STUN Binding Response 构造：在 dgram fd 上回包。
 * reactor 回调里 dtls_server_on_read 自动调它——暴露 API 给 relay_cb demux。
 * 内部：按 STUN 消息头 method/class 判断 → 解析 USERNAME/MESSAGE-INTEGRITY/
 * FINGERPRINT → 回 Binding Success Response（含 XOR-MAPPED-ADDRESS + FINGERPRINT）。 */
int dtls_ice_handle_stun(dtls_server_t *d, const uint8_t *pkt, size_t pkt_len,
                         const nsbc_sa_t *src);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_DTLS_SRTP_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
