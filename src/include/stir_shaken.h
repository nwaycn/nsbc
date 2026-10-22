#ifndef NSBC_STIR_SHAKEN_H
#define NSBC_STIR_SHAKEN_H

/* STIR/SHAKEN 身份标识（P3：RFC8224 框架 / RFC8225 PASSporT 分片 /
 * RFC8226 证书指纹 / RFC8588 SIP 侧 Identity 头）
 *
 * 处理链（入中继）：
 *  Identity: <b64JWT>;info=<https://x5u>;alg=ES256;ppt="shaken"
 *  1) 取 Identity 头（原始头区扫描，直达枚举暂无该槽）；
 *  2) 拆 JWT 三段 base64url，header.x5u/alg/ppt，payload.attest/orig/
 *     dest/iat（PASSporT 分片）；
 *  3) 经证书回调取 x5u 证书链（HTTP 由 core 异步客户端做，本模块不
 *     直连网络），ECDSA P-256 验签 + RFC8226 TNAuth 时间窗校验；
 *  4) 结果给 P-Asserted-Identity 策略（verified→保留；失败→降级或拒）。
 * 无 NSBC_WITH_OPENSSL：验签 NOTSUP，解析仍可用（灰度观测先行）。
 * 分层：sip（2）。
 */
#include "nsbc_common.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SS_X5U_MAX      256
#define SS_TN_MAX       32
#define SS_JWT_MAX      4096
#define SS_B64_MAX      3072

typedef enum {
    SS_ATTEST_A = 0,   /* Full：主叫号与客户关系均已知 */
    SS_ATTEST_B,       /* Partial：客户已知，号未知（CB/中继） */
    SS_ATTEST_C,       /* Gateway：均未知（国际入口） */
    SS_ATTEST_UNKNOWN
} ss_attest_t;

typedef enum {
    SS_RES_OK = 0,
    SS_RES_NO_HEADER,
    SS_RES_MALFORMED,
    SS_RES_BAD_ALG,
    SS_RES_CERT_FETCH,
    SS_RES_SIGNATURE,
    SS_RES_EXPIRED,
    SS_RES_NOTSUP
} ss_result_t;

typedef struct {
    char        x5u[SS_X5U_MAX];
    char        alg[16];           /* 期望 ES256 */
    char        ppt[16];           /* 期望 shaken */
    ss_attest_t attest;
    char        orig_tn[SS_TN_MAX];
    char        dest_tn[SS_TN_MAX];
    int64_t     iat;               /* UNIX 秒 */
} ss_passport_t;

/* 从 INVITE 原始视图提取 Identity 头值（token 由调用方给原始区）。
 * 返回值为头值起始（含 <b64>;info=... 整段），未找到 NULL。 */
const char *ss_find_identity(sip_msg_t *msg);

/* 解析（不验签）：头值→passport 结构；jwt_b64_out 可选拷贝 <...> 内
 * b64url 串（cap≥1）。纯字符串处理，失败给 SS_RES_MALFORMED。 */
ss_result_t ss_parse(const char *identity_value, ss_passport_t *out,
                     char *jwt_b64_out, size_t cap);

/* 证书取回回调（core HTTP 客户端装配；证书 PEM 出参，粗骨架） */
typedef struct {
    int (*fetch_cert)(void *ud, const char *x5u,
                      char *pem_out, size_t cap);
    void *ud;
} ss_ops_t;

/* 完整验证（解析已做可直接传头值）：now 为 UNIX 秒，iat 窗口
 * ±slew_s（RFC8224 建议 120s）。无 OpenSSL 返回 SS_RES_NOTSUP。 */
ss_result_t ss_verify(const char *identity_value, const ss_ops_t *ops,
                      int64_t now_s, int slew_s);

const char *ss_attest_name(ss_attest_t a);
const char *ss_result_name(ss_result_t r);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_STIR_SHAKEN_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
