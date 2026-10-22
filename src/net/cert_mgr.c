/* 证书管理——粗骨架（REQ-SEC-002）
 *
 * OpenSSL 分支：接线批在 NSBC_WITH_OPENSSL 下实现 PEM_read/PEM 密钥
 * 匹配/X509_verify_cert/SHA256 指纹/CRL(X509_STORE_add_crl)/
 * stapling(SSL_CTX_set_ocsp_responder)；热加载走"建后换"双缓冲。
 * 当前无密码库分支全部 NOTSUP；set 管理（路径表/监听者）为纯逻辑。
 */
#include "cert_mgr.h"

typedef struct {
    cert_set_t set;
    int  loaded;
    int  days_left;     /* 装载时计算的剩余天数（-1=未装载） */
} cert_slot_t;

struct cert_mgr {
    cert_slot_t slots[CERT_SET_MAX];
    int n;
    cert_refresh_cb cb;
    void *cb_ud;
};

int cert_mgr_create(cert_mgr_t **out)
{
    cert_mgr_t *m;
    int i;
    if (out == NULL)
        return NSBC_INVAL;
    m = (cert_mgr_t *)calloc(1, sizeof(*m));
    if (m == NULL)
        return NSBC_NOMEM;
    for (i = 0; i < CERT_SET_MAX; ++i)
        m->slots[i].days_left = -1;
    *out = m;
    return NSBC_OK;
}

void cert_mgr_destroy(cert_mgr_t *m)
{
    free(m);
}

static int slot_idx(const cert_mgr_t *m, const char *name)
{
    int i;
    for (i = 0; i < m->n; ++i) {
        if (strcmp(m->slots[i].set.name, name) == 0)
            return i;
    }
    return -1;
}

int cert_set_add(cert_mgr_t *m, const cert_set_t *s)
{
    int i;
    if (m == NULL || s == NULL || s->name[0] == '\0')
        return NSBC_INVAL;
    i = slot_idx(m, s->name);
    if (i >= 0) {
        m->slots[i].set = *s;
        m->slots[i].loaded = 0;
        return NSBC_OK;
    }
    if (m->n >= CERT_SET_MAX)
        return NSBC_NOSPC;
    m->slots[m->n].set = *s;
    m->slots[m->n].days_left = -1;
    ++m->n;
    return NSBC_OK;
}

int cert_set_remove(cert_mgr_t *m, const char *name)
{
    int i = slot_idx(m, name);
    if (i < 0)
        return NSBC_NOENT;
    m->slots[i] = m->slots[--m->n];
    m->slots[m->n].days_left = -1;
    return NSBC_OK;
}

const cert_set_t *cert_set_get(cert_mgr_t *m, const char *name)
{
    int i;
    if (m == NULL || name == NULL)
        return NULL;
    i = slot_idx(m, name);
    return i < 0 ? NULL : &m->slots[i].set;
}

/* 注：OpenSSL 接线批实现 reload 置 loaded/days_left 时，本参数需去掉
 * const（与桩阶段其他输出型参数同一口径）。 */
int cert_load(const cert_mgr_t *m, const char *name)
{
    int i;
    if (m == NULL || name == NULL)
        return NSBC_INVAL;
    i = slot_idx(m, name);
    if (i < 0)
        return NSBC_NOENT;
#ifdef NSBC_WITH_OPENSSL
    /* TODO：PEM 读盘 → 密钥一致性 → 链校验 → notAfter 天数；
     * 成功置 loaded=1/days_left，失败返回 AUTH 并不替换现役 ctx。 */
    (void)i;
    return NSBC_NOTSUP;
#else
    (void)i;
    return NSBC_NOTSUP;
#endif
}

int cert_reload_all(cert_mgr_t *m)
{
    int i;
    int any_ok = 0;
    if (m == NULL)
        return NSBC_INVAL;
    for (i = 0; i < m->n; ++i) {
        int rc = cert_load(m, m->slots[i].set.name);
        if (rc == NSBC_OK) {
            any_ok = 1;
            if (m->cb != NULL)
                m->cb(m->cb_ud, m->slots[i].set.name);
        }
    }
    return any_ok ? NSBC_OK : NSBC_NOTSUP;
}

int cert_min_days_left(cert_mgr_t *m, int64_t now_days,
                       char *name_out, size_t cap)
{
    int best = -1;
    int i;
    if (m == NULL)
        return NSBC_INVAL;
    for (i = 0; i < m->n; ++i) {
        if (!m->slots[i].loaded || m->slots[i].days_left < 0)
            continue;
        if (best < 0 || m->slots[i].days_left < best) {
            best = m->slots[i].days_left;
            if (name_out != NULL && cap > 0)
                snprintf(name_out, cap, "%s", m->slots[i].set.name);
        }
    }
    (void)now_days; /* 接线批用 X509_notAfter 直接换算，不依赖入参基准 */
    return best;
}

void cert_on_refresh(cert_mgr_t *m, cert_refresh_cb cb, void *ud)
{
    if (m == NULL)
        return;
    m->cb = cb;
    m->cb_ud = ud;
}

int cert_fingerprint256(const cert_mgr_t *m, const char *name,
                        const char *out, size_t cap)
{
    if (m == NULL || name == NULL || out == NULL || cap < 96)
        return NSBC_INVAL;
    if (slot_idx(m, name) < 0)
        return NSBC_NOENT;
    /* TODO(NSBC_WITH_OPENSSL)：X509_digest(..., EVP_sha256(), ...)
     * 渲染 32B 冒号十六进制（95 字符 + NUL）。 */
    return NSBC_NOTSUP;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
