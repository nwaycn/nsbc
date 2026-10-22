#ifndef NSBC_AUTHZ_H
#define NSBC_AUTHZ_H

/* 出局鉴权/PIN 权限（REQ-PBX-DP-004，P0）
 *
 * 两层：
 *  1) CoS 等级门：分机 cos（extension.h）与 dialplan 规则 perm
 *     的包含判定（ext_has_perm），不足直接拒（403）；
 *  2) PIN 二次授权：高权规则（国际/声讯）强制 PIN，PIN 经 DTMF 收号
 *     （经 DigitMap/IVR 通道），只存哈希（与 Digest 同族 H(A1) 口径
 *     的 HEX 或 argon 风格，统一存 32B HEX 占位，验证常量时间）。
 * 紧急呼叫豁免 PIN/余额（REQ-PBX-DP-006，见 emergency.h）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTHZ_PIN_MAX     16   /* 4~16 位数字 */
#define AUTHZ_PIN_HASH_LEN 32  /* 与 EP_HA1_LEN 对齐（HEX） */

typedef struct authz_mgr authz_mgr_t;

int  authz_create(authz_mgr_t **out);
void authz_destroy(authz_mgr_t *a);

/* 管理面设置 PIN 哈希（不入明文；哈希由 Web/CLI hash 工具产出） */
int  authz_set_pin_hash(authz_mgr_t *a, const char *tenant,
                        const char *number, const char *hash_hex);
int  authz_remove(authz_mgr_t *a, const char *tenant, const char *number);

/* 呼叫面：PIN 校验（数字串 0-9，长度≤16）；常量时间比对，
 * 不存在用户与 PIN 错统一返回失败（防枚举，同 Web 登录口径）。 */
int  authz_verify_pin(authz_mgr_t *a, const char *tenant,
                      const char *number, const char *pin);

/* 规则是否需要 PIN：perm 命中 require_mask 的任一位置位即需要 */
int  authz_rule_needs_pin(uint32_t rule_perm, uint32_t require_mask);

/* 便捷：CoS 不足 →0；CoS 够且无需 PIN →1；CoS 够但需 PIN →2 */
enum {
    AUTHZ_DENY = 0,
    AUTHZ_ALLOW = 1,
    AUTHZ_NEED_PIN = 2
};
int  authz_decide(uint32_t cos, uint32_t rule_perm, uint32_t pin_mask);

/* PIN 输入规整：仅数字、长度 4..16；非法返回 INVAL */
int  authz_pin_format_ok(const char *pin);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_AUTHZ_H */
