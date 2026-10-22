/* 注册增强——粗骨架（REQ-REG-ENH，P1/P2）
 *
 * 安全备注（接线批必须处理，勿在评审中声称已安全）：
 *  - 当前 gruu_token 用 FNV-1a 占位，非密码学；生产必须换 HMAC（
 *    md5 头目前在 src/sip 私有目录，接线批将其提升为公共导出或经
 *    digest 模块挂回调），temp-gruu 用随机 128bit + 映射表而非派生；
 *  - 映射表/授权表为无锁定槽，仅单线程口径；接线批分片哈希 + RCU。
 */
#include "reg_enhance.h"

#define GRUU_OPAQUE_MAX 24  /* base64url 截断长度 */

typedef struct {
    int  used;
    char aor[REGENH_AOR_MAX];
    char opaque[GRUU_OPAQUE_MAX + 1];
    gruu_kind_t kind;
    uint64_t salt_epoch_ms;
} gruu_rec_t;

typedef struct {
    int  used;
    char tenant[32];
    char subject[REGENH_USER_MAX]; /* 被授权主体：from 账号/网关账号 */
    char prefix[REGENH_PREFIX_MAX];
} grant_rec_t;

struct reg_enh {
    gruu_cfg_t cfg;
    gruu_rec_t gruu[REGENH_GRANT_CAP];
    int n_gruu;
    grant_rec_t grants[REGENH_GRANT_CAP];
    int n_grants;
    reg_sync_ops_t sync;
};

/* FNV-1a 64（占位；见文件头安全备注） */
static uint64_t fnv1a(const char *s, uint64_t salt)
{
    uint64_t h = 1469598103934665603ULL ^ salt;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void b64url(uint64_t v, char *out, size_t cap)
{
    static const char a[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t n = 0;
    while (v != 0 && n + 1 < cap) {
        out[n++] = a[v & 63u];
        v >>= 6;
    }
    if (n == 0 && cap > 1)
        out[n++] = a[0];
    out[n] = '\0';
}

int reg_enh_create(const gruu_cfg_t *cfg, reg_enh_t **out)
{
    reg_enh_t *e;
    if (out == NULL)
        return NSBC_INVAL;
    e = (reg_enh_t *)calloc(1, sizeof(*e));
    if (e == NULL)
        return NSBC_NOMEM;
    if (cfg != NULL)
        e->cfg = *cfg;
    *out = e;
    return NSBC_OK;
}

void reg_enh_destroy(reg_enh_t *e)
{
    free(e);
}

int gruu_issue(reg_enh_t *e, const char *aor, gruu_kind_t kind,
               char *out, size_t cap)
{
    gruu_rec_t *r;
    uint64_t h;
    size_t need;
    if (e == NULL || aor == NULL || out == NULL || cap < 8)
        return NSBC_INVAL;
    if (kind != GRUU_PUBLIC && kind != GRUU_TEMP)
        return NSBC_INVAL;
    /* 已签发过同类 GRUU 则幂等返回（RFC5627 刷新注册不变 token） */
    for (int i = 0; i < e->n_gruu; ++i) {
        if (e->gruu[i].used && e->gruu[i].kind == kind &&
            strcmp(e->gruu[i].aor, aor) == 0 &&
            (kind == GRUU_PUBLIC ||
             e->gruu[i].salt_epoch_ms == e->cfg.salt_epoch_ms)) {
            need = strlen(e->gruu[i].opaque);
            if (need >= cap)
                return NSBC_NOSPC;
            memcpy(out, e->gruu[i].opaque, need + 1);
            return NSBC_OK;
        }
    }
    if (e->n_gruu >= REGENH_GRANT_CAP)
        return NSBC_NOSPC;
    r = &e->gruu[e->n_gruu++];
    r->used = 1;
    snprintf(r->aor, sizeof(r->aor), "%s", aor);
    r->kind = kind;
    r->salt_epoch_ms = kind == GRUU_TEMP ? e->cfg.salt_epoch_ms : 0;
    /* TODO：temp 改加密随机数；公 GRUU 改 HMAC(AOR) 定长 */
    h = fnv1a(aor, kind == GRUU_TEMP ? e->cfg.salt_epoch_ms
                                     : (uint64_t)(uintptr_t)r);
    b64url(h, r->opaque, sizeof(r->opaque));
    need = strlen(r->opaque);
    if (need >= cap)
        return NSBC_NOSPC;
    memcpy(out, r->opaque, need + 1);
    return NSBC_OK;
}

int gruu_resolve(reg_enh_t *e, const char *opaque,
                 char *aor_out, size_t cap, gruu_kind_t *kind_out)
{
    int i;
    if (e == NULL || opaque == NULL || aor_out == NULL || cap == 0)
        return NSBC_INVAL;
    for (i = 0; i < e->n_gruu; ++i) {
        const gruu_rec_t *r = &e->gruu[i];
        if (r->used && strcmp(r->opaque, opaque) == 0) {
            if (strlen(r->aor) >= cap)
                return NSBC_NOSPC;
            snprintf(aor_out, cap, "%s", r->aor);
            if (kind_out != NULL)
                *kind_out = r->kind;
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

/* 在参数区（形如 ;q=0.8;expires=600，token 视图）定位 q 值小数 */
int reg_parse_q(const sip_tok_t *tp, int *q_milli)
{
    const char *p;
    const char *end;
    int whole = 0, frac = 0, ndig = 0;
    if (tp == NULL || q_milli == NULL)
        return NSBC_INVAL;
    p = tp->p;
    end = p + tp->len;
    while (p + 2 < end) {
        if ((p[0] == ';' || p[0] == ' ' || p[0] == '\t') &&
            (p[1] == 'q' || p[1] == 'Q') && p[2] == '=') {
            p += 3;
            break;
        }
        ++p;
    }
    if (p + 2 >= end) {
        *q_milli = 1000; /* 缺省 1.000 */
        return NSBC_OK;
    }
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    if (p >= end || *p < '0' || *p > '9')
        return NSBC_INVAL;
    while (p < end && *p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        ++p;
    }
    if (p < end && *p == '.') {
        ++p;
        while (p < end && *p >= '0' && *p <= '9' && ndig < 3) {
            frac = frac * 10 + (*p - '0');
            ++ndig;
            ++p;
        }
    }
    while (ndig < 3) {
        frac *= 10;
        ++ndig;
    }
    if (whole > 1 || (whole == 1 && frac > 0))
        return NSBC_INVAL; /* q ∈ [0,1] */
    *q_milli = whole * 1000 + frac;
    return NSBC_OK;
}

static int grant_add(reg_enh_t *e, const char *tenant, const char *subject,
                     const char *prefix)
{
    grant_rec_t *g;
    int i;
    if (e == NULL || tenant == NULL || subject == NULL || prefix == NULL)
        return NSBC_INVAL;
    for (i = 0; i < e->n_grants; ++i) {
        if (e->grants[i].used &&
            strcmp(e->grants[i].tenant, tenant) == 0 &&
            strcmp(e->grants[i].subject, subject) == 0 &&
            strcmp(e->grants[i].prefix, prefix) == 0)
            return NSBC_OK;
    }
    if (e->n_grants >= REGENH_GRANT_CAP)
        return NSBC_NOSPC;
    g = &e->grants[e->n_grants++];
    g->used = 1;
    snprintf(g->tenant, sizeof(g->tenant), "%s", tenant);
    snprintf(g->subject, sizeof(g->subject), "%s", subject);
    snprintf(g->prefix, sizeof(g->prefix), "%s", prefix);
    return NSBC_OK;
}

static int grant_match(reg_enh_t *e, const char *tenant,
                       const char *subject, const char *user)
{
    int i;
    for (i = 0; i < e->n_grants; ++i) {
        const grant_rec_t *g = &e->grants[i];
        size_t plen;
        if (!g->used || strcmp(g->tenant, tenant) != 0 ||
            strcmp(g->subject, subject) != 0)
            continue;
        plen = strlen(g->prefix);
        if (strncmp(g->prefix, user, plen) == 0 && strlen(user) >= plen)
            return 1;
    }
    return 0;
}

int reg3p_grant_add(reg_enh_t *e, const char *tenant,
                    const char *from_user, const char *to_prefix)
{
    return grant_add(e, tenant, from_user, to_prefix);
}

int reg3p_check(reg_enh_t *e, const char *tenant,
                const char *from_user, const char *to_user)
{
    if (e == NULL || tenant == NULL || from_user == NULL ||
        to_user == NULL)
        return NSBC_INVAL;
    return grant_match(e, tenant, from_user, to_user)
               ? NSBC_OK : NSBC_PERM_DENIED;
}

int reg_range_grant(reg_enh_t *e, const char *tenant,
                    const char *account, const char *prefix)
{
    return grant_add(e, tenant, account, prefix);
}

int reg_range_check(reg_enh_t *e, const char *tenant,
                    const char *account, const char *aor_user)
{
    if (e == NULL || tenant == NULL || account == NULL ||
        aor_user == NULL)
        return NSBC_INVAL;
    return grant_match(e, tenant, account, aor_user)
               ? NSBC_OK : NSBC_PERM_DENIED;
}

int reg_throttle_validate(int ip_cps, int aor_cps, int global_cps)
{
    /* 粗口径：单 AoR 不可能超过单 IP 桶；单 IP×64（粗放大系数）不超
     * 全局桶，避免三级桶语义倒挂（接线批把 64 提为配置并加单测）。 */
    if (ip_cps <= 0 || aor_cps <= 0 || global_cps <= 0)
        return NSBC_INVAL;
    if (aor_cps > ip_cps || (long long)ip_cps * 64 > global_cps)
        return NSBC_INVAL;
    return NSBC_OK;
}

void reg_sync_set_ops(reg_enh_t *e, const reg_sync_ops_t *ops)
{
    if (e == NULL)
        return;
    if (ops == NULL) {
        memset(&e->sync, 0, sizeof(e->sync));
        return;
    }
    e->sync = *ops;
}

int reg_sync_publish(reg_enh_t *e, const char *aor, const char *contact,
                     int expires_s, int removed)
{
    int rc1 = NSBC_OK, rc2 = NSBC_OK;
    if (e == NULL || aor == NULL || contact == NULL)
        return NSBC_INVAL;
    if (e->sync.persist != NULL) {
        rc1 = e->sync.persist(e->sync.ud, aor, contact, expires_s, removed);
    }
    if (e->sync.cluster_sync != NULL) {
        rc2 = e->sync.cluster_sync(e->sync.ud, aor, contact, expires_s,
                                   removed);
    }
    return rc1 != NSBC_OK ? rc1 : rc2;
}
