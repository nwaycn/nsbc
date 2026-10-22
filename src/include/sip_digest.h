#ifndef NSBC_SIP_DIGEST_H
#define NSBC_SIP_DIGEST_H

#include "nsbc_common.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RFC 2617 Digest（SIP 剖面，RFC3261 §22）：
 * - 仅 algorithm=MD5（/MD5-sess 可在 P2 补），qop=auth（强制）；
 * - nonce 带过期时间与服务器密钥签名，过期重质询 stale=true；
 * - HA1 = H(username:realm:password) 由凭证库回调给出，密码不进事务层；
 * - 防重放：nonce 内单调序号由 registrar/oam 记录（P0 检查 nc 不回退
 *   的责任在调用方，本模块只做无状态校验与签名）。
 */

#define SIP_NONCE_LEN 64
#define SIP_RESP_LEN 33

/* HA1 解析回调：按 realm/username 取预计算 32-hex HA1；无此用户返回 NULL */
typedef const char *(*digest_ha1_fn)(const char *realm, size_t rlen,
                                     const char *user, size_t ulen,
                                     void *ud);

typedef struct {
    const char *realm;         /* digest_init strdup 私有副本；
                                  也可在初始化后临时重指向（profile realm override） */
    const char *server_secret; /* 私有副本；仅 digest_init 内 strdup/free */
    uint64_t nonce_ttl_ms;
    digest_ha1_fn ha1_cb;
    void *ha1_ud;
} digest_cfg_t;

/* 返回 NSBC_OK / NSBC_NOMEM（strdup 失败时）。
 * digest_cfg_t.realm/server_secret 由本函数 strdup 私有持有，
 * 调用方传入的指针可在返回后释放。
 * 也支持调用方在 digest_init 之后，显式赋回外部常量（如 profile realm），
 * 此时调用方负责该指针在 digest_verify 使用期间保持有效。 */
int digest_init(digest_cfg_t *d, const char *realm, const char *secret,
                uint64_t ttl_ms, digest_ha1_fn cb, void *ud);

/* 配套释放：digest_init 分配的 strdup 副本。
 * 若调用方在 init 之后用外部常量覆盖了 realm/secret（如 reg_on_register
 * 的 profile realm override），则不要让 digest_free 去 free 外部指针——
 * 建议在 digest_cfg_t 上加 owned 标志位，或改用 digest_set_realm() API。 */
void digest_free(digest_cfg_t *d);

/* 生成质询头值（不含 "WWW-Authenticate: " 前缀），proxy=1 发 407 时用同值 */
int digest_make_challenge(const digest_cfg_t *d, int stale, char *out,
                          size_t outsz);

/* 生成新 nonce */
int digest_make_nonce(const digest_cfg_t *d, uint64_t now_ms, char *out,
                      size_t outsz);
/* 校验 nonce 签名与时效：NSBC_OK / NSBC_AUTH(签名坏) / NSBC_EXPIRED */
int digest_check_nonce(const digest_cfg_t *d, const char *nonce, size_t nlen,
                       uint64_t now_ms);

typedef enum {
    DIG_AUTH_OK = 0,
    DIG_AUTH_USER_UNKNOWN,
    DIG_AUTH_BAD_NONCE,
    DIG_AUTH_STALE_NONCE,
    DIG_AUTH_MISMATCH,
    DIG_AUTH_BAD_FORMAT,
    DIG_AUTH_NOTSUP
} dig_auth_rc_t;

/* 校验入向请求的 Authorization/Proxy-Authorization 头（深解析后）。
 * m 仅用于取 method 与 R-URI；auth 为 sip_hdr_parse_auth 的结果。 */
dig_auth_rc_t digest_verify(const digest_cfg_t *d, const sip_msg_t *m,
                            const sip_auth_t *auth, uint64_t now_ms);

/* 出向：按密码直接计算 response（UAC/中继外呼注册用），qop=auth 形式 */
int digest_client_response(const char *user, const char *realm,
                           const char *pass, const char *method,
                           const char *uri, const char *nonce,
                           const char *nc /*8 hex*/, const char *cnonce,
                           char resp33[SIP_RESP_LEN]);

/* 预计算 HA1（凭证入库时调用一次，避免明文落库也避免每呼叫碰密码） */
void digest_make_ha1(const char *user, const char *realm, const char *pass,
                     char out33[SIP_RESP_LEN]);

/* ---- UAC 侧：解析 challenge + 构建 Authorization ---- */

/* 解析 WWW-Authenticate / Proxy-Authenticate 头值（不含前缀），抽取
 * realm / nonce / opaque / qop / algorithm / stale。输入示例：
 *   "Digest realm=\"ims.example.com\", nonce=\"abc123\", qop=\"auth\""
 * 返回 NSBC_OK / NSBC_INVAL（格式不对）/ NSBC_NOTSUP（非 Digest scheme）。
 * 只接受 algorithm=MD5；不支持的参数静默忽略。 */
typedef struct {
    char realm[96];
    char nonce[128];
    char opaque[128];
    char qop[16];           /* 服务器 qop 值（"auth" / "auth-int" 等） */
    char algorithm[16];     /* "MD5" / "MD5-sess" 等 */
    int  stale;             /* 服务器标 stale=true → 重新算 */
} digest_challenge_t;

int digest_parse_challenge(const char *hdr_value, digest_challenge_t *out);

/* 直接用预计算 HA1（32 hex）算 response —— 跳过明文密码步骤，UAC 侧
 * 最安全路径。参数语义同 digest_client_response。 */
int digest_client_response_ha1(const char *ha132,
                               const char *method, const char *uri,
                               const char *nonce,
                               const char *nc /* 8 hex */,
                               const char *cnonce,
                               const char *qop /* 可为 NULL 表示无 qop */,
                               char resp33[SIP_RESP_LEN]);

/* 拼完整 Authorization 头字符串（不含 "Authorization: " 前缀）。
 * 输出如："Digest username=\"alice\", realm=\"ims.example.com\",
 *   nonce=\"abc\", uri=\"sip:ims.example.com\", qop=auth, nc=00000001,
 *   cnonce=\"xyz\", response=\"...\", algorithm=MD5[, opaque=\"...\"]" */
int digest_client_build_auth(const char *username, const char *realm,
                             const char *nonce, const char *opaque /* 可 NULL */,
                             const char *qop /* 可 NULL */,
                             const char *nc, const char *cnonce,
                             const char *method, const char *uri,
                             const char *ha1_or_pass, int is_ha1,
                             char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
#endif
