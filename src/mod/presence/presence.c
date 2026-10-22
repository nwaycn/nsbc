/* 状态呈现——粗骨架（REQ-PBX-PRES-001~004，P1）
 *
 * 装配：presence 模块 start 建 store，ops.notify 由 core 装配到
 * sip_event NOTIFY 原语。SUBSCRIBE 入口在 signaling（event 头路由：
 * presence/Message-Summary），登记 watcher；registrar 上下线与话务
 * 振铃/应答/挂机事件经 A4 回调 pres_set，变化扇出 NOTIFY。
 * 粗骨架：定槽数组，无 RCU；接线批换分片哈希+订阅对话指针回收。
 */
#include "presence.h"
#include "module.h"

typedef struct {
    char tenant[PRES_TENANT_MAX];
    char entity[PRES_ENTITY_MAX];
    pres_state_t state;
    int has_state;
    pres_watcher_t watchers[PRES_WATCHER_MAX];
    int n_watchers;
} pres_rec_t;

struct pres_store {
    pres_ops_t ops;
    pres_rec_t recs[PRES_CAP];
    int n;
};

int pres_create(const pres_ops_t *ops, pres_store_t **out)
{
    pres_store_t *s;
    if (out == NULL)
        return NSBC_INVAL;
    s = (pres_store_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NSBC_NOMEM;
    if (ops != NULL)
        s->ops = *ops;
    *out = s;
    return NSBC_OK;
}

void pres_destroy(pres_store_t *s)
{
    free(s);
}

static pres_rec_t *rec_find(pres_store_t *s, const char *tenant,
                            const char *entity)
{
    int i;
    for (i = 0; i < s->n; ++i) {
        if (strcmp(s->recs[i].tenant, tenant) == 0 &&
            strcmp(s->recs[i].entity, entity) == 0)
            return &s->recs[i];
    }
    return NULL;
}

static pres_rec_t *rec_get(pres_store_t *s, const char *tenant,
                           const char *entity)
{
    pres_rec_t *r = rec_find(s, tenant, entity);
    if (r != NULL)
        return r;
    if (s->n >= PRES_CAP)
        return NULL;
    r = &s->recs[s->n++];
    snprintf(r->tenant, sizeof(r->tenant), "%s", tenant);
    snprintf(r->entity, sizeof(r->entity), "%s", entity);
    return r;
}

static void fanout(pres_store_t *s, pres_rec_t *r)
{
    char body[512];
    int n;
    int i;
    if (s->ops.notify == NULL || !r->has_state)
        return;
    n = pres_render_pidf(r->tenant, r->entity, r->state,
                         body, sizeof(body));
    if (n <= 0)
        return;
    for (i = 0; i < r->n_watchers; ++i) {
        (void)s->ops.notify(s->ops.ud, r->watchers[i].dlg, "presence",
                            "application/pidf+xml", body);
    }
}

int pres_set(pres_store_t *s, const char *tenant, const char *entity,
             pres_state_t st)
{
    pres_rec_t *r;
    int changed;
    if (s == NULL || tenant == NULL || entity == NULL)
        return NSBC_INVAL;
    r = rec_get(s, tenant, entity);
    if (r == NULL)
        return NSBC_NOSPC;
    changed = !r->has_state || r->state.basic != st.basic ||
              r->state.note != st.note;
    r->state = st;
    r->has_state = 1;
    if (changed)
        fanout(s, r);
    return changed ? 1 : 0;
}

int pres_get(pres_store_t *s, const char *tenant, const char *entity,
             pres_state_t *out)
{
    const pres_rec_t *r;
    if (s == NULL || tenant == NULL || entity == NULL || out == NULL)
        return NSBC_INVAL;
    r = rec_find(s, tenant, entity);
    if (r == NULL || !r->has_state)
        return NSBC_NOENT;
    *out = r->state;
    return NSBC_OK;
}

int pres_watch(pres_store_t *s, const char *tenant, const char *entity,
               const char *aor, void *dlg, int expires_s)
{
    pres_rec_t *r;
    pres_watcher_t *w;
    int i;
    if (s == NULL || tenant == NULL || entity == NULL || dlg == NULL ||
        expires_s <= 0)
        return NSBC_INVAL;
    r = rec_get(s, tenant, entity);
    if (r == NULL)
        return NSBC_NOSPC;
    for (i = 0; i < r->n_watchers; ++i) {
        if (r->watchers[i].dlg == dlg) {
            r->watchers[i].expires_s = expires_s;
            if (aor != NULL)
                snprintf(r->watchers[i].aor,
                         sizeof(r->watchers[i].aor), "%s", aor);
            return NSBC_OK;
        }
    }
    if (r->n_watchers >= PRES_WATCHER_MAX)
        return NSBC_NOSPC;
    w = &r->watchers[r->n_watchers++];
    memset(w, 0, sizeof(*w));
    if (aor != NULL)
        snprintf(w->aor, sizeof(w->aor), "%s", aor);
    w->dlg = dlg;
    w->expires_s = expires_s;
    /* 新订阅立即推一次当前状态（RFC6665 授权后立即 NOTIFY） */
    fanout(s, r);
    return NSBC_OK;
}

int pres_unwatch(pres_store_t *s, void *dlg)
{
    int ri, wi;
    if (s == NULL || dlg == NULL)
        return NSBC_INVAL;
    for (ri = 0; ri < s->n; ++ri) {
        pres_rec_t *r = &s->recs[ri];
        for (wi = 0; wi < r->n_watchers; ++wi) {
            if (r->watchers[wi].dlg == dlg) {
                r->watchers[wi] = r->watchers[--r->n_watchers];
                return NSBC_OK;
            }
        }
    }
    return NSBC_NOENT;
}

int pres_watcher_count(pres_store_t *s, const char *tenant,
                       const char *entity)
{
    pres_rec_t *r;
    if (s == NULL || tenant == NULL || entity == NULL)
        return 0;
    r = rec_find(s, tenant, entity);
    return r == NULL ? 0 : r->n_watchers;
}

static const char *note_word(pres_note_t note)
{
    switch (note) {
    case PRES_NOTE_IDLE:           return "available";
    case PRES_NOTE_RINGING:        return "ringing";
    case PRES_NOTE_ON_THE_PHONE:   return "on the phone";
    case PRES_NOTE_DND:            return "do not disturb";
    case PRES_NOTE_AWAY:           return "away";
    case PRES_NOTE_UNKNOWN:
    default:                       return "unknown";
    }
}

int pres_render_pidf(const char *tenant, const char *entity,
                     pres_state_t st, char *out, size_t cap)
{
    const char *basic = st.basic == PRES_BASIC_OPEN ? "open" : "closed";
    int n;
    if (tenant == NULL || entity == NULL || out == NULL || cap == 0)
        return NSBC_INVAL;
    /* TODO(P2)：多 tuple（多设备）+ entity 转 pres URI 转义；
     * RPID note 用私有命名空间，粗骨架直接放 <note>。 */
    n = snprintf(out, cap,
        "<?xml version=\"1.0\"?>"
        "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" "
        "entity=\"sip:%s@%s\">"
        "<tuple id=\"t1\"><status><basic>%s</basic></status>"
        "<note>%s</note></tuple></presence>",
        entity, tenant, basic, note_word(st.note));
    if (n < 0 || (size_t)n >= cap)
        return NSBC_NOSPC;
    return n;
}

int pres_render_mwi(int n_new, int n_total, char *out, size_t cap)
{
    int n;
    if (out == NULL || cap == 0 || n_new < 0 || n_total < 0)
        return NSBC_INVAL;
    n = snprintf(out, cap,
        "Messages-Waiting: %s\r\n"
        "Voice-Message: %d/%d (0/0)\r\n",
        n_new > 0 ? "yes" : "no", n_new, n_total);
    if (n < 0 || (size_t)n >= cap)
        return NSBC_NOSPC;
    return n;
}

static int presence_init(struct nsbc_ctx *ctx)
{
    (void)ctx;
    /* TODO：建 pres_store 挂运行时槽；ops.notify 待 sip_event 就绪后
     * 由 signaling 装配；模块本身保持 P1 占位不强制 REQUIRED。 */
    return NSBC_OK;
}

NSBC_MOD0_CAT(presence, 1, MOD_L_FINAL, 0, MOD_CAT_UTILITY, presence_init, NULL);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
