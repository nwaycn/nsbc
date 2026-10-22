/* IVR 执行器——粗骨架（REQ-PBX-IVR-001/002/004/005 P0；003/006 P1）
 *
 * 修正历史：applications.c 旧注释称"App 执行器已落地"不实——实际只有
 * 描述符占位。本文件提供真实执行器轮廓；applications 模块在接线批
 * 装配本文件与 callctrl/voicemail。
 * 状态机：进节点→播 prompt（ops.play）→收号计时→匹配 choice：
 *  - 立即匹配或收齐→执行 action；
 *  - 超时/未命中→retries--，耗尽走 timeout_action。
 */
#include "ivr.h"

struct ivr_sess {
    const ivr_profile_t *p;
    void               *call_key;
    ivr_ops_t           ops;
    const ivr_node_t   *cur;
    char                buf[IVR_DIGITS_MAX];
    int                 blen;
    int                 retries;
    int                 finished;
    uint64_t            deadline_ms;
};

static const ivr_node_t *node_find(const ivr_profile_t *p, const char *name)
{
    int i;
    for (i = 0; i < p->n_nodes; ++i) {
        if (strcmp(p->nodes[i].name, name) == 0)
            return &p->nodes[i];
    }
    return NULL;
}

static void enter_node(ivr_sess_t *s, const ivr_node_t *n)
{
    s->cur = n;
    s->buf[0] = '\0';
    s->blen = 0;
    if (s->ops.play != NULL && n->prompt[0] != '\0')
        (void)s->ops.play(s->ops.ud, s->call_key, n->prompt, 0);
}

int ivr_session_start(const ivr_profile_t *p, const char *entry,
                      void *call_key, const ivr_ops_t *ops,
                      ivr_sess_t **out)
{
    const ivr_node_t *n;
    ivr_sess_t *s;
    if (p == NULL || entry == NULL || call_key == NULL || ops == NULL ||
        out == NULL)
        return NSBC_INVAL;
    n = node_find(p, entry);
    if (n == NULL)
        return NSBC_NOENT;
    s = (ivr_sess_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NSBC_NOMEM;
    s->p = p;
    s->call_key = call_key;
    s->ops = *ops;
    *out = s;
    enter_node(s, n);
    return NSBC_OK;
}

void ivr_session_end(ivr_sess_t *s)
{
    free(s);
}

static const ivr_choice_t *match_choice(const ivr_node_t *n, const char *buf)
{
    int i;
    for (i = 0; i < n->nchoices; ++i) {
        const char *key = n->choices[i].key;
        if (strcmp(key, "t") == 0)
            continue; /* 超时键只在 ivr_on_timeout 使用 */
        if (strcmp(key, buf) == 0)
            return &n->choices[i];
    }
    return NULL;
}

int ivr_on_dtmf(ivr_sess_t *s, char digit, ivr_action_t *out_act)
{
    const ivr_choice_t *c;
    if (s == NULL || out_act == NULL || s->cur == NULL)
        return NSBC_INVAL;
    if (s->blen >= IVR_DIGITS_MAX - 1)
        return NSBC_NOSPC;
    s->buf[s->blen++] = digit;
    s->buf[s->blen] = '\0';
    c = match_choice(s->cur, s->buf);
    if (c == NULL) {
        /* 无任何前缀：无效键，重试（粗骨架按重试计数，不立即重播） */
        s->buf[0] = '\0';
        s->blen = 0;
        return 0;
    }
    /* 精确命中→执行 */
    s->finished = 1;
    *out_act = c->action;
    return 1;
    /* TODO(P1)：SUBMENU 不终态——进新节点返回 0（接线时按 act 分支
     * 处理，HTTP_QUERY 经 ops.http_get 解析目标后再决定）。 */
}

int ivr_on_timeout(ivr_sess_t *s, ivr_action_t *out_act)
{
    int i;
    if (s == NULL || out_act == NULL || s->cur == NULL)
        return NSBC_INVAL;
    /* 显式超时键 t 优先 */
    for (i = 0; i < s->cur->nchoices; ++i) {
        if (strcmp(s->cur->choices[i].key, "t") == 0) {
            *out_act = s->cur->choices[i].action;
            s->finished = 1;
            return 1;
        }
    }
    if (s->retries < s->cur->max_retries) {
        ++s->retries;
        enter_node(s, s->cur); /* 重播提示 */
        return 0;
    }
    *out_act = s->cur->timeout_action;
    s->finished = 1;
    return 1;
}

void ivr_on_prompt_end(ivr_sess_t *s)
{
    if (s == NULL || s->cur == NULL)
        return;
    /* TODO：arm 首位/位间超时（时间轮；初值取 current node 配置） */
}

const char *ivr_digits(const ivr_sess_t *s)
{
    return s == NULL ? NULL : s->buf;
}
