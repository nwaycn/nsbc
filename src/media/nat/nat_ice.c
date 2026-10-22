/* ICE 代理——粗骨架（RFC 8445，REQ-NAT-001，P1；DTLS-SRTP 随 WebRTC）
 *
 * 接线批要点：
 *  - 候选优先级 priority = 2^26*typePref + 2^24*localPref +
 *    2^8*(256-componentID) + 256 - (addrPref&255)（§5.1.2.1）；
 *  - pair 优先级角色相关（§6.1.2.3），controlling 方主导提名；
 *  - 入向 Binding USERNAME="frag:rag"（RFC8445 §7.3），
 *    MESSAGE-INTEGRITY 用 remote pwd 作 HMAC key；
 *  - prflx 触发 frozen 提权与 USE-CANDIDATE 配合；
 *  - 候选/pair 槽位固定（ICE_*_MAX），无呼叫中 malloc。
 */
#include "nat_ice.h"

struct ice_agent {
    ice_cfg_t        cfg;
    ice_agent_state_t state;
    char             l_ufrag[ICE_UFRAG_MAX];
    char             l_pwd[ICE_PWD_MAX];
    char             r_ufrag[ICE_UFRAG_MAX];
    char             r_pwd[ICE_PWD_MAX];
    int              controlling;
    uint64_t         tiebreaker;
    int              n_local;
    int              n_remote;
    ice_cand_t       local[ICE_FOUND_MAX];
    ice_cand_t       remote[ICE_FOUND_MAX];
    ice_pair_t       pairs[ICE_PAIR_MAX];
    int              n_pairs;
    int              eoc; /* end-of-candidates */
};

int ice_agent_create(const ice_cfg_t *cfg, ice_agent_t **out)
{
    ice_agent_t *a;
    if (cfg == NULL || out == NULL)
        return NSBC_INVAL;
    a = (ice_agent_t *)calloc(1, sizeof(*a));
    if (a == NULL)
        return NSBC_NOMEM;
    a->cfg = *cfg;
    a->state = ICE_ST_NOT_RUNNING;
    a->controlling = cfg->controlling;
    *out = a;
    return NSBC_OK;
}

void ice_agent_destroy(ice_agent_t *a)
{
    if (a != NULL)
        free(a);
}

static int cred_copy(char *dst, size_t cap, const char *s)
{
    size_t n;
    if (s == NULL)
        return NSBC_INVAL;
    n = strlen(s);
    if (n == 0 || n >= cap)
        return NSBC_INVAL; /* RFC8445：ufrag≥24 位/pwd≥128 位推荐 */
    memcpy(dst, s, n + 1);
    return NSBC_OK;
}

int ice_set_local_cred(ice_agent_t *a, const char *ufrag, const char *pwd)
{
    int rc;
    if (a == NULL)
        return NSBC_INVAL;
    rc = cred_copy(a->l_ufrag, sizeof(a->l_ufrag), ufrag);
    if (rc != NSBC_OK)
        return rc;
    return cred_copy(a->l_pwd, sizeof(a->l_pwd), pwd);
}

int ice_set_remote_cred(ice_agent_t *a, const char *ufrag, const char *pwd)
{
    int rc;
    if (a == NULL)
        return NSBC_INVAL;
    rc = cred_copy(a->r_ufrag, sizeof(a->r_ufrag), ufrag);
    if (rc != NSBC_OK)
        return rc;
    return cred_copy(a->r_pwd, sizeof(a->r_pwd), pwd);
}

int ice_add_local_candidate(ice_agent_t *a, const ice_cand_t *c)
{
    if (a == NULL || c == NULL)
        return NSBC_INVAL;
    if (a->n_local >= ICE_FOUND_MAX)
        return NSBC_NOSPC;
    a->local[a->n_local++] = *c;
    return NSBC_OK;
}

int ice_add_remote_candidate(ice_agent_t *a, const ice_cand_t *c)
{
    if (a == NULL || c == NULL)
        return NSBC_INVAL;
    if (a->n_remote >= ICE_FOUND_MAX)
        return NSBC_NOSPC;
    a->remote[a->n_remote++] = *c;
    /* TODO(trickle)：CHECKING 中到达→增 pair、解冻并插入清单 */
    return NSBC_OK;
}

int ice_start_checks(ice_agent_t *a, uint64_t tiebreaker)
{
    if (a == NULL || a->l_pwd[0] == '\0' || a->r_pwd[0] == '\0')
        return NSBC_INVAL;
    a->tiebreaker = tiebreaker;
    /* TODO：笛卡尔积组 pair（同 component）→算优先级→排序→
     * frozen 解链（§6.1.2.5/6）→置首对 WAITING；本批不展开。 */
    a->state = ICE_ST_CHECKING;
    return NSBC_NOTSUP;
}

int ice_switch_role(ice_agent_t *a, uint64_t tiebreaker)
{
    if (a == NULL)
        return NSBC_INVAL;
    /* TODO：487 后按 §7.3.1.1 tiebreaker 比对翻转 controlling，
     * pair 优先级重算重排，检查从头来。 */
    a->tiebreaker = tiebreaker;
    a->controlling = !a->controlling;
    return NSBC_OK;
}

int ice_on_stun(ice_agent_t *a, const uint8_t *buf, size_t len, const nsbc_sa_t *src, const nsbc_sa_t *local, uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    stun_msg_t m;
    if (a == NULL || buf == NULL || src == NULL || local == NULL ||
        resp == NULL || resp_len == NULL)
        return NSBC_INVAL;
    (void)resp_cap;
    *resp_len = 0;
    if (stun_parse(buf, len, &m) != NSBC_OK)
        return NSBC_NOENT;
    /* TODO：方法=Binding 且 class=Request→USERNAME/控制/MI 校验→
     * 成功响应（XOR-MAPPED-ADDRESS=src，MESSAGE-INTEGRITY=l_pwd）；
     * 非本 agent 事务返 NOENT；consent 刷新见 ice_tick 联管。 */
    return NSBC_NOTSUP;
}

int ice_tick(ice_agent_t *a, uint64_t now_ms, uint8_t *out, size_t cap, size_t *out_len, nsbc_sa_t *dst, int *need_tx)
{
    if (a == NULL || need_tx == NULL)
        return NSBC_INVAL;
    *need_tx = 0;
    (void)now_ms;
    (void)out;
    (void)cap;
    (void)out_len;
    (void)dst;
    /* TODO：pacing 发检查（RTO 重传 500ms×2 封顶）、consent 周期事务、
     * 30s 无验证→DISCONNECTED，再失败→FAILED 通知呼叫面。 */
    return NSBC_OK;
}

ice_agent_state_t ice_state(const ice_agent_t *a)
{
    return a == NULL ? ICE_ST_CLOSED : a->state;
}

int ice_selected_pair(const ice_agent_t *a, ice_pair_t *out)
{
    int i;
    int best = -1;
    if (a == NULL || out == NULL)
        return NSBC_INVAL;
    for (i = 0; i < a->n_pairs; ++i) {
        if (a->pairs[i].state != ICE_PAIR_SUCCEEDED ||
            !a->pairs[i].nominated)
            continue;
        if (best < 0 || a->pairs[i].priority > a->pairs[best].priority)
            best = i;
    }
    if (best < 0)
        return NSBC_NOENT;
    *out = a->pairs[best];
    return NSBC_OK;
}

int ice_end_of_candidates(ice_agent_t *a)
{
    if (a == NULL)
        return NSBC_INVAL;
    a->eoc = 1;
    /* TODO：清单冻结；COMPLETED 前无 nominated 成功对→FAILED */
    return NSBC_OK;
}
