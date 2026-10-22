/* PIN 权限——粗骨架（REQ-PBX-DP-004，P0）
 *
 * 实现备注（接线批）：
 *  - 哈希方案最终选 HA1 同形 MD5(user:pin-realm:pin) 还是更强算法，
 *    与 Web hash-password 工具链统一（PIN 不进日志/CDR 明文）；
 *  - 呼叫面 PIN 通过 DTMF（dtmf 事件）在 IVR 收号器输入，不经过
 *    SIP URI/Contact（防代理记录）；
 *  - 连续 PIN 错误锁定复用 registrar 三级令牌桶口径（防暴力试码）。
 */
#include "authz.h"

#define AUTHZ_CAP 200000

typedef struct {
    char tenant[32];
    char number[32];
    char hash_hex[AUTHZ_PIN_HASH_LEN + 1];
    int  used;
} authz_rec_t;

struct authz_mgr {
    authz_rec_t *recs;
    size_t       cap;
};

int authz_create(authz_mgr_t **out)
{
    authz_mgr_t *a;
    if (out == NULL)
        return NSBC_INVAL;
    a = (authz_mgr_t *)calloc(1, sizeof(*a));
    if (a == NULL)
        return NSBC_NOMEM;
    /* TODO：slab/分片哈希（P0 接线时与 endpoints 同分配器）；
     * 粗骨架给一块堆数组占位，不实际索引。 */
    a->cap = AUTHZ_CAP;
    a->recs = (authz_rec_t *)calloc(a->cap, sizeof(authz_rec_t));
    if (a->recs == NULL) {
        free(a);
        return NSBC_NOMEM;
    }
    *out = a;
    return NSBC_OK;
}

void authz_destroy(authz_mgr_t *a)
{
    if (a == NULL)
        return;
    free(a->recs);
    free(a);
}

int authz_set_pin_hash(authz_mgr_t *a, const char *tenant, const char *number, const char *hash_hex)
{
    if (a == NULL || tenant == NULL || number == NULL || hash_hex == NULL)
        return NSBC_INVAL;
    if (strlen(hash_hex) != AUTHZ_PIN_HASH_LEN)
        return NSBC_INVAL;
    /* TODO：定位记录（分片哈希），无则取空槽写入 */
    return NSBC_NOTSUP;
}

int authz_remove(authz_mgr_t *a, const char *tenant, const char *number)
{
    if (a == NULL || tenant == NULL || number == NULL)
        return NSBC_INVAL;
    return NSBC_NOENT; /* TODO */
}

int authz_verify_pin(authz_mgr_t *a, const char *tenant, const char *number, const char *pin)
{
    if (a == NULL || tenant == NULL || number == NULL || pin == NULL)
        return NSBC_INVAL;
    if (!authz_pin_format_ok(pin))
        return NSBC_INVAL;
    /* TODO：取记录哈希，与 pin 同算法重算后 CRYPTO_memcmp/自写
     * 常量时间比较；无记录同样跑一遍假比较防时序枚举。 */
    return NSBC_NOTSUP;
}

int authz_rule_needs_pin(uint32_t rule_perm, uint32_t require_mask)
{
    return (rule_perm & require_mask) != 0;
}

int authz_decide(uint32_t cos, uint32_t rule_perm, uint32_t pin_mask)
{
    if ((cos & rule_perm) != rule_perm)
        return AUTHZ_DENY;
    if (authz_rule_needs_pin(rule_perm, pin_mask))
        return AUTHZ_NEED_PIN;
    return AUTHZ_ALLOW;
}

int authz_pin_format_ok(const char *pin)
{
    size_t n;
    size_t i;
    if (pin == NULL)
        return 0;
    n = strlen(pin);
    if (n < 4 || n > AUTHZ_PIN_MAX)
        return 0;
    for (i = 0; i < n; ++i) {
        if (pin[i] < '0' || pin[i] > '9')
            return 0;
    }
    return 1;
}
