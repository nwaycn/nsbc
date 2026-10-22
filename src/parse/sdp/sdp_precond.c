/* RFC 3312/4032 前提条件 SDP 属性解析与状态判定。
 * BNF（RFC 3312 §4）：
 *   a=curr:<ptype> <status-type> <direction-tag>
 *   a=des :<ptype> <strength> <status-type> <direction-tag>
 *   a=conf:<ptype> <status-type> <direction-tag>
 * status-type ∈ e2e/local/remote；direction ∈ none/send/recv/sendrecv。
 * lazy 构建（同 codec）；mandatory des 必须被同 ptype、同 status-type 的
 * curr 方向达到/覆盖（RFC 3312 §5.2）。 */

#include "sip_common.h"
#include "sdp.h"

static int split_sp(sip_tok_t *out, int cap, const char *p,
                    const char *end)
{
    int n = 0;
    while (p < end) {
        p = sip_skip_lws_run(p, end);
        if (p >= end)
            break;
        if (n >= cap)
            return -1;
        const char *b = p;
        while (p < end && !sip_is_sp(*p))
            ++p;
        out[n].p = b;
        out[n].len = (size_t)(p - b);
        ++n;
    }
    return n;
}

static sdp_pc_kind_t kind_of(const sdp_attr_t *a)
{
    if (a->name.len == 4 && sip_ci_eq(a->name.p, "curr", 4))
        return SDP_PC_CURR;
    if (a->name.len == 4 && sip_ci_eq(a->name.p, "conf", 4))
        return SDP_PC_CONF;
    if (a->name.len == 3 && sip_ci_eq(a->name.p, "des", 3))
        return SDP_PC_DES;
    return 0;
}

nsbc_rc_t sdp_media_build_preconds(sdp_t *s, sdp_media_t *m)
{
    (void)s;
    if (m->pc_built)
        return NSBC_OK;

    int n = 0;
    for (int i = 0; i < m->nattrs; ++i) {
        const sdp_attr_t *a = &m->attrs[i];
        sdp_pc_kind_t k = kind_of(a);
        if (!k || a->val.p == NULL)
            continue;
        if (n >= 12)
            return NSBC_ERR;

        sip_tok_t f[4];
        int nf = split_sp(f, 4, a->val.p, a->val.p + a->val.len);
        sdp_precond_t *p = &m->pcs[n];
        memset(p, 0, sizeof(*p));
        p->kind = k;

        if (k == SDP_PC_DES) {
            if (nf != 4)
                return NSBC_ERR;
            p->ptype = f[0];
            p->strength = f[1];
            p->status_type = f[2];
            p->direction = f[3];
        } else { /* curr/conf：ptype + status-type + direction */
            if (nf != 3)
                return NSBC_ERR;
            p->ptype = f[0];
            p->status_type = f[1];
            p->direction = f[2];
        }
        ++n;
    }
    m->npcs = n;
    m->pc_built = 1;
    return NSBC_OK;
}

static const sdp_precond_t *find_curr(const sdp_media_t *m,
                                     const sdp_precond_t *want)
{
    for (int i = 0; i < m->npcs; ++i) {
        const sdp_precond_t *c = &m->pcs[i];
        if (c->kind == SDP_PC_CURR &&
            c->ptype.len == want->ptype.len &&
            sip_ci_eq(c->ptype.p, want->ptype.p, c->ptype.len) &&
            c->status_type.len == want->status_type.len &&
            sip_ci_eq(c->status_type.p, want->status_type.p,
                      c->status_type.len))
            return c;
    }
    return NULL;
}

/* 方向分类：0 none，1 send，2 recv，3 sendrecv，-1 未知 */
static int dir_class(const sip_tok_t *d)
{
    if (d->len == 4 && sip_ci_eq(d->p, "none", 4))
        return 0;
    if (d->len == 4 && sip_ci_eq(d->p, "send", 4))
        return 1;
    if (d->len == 4 && sip_ci_eq(d->p, "recv", 4))
        return 2;
    if (d->len == 8 && sip_ci_eq(d->p, "sendrecv", 8))
        return 3;
    return -1;
}

/* curr 方向是否覆盖 des 方向（RFC 3312 §5.2 的"等于或优于"） */
static int dir_covers(const sip_tok_t *cur, const sip_tok_t *want)
{
    int cc = dir_class(cur);
    int ww = dir_class(want);
    if (cc < 0 || ww < 0)
        return 0;
    if (ww == 0)
        return 1;
    if (ww == 1)
        return cc == 1 || cc == 3;
    if (ww == 2)
        return cc == 2 || cc == 3;
    return cc == 3;
}

int sdp_media_preconditions_active(const sdp_media_t *m)
{
    for (int i = 0; i < m->npcs; ++i) {
        const sdp_precond_t *p = &m->pcs[i];
        if (p->kind == SDP_PC_DES &&
            p->ptype.len == 3 && sip_ci_eq(p->ptype.p, "qos", 3) &&
            ((p->strength.len == 9 &&
              sip_ci_eq(p->strength.p, "mandatory", 9)) ||
             (p->strength.len == 8 &&
              sip_ci_eq(p->strength.p, "optional", 8))))
            return 1;
    }
    return 0;
}

int sdp_media_preconditions_met(const sdp_media_t *m)
{
    for (int i = 0; i < m->npcs; ++i) {
        const sdp_precond_t *p = &m->pcs[i];
        if (p->kind != SDP_PC_DES ||
            !(p->strength.len == 9 &&
              sip_ci_eq(p->strength.p, "mandatory", 9)))
            continue;
        const sdp_precond_t *c = find_curr(m, p);
        if (!c || !dir_covers(&c->direction, &p->direction))
            return 0;
    }
    return 1;
}

int sdp_media_preconditions_confirmed(const sdp_media_t *m)
{
    for (int i = 0; i < m->npcs; ++i) {
        const sdp_precond_t *p = &m->pcs[i];
        if (p->kind == SDP_PC_CONF && dir_class(&p->direction) != 0)
            return 1;
    }
    return 0;
}
